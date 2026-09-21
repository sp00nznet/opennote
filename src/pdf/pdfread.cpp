// Reading a PDF, by asking Windows.
//
// `Windows.Data.Pdf` is a WinRT component, which means three things this file
// has to deal with and nothing else does: the runtime has to be initialised,
// the class is reached through an activation factory rather than CoCreate, and
// every call that might take a while is asynchronous. The async part is the
// only awkward one, and it is awkward in a small way -- these operations run
// against a local file and finish in milliseconds, so waiting for one is a
// poll rather than a state machine.
//
// What comes back is a page rendered to PNG. That is deliberate: every other
// picture in this program is bytes in a buffer decoded by WIC, so a PDF page
// arrives as the same thing the layout engine already draws, and nothing new
// has to learn how to paint.

#include "supernote.h"

#include <roapi.h>
#include <winstring.h>
#include <shcore.h>
#include <windows.data.pdf.h>
#include <windows.storage.streams.h>

#include "pdf/pdfread.h"
#include "core/imagedib.h"

using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Data::Pdf;
using namespace ABI::Windows::Storage::Streams;

// ---------------------------------------------------------------------------
// The runtime, and waiting for it
// ---------------------------------------------------------------------------

// Initialised once and left that way: the renderer is used from the UI thread
// and from the self-check, and RoInitialize refuses a second, different
// threading model rather than complaining later.
static bool EnsureRuntime() {
    static bool tried = false;
    static bool ready = false;

    if (tried) return ready;
    tried = true;

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    ready = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
    return ready;
}

// An operation that has already been started, waited on until it is not
// running any more. Local file work finishes in milliseconds; the timeout is
// there so a broken provider cannot hang the program.
template <typename T>
static bool Await(T* operation, IAsyncInfo** infoOut = NULL) {
    if (!operation) return false;

    IAsyncInfo* info = NULL;
    if (FAILED(operation->QueryInterface(__uuidof(IAsyncInfo), (void**)&info))) return false;

    AsyncStatus status = AsyncStatus::Started;
    for (int waited = 0; waited < 10000; waited += 5) {
        if (FAILED(info->get_Status(&status))) break;
        if (status != AsyncStatus::Started) break;
        Sleep(5);
    }

    if (infoOut) *infoOut = info;
    else         info->Release();

    return status == AsyncStatus::Completed;
}

