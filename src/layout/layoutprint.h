#ifndef LAYOUTPRINT_H
#define LAYOUTPRINT_H

// Printing what the layout engine produced, and with it PDF export.
//
// Windows has shipped a PDF printer since Windows 10, so export is the
// ordinary printing path with the job's output sent to a file. There is no PDF
// library here and there does not need to be.

// The in-box PDF printer. Naming it is what makes an export an export rather
// than a print.
#define LAYOUTPRINT_PDF_DEVICE L"Microsoft Print to PDF"

// Lay `doc` out and print it to a named printer. `outputFile` sends the job's
// output to that path instead of to the device -- which, on the PDF printer,
// writes a .pdf and asks nothing. NULL for an ordinary print.
//
// A printer name rather than a device context, because Direct2D cannot draw on
// a printer DC: `ID2D1DCRenderTarget::BindDC` rejects one outright. Its
// printing path is `ID2D1PrintControl`, which addresses the printer by name
// and keeps the output vector -- text in the PDF stays text.
BOOL LayoutPrint_ToPrinter(DocModel* doc, const WCHAR* printerName,
                           const WCHAR* docName, const WCHAR* outputFile,
                           int* pagesOut);

#endif // LAYOUTPRINT_H
