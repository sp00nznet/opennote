// Decoding a picture into a DIB, through WIC. See imagedib.h for why this
// exists at all.

#define COBJMACROS

#include "supernote.h"
#include "core/imagedib.h"

#include <wincodec.h>
#include <objbase.h>

#pragma comment(lib, "windowscodecs.lib")


// The same road as above, stopping at 32-bit BGRA and keeping the rows the way
// round a picture has them rather than the way round a DIB does.

// Encoding, which is the same road in the other direction: a stream, an
// encoder, a frame, the pixels, and a commit at each level because WIC writes
// nothing until it is told to.
BYTE* ImageDib_EncodePng(const BYTE* bgra, int width, int height, size_t* lenOut) {
    if (lenOut) *lenOut = 0;
    if (!bgra || width <= 0 || height <= 0) return NULL;

    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL ownCom = SUCCEEDED(init);

    IWICImagingFactory* factory = NULL;
    IWICStream* stream = NULL;
    IWICBitmapEncoder* encoder = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    IStream* memory = NULL;
    BYTE* out = NULL;

    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void**)&factory);

    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(NULL, TRUE, &memory);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromIStream(stream, memory);

    if (SUCCEEDED(hr)) {
        hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL,
                                              &encoder);
    }
    if (SUCCEEDED(hr)) {
        hr = IWICBitmapEncoder_Initialize(encoder, (IStream*)stream,
                                          WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, (UINT)width, (UINT)height);

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);

    if (SUCCEEDED(hr)) {
        UINT stride = (UINT)width * 4;
        hr = IWICBitmapFrameEncode_WritePixels(frame, (UINT)height, stride,
                                               stride * (UINT)height, (BYTE*)bgra);
    }

    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);

    if (SUCCEEDED(hr)) {
        HGLOBAL global = NULL;
        if (SUCCEEDED(GetHGlobalFromStream(memory, &global)) && global) {
            SIZE_T len = GlobalSize(global);
            void* locked = GlobalLock(global);

            if (locked && len > 0) {
                out = (BYTE*)malloc(len);
                if (out) {
                    memcpy(out, locked, len);
                    if (lenOut) *lenOut = len;
                }
            }
            if (locked) GlobalUnlock(global);
        }
    }

    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (memory) IStream_Release(memory);
    if (factory) IWICImagingFactory_Release(factory);
    if (ownCom) CoUninitialize();

    return out;
}

