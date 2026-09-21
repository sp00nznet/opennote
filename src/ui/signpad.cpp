// Drawing a signature.
//
// The strokes are kept as points, not as pixels: the window draws them at
// whatever size it happens to be, and the PNG is rendered separately at a
// fixed size with Direct2D, which draws a smooth line where GDI draws a
// staircase. A signature is mostly diagonal strokes, so that difference is
// the whole difference.
//
// The background is transparent. A signature on a white rectangle covers the
// line it is meant to sit on, which is exactly what the scanned-and-emailed
// version does wrong.

#include "supernote.h"
#include "res/resource.h"

#include <windowsx.h>
#include <d2d1.h>
#include <wincodec.h>

#include "core/imagedib.h"
#include "ui/signpad.h"

#define SIGNPAD_CLASS  L"OpenNoteSignPad"

// The picture that comes out. Wide enough for a name, in the proportions a
// signature on paper has.
#define OUTPUT_WIDTH   600
#define OUTPUT_HEIGHT  200

#define MAX_POINTS     4096
#define MAX_STROKES    64

struct Stroke {
    POINT points[MAX_POINTS];
    int   count;
};

struct SignPadState {
    Stroke strokes[MAX_STROKES];
    int    strokeCount;
    BOOL   drawing;

    // The canvas the strokes were drawn on, so they can be scaled to the
    // picture without the aspect changing under them.
    int canvasWidth;
    int canvasHeight;
};

// ---------------------------------------------------------------------------
// The canvas
// ---------------------------------------------------------------------------

static void PaintCanvas(HWND hwnd, SignPadState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);

    // Painted into a bitmap first: a signature drawn stroke by stroke onto the
    // window flickers, and flicker while somebody is signing their name is
    // the one place it is unforgivable.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(mem, bitmap);

    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(mem, &rc, white);
    DeleteObject(white);

    // The line somebody signs on, because a blank white box gives the hand
    // nothing to aim at.
    HPEN guide = CreatePen(PS_SOLID, 1, RGB(190, 195, 205));
    HPEN oldPen = (HPEN)SelectObject(mem, guide);

    int baseline = rc.bottom * 3 / 4;
    MoveToEx(mem, 24, baseline, NULL);
    LineTo(mem, rc.right - 24, baseline);

    SelectObject(mem, oldPen);
    DeleteObject(guide);

    HPEN ink = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
    oldPen = (HPEN)SelectObject(mem, ink);

    for (int s = 0; s < st->strokeCount; s++) {
        const Stroke* stroke = &st->strokes[s];
        if (stroke->count < 2) {
            // A dot: the tittle on an i, and whatever somebody's flourish does.
            if (stroke->count == 1) {
                Ellipse(mem, stroke->points[0].x - 1, stroke->points[0].y - 1,
                        stroke->points[0].x + 2, stroke->points[0].y + 2);
            }
            continue;
        }
        Polyline(mem, stroke->points, stroke->count);
    }

    SelectObject(mem, oldPen);
    DeleteObject(ink);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);

    EndPaint(hwnd, &ps);
}

static void AddPoint(SignPadState* st, int x, int y) {
    if (st->strokeCount == 0 || st->strokeCount > MAX_STROKES) return;

    Stroke* stroke = &st->strokes[st->strokeCount - 1];
    if (stroke->count >= MAX_POINTS) return;

    stroke->points[stroke->count].x = x;
    stroke->points[stroke->count].y = y;
    stroke->count++;
}

