// Signing, with a certificate.
//
// Windows does the cryptography: CryptSignMessage builds a detached PKCS#7
// over however many pieces of data it is given, which is exactly the shape a
// PDF's byte range has -- the file before the signature, and the file after
// it. Nothing here hashes or encodes anything itself.
//
// The certificate comes from the user's own store, chosen in the standard
// Windows dialog. This program never generates one for real use: a signature
// is only worth what the certificate behind it is worth, and a certificate
// this program made up would be worth nothing.

#include "supernote.h"

#include <wincrypt.h>
#include <cryptuiapi.h>

#include "pdf/pdfsign.h"

// ---------------------------------------------------------------------------
// The certificate
// ---------------------------------------------------------------------------

extern "C" PdfCertificate PdfSign_ChooseCertificate(HWND owner) {
    // "MY" is the personal store: the certificates the user has a private key
    // for, which are the only ones that can sign anything.
    HCERTSTORE store = CertOpenSystemStoreW(0, L"MY");
    if (!store) return NULL;

    PCCERT_CONTEXT chosen = CryptUIDlgSelectCertificateFromStore(
        store, owner,
        L"Sign this PDF",
        L"Choose the certificate to sign with. Only a certificate you hold the "
        L"private key for can sign.",
        CRYPTUI_SELECT_LOCATION_COLUMN, 0, NULL);

    CertCloseStore(store, 0);
    return (PdfCertificate)chosen;
}

extern "C" void PdfSign_ReleaseCertificate(PdfCertificate certificate) {
    if (certificate) CertFreeCertificateContext((PCCERT_CONTEXT)certificate);
}

extern "C" BOOL PdfSign_SubjectName(PdfCertificate certificate, WCHAR* out, size_t outChars) {
    if (!out || outChars == 0) return FALSE;
    out[0] = L'\0';
    if (!certificate) return FALSE;

    DWORD written = CertGetNameStringW((PCCERT_CONTEXT)certificate,
                                       CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                                       out, (DWORD)outChars);
    return written > 1;
}

// ---------------------------------------------------------------------------
// Signing, and checking
// ---------------------------------------------------------------------------

extern "C" BYTE* PdfSign_Detached(PdfCertificate certificate,
                                  const BYTE* a, size_t aLen,
                                  const BYTE* b, size_t bLen,
                                  size_t* outLen) {
    if (outLen) *outLen = 0;
    if (!certificate || !a || aLen == 0) return NULL;

    PCCERT_CONTEXT cert = (PCCERT_CONTEXT)certificate;

    CRYPT_SIGN_MESSAGE_PARA para = {};
    para.cbSize = sizeof(para);
    para.dwMsgEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    para.pSigningCert = cert;
    para.HashAlgorithm.pszObjId = (LPSTR)szOID_NIST_sha256;

    // The signer's own certificate travels with the signature, so whoever
    // opens the file can see who it claims to be from without being told
    // separately.
    para.cMsgCert = 1;
    para.rgpMsgCert = &cert;

    const BYTE* parts[2] = { a, b };
    DWORD sizes[2] = { (DWORD)aLen, (DWORD)bLen };
    DWORD count = (b && bLen) ? 2 : 1;

    DWORD len = 0;
    if (!CryptSignMessage(&para, TRUE, count, parts, sizes, NULL, &len) || len == 0) {
        return NULL;
    }

    BYTE* signature = (BYTE*)malloc(len);
    if (!signature) return NULL;

    if (!CryptSignMessage(&para, TRUE, count, parts, sizes, signature, &len)) {
        free(signature);
        return NULL;
    }

    if (outLen) *outLen = len;
    return signature;
}

extern "C" BOOL PdfSign_VerifyDetached(const BYTE* signature, size_t signatureLen,
                                       const BYTE* a, size_t aLen,
                                       const BYTE* b, size_t bLen) {
    if (!signature || signatureLen == 0 || !a || aLen == 0) return FALSE;

    CRYPT_VERIFY_MESSAGE_PARA para = {};
    para.cbSize = sizeof(para);
    para.dwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

    const BYTE* parts[2] = { a, b };
    DWORD sizes[2] = { (DWORD)aLen, (DWORD)bLen };
    DWORD count = (b && bLen) ? 2 : 1;

    // This checks the signature against the bytes, and nothing else: whether
    // the certificate is trusted, current, or revoked is a separate question
    // and a harder one -- see ROADMAP.md before believing a green tick.
    return CryptVerifyDetachedMessageSignature(&para, 0, signature, (DWORD)signatureLen,
                                               count, parts, sizes, NULL);
}

extern "C" BOOL PdfSign_VerifyDetachedNamed(const BYTE* signature, size_t signatureLen,
                                            const BYTE* a, size_t aLen,
                                            const BYTE* b, size_t bLen,
                                            WCHAR* signerOut, size_t signerChars) {
    if (signerOut && signerChars) signerOut[0] = L'\0';
    if (!signature || signatureLen == 0 || !a || aLen == 0) return FALSE;

    CRYPT_VERIFY_MESSAGE_PARA para = {};
    para.cbSize = sizeof(para);
    para.dwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

    const BYTE* parts[2] = { a, b };
    DWORD sizes[2] = { (DWORD)aLen, (DWORD)bLen };
    DWORD count = (b && bLen) ? 2 : 1;

    PCCERT_CONTEXT signer = NULL;
    BOOL ok = CryptVerifyDetachedMessageSignature(&para, 0, signature, (DWORD)signatureLen,
                                                  count, parts, sizes, &signer);

    if (signer) {
        if (ok && signerOut && signerChars) {
            CertGetNameStringW(signer, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                               signerOut, (DWORD)signerChars);
        }
        CertFreeCertificateContext(signer);
    }

    return ok;
}

