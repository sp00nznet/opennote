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

// Where to ask for a timestamp, and whether to ask at all.
//
// A signature says "these bytes have not changed since somebody signed them".
// It does not say *when* -- and once the certificate expires, or is revoked,
// there is no way to tell a signature made while it was valid from one made
// afterwards. A timestamp from a third party is what fixes that, and it is
// why a signature from 2015 is still worth something today.
//
// It costs a network call to somebody else's server. If that fails, the
// signature is still made and still covers the bytes; what is lost is the
// proof of when, and the caller is told so rather than left to assume.
extern const WCHAR* PDFSIGN_DEFAULT_TIMESTAMP;

// A detached PKCS#7 signature over `a` followed by `b` -- the two halves a
// PDF's byte range leaves either side of the signature itself. The caller
// frees the result.
BYTE* PdfSign_Detached(PdfCertificate certificate,
                       const BYTE* a, size_t aLen,
                       const BYTE* b, size_t bLen,
                       size_t* outLen);

// The same, timestamped by `timestampUrl` when one is given and reachable.
// `timestampedOut` says whether it was: FALSE means the signature is made and
// carries no proof of when.
BYTE* PdfSign_DetachedTimestamped(PdfCertificate certificate,
                                  const BYTE* a, size_t aLen,
                                  const BYTE* b, size_t bLen,
                                  const WCHAR* timestampUrl,
                                  BOOL* timestampedOut,
                                  size_t* outLen);

// Check one, over the same two ranges. This is what makes the self-check mean
// anything, and it is the first half of reading somebody else's signature.
BOOL PdfSign_VerifyDetached(const BYTE* signature, size_t signatureLen,
                            const BYTE* a, size_t aLen,
                            const BYTE* b, size_t bLen);

// What can be said about the certificate behind a signature, beyond the fact
// that the bytes match.
//
// These are deliberately separate from "the bytes are unchanged". A signature
// can be perfectly intact and made with a certificate nobody has any reason to
// believe -- a self-signed one, for instance, which anybody can make in a
// minute. Reporting the two as one thing is how a green tick comes to mean
// nothing.
typedef enum {
    PDFTRUST_NOT_CHECKED,
    PDFTRUST_TRUSTED,             // chain built to a trusted root, in date
    PDFTRUST_UNTRUSTED_ROOT,      // the chain ends somewhere this machine does not trust
    PDFTRUST_EXPIRED,
    PDFTRUST_REVOKED,
    PDFTRUST_REVOCATION_UNKNOWN,  // ...could not be checked, which is not the same as fine
    PDFTRUST_NO_CHAIN             // the chain could not be built at all
} PdfTrust;

// A sentence for the status bar or the console, saying what was and was not
// checked. Never claims more than was.
const WCHAR* PdfSign_TrustSentence(PdfTrust trust);

// The same check, reporting who the signature says it is from. The name comes
// out of the certificate carried inside the signature, so it is a claim rather
// than a fact: this says the bytes have not changed since somebody holding
// that key signed them, and nothing at all about whether that certificate is
// one anybody should trust. Saying more than that would be the dangerous part.
BOOL PdfSign_VerifyDetachedNamed(const BYTE* signature, size_t signatureLen,
                                 const BYTE* a, size_t aLen,
                                 const BYTE* b, size_t bLen,
                                 WCHAR* signerOut, size_t signerChars,
                                 PdfTrust* trustOut);

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
