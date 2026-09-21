// The PDF view.
//
// Windows renders a page to a PNG; WIC turns that into a Direct2D bitmap; this
// draws it on a grey background with a shadow, which is what every page view
// in every program looks like and what the layout view here already looks
// like. There is no parsing, no text, no selection -- a page is a picture
// until the file itself is opened, which is what filling in a form needs and
// is the next thing along.
//
// Rendering is done when a page comes into view and thrown away when it goes
// out of it, with a handful kept: a 500-page PDF at 200% is more bitmaps than
// anybody's video memory wants, and the pages nobody is looking at are the
// cheapest thing to give back.

#include "supernote.h"
#include "res/resource.h"

#include <windowsx.h>
#include <math.h>
#include <d2d1.h>
#include <shellscalingapi.h>

#include "core/doctree.h"
#include "layout/layoutimage.h"
#include "pdf/pdfread.h"
#include "pdf/pdfform.h"
#include "pdf/pdfview.h"
#include "ui/dialogs.h"
#include "ui/statusbar.h"

#define PDFVIEW_CLASS L"OpenNotePdfView"

// How many rendered pages to keep. Eight covers what fits on a screen at any
// sensible zoom, plus the ones either side of it.
#define CACHE_SIZE 8

#define PAGE_GAP   16.0f     // DIPs between pages
#define PAGE_MARGIN 20.0f    // ...and around the stack

struct CachedPage {
    int          index;
    ID2D1Bitmap* bitmap;
    UINT64       used;       // when it was last drawn, for eviction
};

struct PdfViewState {
    PdfFile* pdf;
    WCHAR    path[MAX_PATH];

    int   pageCount;
    float pageWidthPt;       // the first page's size, which sets the layout
    float pageHeightPt;

    float zoom;
    BOOL  userZoomed;        // ...or fitted to the window, which is the default
    float scrollY;           // DIPs from the top of the stack

    ID2D1Factory*          d2d;
    ID2D1HwndRenderTarget* target;
    ID2D1SolidColorBrush*  background;
    ID2D1SolidColorBrush*  paper;
    ID2D1SolidColorBrush*  edge;

    CachedPage cache[CACHE_SIZE];
    UINT64     clock;

    // Placing a picture: the file to put down, and the box being dragged for
    // it, in view coordinates.
    WCHAR stampPath[MAX_PATH];
    BOOL  placing;
    BOOL  dragging;
    float dragFrom[2];
    float dragTo[2];
};

// ---------------------------------------------------------------------------
// Geometry
//
// Every page is laid out as if it were the size of the first one. A PDF may
// mix sizes; drawing each at its own size means the stack's height cannot be
// known without rendering every page, and this is a viewer rather than a
// layout engine.
// ---------------------------------------------------------------------------

static float PageWidthDip(const PdfViewState* st) {
    return st->pageWidthPt * (96.0f / 72.0f) * st->zoom;
}

static float PageHeightDip(const PdfViewState* st) {
    return st->pageHeightPt * (96.0f / 72.0f) * st->zoom;
}

static float StackHeight(const PdfViewState* st) {
    return PAGE_MARGIN * 2 + st->pageCount * PageHeightDip(st) +
           (st->pageCount - 1) * PAGE_GAP;
}

static float PageTop(const PdfViewState* st, int index) {
    return PAGE_MARGIN + index * (PageHeightDip(st) + PAGE_GAP);
}

// The zoom that fits a page across the window, which is what it opens at.
static void FitZoom(HWND hwnd, PdfViewState* st) {
    if (st->userZoomed) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    float width = (float)(rc.right - rc.left);
    if (width < 40.0f) return;

    float pageDip = st->pageWidthPt * (96.0f / 72.0f);
    if (pageDip < 1.0f) return;

    st->zoom = (width - PAGE_MARGIN * 2 - 16.0f) / pageDip;
    if (st->zoom < 0.1f) st->zoom = 0.1f;
    if (st->zoom > 4.0f) st->zoom = 4.0f;
}

static void UpdateScrollRange(HWND hwnd, PdfViewState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    float height = (float)(rc.bottom - rc.top);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)StackHeight(st);
    si.nPage = (UINT)height;
    si.nPos = (int)st->scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

    float maxScroll = StackHeight(st) - height;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (st->scrollY > maxScroll) st->scrollY = maxScroll;
    if (st->scrollY < 0.0f) st->scrollY = 0.0f;
}

