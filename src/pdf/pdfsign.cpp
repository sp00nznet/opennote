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

// ---------------------------------------------------------------------------
// Trust
//
// Whether the certificate means anything is a different question from whether
// the bytes match, and a harder one: it is a chain from the signer up to a
// root this machine has been told to believe, with every link in date and none
// of them revoked.
//
// Revocation is checked from the cache only. Going out to the network for it
// would hang the window on somebody else's web server, and an answer that
// takes thirty seconds is one nobody waits for -- so "not checked" is reported
// as "not checked" rather than quietly counted as fine.
// ---------------------------------------------------------------------------

static PdfTrust TrustOfCertificate(PCCERT_CONTEXT signer) {
    if (!signer) return PDFTRUST_NO_CHAIN;

    CERT_CHAIN_PARA para = {};
    para.cbSize = sizeof(para);

    // Signing is what the certificate has to be for; a chain that is fine for
    // a web server is not automatically fine for this.
    LPSTR usage[] = { (LPSTR)szOID_PKIX_KP_CODE_SIGNING, (LPSTR)szOID_PKIX_KP_EMAIL_PROTECTION };
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
    para.RequestedUsage.Usage.cUsageIdentifier = 2;
    para.RequestedUsage.Usage.rgpszUsageIdentifier = usage;

    PCCERT_CHAIN_CONTEXT chain = NULL;
    if (!CertGetCertificateChain(NULL, signer, NULL, NULL, &para,
                                 CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
                                 CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY,
                                 NULL, &chain)) {
        return PDFTRUST_NO_CHAIN;
    }

    DWORD status = chain->TrustStatus.dwErrorStatus;
    CertFreeCertificateChain(chain);

    // Reported in the order somebody would want to hear them: the worst first.
    if (status & CERT_TRUST_IS_REVOKED)              return PDFTRUST_REVOKED;
    if (status & CERT_TRUST_IS_UNTRUSTED_ROOT)       return PDFTRUST_UNTRUSTED_ROOT;
    if (status & CERT_TRUST_IS_PARTIAL_CHAIN)        return PDFTRUST_NO_CHAIN;
    if (status & CERT_TRUST_IS_NOT_TIME_VALID)       return PDFTRUST_EXPIRED;
    if (status & CERT_TRUST_REVOCATION_STATUS_UNKNOWN) return PDFTRUST_REVOCATION_UNKNOWN;
    if (status & CERT_TRUST_IS_OFFLINE_REVOCATION)   return PDFTRUST_REVOCATION_UNKNOWN;
    if (status != CERT_TRUST_NO_ERROR)               return PDFTRUST_NO_CHAIN;

    return PDFTRUST_TRUSTED;
}

extern "C" const WCHAR* PdfSign_TrustSentence(PdfTrust trust) {
    switch (trust) {
        case PDFTRUST_TRUSTED:
            return L"the certificate chains to a root this machine trusts";
        case PDFTRUST_UNTRUSTED_ROOT:
            return L"the certificate is not one this machine trusts -- anybody can make one";
        case PDFTRUST_EXPIRED:
            return L"the certificate is out of date";
        case PDFTRUST_REVOKED:
            return L"the certificate has been revoked";
        case PDFTRUST_REVOCATION_UNKNOWN:
            return L"the certificate chains to a trusted root, but whether it has been "
                   L"revoked could not be checked";
        case PDFTRUST_NO_CHAIN:
            return L"the certificate could not be traced to any root";
        default:
            return L"the certificate was not checked";
    }
}