BYTE* ImageDib_DecodeAlpha(const BYTE* bytes, size_t len, int* widthOut, int* heightOut) {
    if (!bytes || !len || !widthOut || !heightOut) return NULL;

    *widthOut = 0;
    *heightOut = 0;

    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL ownCom = SUCCEEDED(init);

    IWICImagingFactory* factory = NULL;
    IWICStream* stream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    BYTE* pixels = NULL;

    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void**)&factory);

    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(stream, (BYTE*)bytes, (DWORD)len);
    if (SUCCEEDED(hr)) {
        hr = IWICImagingFactory_CreateDecoderFromStream(
            factory, (IStream*)stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (SUCCEEDED(hr)) {
        hr = IWICFormatConverter_Initialize(converter, (IWICBitmapSource*)frame,
                                            &GUID_WICPixelFormat32bppBGRA,
                                            WICBitmapDitherTypeNone, NULL, 0.0,
                                            WICBitmapPaletteTypeCustom);
    }

    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_GetSize(converter, &width, &height);
    if (SUCCEEDED(hr) && (width == 0 || height == 0 || width > 20000 || height > 20000)) {
        hr = E_FAIL;
    }

    if (SUCCEEDED(hr)) {
        UINT stride = width * 4;
        size_t total = (size_t)stride * height;

        pixels = (BYTE*)malloc(total);
        if (!pixels) {
            hr = E_OUTOFMEMORY;
        } else {
            hr = IWICFormatConverter_CopyPixels(converter, NULL, stride,
                                                (UINT)total, pixels);
        }
    }

    if (SUCCEEDED(hr)) {
        *widthOut = (int)width;
        *heightOut = (int)height;
    } else {
        free(pixels);
        pixels = NULL;
    }

    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (ownCom) CoUninitialize();

    return pixels;
}

BYTE* ImageDib_Decode(const BYTE* bytes, size_t len,
                      BITMAPINFOHEADER* headerOut, size_t* pixelsLenOut) {
    if (!bytes || !len || !headerOut || !pixelsLenOut) return NULL;

    *pixelsLenOut = 0;
    memset(headerOut, 0, sizeof(*headerOut));

    // The caller may or may not have initialised the apartment; the console
    // tools have not.
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL ownCom = SUCCEEDED(init);

    IWICImagingFactory* factory = NULL;
    IWICStream* stream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    BYTE* pixels = NULL;

    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void**)&factory);

    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr)) {
        hr = IWICStream_InitializeFromMemory(stream, (BYTE*)bytes, (DWORD)len);
    }
    if (SUCCEEDED(hr)) {
        hr = IWICImagingFactory_CreateDecoderFromStream(
            factory, (IStream*)stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);

    // 24-bit BGR, which is what a DIB in RTF wants: no alpha, no palette.
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (SUCCEEDED(hr)) {
        hr = IWICFormatConverter_Initialize(converter, (IWICBitmapSource*)frame,
                                            &GUID_WICPixelFormat24bppBGR,
                                            WICBitmapDitherTypeNone, NULL, 0.0,
                                            WICBitmapPaletteTypeCustom);
    }

    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) {
        hr = IWICFormatConverter_GetSize(converter, &width, &height);
    }
    if (SUCCEEDED(hr) && (width == 0 || height == 0 || width > 20000 || height > 20000)) {
        hr = E_FAIL;
    }

    if (SUCCEEDED(hr)) {
        UINT stride = (width * 3 + 3) & ~3u;     // DIB rows are 4-byte aligned
        size_t total = (size_t)stride * height;

        pixels = (BYTE*)malloc(total);
        if (!pixels) {
            hr = E_OUTOFMEMORY;
        } else {
            // A DIB is bottom-up, so each row is copied to the mirrored one
            // rather than the whole image in one go.
            BYTE* row = (BYTE*)malloc(stride);
            if (!row) {
                hr = E_OUTOFMEMORY;
            } else {
                for (UINT y = 0; y < height && SUCCEEDED(hr); y++) {
                    WICRect rect = { 0, (INT)y, (INT)width, 1 };
                    hr = IWICFormatConverter_CopyPixels(converter, &rect, stride,
                                                        stride, row);
                    if (SUCCEEDED(hr)) {
                        memcpy(pixels + (size_t)(height - 1 - y) * stride, row, stride);
                    }
                }
                free(row);
            }

            if (SUCCEEDED(hr)) {
                headerOut->biSize = sizeof(BITMAPINFOHEADER);
                headerOut->biWidth = (LONG)width;
                headerOut->biHeight = (LONG)height;
                headerOut->biPlanes = 1;
                headerOut->biBitCount = 24;
                headerOut->biCompression = BI_RGB;
                headerOut->biSizeImage = (DWORD)total;
                *pixelsLenOut = total;
            } else {
                free(pixels);
                pixels = NULL;
            }
        }
    }

    if (converter) IWICFormatConverter_Release(converter);
    if (frame)     IWICBitmapFrameDecode_Release(frame);
    if (decoder)   IWICBitmapDecoder_Release(decoder);
    if (stream)    IWICStream_Release(stream);
    if (factory)   IWICImagingFactory_Release(factory);
    if (ownCom)    CoUninitialize();

    return pixels;
}

// A metafile holding one instruction: draw this bitmap. RichEdit takes
// `\wmetafile8` and ignores `\dibitmap` and `\pngblip` entirely, so this is
// what a picture has to become on its way to that view.
BYTE* ImageDib_ToMetafile(const BYTE* pixels, const BITMAPINFOHEADER* header,
                          size_t* lenOut) {
    if (!pixels || !header || !lenOut) return NULL;
    *lenOut = 0;

    HDC screen = GetDC(NULL);
    if (!screen) return NULL;

    // The frame is in hundredths of a millimetre, which is what an enhanced
    // metafile measures itself in.
    int pxPerInchX = GetDeviceCaps(screen, LOGPIXELSX);
    int pxPerInchY = GetDeviceCaps(screen, LOGPIXELSY);
    if (pxPerInchX <= 0) pxPerInchX = 96;
    if (pxPerInchY <= 0) pxPerInchY = 96;

    RECT frame = {
        0, 0,
        (LONG)((double)header->biWidth / pxPerInchX * 2540.0),
        (LONG)((double)header->biHeight / pxPerInchY * 2540.0)
    };

    HDC meta = CreateEnhMetaFileW(screen, NULL, &frame, NULL);
    if (!meta) {
        ReleaseDC(NULL, screen);
        return NULL;
    }

    StretchDIBits(meta, 0, 0, header->biWidth, header->biHeight,
                  0, 0, header->biWidth, header->biHeight,
                  pixels, (const BITMAPINFO*)header, DIB_RGB_COLORS, SRCCOPY);

    HENHMETAFILE emf = CloseEnhMetaFile(meta);
    BYTE* bits = NULL;

    if (emf) {
        UINT size = GetWinMetaFileBits(emf, 0, NULL, MM_ANISOTROPIC, screen);
        if (size > 0) {
            bits = (BYTE*)malloc(size);
            if (bits) {
                if (GetWinMetaFileBits(emf, size, bits, MM_ANISOTROPIC, screen) == size) {
                    *lenOut = size;
                } else {
                    free(bits);
                    bits = NULL;
                }
            }
        }
        DeleteEnhMetaFile(emf);
    }

    ReleaseDC(NULL, screen);
    return bits;
}