// ---------------------------------------------------------------------------
// Rendering, and what is kept
// ---------------------------------------------------------------------------

static void DropCache(PdfViewState* st) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (st->cache[i].bitmap) st->cache[i].bitmap->Release();
        st->cache[i].bitmap = NULL;
        st->cache[i].index = -1;
    }
}

static ID2D1Bitmap* PageBitmap(PdfViewState* st, int index) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (st->cache[i].bitmap && st->cache[i].index == index) {
            st->cache[i].used = ++st->clock;
            return st->cache[i].bitmap;
        }
    }

    // Rendered at the size it is drawn, so the text in it is as sharp as the
    // screen allows rather than a scaled-up thumbnail.
    int widthPx = (int)(PageWidthDip(st) + 0.5f);
    if (widthPx < 1) return NULL;
    if (widthPx > 4000) widthPx = 4000;

    BYTE* bytes = NULL;
    size_t len = 0;
    if (!Pdf_RenderPage(st->pdf, index, widthPx, &bytes, &len)) return NULL;

    // A rendered page is a picture like any other, so it becomes a bitmap the
    // same way every other picture in this program does.
    DocImage image = {};
    image.bytes = bytes;
    image.len = len;
    wcscpy_s(image.contentType, 64, L"image/png");

    ID2D1Bitmap* bitmap = LayoutImage_Create(st->target, &image);
    free(bytes);
    if (!bitmap) return NULL;

    // The least recently drawn slot gives way.
    int slot = 0;
    for (int i = 1; i < CACHE_SIZE; i++) {
        if (!st->cache[i].bitmap) { slot = i; break; }
        if (st->cache[i].used < st->cache[slot].used) slot = i;
    }

    if (st->cache[slot].bitmap) st->cache[slot].bitmap->Release();
    st->cache[slot].bitmap = bitmap;
    st->cache[slot].index = index;
    st->cache[slot].used = ++st->clock;
    return bitmap;
}

static void DiscardTarget(PdfViewState* st) {
    DropCache(st);
    if (st->background) { st->background->Release(); st->background = NULL; }
    if (st->paper)      { st->paper->Release();      st->paper = NULL; }
    if (st->edge)       { st->edge->Release();       st->edge = NULL; }
    if (st->target)     { st->target->Release();     st->target = NULL; }
}

static BOOL EnsureTarget(HWND hwnd, PdfViewState* st) {
    if (st->target) return TRUE;

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

    st->target->CreateSolidColorBrush(D2D1::ColorF(0.43f, 0.45f, 0.48f), &st->background);
    st->target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &st->paper);
    st->target->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.32f, 0.35f), &st->edge);
    return TRUE;
}