extern "C" BOOL PdfSign_VerifyDetachedNamed(const BYTE* signature, size_t signatureLen,
                                            const BYTE* a, size_t aLen,
                                            const BYTE* b, size_t bLen,
                                            WCHAR* signerOut, size_t signerChars,
                                            PdfTrust* trustOut) {
    if (signerOut && signerChars) signerOut[0] = L'\0';
    if (trustOut) *trustOut = PDFTRUST_NOT_CHECKED;
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

        // Only worth asking when the bytes matched: a certificate behind a
        // broken signature is not evidence of anything.
        if (ok && trustOut) *trustOut = TrustOfCertificate(signer);

        CertFreeCertificateContext(signer);
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Timestamping
//
// RFC 3161: hand a hash to somebody else's server, get back a token saying
// they saw it at a particular moment, and carry that token inside the
// signature as an unauthenticated attribute. Windows fetches the token
// (CryptRetrieveTimeStamp) and attaches it (CryptMsgControl), so again there
// is no cryptography here -- only the plumbing and the honesty about what
// happens when the server does not answer.
// ---------------------------------------------------------------------------

const WCHAR* PDFSIGN_DEFAULT_TIMESTAMP = L"http://timestamp.digicert.com";

// The attribute RFC 3161 uses, which the SDK header does not name.
static const char* const TIMESTAMP_ATTRIBUTE = "1.2.840.113549.1.9.16.2.14";

// What a timestamp is over: the signature value, not the whole message.
// Getting this wrong makes a token only this program can check -- the
// signature verifies everywhere and the timestamp verifies nowhere, which is
// the worst of the three ways this can end.
static BYTE* SignatureValue(const BYTE* signature, size_t signatureLen, DWORD* lenOut) {
    *lenOut = 0;

    HCRYPTMSG message = CryptMsgOpenToDecode(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                             0, 0, 0, NULL, NULL);
    if (!message) return NULL;

    BYTE* value = NULL;

    if (CryptMsgUpdate(message, signature, (DWORD)signatureLen, TRUE)) {
        DWORD len = 0;

        if (CryptMsgGetParam(message, CMSG_ENCRYPTED_DIGEST, 0, NULL, &len) && len > 0) {
            value = (BYTE*)malloc(len);

            if (value && CryptMsgGetParam(message, CMSG_ENCRYPTED_DIGEST, 0, value, &len)) {
                *lenOut = len;
            } else {
                free(value);
                value = NULL;
            }
        }
    }

    CryptMsgClose(message);
    return value;
}

extern "C" BYTE* PdfSign_DetachedTimestamped(PdfCertificate certificate,
                                             const BYTE* a, size_t aLen,
                                             const BYTE* b, size_t bLen,
                                             const WCHAR* timestampUrl,
                                             BOOL* timestampedOut,
                                             size_t* outLen) {
    if (timestampedOut) *timestampedOut = FALSE;

    BYTE* signature = PdfSign_Detached(certificate, a, aLen, b, bLen, outLen);
    if (!signature || !timestampUrl || !timestampUrl[0]) return signature;

    size_t signatureLen = outLen ? *outLen : 0;
    if (signatureLen == 0) return signature;

    DWORD valueLen = 0;
    BYTE* value = SignatureValue(signature, signatureLen, &valueLen);
    if (!value) return signature;

    CRYPT_TIMESTAMP_CONTEXT* token = NULL;
    CRYPT_TIMESTAMP_PARA para = {};
    para.fRequestCerts = TRUE;

    // The one thing that can hang here is somebody else's web server, so it
    // is given a few seconds and no more.
    BOOL got = CryptRetrieveTimeStamp(timestampUrl, TIMESTAMP_NO_AUTH_RETRIEVAL, 8000,
                                      szOID_NIST_sha256, &para, value, valueLen,
                                      &token, NULL, NULL);
    free(value);

    if (!got) return signature;      // unstamped, and the caller is told

    // Open the signature again so the token can be added to it.
    HCRYPTMSG message = CryptMsgOpenToDecode(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                             0, 0, 0, NULL, NULL);
    BYTE* stamped = NULL;

    if (message) {
        if (CryptMsgUpdate(message, signature, (DWORD)signatureLen, TRUE)) {
            CRYPT_ATTRIBUTE attribute = {};
            CRYPT_ATTR_BLOB blob = { token->cbEncoded, token->pbEncoded };

            attribute.pszObjId = (LPSTR)TIMESTAMP_ATTRIBUTE;
            attribute.cValue = 1;
            attribute.rgValue = &blob;

            BYTE* encodedAttribute = NULL;
            DWORD encodedLen = 0;

            if (CryptEncodeObject(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                  PKCS_ATTRIBUTE, &attribute, NULL, &encodedLen) &&
                encodedLen > 0) {
                encodedAttribute = (BYTE*)malloc(encodedLen);
            }

            if (encodedAttribute &&
                CryptEncodeObject(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                  PKCS_ATTRIBUTE, &attribute, encodedAttribute,
                                  &encodedLen)) {

                CMSG_CTRL_ADD_SIGNER_UNAUTH_ATTR_PARA add = {};
                add.cbSize = sizeof(add);
                add.dwSignerIndex = 0;
                add.blob.cbData = encodedLen;
                add.blob.pbData = encodedAttribute;

                if (CryptMsgControl(message, 0, CMSG_CTRL_ADD_SIGNER_UNAUTH_ATTR, &add)) {
                    DWORD stampedLen = 0;

                    if (CryptMsgGetParam(message, CMSG_ENCODED_MESSAGE, 0, NULL,
                                         &stampedLen) && stampedLen > 0) {
                        stamped = (BYTE*)malloc(stampedLen);

                        if (stamped && CryptMsgGetParam(message, CMSG_ENCODED_MESSAGE, 0,
                                                        stamped, &stampedLen)) {
                            if (outLen) *outLen = stampedLen;
                            if (timestampedOut) *timestampedOut = TRUE;
                        } else {
                            free(stamped);
                            stamped = NULL;
                        }
                    }
                }
            }

            free(encodedAttribute);
        }
        CryptMsgClose(message);
    }

    CryptMemFree(token);

    if (stamped) {
        free(signature);
        return stamped;
    }

    // The token came back and could not be attached: better an untimestamped
    // signature than a broken one.
    return signature;
}

extern "C" BOOL PdfSign_ReadTimestamp(const BYTE* signature, size_t signatureLen,
                                      SYSTEMTIME* whenOut, WCHAR* authorityOut,
                                      size_t authorityChars) {
    if (authorityOut && authorityChars) authorityOut[0] = L'\0';
    if (!signature || signatureLen == 0) return FALSE;

    DWORD valueLen = 0;
    BYTE* value = SignatureValue(signature, signatureLen, &valueLen);
    if (!value) return FALSE;

    HCRYPTMSG message = CryptMsgOpenToDecode(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                             0, 0, 0, NULL, NULL);
    if (!message) {
        free(value);
        return FALSE;
    }

    BOOL found = FALSE;

    if (CryptMsgUpdate(message, signature, (DWORD)signatureLen, TRUE)) {
        DWORD attrLen = 0;

        if (CryptMsgGetParam(message, CMSG_SIGNER_UNAUTH_ATTR_PARAM, 0, NULL, &attrLen) &&
            attrLen > 0) {

            CRYPT_ATTRIBUTES* attributes = (CRYPT_ATTRIBUTES*)malloc(attrLen);

            if (attributes &&
                CryptMsgGetParam(message, CMSG_SIGNER_UNAUTH_ATTR_PARAM, 0,
                                 attributes, &attrLen)) {

                for (DWORD i = 0; i < attributes->cAttr && !found; i++) {
                    CRYPT_ATTRIBUTE* attribute = &attributes->rgAttr[i];

                    if (!attribute->pszObjId ||
                        strcmp(attribute->pszObjId, TIMESTAMP_ATTRIBUTE) != 0) continue;
                    if (attribute->cValue == 0) continue;

                    // Checked against the same bytes it was made over. A date
                    // read out of a token nobody verified is a date anybody
                    // could have written.
                    CRYPT_TIMESTAMP_CONTEXT* context = NULL;
                    PCCERT_CONTEXT authority = NULL;

                    if (!CryptVerifyTimeStampSignature(attribute->rgValue[0].pbData,
                                                       attribute->rgValue[0].cbData,
                                                       value, valueLen, NULL,
                                                       &context, &authority, NULL) ||
                        !context) {
                        continue;
                    }

                    if (whenOut) FileTimeToSystemTime(&context->pTimeStamp->ftTime, whenOut);

                    // Who says so, when the token brought its certificate
                    // along -- which is why fRequestCerts was asked for.
                    if (authority && authorityOut && authorityChars) {
                        CertGetNameStringW(authority, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                           0, NULL, authorityOut, (DWORD)authorityChars);
                    }

                    if (authority) CertFreeCertificateContext(authority);
                    CryptMemFree(context);
                    found = TRUE;
                }
            }

            free(attributes);
        }
    }

    CryptMsgClose(message);
    free(value);
    return found;
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

    // The whole point of keeping trust separate: this signature is perfectly
    // intact and made with a certificate worth nothing, and the two answers
    // have to come back different.
    PdfTrust trust = PDFTRUST_NOT_CHECKED;
    WCHAR signer[256] = L"";

    if (!PdfSign_VerifyDetachedNamed(signature, signatureLen,
                                     before, sizeof(before) - 1,
                                     after, sizeof(after) - 1,
                                     signer, 256, &trust)) {
        FAIL("the named verify disagreed with the plain one");
    }

    if (!wcsstr(signer, L"opennote self-check")) FAIL("the signer was not reported");

    if (trust == PDFTRUST_TRUSTED) {
        FAIL("a certificate made a moment ago was reported as trusted");
    }
    if (trust == PDFTRUST_NOT_CHECKED) FAIL("the certificate was not checked at all");

    if (!PdfSign_TrustSentence(trust)[0]) FAIL("there is nothing to say about the trust");

    free(signature);
    PdfSign_DiscardTemporary(cert);

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
