// Printing the engine's output, and with it PDF export.
//
// The obvious arrangement -- an ID2D1DCRenderTarget bound to the DC PrintDlg
// hands back -- does not work: BindDC answers E_INVALIDARG for a printer DC,
// because a DC render target blits and a printer cannot be blitted to. Direct2D
// has its own printing path instead, and this is it: a print control takes the
// drawing commands for each page, hands them to the spooler as XPS, and the
// driver turns those into whatever the device wants. It stays vector the whole
// way, so text in the resulting PDF is still text.
//
// Export is the same path with the job's output pointed at a file, which is
// all "print to PDF" has ever been. There is no PDF library here.
//
// This draws the same layout the page view draws. One engine, one set of
// geometry, and the screen and the page cannot drift apart.

#include "supernote.h"
#include "layout/layout.h"
#include "layout/layout_internal.h"
#include "layout/layoutprint.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <documenttarget.h>
#include <shlwapi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

// Draw one laid-out page. Page coordinates: the origin is the paper's corner,
// which is what the print control expects and what the page view draws too.
static void DrawPage(ID2D1DeviceContext* dc, const LaidPage* page,
                     ID2D1SolidColorBrush* text, ID2D1SolidColorBrush* line) {
    for (int c = 0; c < page->cellCount; c++) {
        const LaidCell* cell = &page->cells[c];
        dc->DrawRectangle(D2D1::RectF(cell->x, cell->y,
                                      cell->x + cell->width, cell->y + cell->height),
                          line, 0.75f);
    }

    for (int t = 0; t < page->textCount; t++) {
        const LaidText* lt = &page->texts[t];
        if (!lt->layout) continue;

        // Colour, the same arrangement the page view uses: the engine records
        // which ranges are coloured, and whoever has a device makes the brush.
        for (int i = 0; i < lt->colorCount; i++) {
            COLORREF col = lt->colors[i].color;
            ID2D1SolidColorBrush* brush = NULL;
            if (SUCCEEDED(dc->CreateSolidColorBrush(
                    D2D1::ColorF(GetRValue(col) / 255.0f,
                                 GetGValue(col) / 255.0f,
                                 GetBValue(col) / 255.0f), &brush))) {
                DWRITE_TEXT_RANGE range = { lt->colors[i].start, lt->colors[i].len };
                lt->layout->SetDrawingEffect(brush, range);
                brush->Release();
            }
        }

        dc->DrawTextLayout(D2D1::Point2F(lt->x, lt->y), lt->layout, text,
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

// A Direct2D device. Printing needs the D2D 1.1 interfaces, which hang off a
// Direct3D device -- WARP when there is no usable GPU, which is the common
// case on a server and costs nothing here because nothing is rasterised.
static HRESULT MakeDevice(ID2D1Factory1** factoryOut, ID2D1Device** deviceOut) {
    *factoryOut = NULL;
    *deviceOut = NULL;

    ID3D11Device* d3d = NULL;
    D3D_DRIVER_TYPE types[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    HRESULT hr = E_FAIL;

    for (int i = 0; i < 2 && FAILED(hr); i++) {
        hr = D3D11CreateDevice(NULL, types[i], NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               NULL, 0, D3D11_SDK_VERSION, &d3d, NULL, NULL);
    }
    if (FAILED(hr)) return hr;

    IDXGIDevice* dxgi = NULL;
    hr = d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi);
    d3d->Release();
    if (FAILED(hr)) return hr;

    ID2D1Factory1* factory = NULL;
    D2D1_FACTORY_OPTIONS opts = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                           __uuidof(ID2D1Factory1), &opts, (void**)&factory);
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDevice(dxgi, deviceOut);
        if (SUCCEEDED(hr)) *factoryOut = factory;
        else               factory->Release();
    }

    dxgi->Release();
    return hr;
}

// The spooler's target for one job. `outputFile`, when given, is where the
// job's output lands instead of the device.
static HRESULT MakeTarget(const WCHAR* printerName, const WCHAR* docName,
                          const WCHAR* outputFile,
                          IPrintDocumentPackageTarget** targetOut,
                          IStream** streamOut) {
    *targetOut = NULL;
    *streamOut = NULL;

    IPrintDocumentPackageTargetFactory* factory = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(PrintDocumentPackageTargetFactory), NULL,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof(IPrintDocumentPackageTargetFactory),
                                  (void**)&factory);
    if (FAILED(hr)) return hr;

    IStream* out = NULL;
    if (outputFile && outputFile[0]) {
        hr = SHCreateStreamOnFileEx(outputFile,
                                    STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                    FILE_ATTRIBUTE_NORMAL, TRUE, NULL, &out);
        if (FAILED(hr)) {
            factory->Release();
            return hr;
        }
    }

    hr = factory->CreateDocumentPackageTargetForPrintJob(
        printerName, docName ? docName : L"Document", out, NULL, targetOut);

    factory->Release();

    if (FAILED(hr)) {
        if (out) out->Release();
        return hr;
    }

    *streamOut = out;
    return S_OK;
}