static void Paint(HWND hwnd, PdfViewState* st) {
    if (!EnsureTarget(hwnd, st)) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    float viewHeight = (float)(rc.bottom - rc.top);
    float viewWidth = (float)(rc.right - rc.left);

    st->target->BeginDraw();
    st->target->Clear(D2D1::ColorF(0.43f, 0.45f, 0.48f));

    float pageW = PageWidthDip(st);
    float pageH = PageHeightDip(st);
    float left = (viewWidth - pageW) / 2.0f;
    if (left < PAGE_MARGIN) left = PAGE_MARGIN;

    for (int i = 0; i < st->pageCount; i++) {
        float top = PageTop(st, i) - st->scrollY;
        if (top > viewHeight) break;
        if (top + pageH < 0.0f) continue;

        D2D1_RECT_F paper = D2D1::RectF(left, top, left + pageW, top + pageH);

        // The paper goes down first: a page still being rendered is a blank
        // sheet rather than a hole in the window.
        st->target->FillRectangle(paper, st->paper);

        ID2D1Bitmap* bitmap = PageBitmap(st, i);
        if (bitmap) st->target->DrawBitmap(bitmap, paper);

        st->target->DrawRectangle(paper, st->edge, 1.0f);
    }

    // The box being dragged for a stamp, over the top of everything.
    if (st->dragging) {
        D2D1_RECT_F box = D2D1::RectF(
            min(st->dragFrom[0], st->dragTo[0]), min(st->dragFrom[1], st->dragTo[1]),
            max(st->dragFrom[0], st->dragTo[0]), max(st->dragFrom[1], st->dragTo[1]));

        st->target->DrawRectangle(box, st->edge, 2.0f);
    }

    HRESULT hr = st->target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DiscardTarget(st);
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

static void ScrollTo(HWND hwnd, PdfViewState* st, float y) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    float maxScroll = StackHeight(st) - (float)(rc.bottom - rc.top);
    if (maxScroll < 0.0f) maxScroll = 0.0f;

    if (y < 0.0f) y = 0.0f;
    if (y > maxScroll) y = maxScroll;

    if (y == st->scrollY) return;
    st->scrollY = y;

    SetScrollPos(hwnd, SB_VERT, (int)y, TRUE);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void Zoom(HWND hwnd, PdfViewState* st, float factor) {
    float was = st->zoom;
    st->zoom *= factor;
    if (st->zoom < 0.1f) st->zoom = 0.1f;
    if (st->zoom > 4.0f) st->zoom = 4.0f;
    if (st->zoom == was) return;

    st->userZoomed = TRUE;

    // Keep the place: the page under the middle of the window stays there.
    st->scrollY = st->scrollY * (st->zoom / was);

    DropCache(st);     // every bitmap was rendered for the old size
    UpdateScrollRange(hwnd, st);
    InvalidateRect(hwnd, NULL, FALSE);

    WCHAR status[64];
    PdfView_Describe(hwnd, status, 64);
    StatusBar_SetMessage(status);
}

static void FinishStamp(HWND hwnd, PdfViewState* st);

static LRESULT CALLBACK PdfViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PdfViewState* st = (PdfViewState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (st) Paint(hwnd, st);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;      // every pixel is painted

        case WM_SIZE:
            if (st) {
                if (st->target) {
                    D2D1_SIZE_U size = D2D1::SizeU(LOWORD(lParam), HIWORD(lParam));
                    st->target->Resize(size);
                }
                FitZoom(hwnd, st);
                DropCache(st);
                UpdateScrollRange(hwnd, st);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_VSCROLL:
            if (st) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                float page = (float)(rc.bottom - rc.top);
                float line = 40.0f;

                switch (LOWORD(wParam)) {
                    case SB_LINEUP:   ScrollTo(hwnd, st, st->scrollY - line); break;
                    case SB_LINEDOWN: ScrollTo(hwnd, st, st->scrollY + line); break;
                    case SB_PAGEUP:   ScrollTo(hwnd, st, st->scrollY - page); break;
                    case SB_PAGEDOWN: ScrollTo(hwnd, st, st->scrollY + page); break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: {
                        SCROLLINFO si = {};
                        si.cbSize = sizeof(si);
                        si.fMask = SIF_TRACKPOS;
                        GetScrollInfo(hwnd, SB_VERT, &si);
                        ScrollTo(hwnd, st, (float)si.nTrackPos);
                        break;
                    }
                }
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (st) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (GetKeyState(VK_CONTROL) < 0) {
                    Zoom(hwnd, st, delta > 0 ? 1.1f : 1.0f / 1.1f);
                } else {
                    ScrollTo(hwnd, st, st->scrollY - delta * 0.6f);
                }
            }
            return 0;

        case WM_KEYDOWN:
            if (st) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                float page = PageHeightDip(st) + PAGE_GAP;

                switch (wParam) {
                    case VK_DOWN:  ScrollTo(hwnd, st, st->scrollY + 40.0f); return 0;
                    case VK_UP:    ScrollTo(hwnd, st, st->scrollY - 40.0f); return 0;
                    case VK_NEXT:  ScrollTo(hwnd, st, st->scrollY + page); return 0;
                    case VK_PRIOR: ScrollTo(hwnd, st, st->scrollY - page); return 0;
                    case VK_HOME:  ScrollTo(hwnd, st, 0.0f); return 0;
                    case VK_END:   ScrollTo(hwnd, st, StackHeight(st)); return 0;

                    case '0':
                        if (GetKeyState(VK_CONTROL) < 0) {
                            st->userZoomed = FALSE;
                            FitZoom(hwnd, st);
                            DropCache(st);
                            UpdateScrollRange(hwnd, st);
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                        return 0;
                }
            }
            return 0;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            if (st && st->placing) {
                st->dragging = TRUE;
                st->dragFrom[0] = (float)GET_X_LPARAM(lParam);
                st->dragFrom[1] = (float)GET_Y_LPARAM(lParam);
                st->dragTo[0] = st->dragFrom[0];
                st->dragTo[1] = st->dragFrom[1];
                SetCapture(hwnd);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (st && st->dragging) {
                st->dragTo[0] = (float)GET_X_LPARAM(lParam);
                st->dragTo[1] = (float)GET_Y_LPARAM(lParam);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (st && st->dragging) {
                ReleaseCapture();
                st->dragTo[0] = (float)GET_X_LPARAM(lParam);
                st->dragTo[1] = (float)GET_Y_LPARAM(lParam);
                FinishStamp(hwnd, st);
            }
            return 0;

        case WM_SETCURSOR:
            if (st && st->placing) {
                SetCursor(LoadCursorW(NULL, IDC_CROSS));
                return TRUE;
            }
            break;

        case WM_DESTROY:
            if (st) {
                DiscardTarget(st);
                if (st->d2d) st->d2d->Release();
                Pdf_Close(st->pdf);
                free(st);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureClass() {
    static BOOL registered = FALSE;
    if (registered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PdfViewProc;
    wc.hInstance = g_app->hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = PDFVIEW_CLASS;

    RegisterClassExW(&wc);
    registered = TRUE;
}

extern "C" HWND PdfView_Create(HWND hParent, const WCHAR* path) {
    if (!hParent || !path || !path[0]) return NULL;

    PdfFile* pdf = Pdf_Open(path);
    if (!pdf) return NULL;

    PdfViewState* st = (PdfViewState*)calloc(1, sizeof(PdfViewState));
    if (!st) {
        Pdf_Close(pdf);
        return NULL;
    }

    st->pdf = pdf;
    st->pageCount = Pdf_PageCount(pdf);
    st->zoom = 1.0f;
    wcsncpy_s(st->path, MAX_PATH, path, _TRUNCATE);

    for (int i = 0; i < CACHE_SIZE; i++) st->cache[i].index = -1;

    // The first page sets the size of the stack. Letter, if the file will not
    // say -- a viewer that cannot lay out a page is worse than one that
    // guesses the commonest one.
    if (!Pdf_PageSize(pdf, 0, &st->pageWidthPt, &st->pageHeightPt) ||
        st->pageWidthPt < 1.0f || st->pageHeightPt < 1.0f) {
        st->pageWidthPt = 612.0f;
        st->pageHeightPt = 792.0f;
    }

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &st->d2d))) {
        Pdf_Close(pdf);
        free(st);
        return NULL;
    }

    EnsureClass();

    HWND hwnd = CreateWindowExW(
        0, PDFVIEW_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 100, 100,
        hParent, NULL, g_app->hInstance, st);

    if (!hwnd) {
        st->d2d->Release();
        Pdf_Close(pdf);
        free(st);
        return NULL;
    }

    FitZoom(hwnd, st);
    UpdateScrollRange(hwnd, st);
    SetFocus(hwnd);
    return hwnd;
}


// ---------------------------------------------------------------------------
// Placing a stamp
//
// A point on the view is a point on a page, and a page measures in points from
// its bottom left -- so the box somebody drags has to be turned upside down on
// the way in. Getting that wrong puts the signature at the top of the page,
// which is the one mistake that looks like the program working.
// ---------------------------------------------------------------------------

// The arithmetic on its own, so it can be checked without a window: which
// page a point is on, and where on it in the page's own measure.
struct ViewGeometry {
    float viewWidth;
    float scrollY;
    float zoom;
    float pageWidthPt, pageHeightPt;
    int   pageCount;
};

static BOOL MapPoint(const ViewGeometry* g, float x, float y,
                     int* pageOut, float* pageXOut, float* pageYOut) {
    float pageW = g->pageWidthPt * (96.0f / 72.0f) * g->zoom;
    float pageH = g->pageHeightPt * (96.0f / 72.0f) * g->zoom;

    float left = (g->viewWidth - pageW) / 2.0f;
    if (left < PAGE_MARGIN) left = PAGE_MARGIN;

    for (int i = 0; i < g->pageCount; i++) {
        float top = PAGE_MARGIN + i * (pageH + PAGE_GAP) - g->scrollY;
        if (y < top || y > top + pageH) continue;

        *pageOut = i;
        *pageXOut = (x - left) / g->zoom * (72.0f / 96.0f);

        // Down the window is up the page: a PDF measures from the bottom left.
        *pageYOut = g->pageHeightPt - ((y - top) / g->zoom * (72.0f / 96.0f));
        return TRUE;
    }
    return FALSE;
}

static BOOL PageAt(HWND hwnd, PdfViewState* st, float x, float y,
                   int* pageOut, float* pageXOut, float* pageYOut) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    ViewGeometry g;
    g.viewWidth = (float)(rc.right - rc.left);
    g.scrollY = st->scrollY;
    g.zoom = st->zoom;
    g.pageWidthPt = st->pageWidthPt;
    g.pageHeightPt = st->pageHeightPt;
    g.pageCount = st->pageCount;

    return MapPoint(&g, x, y, pageOut, pageXOut, pageYOut);
}

static void FinishStamp(HWND hwnd, PdfViewState* st) {
    st->dragging = FALSE;
    st->placing = FALSE;
    InvalidateRect(hwnd, NULL, FALSE);

    int page = 0;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

    if (!PageAt(hwnd, st, st->dragFrom[0], st->dragFrom[1], &page, &x0, &y0) ||
        !PageAt(hwnd, st, st->dragTo[0], st->dragTo[1], &page, &x1, &y1)) {
        StatusBar_SetMessage(L"The signature has to be placed on a page");
        return;
    }

    float x = min(x0, x1), y = min(y0, y1);
    float width = (float)fabs(x1 - x0), height = (float)fabs(y1 - y0);

    // A click rather than a drag: two inches by three quarters of one, which
    // is about the size of a signature on paper.
    if (width < 8.0f || height < 6.0f) {
        width = 144.0f;
        height = 54.0f;
        y = y - height;
    }

    const WCHAR* why = NULL;
    PdfForm* form = PdfForm_Open(st->path, &why);
    if (!form) {
        MessageBoxW(GetAncestor(hwnd, GA_ROOT),
                    why ? why : L"This PDF could not be opened for signing.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }

    if (!PdfForm_StampImage(form, page, st->stampPath, x, y, width, height)) {
        PdfForm_Close(form);
        MessageBoxW(GetAncestor(hwnd, GA_ROOT),
                    L"That picture could not be placed on the page.",
                    APP_NAME, MB_ICONWARNING);
        return;
    }

    WCHAR saveTo[MAX_PATH] = {0};
    if (!Dialogs_SaveFile(GetAncestor(hwnd, GA_ROOT), saveTo, MAX_PATH, L"signed.pdf")) {
        PdfForm_Close(form);
        return;
    }
    if (!wcsrchr(saveTo, L'.')) wcscat_s(saveTo, MAX_PATH, L".pdf");

    BOOL ok = PdfForm_Save(form, saveTo);
    PdfForm_Close(form);

    if (!ok) {
        MessageBoxW(GetAncestor(hwnd, GA_ROOT), L"The signed PDF could not be written.",
                    APP_NAME, MB_ICONWARNING);
        return;
    }

    // Show what was written, which is the only way anybody trusts it.
    Document* signedDoc = Document_CreateFromFile(saveTo);
    if (signedDoc) MainWindow_OpenDocument(signedDoc);
}

extern "C" BOOL PdfView_BeginStamp(HWND hPdfView, const WCHAR* imagePath) {
    if (!hPdfView || !IsWindow(hPdfView) || !imagePath || !imagePath[0]) return FALSE;

    PdfViewState* st = (PdfViewState*)GetWindowLongPtrW(hPdfView, GWLP_USERDATA);
    if (!st) return FALSE;

    wcsncpy_s(st->stampPath, MAX_PATH, imagePath, _TRUNCATE);
    st->placing = TRUE;
    st->dragging = FALSE;

    SetFocus(hPdfView);
    StatusBar_SetMessage(L"Drag a box where the signature goes, or click to drop one");
    return TRUE;
}

extern "C" BOOL PdfView_Path(HWND hPdfView, WCHAR* out, size_t outChars) {
    if (!out || outChars == 0) return FALSE;
    out[0] = L'\0';

    if (!hPdfView || !IsWindow(hPdfView)) return FALSE;

    PdfViewState* st = (PdfViewState*)GetWindowLongPtrW(hPdfView, GWLP_USERDATA);
    if (!st || !st->path[0]) return FALSE;

    wcsncpy_s(out, outChars, st->path, _TRUNCATE);
    return TRUE;
}

extern "C" void PdfView_Describe(HWND hPdfView, WCHAR* out, size_t outChars) {
    if (!out || outChars == 0) return;
    out[0] = L'\0';

    if (!hPdfView || !IsWindow(hPdfView)) return;

    PdfViewState* st = (PdfViewState*)GetWindowLongPtrW(hPdfView, GWLP_USERDATA);
    if (!st) return;

    swprintf_s(out, outChars, L"PDF - %d page%s at %d%%",
               st->pageCount, st->pageCount == 1 ? L"" : L"s",
               (int)(st->zoom * 100.0f + 0.5f));
}

// ---------------------------------------------------------------------------
// Self-check
//
// Only the mapping, which is the part with arithmetic in it. Putting a
// signature at the top of the page when it was dropped at the bottom is the
// one mistake here that looks like the program working.
// ---------------------------------------------------------------------------

extern "C" BOOL PdfView_SelfTest(char* failure, size_t failureSize) {
    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        return FALSE; \
    } while (0)

    ViewGeometry g;
    g.viewWidth = 1000.0f;
    g.scrollY = 0.0f;
    g.zoom = 1.0f;
    g.pageWidthPt = 612.0f;      // Letter
    g.pageHeightPt = 792.0f;
    g.pageCount = 3;

    float pageH = 792.0f * (96.0f / 72.0f);
    float left = (1000.0f - 612.0f * (96.0f / 72.0f)) / 2.0f;

    int page = -1;
    float x = 0.0f, y = 0.0f;

    // The top left corner of the first page is the *top* left in points.
    if (!MapPoint(&g, left, PAGE_MARGIN, &page, &x, &y)) FAIL("the first page was not found");
    if (page != 0) FAIL("the top of the document is not page one");
    if (x < -0.5f || x > 0.5f) FAIL("the left edge is not zero across");
    if (y < 791.0f || y > 793.0f) FAIL("the top of the page is not its height up");

    // ...and its bottom left corner is the origin.
    if (!MapPoint(&g, left, PAGE_MARGIN + pageH, &page, &x, &y)) FAIL("the page bottom was not found");
    if (y < -1.0f || y > 1.0f) FAIL("the bottom of the page is not the origin");

    // A point between two pages is on neither.
    if (MapPoint(&g, left, PAGE_MARGIN + pageH + PAGE_GAP / 2.0f, &page, &x, &y)) {
        FAIL("the gap between pages was taken for a page");
    }

    // The second page, which is where scrolling and gaps have to agree.
    if (!MapPoint(&g, left, PAGE_MARGIN + pageH + PAGE_GAP + 10.0f, &page, &x, &y)) {
        FAIL("the second page was not found");
    }
    if (page != 1) FAIL("the page after the first is not page two");

    // Scrolled down by a page, the same window point is the next page.
    g.scrollY = pageH + PAGE_GAP;
    if (!MapPoint(&g, left, PAGE_MARGIN + 10.0f, &page, &x, &y)) FAIL("scrolling lost the page");
    if (page != 1) FAIL("scrolling by a page did not move by a page");

    // Zoomed in, a point twice as far down the window is the same place on the
    // page.
    g.scrollY = 0.0f;
    g.zoom = 2.0f;
    if (!MapPoint(&g, left, PAGE_MARGIN + 2.0f * 96.0f, &page, &x, &y)) {
        FAIL("the zoomed page was not found");
    }
    if (y < 719.0f || y > 721.0f) FAIL("zoom moved the point on the page");

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