// ---------------------------------------------------------------------------
// A certificate for a self-check
// ---------------------------------------------------------------------------

static const WCHAR* const TEMP_CONTAINER = L"opennote-selftest-signing-key";

extern "C" PdfCertificate PdfSign_TemporaryCertificate(void) {
    HCRYPTPROV provider = 0;

    // Any leftover from a run that failed halfway goes first.
    CryptAcquireContextW(&provider, TEMP_CONTAINER, NULL, PROV_RSA_AES, CRYPT_DELETEKEYSET);
    provider = 0;

    if (!CryptAcquireContextW(&provider, TEMP_CONTAINER, NULL, PROV_RSA_AES, CRYPT_NEWKEYSET)) {
        return NULL;
    }

    HCRYPTKEY key = 0;
    if (!CryptGenKey(provider, AT_SIGNATURE, (2048 << 16) | CRYPT_EXPORTABLE, &key)) {
        CryptReleaseContext(provider, 0);
        return NULL;
    }

    BYTE encodedName[512];
    DWORD encodedLen = sizeof(encodedName);
    PCCERT_CONTEXT cert = NULL;

    if (CertStrToNameW(X509_ASN_ENCODING, L"CN=opennote self-check, O=not a real signer",
                       CERT_X500_NAME_STR, NULL, encodedName, &encodedLen, NULL)) {
        CERT_NAME_BLOB subject = { encodedLen, encodedName };

        CRYPT_KEY_PROV_INFO keyInfo = {};
        keyInfo.pwszContainerName = (LPWSTR)TEMP_CONTAINER;
        keyInfo.dwProvType = PROV_RSA_AES;
        keyInfo.dwKeySpec = AT_SIGNATURE;

        CRYPT_ALGORITHM_IDENTIFIER algorithm = {};
        algorithm.pszObjId = (LPSTR)szOID_RSA_SHA256RSA;

        cert = CertCreateSelfSignCertificate(0, &subject, 0, &keyInfo, &algorithm,
                                             NULL, NULL, NULL);
    }

    CryptDestroyKey(key);
    CryptReleaseContext(provider, 0);

    return (PdfCertificate)cert;
}

extern "C" void PdfSign_DiscardTemporary(PdfCertificate certificate) {
    if (certificate) CertFreeCertificateContext((PCCERT_CONTEXT)certificate);

    HCRYPTPROV provider = 0;
    CryptAcquireContextW(&provider, TEMP_CONTAINER, NULL, PROV_RSA_AES, CRYPT_DELETEKEYSET);
}

// ---------------------------------------------------------------------------
// Self-check
//
// A certificate made for the occasion, in memory, with a key container that
// goes away afterwards. Nothing touches the user's store, and nothing this
// makes could be trusted by anybody -- which is the point: what is being
// checked is that a signature made over two ranges verifies over those two
// ranges and fails over anything else.
// ---------------------------------------------------------------------------

extern "C" BOOL PdfSign_SelfTest(char* failure, size_t failureSize) {
    PdfCertificate cert = PdfSign_TemporaryCertificate();
    BYTE* signature = NULL;

    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        free(signature); \
        PdfSign_DiscardTemporary(cert); \
        return FALSE; \
    } while (0)

    if (!cert) FAIL("a self-signed certificate could not be made");

    // Two ranges, the way a PDF hands them over: before the signature and
    // after it.
    const BYTE before[] = "%PDF-1.7\nthe bytes in front of the signature\n";
    const BYTE after[] = "\nthe bytes behind it\n%%EOF\n";

    size_t signatureLen = 0;
    signature = PdfSign_Detached(cert, before, sizeof(before) - 1,
                                 after, sizeof(after) - 1, &signatureLen);
    if (!signature || signatureLen == 0) FAIL("nothing was signed");

    if (!PdfSign_VerifyDetached(signature, signatureLen,
                                before, sizeof(before) - 1,
                                after, sizeof(after) - 1)) {
        FAIL("a signature made here did not verify here");
    }

    // ...and it is a signature over those bytes rather than a rubber stamp: a
    // changed byte, a changed length and a swapped order all have to fail.
    BYTE tampered[sizeof(before)];
    memcpy(tampered, before, sizeof(before));
    tampered[12] = 'X';

    if (PdfSign_VerifyDetached(signature, signatureLen, tampered, sizeof(before) - 1,
                               after, sizeof(after) - 1)) {
        FAIL("a changed byte still verified");
    }

    if (PdfSign_VerifyDetached(signature, signatureLen, before, sizeof(before) - 1,
                               after, sizeof(after) - 2)) {
        FAIL("a shortened range still verified");
    }

    if (PdfSign_VerifyDetached(signature, signatureLen, after, sizeof(after) - 1,
                               before, sizeof(before) - 1)) {
        FAIL("the ranges swapped round still verified");
    }

    WCHAR name[256];
    if (!PdfSign_SubjectName(cert, name, 256) || !name[0]) {
        FAIL("the certificate would not say who it is");
    }

    free(signature);
    PdfSign_DiscardTemporary(cert);

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