extern "C" BOOL LayoutPrint_ToPrinter(const DocModel* doc, const WCHAR* printerName,
                                      const WCHAR* docName, const WCHAR* outputFile,
                                      int* pagesOut) {
    if (pagesOut) *pagesOut = 0;
    if (!doc || !printerName || !printerName[0]) return FALSE;

    LayoutResult* layout = Layout_Build(doc, L"Calibri", 11.0f);
    if (!layout) return FALSE;

    // The console tools reach this without a window, so the apartment may not
    // be initialised yet. Only undone if this call is the one that made it.
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL ownCom = SUCCEEDED(init);

    ID2D1Factory1* factory = NULL;
    ID2D1Device* device = NULL;
    ID2D1DeviceContext* dc = NULL;
    IWICImagingFactory* wic = NULL;
    IPrintDocumentPackageTarget* target = NULL;
    IStream* stream = NULL;
    ID2D1PrintControl* print = NULL;
    ID2D1SolidColorBrush* textBrush = NULL;
    ID2D1SolidColorBrush* lineBrush = NULL;
    int printed = 0;

    HRESULT hr = MakeDevice(&factory, &device);
    if (SUCCEEDED(hr)) {
        hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    }
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wic));
    }
    if (SUCCEEDED(hr)) {
        hr = MakeTarget(printerName, docName, outputFile, &target, &stream);
    }
    if (SUCCEEDED(hr)) {
        // Anything that cannot stay vector -- nothing here yet, images later --
        // is rasterised at this resolution. The rest is left as drawing
        // commands, which is why the text in an exported PDF is selectable.
        D2D1_PRINT_CONTROL_PROPERTIES props = {
            D2D1_PRINT_FONT_SUBSET_MODE_DEFAULT, 300.0f, D2D1_COLOR_SPACE_SRGB
        };
        hr = device->CreatePrintControl(wic, target, &props, &print);
    }
    if (SUCCEEDED(hr)) {
        hr = dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &textBrush);
    }
    if (SUCCEEDED(hr)) {
        hr = dc->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.4f, 0.4f), &lineBrush);
    }

    if (SUCCEEDED(hr)) {
        float pw = 0.0f, ph = 0.0f;
        Layout_PageSize(layout, &pw, &ph);

        int pages = Layout_PageCount(layout);
        for (int i = 0; i < pages; i++) {
            // A page is a command list: the drawing is recorded rather than
            // rasterised, and the print control replays it into the job.
            ID2D1CommandList* list = NULL;
            if (FAILED(dc->CreateCommandList(&list))) break;

            dc->SetTarget(list);
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
            DrawPage(dc, &layout->pages[i], textBrush, lineBrush);
            HRESULT drawn = dc->EndDraw();
            dc->SetTarget(NULL);

            if (SUCCEEDED(drawn)) drawn = list->Close();
            if (SUCCEEDED(drawn)) drawn = print->AddPage(list, D2D1::SizeF(pw, ph), NULL);

            list->Release();
            if (FAILED(drawn)) break;

            printed++;
        }

        // Closing is what commits the job; a job left open writes nothing.
        if (FAILED(print->Close())) printed = 0;
    }

    if (lineBrush) lineBrush->Release();
    if (textBrush) textBrush->Release();
    if (print)     print->Release();
    if (target)    target->Release();
    if (stream)    stream->Release();
    if (wic)       wic->Release();
    if (dc)        dc->Release();
    if (device)    device->Release();
    if (factory)   factory->Release();

    if (ownCom) CoUninitialize();
    Layout_Free(layout);

    if (pagesOut) *pagesOut = printed;
    return printed > 0;
}
