// A picture's bytes to a Direct2D bitmap, through WIC. See layoutimage.h.

#include "supernote.h"
#include "layout/layoutimage.h"

#include <d2d1.h>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

// One factory for the process. Creating one per picture is a COM activation
// each time for no reason, and a document can hold a lot of pictures.
static IWICImagingFactory* WicFactory(void) {
    static IWICImagingFactory* factory = NULL;
    if (factory) return factory;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        factory = NULL;
    }
    return factory;
}

ID2D1Bitmap* LayoutImage_Create(ID2D1RenderTarget* target, const DocImage* image) {
    if (!target || !image || !image->bytes || !image->len) return NULL;

    IWICImagingFactory* wic = WicFactory();
    if (!wic) return NULL;

    IWICStream* stream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    ID2D1Bitmap* bitmap = NULL;

    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory((BYTE*)image->bytes, (DWORD)image->len);
    }
    if (SUCCEEDED(hr)) {
        hr = wic->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnLoad,
                                          &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);

    // Premultiplied BGRA is what Direct2D draws; the conversion also handles a
    // picture with transparency, which a document is entitled to hold.
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, NULL, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        hr = target->CreateBitmapFromWicBitmap(converter, NULL, &bitmap);
    }

    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
    if (stream)    stream->Release();

    return SUCCEEDED(hr) ? bitmap : NULL;
}
