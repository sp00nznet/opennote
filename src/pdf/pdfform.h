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

#include "pdf/pdfsign.h"

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

// What kind of field it is. A form is mostly boxes to type in, but the two
// that get ticked and chosen are half of what anybody is actually sent.
typedef enum {
    PDF_FIELD_OTHER,
    PDF_FIELD_TEXT,
    PDF_FIELD_CHECKBOX,
    PDF_FIELD_CHOICE
} PdfFieldKind;

PdfFieldKind PdfForm_FieldKind(const PdfForm* form, int index);

// Is this one a text field somebody is meant to type in?
BOOL PdfForm_FieldIsText(const PdfForm* form, int index);

// A tick box: whether it is ticked, and ticking it. The name of the "on"
// state comes out of the widget's own appearance dictionary -- it is /Yes in
// most forms and something else in plenty of them, and writing the wrong one
// leaves a box that is ticked according to the file and blank on the page.
BOOL PdfForm_FieldChecked(const PdfForm* form, int index);
BOOL PdfForm_SetFieldChecked(PdfForm* form, int index, BOOL checked);

// A choice list: what it offers. The value is set with PdfForm_SetFieldValue,
// like a text field, because that is what a choice's value is.
int          PdfForm_FieldOptionCount(const PdfForm* form, int index);
const WCHAR* PdfForm_FieldOption(const PdfForm* form, int index, int option);

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

// Sign the document with a certificate, cryptographically: a detached PKCS#7
// over the whole file except the hole the signature sits in. What it proves is
// that the bytes have not changed since the holder of that key saw them --
// which is a different claim from the picture PdfForm_StampImage puts on the
// page, and a stronger one.
//
// Nothing is written until PdfForm_Save, which is also when the signing
// happens: the bytes have to exist before they can be signed.
BOOL PdfForm_SignWithCertificate(PdfForm* form, PdfCertificate certificate,
                                 const WCHAR* name, const WCHAR* reason);

// Write the filled form. `path` may be the file it came from -- the update is
// appended -- or another name, in which case the original is copied first.
BOOL PdfForm_Save(PdfForm* form, const WCHAR* path);

// What a file's signature says, if it has one.
//
// `intact` means the bytes covered by the signature have not changed since it
// was made. It does *not* mean the certificate is trusted, current, or
// anybody's in particular -- that is a chain of checks this does not do, and
// a green tick that implied otherwise would be worse than none.
typedef struct {
    BOOL     present;
    BOOL     intact;
    BOOL     coversWholeFile;  // FALSE when something was appended afterwards
    WCHAR    signer[256];

    // What the certificate is worth, which is a separate question from
    // whether the bytes match. See PdfTrust.
    PdfTrust trust;
} PdfSignatureReport;

BOOL PdfForm_CheckSignature(const WCHAR* path, PdfSignatureReport* out);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL PdfForm_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // PDFFORM_H
