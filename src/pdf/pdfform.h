#ifndef PDFFORM_H
#define PDFFORM_H

// Filling in a PDF form.
//
// The form you have to fill in and send back arrives as a PDF, and the free
// tools for it are adware or a web upload. Windows will render a PDF but it
// will not tell you what is in one, so this is the other half: enough of the
// file format to find the fields, put text in them, and write the file back.
//
// It writes an **incremental update** -- the original bytes, untouched, with
// the changed objects and a new cross-reference table appended. That is how
// PDF was designed to be edited, and it means a signed document keeps the
// bytes its signature covers, and a mistake can be undone by truncating the
// file back to the previous `%%EOF`.
//
// What it understands is deliberately narrow: classic cross-reference tables,
// text fields, and appearance streams simple enough to write by hand. A file
// with a compressed cross-reference stream (PDF 1.5 and later, which needs
// inflate) is refused rather than half-read -- see ROADMAP.md.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PdfForm PdfForm;

// Open a PDF and find its form fields. NULL when the file is not a PDF, has
// no form, or is in a shape this does not read; `whyOut` (optional) is given
// a sentence about which.
PdfForm* PdfForm_Open(const WCHAR* path, const WCHAR** whyOut);
void     PdfForm_Close(PdfForm* form);

int  PdfForm_FieldCount(const PdfForm* form);

// The field's name as the form states it, and what is in it now.
const WCHAR* PdfForm_FieldName(const PdfForm* form, int index);
const WCHAR* PdfForm_FieldValue(const PdfForm* form, int index);

// Is this one a text field somebody is meant to type in? Buttons and choice
// lists are listed but not filled.
BOOL PdfForm_FieldIsText(const PdfForm* form, int index);

// Put text in a field. Nothing is written to disk until PdfForm_Save.
BOOL PdfForm_SetFieldValue(PdfForm* form, int index, const WCHAR* text);

// How many pages it has, for the commands that put something on one.
int PdfForm_PageCount(PdfForm* form);

// Stamp a picture onto a page: a signature, an initial, a scanned scribble.
// The rectangle is in points from the bottom left of the page, which is how a
// PDF measures. Nothing is written until PdfForm_Save.
//
// It goes on as an annotation with the picture as its appearance, which is
// the same shape a filled field takes and draws in every reader. This is a
// *visible* signature -- a picture of one -- and not a cryptographic one; see
// ROADMAP.md for the difference and why the second is its own job.
BOOL PdfForm_StampImage(PdfForm* form, int pageIndex, const WCHAR* imagePath,
                        float x, float y, float width, float height);

// Write the filled form. `path` may be the file it came from -- the update is
// appended -- or another name, in which case the original is copied first.
BOOL PdfForm_Save(PdfForm* form, const WCHAR* path);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL PdfForm_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // PDFFORM_H
