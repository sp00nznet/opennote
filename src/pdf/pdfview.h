#ifndef PDFVIEW_H
#define PDFVIEW_H

// The PDF view: somebody else's document, shown.
//
// A child window in a tab, like the page layout view, and for the same
// reason -- a document does not move house to be looked at. It draws the
// pages Windows renders for it, and nothing more: a PDF opened here cannot be
// edited yet, which is honest about where this is (see ROADMAP.md, v0.11).
//
// The same keys as the page layout view, for the parts that mean anything
// here: Page Up and Page Down move a page, Ctrl+wheel zooms, Ctrl+0 fits.

#ifdef __cplusplus
extern "C" {
#endif

// NULL when the file is not a PDF, or when this Windows has no renderer.
HWND PdfView_Create(HWND hParent, const WCHAR* path);

// Put a picture on a page: the next drag across the view says where it goes,
// and the file is written when it lands. This is a visible signature -- a
// picture of one -- rather than a cryptographic one.
BOOL PdfView_BeginStamp(HWND hPdfView, const BYTE* png, size_t len);

// Type a line onto a page instead: the next click says where the text goes,
// and the file is written when it lands. For the forms nobody declared -- a
// scan, a fax, a page with lines drawn on it and no fields at all.
BOOL PdfView_BeginType(HWND hPdfView, const WCHAR* text, float size);

// Which file this view is showing, for the commands that work on the file
// rather than on what is drawn -- filling in its form, for one.
BOOL PdfView_Path(HWND hPdfView, WCHAR* out, size_t outChars);

// What to show about it: "3 pages at 100%", for the status bar.
void PdfView_Describe(HWND hPdfView, WCHAR* out, size_t outChars);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL PdfView_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // PDFVIEW_H