static LRESULT CALLBACK SignPadProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SignPadState* st = (SignPadState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }

        case WM_PAINT:
            if (st) PaintCanvas(hwnd, st);
            else    ValidateRect(hwnd, NULL);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN:
            if (st && st->strokeCount < MAX_STROKES) {
                st->drawing = TRUE;
                st->strokes[st->strokeCount].count = 0;
                st->strokeCount++;

                AddPoint(st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                SetCapture(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (st && st->drawing) {
                AddPoint(st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (st && st->drawing) {
                st->drawing = FALSE;
                ReleaseCapture();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_SIZE:
            if (st) {
                st->canvasWidth = LOWORD(lParam);
                st->canvasHeight = HIWORD(lParam);
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
    wc.lpfnWndProc = SignPadProc;
    wc.hInstance = g_app->hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
    wc.lpszClassName = SIGNPAD_CLASS;

    RegisterClassExW(&wc);
    registered = TRUE;
}

// ---------------------------------------------------------------------------
// The picture
// ---------------------------------------------------------------------------

// The strokes, drawn again at the size the picture wants, with Direct2D --
// which antialiases, so the diagonals a signature is made of come out smooth
// rather than as stairs.
static BYTE* RenderStrokes(const SignPadState* st, size_t* lenOut) {
    if (st->strokeCount == 0 || st->canvasWidth < 2 || st->canvasHeight < 2) return NULL;

    ID2D1Factory* factory = NULL;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) return NULL;

    // A bitmap to draw into, and a WIC one so the pixels can be read back.
    IWICImagingFactory* wic = NULL;
    IWICBitmap* bitmap = NULL;
    ID2D1RenderTarget* target = NULL;
    ID2D1SolidColorBrush* ink = NULL;
    BYTE* png = NULL;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic));

    if (SUCCEEDED(hr)) {
        hr = wic->CreateBitmap(OUTPUT_WIDTH, OUTPUT_HEIGHT, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnLoad, &bitmap);
    }

    if (SUCCEEDED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        hr = factory->CreateWicBitmapRenderTarget(bitmap, properties, &target);
    }

    if (SUCCEEDED(hr)) {
        hr = target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &ink);
    }

    if (SUCCEEDED(hr)) {
        float scaleX = (float)OUTPUT_WIDTH / (float)st->canvasWidth;
        float scaleY = (float)OUTPUT_HEIGHT / (float)st->canvasHeight;

        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 0.0f));      // transparent, not white

        for (int s = 0; s < st->strokeCount; s++) {
            const Stroke* stroke = &st->strokes[s];

            if (stroke->count == 1) {
                D2D1_ELLIPSE dot = D2D1::Ellipse(
                    D2D1::Point2F(stroke->points[0].x * scaleX, stroke->points[0].y * scaleY),
                    2.0f, 2.0f);
                target->FillEllipse(dot, ink);
                continue;
            }

            for (int i = 1; i < stroke->count; i++) {
                target->DrawLine(
                    D2D1::Point2F(stroke->points[i - 1].x * scaleX,
                                  stroke->points[i - 1].y * scaleY),
                    D2D1::Point2F(stroke->points[i].x * scaleX,
                                  stroke->points[i].y * scaleY),
                    ink, 3.0f);
            }
        }

        hr = target->EndDraw();
    }

    // Out of the bitmap and into a PNG, premultiplied alpha undone on the way
    // so the edges are not dark where they are half transparent.
    if (SUCCEEDED(hr)) {
        IWICBitmapLock* lock = NULL;
        WICRect all = { 0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT };

        if (SUCCEEDED(bitmap->Lock(&all, WICBitmapLockRead, &lock))) {
            UINT len = 0;
            BYTE* pixels = NULL;

            if (SUCCEEDED(lock->GetDataPointer(&len, &pixels)) && pixels) {
                BYTE* straight = (BYTE*)malloc(len);
                if (straight) {
                    for (UINT i = 0; i + 3 < len; i += 4) {
                        BYTE alpha = pixels[i + 3];

                        if (alpha == 0) {
                            straight[i] = straight[i + 1] = straight[i + 2] = 0;
                        } else {
                            straight[i + 0] = (BYTE)(pixels[i + 0] * 255 / alpha);
                            straight[i + 1] = (BYTE)(pixels[i + 1] * 255 / alpha);
                            straight[i + 2] = (BYTE)(pixels[i + 2] * 255 / alpha);
                        }
                        straight[i + 3] = alpha;
                    }

                    png = ImageDib_EncodePng(straight, OUTPUT_WIDTH, OUTPUT_HEIGHT, lenOut);
                    free(straight);
                }
            }
            lock->Release();
        }
    }

    if (ink) ink->Release();
    if (target) target->Release();
    if (bitmap) bitmap->Release();
    if (wic) wic->Release();
    factory->Release();

    return png;
}

// ---------------------------------------------------------------------------
// The dialog around it
// ---------------------------------------------------------------------------

struct SignPadDialog {
    SignPadState* pad;
    HWND          canvas;
    BOOL          save;
    WCHAR         name[64];
};

static INT_PTR CALLBACK SignPadDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SignPadDialog* data = (SignPadDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_INITDIALOG: {
            data = (SignPadDialog*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);

            EnsureClass();

            // The canvas sits where the placeholder is in the template, which
            // is how it inherits the dialog's layout without doing arithmetic.
            HWND placeholder = GetDlgItem(hwnd, IDC_SIGNPAD_CANVAS);
            RECT rc;
            GetWindowRect(placeholder, &rc);
            MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
            ShowWindow(placeholder, SW_HIDE);

            data->canvas = CreateWindowExW(
                WS_EX_CLIENTEDGE, SIGNPAD_CLASS, NULL,
                WS_CHILD | WS_VISIBLE,
                rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                hwnd, NULL, g_app->hInstance, data->pad);

            data->pad->canvasWidth = rc.right - rc.left;
            data->pad->canvasHeight = rc.bottom - rc.top;

            CheckDlgButton(hwnd, IDC_SIGNPAD_REMEMBER, BST_CHECKED);
            SetDlgItemTextW(hwnd, IDC_SIGNPAD_NAME, L"My signature");
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SIGNPAD_CLEAR:
                    data->pad->strokeCount = 0;
                    InvalidateRect(data->canvas, NULL, FALSE);
                    return TRUE;

                case IDOK:
                    if (data->pad->strokeCount == 0) {
                        MessageBoxW(hwnd, L"Sign in the box first.", APP_NAME,
                                    MB_ICONINFORMATION);
                        return TRUE;
                    }

                    data->save = IsDlgButtonChecked(hwnd, IDC_SIGNPAD_REMEMBER) == BST_CHECKED;
                    GetDlgItemTextW(hwnd, IDC_SIGNPAD_NAME, data->name, 64);

                    EndDialog(hwnd, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }

    return FALSE;
}

extern "C" BYTE* SignPad_Draw(HWND owner, size_t* lenOut, BOOL* saveOut,
                              WCHAR* nameOut, size_t nameChars) {
    if (lenOut) *lenOut = 0;
    if (saveOut) *saveOut = FALSE;
    if (nameOut && nameChars) nameOut[0] = L'\0';

    SignPadState* pad = (SignPadState*)calloc(1, sizeof(SignPadState));
    if (!pad) return NULL;

    SignPadDialog data = {};
    data.pad = pad;

    INT_PTR result = DialogBoxParamW(g_app->hInstance, MAKEINTRESOURCEW(IDD_SIGNPAD),
                                     owner, SignPadDialogProc, (LPARAM)&data);

    BYTE* png = NULL;
    if (result == IDOK) {
        png = RenderStrokes(pad, lenOut);

        if (saveOut) *saveOut = data.save;
        if (nameOut && nameChars) wcsncpy_s(nameOut, nameChars, data.name, _TRUNCATE);
    }

    free(pad);
    return png;
}
