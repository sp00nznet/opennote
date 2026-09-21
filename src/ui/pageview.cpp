// The page view: draws what the layout engine produced.
//
// The engine answers "where does everything go" and knows nothing about
// windows. This is the other half -- a scrollable window that paints those
// pages with Direct2D, which is also what makes the engine's work visible.
//
// C++ for the same reason the engine is: Direct2D and DirectWrite have no C
// bindings. Everything it exposes to the rest of the project is plain C.

#include "supernote.h"
#include "res/resource.h"
#include "layout/layout.h"
#include "layout/layout_internal.h"
#include "ui/pageview.h"

#include <d2d1.h>

#pragma comment(lib, "d2d1.lib")

#define PAGEVIEW_CLASS L"OpenNotePageView"
#define PAGE_GAP_DIP   16.0f

struct PageViewState {
    LayoutResult*         layout;
    DocModel*             doc;          // owned; the layout points into it

    ID2D1Factory*         d2d;
    ID2D1HwndRenderTarget* target;
    ID2D1SolidColorBrush* textBrush;
    ID2D1SolidColorBrush* pageBrush;
    ID2D1SolidColorBrush* lineBrush;

    float zoom;
    BOOL  userZoomed;     // once true, stop fitting the page to the window
    int   scrollY;        // in DIPs
    int   contentHeight;  // total, at current zoom
};

// The client area in DIPs rather than pixels.
//
// A Direct2D window render target works in DIPs at the system DPI, while
// GetClientRect answers in physical pixels. On a scaled display those are not
// the same number, and treating them as if they were drew the page about 1.5x
// too large and ran it off the right of the window.
static void ClientSizeDip(HWND hwnd, ID2D1RenderTarget* rt, float* w, float* h) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    float dpiX = 96.0f, dpiY = 96.0f;
    if (rt) rt->GetDpi(&dpiX, &dpiY);
    if (dpiX <= 0.0f) dpiX = 96.0f;
    if (dpiY <= 0.0f) dpiY = 96.0f;

    if (w) *w = (rc.right - rc.left) * 96.0f / dpiX;
    if (h) *h = (rc.bottom - rc.top) * 96.0f / dpiY;
}

// ---------------------------------------------------------------------------
// Device resources
// ---------------------------------------------------------------------------

static void DiscardTarget(PageViewState* st) {
    if (st->textBrush) { st->textBrush->Release(); st->textBrush = NULL; }
    if (st->pageBrush) { st->pageBrush->Release(); st->pageBrush = NULL; }
    if (st->lineBrush) { st->lineBrush->Release(); st->lineBrush = NULL; }
    if (st->target)    { st->target->Release();    st->target = NULL; }
}

static BOOL EnsureTarget(HWND hwnd, PageViewState* st) {
    if (st->target) return TRUE;
    if (!st->d2d) return FALSE;

    RECT rc;
    GetClientRect(hwnd, &rc);

    D2D1_SIZE_U size = D2D1::SizeU((UINT32)(rc.right - rc.left),
                                   (UINT32)(rc.bottom - rc.top));
    if (size.width == 0 || size.height == 0) return FALSE;

    if (FAILED(st->d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &st->target))) {
        return FALSE;
    }

    st->target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &st->textBrush);
    st->target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &st->pageBrush);
    st->target->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.70f, 0.72f), &st->lineBrush);

    return st->textBrush && st->pageBrush && st->lineBrush;
}

// Colour is carried beside the layout rather than inside it, because the
// engine has no Direct2D to make a brush with. Applying it here is the other
// end of that arrangement. Done once per layout; the effect sticks.
static void ApplyColors(PageViewState* st, LaidText* t) {
    if (!t->colorCount || !t->colors) return;

    for (int i = 0; i < t->colorCount; i++) {
        COLORREF c = t->colors[i].color;
        ID2D1SolidColorBrush* brush = NULL;
        if (FAILED(st->target->CreateSolidColorBrush(
                D2D1::ColorF(GetRValue(c) / 255.0f,
                             GetGValue(c) / 255.0f,
                             GetBValue(c) / 255.0f),
                &brush))) {
            continue;
        }

        DWRITE_TEXT_RANGE range = { t->colors[i].start, t->colors[i].len };
        t->layout->SetDrawingEffect(brush, range);
        brush->Release();   // the layout holds its own reference
    }

    // Applied; do not do it again on the next paint.
    free(t->colors);
    t->colors = NULL;
    t->colorCount = 0;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

static void SetTitle(HWND hwnd, PageViewState* st, const WCHAR* docTitle);

static void UpdateScrollRange(HWND hwnd, PageViewState* st) {
    if (!st->layout) return;

    float pw = 0, ph = 0;
    Layout_PageSize(st->layout, &pw, &ph);

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);

    int pages = Layout_PageCount(st->layout);
    st->contentHeight = (int)((ph + PAGE_GAP_DIP) * pages * st->zoom + PAGE_GAP_DIP);

    int visible = (int)clientH;

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = st->contentHeight > 0 ? st->contentHeight : 0;
    si.nPage = (UINT)visible;

    if (st->scrollY > st->contentHeight - visible) st->scrollY = st->contentHeight - visible;
    if (st->scrollY < 0) st->scrollY = 0;
    si.nPos = st->scrollY;

    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

// Until the user zooms, the page is sized to fit the window's width, the way
// every print preview does -- otherwise a Letter page is wider than a default
// window and the reader only ever sees its left half.
//
// This has to run against the DPI the drawing will use, which means after the
// render target exists. Computing it earlier assumed 96 DPI and the real DPI
// then undid it.
static void FitIfNeeded(HWND hwnd, PageViewState* st) {
    if (st->userZoomed || !st->layout || !st->target) return;

    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(st->layout, &pw, &ph);
    if (pw <= 0.0f) return;

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);
    if (clientW <= 0.0f) return;

    float fit = (clientW - 2 * PAGE_GAP_DIP) / pw;
    if (fit < 0.15f) fit = 0.15f;
    if (fit > 2.0f)  fit = 2.0f;

    if (fit != st->zoom) {
        st->zoom = fit;
        UpdateScrollRange(hwnd, st);
        SetTitle(hwnd, st, NULL);
    }
}