static HSTRING MakeString(const WCHAR* text) {
    HSTRING s = NULL;
    if (FAILED(WindowsCreateString(text, (UINT32)wcslen(text), &s))) return NULL;
    return s;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

struct PdfFile {
    IPdfDocument* document;
    int           pageCount;
};

extern "C" PdfFile* Pdf_Open(const WCHAR* path) {
    if (!path || !path[0] || !EnsureRuntime()) return NULL;

    // A random access stream over the file, which is the shape WinRT wants and
    // the one thing the Win32 side of the SDK will hand over synchronously.
    IRandomAccessStream* stream = NULL;
    // 0 is read, 1 is read/write -- the enum lives in the WinRT headers this
    // file does not need for anything else.
    if (FAILED(CreateRandomAccessStreamOnFile(path, 0,
                                              __uuidof(IRandomAccessStream),
                                              (void**)&stream))) {
        return NULL;
    }

    HSTRING className = MakeString(RuntimeClass_Windows_Data_Pdf_PdfDocument);
    IPdfDocumentStatics* statics = NULL;
    HRESULT hr = className
        ? RoGetActivationFactory(className, __uuidof(IPdfDocumentStatics), (void**)&statics)
        : E_FAIL;
    if (className) WindowsDeleteString(className);

    if (FAILED(hr) || !statics) {
        stream->Release();
        return NULL;
    }

    IAsyncOperation<PdfDocument*>* op = NULL;
    hr = statics->LoadFromStreamAsync(stream, &op);
    statics->Release();
    stream->Release();

    if (FAILED(hr) || !op) return NULL;

    IPdfDocument* document = NULL;
    if (Await(op)) op->GetResults(&document);
    op->Release();

    if (!document) return NULL;

    PdfFile* pdf = (PdfFile*)calloc(1, sizeof(PdfFile));
    if (!pdf) {
        document->Release();
        return NULL;
    }

    UINT32 pages = 0;
    document->get_PageCount(&pages);

    pdf->document = document;
    pdf->pageCount = (int)pages;
    return pdf;
}

extern "C" void Pdf_Close(PdfFile* pdf) {
    if (!pdf) return;
    if (pdf->document) pdf->document->Release();
    free(pdf);
}

extern "C" int Pdf_PageCount(const PdfFile* pdf) {
    return pdf ? pdf->pageCount : 0;
}

static IPdfPage* GetPage(PdfFile* pdf, int index) {
    if (!pdf || !pdf->document || index < 0 || index >= pdf->pageCount) return NULL;

    IPdfPage* page = NULL;
    if (FAILED(pdf->document->GetPage((UINT32)index, &page))) return NULL;
    return page;
}

extern "C" BOOL Pdf_PageSize(PdfFile* pdf, int pageIndex, float* widthPt, float* heightPt) {
    IPdfPage* page = GetPage(pdf, pageIndex);
    if (!page) return FALSE;

    Size size = {};
    HRESULT hr = page->get_Size(&size);
    page->Release();

    if (FAILED(hr)) return FALSE;

    // Windows answers in pixels at 96 to the inch; a PDF measures in points,
    // which are 72 to the inch, and points are what the rest of this program
    // would have to convert back to anyway.
    if (widthPt)  *widthPt = size.Width * 72.0f / 96.0f;
    if (heightPt) *heightPt = size.Height * 72.0f / 96.0f;
    return TRUE;
}

extern "C" BOOL Pdf_RenderPage(PdfFile* pdf, int pageIndex, int widthDip,
                               BYTE** bytesOut, size_t* lenOut) {
    if (!bytesOut || !lenOut) return FALSE;
    *bytesOut = NULL;
    *lenOut = 0;

    IPdfPage* page = GetPage(pdf, pageIndex);
    if (!page) return FALSE;

    // Somewhere for the picture to go. An in-memory stream, because the page
    // is wanted as bytes rather than as a file.
    HSTRING className = MakeString(RuntimeClass_Windows_Storage_Streams_InMemoryRandomAccessStream);
    IInspectable* inspectable = NULL;
    HRESULT hr = className ? RoActivateInstance(className, &inspectable) : E_FAIL;
    if (className) WindowsDeleteString(className);

    IRandomAccessStream* stream = NULL;
    if (SUCCEEDED(hr) && inspectable) {
        inspectable->QueryInterface(__uuidof(IRandomAccessStream), (void**)&stream);
        inspectable->Release();
    }
    if (!stream) {
        page->Release();
        return FALSE;
    }

    // The width, when one is asked for -- in DIPs, not pixels. Windows renders
    // at the display's scale, so a page asked for at 400 comes back 900 across
    // on a screen at 225%. That is what is wanted: the bitmap is drawn into a
    // 400-DIP box and every pixel the screen has goes into it. Everything else
    // about the render is left alone; the default is the page at 96 to the
    // inch, encoded as PNG.
    IPdfPageRenderOptions* options = NULL;
    if (widthDip > 0) {
        HSTRING optionsClass = MakeString(RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions);
        IInspectable* raw = NULL;
        if (optionsClass && SUCCEEDED(RoActivateInstance(optionsClass, &raw)) && raw) {
            raw->QueryInterface(__uuidof(IPdfPageRenderOptions), (void**)&options);
            raw->Release();
        }
        if (optionsClass) WindowsDeleteString(optionsClass);
        if (options) options->put_DestinationWidth((UINT32)widthDip);
    }

    IAsyncAction* action = NULL;
    hr = options ? page->RenderWithOptionsToStreamAsync(stream, options, &action)
                 : page->RenderToStreamAsync(stream, &action);

    if (options) options->Release();
    page->Release();

    if (FAILED(hr) || !action) {
        stream->Release();
        return FALSE;
    }

    bool rendered = Await(action);
    action->Release();

    if (!rendered) {
        stream->Release();
        return FALSE;
    }

    // Reading it back: an IStream over the same bytes, which is the Win32 end
    // of the same object and needs no buffer marshalling.
    IStream* readable = NULL;
    hr = CreateStreamOverRandomAccessStream(stream, __uuidof(IStream), (void**)&readable);
    stream->Release();
    if (FAILED(hr) || !readable) return FALSE;

    STATSTG stat = {};
    if (FAILED(readable->Stat(&stat, STATFLAG_NONAME)) ||
        stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 256 * 1024 * 1024) {
        readable->Release();
        return FALSE;
    }

    size_t len = (size_t)stat.cbSize.QuadPart;
    BYTE* bytes = (BYTE*)malloc(len);
    if (!bytes) {
        readable->Release();
        return FALSE;
    }

    LARGE_INTEGER zero = {};
    readable->Seek(zero, STREAM_SEEK_SET, NULL);

    ULONG read = 0;
    hr = readable->Read(bytes, (ULONG)len, &read);
    readable->Release();

    if (FAILED(hr) || read != len) {
        free(bytes);
        return FALSE;
    }

    *bytesOut = bytes;
    *lenOut = len;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Self-check
//
// A PDF written by hand, because the check has to prove this reads a file
// somebody else produced rather than one of its own. It is the smallest legal
// document: a catalogue, a page tree, one page, and a content stream that
// draws a black rectangle -- which is also what makes the render checkable,
// since a blank page and a broken render look the same.
// ---------------------------------------------------------------------------

static const char* const MINIMAL_PDF_OBJECTS[] = {
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 288 144] /Contents 4 0 R "
        "/Resources << >> >>",
    NULL,   // the content stream, built below
};

static BOOL WriteMinimalPdf(const WCHAR* path) {
    const char* content = "0 0 0 rg 20 20 248 104 re f\n";

    char body[4096];
    int offsets[8] = {0};
    int len = 0;

    len += sprintf_s(body + len, sizeof(body) - len, "%%PDF-1.4\n");

    for (int i = 0; i < 4; i++) {
        offsets[i + 1] = len;

        if (MINIMAL_PDF_OBJECTS[i]) {
            len += sprintf_s(body + len, sizeof(body) - len, "%d 0 obj\n%s\nendobj\n",
                             i + 1, MINIMAL_PDF_OBJECTS[i]);
        } else {
            len += sprintf_s(body + len, sizeof(body) - len,
                             "%d 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n",
                             i + 1, (int)strlen(content), content);
        }
    }

    int xref = len;
    len += sprintf_s(body + len, sizeof(body) - len, "xref\n0 5\n0000000000 65535 f \n");
    for (int i = 1; i <= 4; i++) {
        len += sprintf_s(body + len, sizeof(body) - len, "%010d 00000 n \n", offsets[i]);
    }
    len += sprintf_s(body + len, sizeof(body) - len,
                     "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n", xref);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    DWORD written = 0;
    BOOL ok = WriteFile(file, body, (DWORD)len, &written, NULL) && written == (DWORD)len;
    CloseHandle(file);
    return ok;
}

extern "C" BOOL Pdf_SelfTest(char* failure, size_t failureSize) {
    WCHAR path[MAX_PATH];
    WCHAR temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf_s(path, MAX_PATH, L"%sopennote-selftest.pdf", temp);

    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        DeleteFileW(path); \
        return FALSE; \
    } while (0)

    if (!WriteMinimalPdf(path)) FAIL("could not write a test PDF");

    PdfFile* pdf = Pdf_Open(path);
    if (!pdf) FAIL("a PDF this program wrote could not be opened again");

    if (Pdf_PageCount(pdf) != 1) {
        Pdf_Close(pdf);
        FAIL("a one-page PDF did not report one page");
    }

    // 288 x 144 points is four inches by two.
    float w = 0.0f, h = 0.0f;
    if (!Pdf_PageSize(pdf, 0, &w, &h) || w < 287.0f || w > 289.0f || h < 143.0f || h > 145.0f) {
        Pdf_Close(pdf);
        FAIL("the page came back the wrong size");
    }

    BYTE* bytes = NULL;
    size_t len = 0;
    if (!Pdf_RenderPage(pdf, 0, 400, &bytes, &len) || !bytes || len < 64) {
        Pdf_Close(pdf);
        FAIL("the page did not render");
    }

    // What came back is a PNG, which is what everything else here can draw.
    BOOL isPng = len > 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
                 bytes[2] == 'N' && bytes[3] == 'G';

    // ...and it has something on it: the rectangle the content stream draws.
    BITMAPINFOHEADER header = {};
    size_t pixelsLen = 0;
    BYTE* pixels = ImageDib_Decode(bytes, len, &header, &pixelsLen);

    BOOL hasInk = FALSE;
    if (pixels) {
        for (size_t i = 0; i + 2 < pixelsLen; i += 3) {
            if (pixels[i] < 64 && pixels[i + 1] < 64 && pixels[i + 2] < 64) {
                hasInk = TRUE;
                break;
            }
        }
    }

    int renderedWidth = pixels ? (int)header.biWidth : 0;

    free(pixels);
    free(bytes);
    Pdf_Close(pdf);

    if (!isPng)   FAIL("the rendered page is not a PNG");
    if (!hasInk)  FAIL("the rendered page came out blank");

    // The width asked for is in DIPs: on a screen at 225% the page comes back
    // 900 across, which is the point. What has to hold is that the width was
    // used at all -- a render that ignored it would come back at the page's
    // own 384.
    if (renderedWidth < 400) FAIL("the rendered page ignored the width it was given");

    DeleteFileW(path);
    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
