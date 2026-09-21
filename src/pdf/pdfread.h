#ifndef PDFREAD_H
#define PDFREAD_H

// Reading a PDF.
//
// opennote has written PDFs since v0.8 and could not open one, which is a
// one-way door with a lot of people standing at it: the form you have to fill
// in and send back arrives as a PDF, and the free tools for it are adware or
// a web upload.
//
// This is the first half of the way back -- showing one. Windows has shipped a
// PDF renderer since Windows 8.1 (`Windows.Data.Pdf`), the same one Edge and
// the Reader app use, so nothing here parses a PDF: it asks Windows for a page
// as an image and hands the bytes on. The bytes come back as PNG, which is
// what everything else in this program already knows how to draw.
//
// Filling in a form field and stamping a signature are the other half, and
// they do need the file itself opened -- see ROADMAP.md. Rendering first,
// because a page you cannot see is a page you cannot sign.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PdfFile PdfFile;

// Open a PDF. NULL when the file is not one, is encrypted with a password, or
// when this build of Windows has no PDF renderer.
PdfFile* Pdf_Open(const WCHAR* path);
void     Pdf_Close(PdfFile* pdf);

int  Pdf_PageCount(const PdfFile* pdf);

// The page's size in points, which is what a PDF measures in: 72 to the inch.
BOOL Pdf_PageSize(PdfFile* pdf, int pageIndex, float* widthPt, float* heightPt);

// Render a page to PNG bytes, `widthDip` across (0 for the page's own size at
// 96 to the inch). The width is in DIPs rather than pixels: Windows renders at
// the display's scale, so a page asked for at 400 comes back 900 pixels across
// on a screen at 225%, which is what a bitmap drawn into a 400-DIP box wants.
// The caller frees `bytesOut` with free().
BOOL Pdf_RenderPage(PdfFile* pdf, int pageIndex, int widthDip,
                    BYTE** bytesOut, size_t* lenOut);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL Pdf_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // PDFREAD_H
