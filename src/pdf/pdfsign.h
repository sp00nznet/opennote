#ifndef PDFSIGN_H
#define PDFSIGN_H

// Signing, with a certificate.
//
// The other kind of signature. A picture of one says what ink on paper says;
// this says that the bytes have not changed since somebody with a particular
// private key saw them. A PDF spells that as a detached PKCS#7 over the whole
// file except the hole the signature itself sits in -- the `/ByteRange` -- and
// Windows has done PKCS#7 since 1996, so nothing here implements any crypto.
//
// What this file is careful about: the signature covers what it says it
// covers. The two ranges handed to it are the two ranges written into the
// file, and the self-check verifies a signature it made rather than assuming
// the call that made it worked.

#ifdef __cplusplus
extern "C" {
#endif

// A certificate, as an opaque handle -- a PCCERT_CONTEXT underneath.
typedef void* PdfCertificate;

// Ask the user to choose one of their certificates. NULL when they cancel, or
// when there is nothing to choose from.
PdfCertificate PdfSign_ChooseCertificate(HWND owner);
void           PdfSign_ReleaseCertificate(PdfCertificate certificate);

// Who it says they are, for writing into the signature.
BOOL PdfSign_SubjectName(PdfCertificate certificate, WCHAR* out, size_t outChars);

// A detached PKCS#7 signature over `a` followed by `b` -- the two halves a
// PDF's byte range leaves either side of the signature itself. The caller
// frees the result.
BYTE* PdfSign_Detached(PdfCertificate certificate,
                       const BYTE* a, size_t aLen,
                       const BYTE* b, size_t bLen,
                       size_t* outLen);

// Check one, over the same two ranges. This is what makes the self-check mean
// anything, and it is the first half of reading somebody else's signature.
BOOL PdfSign_VerifyDetached(const BYTE* signature, size_t signatureLen,
                            const BYTE* a, size_t aLen,
                            const BYTE* b, size_t bLen);

// The same check, reporting who the signature says it is from. The name comes
// out of the certificate carried inside the signature, so it is a claim rather
// than a fact: this says the bytes have not changed since somebody holding
// that key signed them, and nothing at all about whether that certificate is
// one anybody should trust. Saying more than that would be the dangerous part.
BOOL PdfSign_VerifyDetachedNamed(const BYTE* signature, size_t signatureLen,
                                 const BYTE* a, size_t aLen,
                                 const BYTE* b, size_t bLen,
                                 WCHAR* signerOut, size_t signerChars);

// A certificate made for a self-check: self-signed, in a key container that
// is deleted with it, and worth nothing to anybody. It exists so that the
// signing path can be exercised without opening the user's certificate store
// -- never call it for a signature anybody is meant to rely on.
PdfCertificate PdfSign_TemporaryCertificate(void);
void           PdfSign_DiscardTemporary(PdfCertificate certificate);

// Self-check, run by `OpenNote.exe --selftest`. Makes a throwaway certificate
// in memory, signs with it and verifies the result -- the user's own
// certificate store is never opened.
BOOL PdfSign_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // PDFSIGN_H
