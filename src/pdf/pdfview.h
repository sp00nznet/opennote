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

// Which file this view is showing, for the commands that work on the file
// rather than on what is drawn -- filling in its form, for one.
BOOL PdfView_Path(HWND hPdfView, WCHAR* out, size_t outChars);

// What to show about it: "3 pages at 100%", for the status bar.
void PdfView_Describe(HWND hPdfView, WCHAR* out, size_t outChars);

#ifdef __cplusplus
}
#endif

#endif // PDFVIEW_H