static void Paint(HWND hwnd, PageViewState* st) {
    if (!EnsureTarget(hwnd, st)) {
        ValidateRect(hwnd, NULL);
        return;
    }

    FitIfNeeded(hwnd, st);

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);

    st->target->BeginDraw();
    st->target->SetTransform(D2D1::Matrix3x2F::Identity());
    st->target->Clear(D2D1::ColorF(0.45f, 0.45f, 0.48f));

    if (st->layout) {
        float pw = 0, ph = 0;
        Layout_PageSize(st->layout, &pw, &ph);

        int pages = Layout_PageCount(st->layout);
        float z = st->zoom;

        // Pages are centred horizontally and stacked down the view.
        float pageLeft = (clientW - pw * z) / 2.0f;
        if (pageLeft < PAGE_GAP_DIP) pageLeft = PAGE_GAP_DIP;

        for (int i = 0; i < pages; i++) {
            float pageTop = (PAGE_GAP_DIP + i * (ph + PAGE_GAP_DIP)) * z - st->scrollY;

            // Skip pages entirely off screen: a hundred-page document should
            // not cost a hundred pages of drawing to show one.
            if (pageTop + ph * z < 0) continue;
            if (pageTop > clientH) break;

            D2D1_RECT_F paper = D2D1::RectF(pageLeft, pageTop,
                                            pageLeft + pw * z, pageTop + ph * z);
            st->target->FillRectangle(paper, st->pageBrush);
            st->target->DrawRectangle(paper, st->lineBrush, 1.0f);

            // Draw the page's contents in page coordinates.
            st->target->SetTransform(
                D2D1::Matrix3x2F::Scale(z, z) *
                D2D1::Matrix3x2F::Translation(pageLeft, pageTop));

            const LaidPage* page = &st->layout->pages[i];

            for (int c = 0; c < page->cellCount; c++) {
                const LaidCell* cell = &page->cells[c];
                st->target->DrawRectangle(
                    D2D1::RectF(cell->x, cell->y,
                                cell->x + cell->width, cell->y + cell->height),
                    st->lineBrush, 0.75f);
            }

            for (int t = 0; t < page->textCount; t++) {
                LaidText* lt = &page->texts[t];
                if (!lt->layout) continue;
                ApplyColors(st, lt);
                st->target->DrawTextLayout(D2D1::Point2F(lt->x, lt->y),
                                           lt->layout, st->textBrush,
                                           D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            st->target->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    HRESULT hr = st->target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // The device was lost -- a display change, a remote session. Drop the
        // resources and the next paint rebuilds them.
        DiscardTarget(st);
    }

    ValidateRect(hwnd, NULL);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void SetTitle(HWND hwnd, PageViewState* st, const WCHAR* docTitle) {
    // Remembered, so a retitle on zoom does not drop the document's name.
    static WCHAR remembered[256] = L"Document";
    if (docTitle && docTitle[0]) wcsncpy_s(remembered, 256, docTitle, _TRUNCATE);

    WCHAR title[512];
    swprintf_s(title, 512, L"Print Preview - %s - %d page%s at %d%%",
               remembered,
               Layout_PageCount(st->layout),
               Layout_PageCount(st->layout) == 1 ? L"" : L"s",
               (int)(st->zoom * 100.0f + 0.5f));
    SetWindowTextW(hwnd, title);
}

static LRESULT CALLBACK PageViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PageViewState* st = (PageViewState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }

        case WM_SIZE:
            if (st) {
                if (st->target) {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    D2D1_SIZE_U size = D2D1::SizeU((UINT32)(rc.right - rc.left),
                                                   (UINT32)(rc.bottom - rc.top));
                    st->target->Resize(size);
                }
                UpdateScrollRange(hwnd, st);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_PAINT:
            if (st) {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                Paint(hwnd, st);
                EndPaint(hwnd, &ps);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;   // the render target covers everything

        case WM_VSCROLL:
            if (st) {
                SCROLLINFO si = {};
                si.cbSize = sizeof(si);
                si.fMask = SIF_ALL;
                GetScrollInfo(hwnd, SB_VERT, &si);

                int pos = si.nPos;
                switch (LOWORD(wParam)) {
                    case SB_LINEUP:   pos -= 40; break;
                    case SB_LINEDOWN: pos += 40; break;
                    case SB_PAGEUP:   pos -= si.nPage; break;
                    case SB_PAGEDOWN: pos += si.nPage; break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                }

                st->scrollY = pos;
                UpdateScrollRange(hwnd, st);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (st) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);

                if (GetKeyState(VK_CONTROL) < 0) {
                    float old = st->zoom;
                    st->userZoomed = TRUE;
                    st->zoom *= (delta > 0) ? 1.1f : (1.0f / 1.1f);
                    if (st->zoom < 0.2f) st->zoom = 0.2f;
                    if (st->zoom > 4.0f) st->zoom = 4.0f;
                    // Keep roughly the same place in the document in view.
                    st->scrollY = (int)(st->scrollY * (st->zoom / old));
                    SetTitle(hwnd, st, NULL);
                } else {
                    st->scrollY -= delta;
                }

                UpdateScrollRange(hwnd, st);
                SetTitle(hwnd, st, NULL);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_KEYDOWN:
            if (st) {
                float cw = 0.0f, ch = 0.0f;
                ClientSizeDip(hwnd, st->target, &cw, &ch);
                switch (wParam) {
                    case VK_ESCAPE: DestroyWindow(hwnd); return 0;
                    case 'P':
                        // Printing from a preview is what a preview is for.
                        // The owner already knows how; it prints the same
                        // model through the same engine.
                        if (GetKeyState(VK_CONTROL) < 0) {
                            PostMessageW(GetWindow(hwnd, GW_OWNER), WM_COMMAND,
                                         MAKEWPARAM(IDM_FILE_PRINT, 0), 0);
                        }
                        return 0;
                    case '0':
                        // Back to fitting the window, for when a zoom has gone
                        // somewhere unhelpful.
                        st->userZoomed = FALSE;
                        break;
                    case VK_NEXT:   st->scrollY += (int)ch; break;
                    case VK_PRIOR:  st->scrollY -= (int)ch; break;
                    case VK_DOWN:   st->scrollY += 40; break;
                    case VK_UP:     st->scrollY -= 40; break;
                    case VK_HOME:   st->scrollY = 0; break;
                    case VK_END:    st->scrollY = st->contentHeight; break;
                    default: return 0;
                }
                UpdateScrollRange(hwnd, st);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_DESTROY:
            if (st) {
                DiscardTarget(st);
                if (st->d2d) st->d2d->Release();
                Layout_Free(st->layout);
                Doc_Free(st->doc);
                free(st);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureClass(void) {
    static BOOL registered = FALSE;
    if (registered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PageViewProc;
    wc.hInstance = g_app->hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = PAGEVIEW_CLASS;
    wc.hIcon = LoadIconW(g_app->hInstance, MAKEINTRESOURCEW(IDI_SUPERNOTE));

    RegisterClassExW(&wc);
    registered = TRUE;
}

extern "C" BOOL PageView_Show(HWND hOwner, HWND hRichEdit, const WCHAR* docTitle) {
    if (!hRichEdit) return FALSE;

    DocModel* doc = DocView_Capture(hRichEdit);
    if (!doc) return FALSE;

    PageViewState* st = (PageViewState*)calloc(1, sizeof(PageViewState));
    if (!st) {
        Doc_Free(doc);
        return FALSE;
    }

    st->doc = doc;
    st->zoom = 1.0f;

    st->layout = Layout_Build(doc, L"Calibri", 11.0f);
    if (!st->layout) {
        Doc_Free(doc);
        free(st);
        return FALSE;
    }

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &st->d2d))) {
        Layout_Free(st->layout);
        Doc_Free(doc);
        free(st);
        return FALSE;
    }

    EnsureClass();

    HWND hwnd = CreateWindowExW(
        0, PAGEVIEW_CLASS, L"Print Preview",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 820,
        hOwner, NULL, g_app->hInstance, st);

    if (!hwnd) {
        DiscardTarget(st);
        st->d2d->Release();
        Layout_Free(st->layout);
        Doc_Free(doc);
        free(st);
        return FALSE;
    }

    SetTitle(hwnd, st, docTitle);
    UpdateScrollRange(hwnd, st);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return TRUE;
}
