#include "supernote.h"
#include "res/resource.h"
#include "sync/crypto.h"
#include "sync/oauth.h"
#include "ui/editor_rich.h"
#include "core/inflate.h"
#include "pdf/pdfread.h"
#include "pdf/pdfform.h"
#include "pdf/pdfview.h"
#include "pdf/pdfsign.h"
#include "db/fill_repo.h"

// Twips per DIP. Mirrors the engine's constant, which lives in a C++-only
// header because DirectWrite has no C binding.
#define TWIPS_PER_DIP 15.0f

// Global application state
AppState* g_app = NULL;

// --selftest: run the checks that guard the crypto and parsing paths, print the
// result and exit. This is a /SUBSYSTEM:WINDOWS binary with no console of its
// own, so borrow the caller's when there is one; CI reads the exit code either
// way. 0 means every check held.
static int RunSelfTest(void) {
    // Only borrow a console when there is genuinely nowhere to write. If the
    // caller already redirected stdout -- a pipe, a file, a CI log -- then
    // reopening it on CONOUT$ would throw that output away.
    BOOL attached = FALSE;
    FILE* out = NULL;
    if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL) {
        attached = AttachConsole(ATTACH_PARENT_PROCESS);
        if (attached) freopen_s(&out, "CONOUT$", "w", stdout);
    }

    struct {
        const char* name;
        BOOL (*run)(char*, size_t);
    } checks[] = {
        { "crypto", Crypto_SelfTest },
        { "oauth",  OAuth_SelfTest  },
        { "rich",   Rich_SelfTest   },
        { "doctree",Doc_SelfTest    },
        { "docedit",DocEdit_SelfTest},
        { "docx",   Docx_SelfTest   },
        { "layout", Layout_SelfTest },
        { "inflate",Inflate_SelfTest},
        { "pdf",    Pdf_SelfTest    },
        { "pdfform",PdfForm_SelfTest},
        { "pdfview",PdfView_SelfTest},
        { "pdfsign",PdfSign_SelfTest},
        { "fill",   Fill_SelfTest   },
    };

    int failed = 0;
    for (size_t i = 0; i < ARRAYSIZE(checks); i++) {
        char failure[256] = {0};
        if (checks[i].run(failure, sizeof(failure))) {
            printf("ok    %s\n", checks[i].name);
        } else {
            printf("FAIL  %s: %s\n", checks[i].name, failure);
            failed++;
        }
    }

    printf("%s\n", failed ? "SELFTEST FAILED" : "selftest passed");
    fflush(stdout);
    if (attached) FreeConsole();

    return failed ? 1 : 0;
}

// --docx2rtf <file.docx> [out.rtf]
// Convert a .docx and print (or save) the RTF the rich text view would be
// given. The conversion is the part worth inspecting when a document comes out
// looking wrong, and it is what the conformance harness below checks.
static int RunDocxToRtf(int argc, WCHAR** argv) {
    if (argc < 3) {
        printf("usage: OpenNote.exe --docx2rtf <file.docx> [out.rtf]\n");
        return 2;
    }

    char* rtf = Docx_ReadToRtf(argv[2]);
    if (!rtf) {
        wprintf(L"FAILED: %s\n", Docx_GetLastError());
        return 1;
    }

    if (argc >= 4) {
        FILE* f = NULL;
        if (_wfopen_s(&f, argv[3], L"wb") == 0 && f) {
            fwrite(rtf, 1, strlen(rtf), f);
            fclose(f);
            wprintf(L"wrote %s (%zu bytes)\n", argv[3], strlen(rtf));
        } else {
            wprintf(L"could not write %s\n", argv[3]);
            free(rtf);
            return 1;
        }
    } else {
        fputs(rtf, stdout);
    }

    free(rtf);
    return 0;
}

// --layout-report <file.docx>
// Lay a document out and print what came of it. The engine produces geometry
// and nothing else, so this is the whole of it made visible without a screen --
// useful when a document paginates oddly, and cheap enough that the harness
// runs it over the corpus.
static int RunLayoutReport(int argc, WCHAR** argv) {
    if (argc < 3) {
        printf("usage: OpenNote.exe --layout-report <file.docx>\n");
        return 2;
    }

    DocModel* doc = Docx_ReadToModel(argv[2]);
    if (!doc) {
        wprintf(L"FAILED: %s\n", Docx_GetLastError());
        return 1;
    }

    LayoutResult* r = Layout_BuildUpdating(doc, L"Calibri", 11.0f);
    if (!r) {
        wprintf(L"FAILED: the document could not be laid out\n");
        Doc_Free(doc);
        return 1;
    }

    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(r, &pw, &ph);

    int pages = Layout_PageCount(r);
    wprintf(L"%s\n", argv[2]);
    wprintf(L"  page size   %.0f x %.0f DIPs (%.2f x %.2f inches)\n",
            pw, ph, pw / 96.0f, ph / 96.0f);
    wprintf(L"  pages       %d\n", pages);
    wprintf(L"  paragraphs  %d in the model\n", Doc_CountParas(doc));
    wprintf(L"  tables      %d, %d cells\n", Doc_CountTables(doc), Doc_CountCells(doc));

    for (int i = 0; i < pages; i++) {
        wprintf(L"  page %-3d    %d text pieces, %d cell boxes, %d pictures, "
                L"content to %.1f DIPs\n",
                i + 1, Layout_PageTextCount(r, i), Layout_PageCellCount(r, i),
                Layout_PageImageCount(r, i), Layout_PageContentBottom(r, i));
    }

    Layout_Free(r);
    Doc_Free(doc);
    return 0;
}

// --export-pdf <file.docx> <out.pdf>
// Lay a document out and print it to Windows' PDF printer -- the same path the
// File menu takes, without a window. Export is the part of the engine with an
// artefact to look at, and this is what makes that artefact checkable.
static int RunExportPdf(int argc, WCHAR** argv) {
    if (argc < 4) {
        printf("usage: OpenNote.exe --export-pdf <file.docx> <out.pdf>\n");
        return 2;
    }

    DocModel* doc = Docx_ReadToModel(argv[2]);
    if (!doc) {
        wprintf(L"FAILED: %s\n", Docx_GetLastError());
        return 1;
    }

    int pages = 0;
    BOOL ok = LayoutPrint_ToPrinter(doc, LAYOUTPRINT_PDF_DEVICE, argv[2], argv[3], &pages);
    Doc_Free(doc);

    if (!ok || pages < 1) {
        wprintf(L"FAILED: nothing was written to %s\n", argv[3]);
        return 1;
    }

    wprintf(L"%s -> %s, %d page%s\n", argv[2], argv[3], pages, pages == 1 ? L"" : L"s");
    return 0;
}

// What a PDF says it is: how many pages, how big, and -- with a third
// argument -- one of them written out as a PNG. The same shape as
// --docx-check: the reading path answering on the command line, where it can
// be checked without a window.
static int RunPdfInfo(int argc, WCHAR** argv) {
    if (argc < 3) {
        printf("usage: OpenNote.exe --pdf-info <file.pdf> [page.png] [pageIndex]\n");
        return 2;
    }

    PdfFile* pdf = Pdf_Open(argv[2]);
    if (!pdf) {
        wprintf(L"FAILED: %s could not be opened as a PDF\n", argv[2]);
        return 1;
    }

    int pages = Pdf_PageCount(pdf);
    wprintf(L"%s: %d page%s\n", argv[2], pages, pages == 1 ? L"" : L"s");

    for (int i = 0; i < pages && i < 10; i++) {
        float w = 0.0f, h = 0.0f;
        if (!Pdf_PageSize(pdf, i, &w, &h)) continue;
        wprintf(L"  page %d: %.0f x %.0f points (%.2f x %.2f inches)\n",
                i + 1, w, h, w / 72.0f, h / 72.0f);
    }

    int rc = 0;
    if (argc >= 4) {
        int index = argc >= 5 ? _wtoi(argv[4]) : 0;

        BYTE* bytes = NULL;
        size_t len = 0;
        int want = argc >= 6 ? _wtoi(argv[5]) : 1000;
        if (Pdf_RenderPage(pdf, index, want, &bytes, &len)) {
            FILE* out = NULL;
            if (_wfopen_s(&out, argv[3], L"wb") == 0 && out) {
                fwrite(bytes, 1, len, out);
                fclose(out);
                wprintf(L"  page %d -> %s (%zu bytes)\n", index + 1, argv[3], len);
            } else {
                wprintf(L"FAILED: could not write %s\n", argv[3]);
                rc = 1;
            }
            free(bytes);
        } else {
            wprintf(L"FAILED: page %d did not render\n", index + 1);
            rc = 1;
        }
    }

    Pdf_Close(pdf);
    return rc;
}

// The form in a PDF, on the command line: what its fields are called, and
// filling them in. The window comes later; this is where the reading and the
// writing can be checked without one.
static const WCHAR* FieldKindName(PdfFieldKind kind) {
    switch (kind) {
        case PDF_FIELD_TEXT:     return L"text";
        case PDF_FIELD_CHECKBOX: return L"tick";
        case PDF_FIELD_CHOICE:   return L"list";
        case PDF_FIELD_RADIO:    return L"choice";
        default:                 return L"other";
    }
}

static int RunPdfFields(int argc, WCHAR** argv) {
    if (argc < 3) {
        printf("usage: OpenNote.exe --pdf-fields <file.pdf>\n");
        return 2;
    }

    const WCHAR* why = NULL;
    PdfForm* form = PdfForm_Open(argv[2], &why);
    if (!form) {
        wprintf(L"FAILED: %s\n", why ? why : L"the form could not be read");
        return 1;
    }

    int count = PdfForm_FieldCount(form);
    wprintf(L"%s: %d field%s\n", argv[2], count, count == 1 ? L"" : L"s");

    for (int i = 0; i < count; i++) {
        PdfFieldKind kind = PdfForm_FieldKind(form, i);

        const WCHAR* shown = PdfForm_FieldValue(form, i);
        if (kind == PDF_FIELD_CHECKBOX) {
            shown = PdfForm_FieldChecked(form, i) ? L"[x]" : L"[ ]";
        }

        wprintf(L"  %d  %-28s %-6s %s\n", i, PdfForm_FieldName(form, i),
                FieldKindName(kind), shown);

        for (int o = 0; o < PdfForm_FieldOptionCount(form, i); o++) {
            wprintf(L"         - %s\n", PdfForm_FieldOption(form, i, o));
        }
    }

    PdfForm_Close(form);
    return 0;
}

static int RunPdfFill(int argc, WCHAR** argv) {
    if (argc < 5) {
        printf("usage: OpenNote.exe --pdf-fill <in.pdf> <out.pdf> <field=value> ...\n");
        return 2;
    }

    const WCHAR* why = NULL;
    PdfForm* form = PdfForm_Open(argv[2], &why);
    if (!form) {
        wprintf(L"FAILED: %s\n", why ? why : L"the form could not be read");
        return 1;
    }

    int filled = 0;

    for (int i = 4; i < argc; i++) {
        WCHAR pair[1024];
        wcsncpy_s(pair, 1024, argv[i], _TRUNCATE);

        WCHAR* equals = wcschr(pair, L'=');
        if (!equals) {
            wprintf(L"FAILED: %s is not <field>=<value>\n", argv[i]);
            PdfForm_Close(form);
            return 2;
        }
        *equals = L'\0';

        BOOL found = FALSE;
        for (int f = 0; f < PdfForm_FieldCount(form); f++) {
            if (_wcsicmp(PdfForm_FieldName(form, f), pair) != 0) continue;

            const WCHAR* wanted = equals + 1;

            if (PdfForm_FieldKind(form, f) == PDF_FIELD_CHECKBOX) {
                // A tick box takes a word rather than a value: the ones people
                // type, in either case.
                BOOL tick = _wcsicmp(wanted, L"yes") == 0 || _wcsicmp(wanted, L"on") == 0 ||
                            _wcsicmp(wanted, L"true") == 0 || _wcsicmp(wanted, L"x") == 0 ||
                            wcscmp(wanted, L"1") == 0;

                if (!PdfForm_SetFieldChecked(form, f, tick)) break;
                wprintf(L"  %s = %s\n", pair, tick ? L"[x]" : L"[ ]");
            } else {
                if (!PdfForm_SetFieldValue(form, f, wanted)) break;
                wprintf(L"  %s = %s\n", pair, wanted);
            }

            filled++;
            found = TRUE;
            break;
        }

        if (!found) wprintf(L"  (nothing called \"%s\" that can be filled in)\n", pair);
    }

    BOOL ok = filled > 0 && PdfForm_Save(form, argv[3]);
    PdfForm_Close(form);

    if (!ok) {
        wprintf(L"FAILED: nothing was written to %s\n", argv[3]);
        return 1;
    }

    wprintf(L"%s -> %s, %d field%s filled\n", argv[2], argv[3], filled,
            filled == 1 ? L"" : L"s");
    return 0;
}

// Stamping a picture on a page: a signature, or anything else that has to be
// put where a form has no field for it.
static int RunPdfStamp(int argc, WCHAR** argv) {
    if (argc < 9) {
        printf("usage: OpenNote.exe --pdf-stamp <in.pdf> <out.pdf> <image> "
               "<page> <x> <y> <width> [height]\n"
               "       x, y, width and height are in points from the bottom left\n");
        return 2;
    }

    const WCHAR* why = NULL;
    PdfForm* form = PdfForm_Open(argv[2], &why);
    if (!form) {
        wprintf(L"FAILED: %s\n", why ? why : L"the file could not be read");
        return 1;
    }

    int page = _wtoi(argv[5]);
    float x = (float)_wtof(argv[6]);
    float y = (float)_wtof(argv[7]);
    float width = (float)_wtof(argv[8]);
    float height = argc >= 10 ? (float)_wtof(argv[9]) : width / 3.0f;

    BOOL ok = PdfForm_StampImage(form, page, argv[4], x, y, width, height) &&
              PdfForm_Save(form, argv[3]);

    int pages = PdfForm_PageCount(form);
    PdfForm_Close(form);

    if (!ok) {
        wprintf(L"FAILED: the stamp was not written (the file has %d page%s)\n",
                pages, pages == 1 ? L"" : L"s");
        return 1;
    }

    wprintf(L"%s -> %s, stamped on page %d\n", argv[2], argv[3], page + 1);
    return 0;
}

// Signing on the command line. The certificate is chosen in the standard
// Windows dialog: a signature is worth what the certificate behind it is
// worth, so this program never picks one for you.
static int RunPdfSign(int argc, WCHAR** argv) {
    if (argc < 4) {
        printf("usage: OpenNote.exe --pdf-sign <in.pdf> <out.pdf>\n");
        return 2;
    }

    PdfCertificate certificate = PdfSign_ChooseCertificate(NULL);
    if (!certificate) {
        wprintf(L"FAILED: no certificate was chosen\n");
        return 1;
    }

    WCHAR who[256] = L"";
    PdfSign_SubjectName(certificate, who, 256);

    const WCHAR* why = NULL;
    PdfForm* form = PdfForm_Open(argv[2], &why);
    if (!form) {
        PdfSign_ReleaseCertificate(certificate);
        wprintf(L"FAILED: %s\n", why ? why : L"the file could not be read");
        return 1;
    }

    PdfForm_SignWithCertificate(form, certificate, who, L"Signed with opennote");
    BOOL ok = PdfForm_Save(form, argv[3]);

    PdfForm_Close(form);
    PdfSign_ReleaseCertificate(certificate);

    if (!ok) {
        wprintf(L"FAILED: nothing was written to %s\n", argv[3]);
        return 1;
    }

    wprintf(L"%s -> %s, signed by %s\n", argv[2], argv[3], who);
    wprintf(L"  timestamp: %s\n", PdfForm_WasTimestamped(form)
            ? L"yes -- it proves when as well as what"
            : L"no -- it proves what, not when");
    return 0;
}

// What a file's signature says. The wording is deliberate: this checks that
// the bytes have not changed, which is not the same as the certificate being
// one anybody should trust.
static int RunPdfVerify(int argc, WCHAR** argv) {
    if (argc < 3) {
        printf("usage: OpenNote.exe --pdf-verify <file.pdf>\n");
        return 2;
    }

    PdfSignatureReport report = {0};
    if (!PdfForm_CheckSignature(argv[2], &report)) {
        wprintf(L"FAILED: %s could not be read\n", argv[2]);
        return 1;
    }

    if (!report.present) {
        wprintf(L"%s: no signature\n", argv[2]);
        return 0;
    }

    wprintf(L"%s: signed%s\n", argv[2],
            report.signer[0] ? L"" : L" (the signer is not named)");
    if (report.signer[0]) wprintf(L"  signer:   %s\n", report.signer);

    wprintf(L"  bytes:    %s\n", report.intact
            ? L"unchanged since it was signed"
            : L"DO NOT MATCH the signature");

    if (report.intact) {
        wprintf(L"  trust:    %s\n", PdfSign_TrustSentence(report.trust));
    }

    if (!report.coversWholeFile) {
        wprintf(L"  warning:  something was appended after the signature, "
                L"and that part is not covered\n");
    }

    wprintf(L"  note:     a signature says the bytes have not changed; who is "
            L"behind the certificate is what the trust line is about\n");
    return report.intact ? 0 : 1;
}

// Is timestamping working from this machine?
//
// Makes a certificate for the occasion, signs a few bytes with it, and asks
// the authority for a token -- which is the one part of signing that depends
// on somebody else's server being up and this machine being allowed to reach
// it. Nothing touches the real certificate store, and the throwaway key is
// deleted afterwards.
static int RunTimestampCheck(int argc, WCHAR** argv) {
    const WCHAR* url = argc >= 3 ? argv[2] : PDFSIGN_DEFAULT_TIMESTAMP;

    PdfCertificate certificate = PdfSign_TemporaryCertificate();
    if (!certificate) {
        wprintf(L"FAILED: a certificate for the check could not be made\n");
        return 1;
    }

    const BYTE bytes[] = "opennote timestamp check";

    size_t plainLen = 0;
    BYTE* plain = PdfSign_Detached(certificate, bytes, sizeof(bytes) - 1, NULL, 0, &plainLen);

    BOOL stamped = FALSE;
    size_t stampedLen = 0;
    BYTE* withToken = PdfSign_DetachedTimestamped(certificate, bytes, sizeof(bytes) - 1,
                                                  NULL, 0, url, &stamped, &stampedLen);

    wprintf(L"authority: %s\n", url);
    wprintf(L"signature: %zu bytes\n", plainLen);

    if (stamped) {
        wprintf(L"timestamp: yes -- the token added %zu bytes\n", stampedLen - plainLen);
    } else {
        wprintf(L"timestamp: NO -- the authority could not be reached, or refused\n");
        wprintf(L"           signing still works; what is missing is proof of when\n");
    }

    // A timestamped signature still has to verify, or the token has broken it.
    BOOL verifies = withToken && PdfSign_VerifyDetached(withToken, stampedLen,
                                                        bytes, sizeof(bytes) - 1, NULL, 0);
    wprintf(L"verifies:  %s\n", verifies ? L"yes" : L"NO");

    free(plain);
    free(withToken);
    PdfSign_DiscardTemporary(certificate);

    return (stamped && verifies) ? 0 : 1;
}

// Does `b` contain the same characters as `a`, ignoring whitespace and the
// control characters RichEdit uses for table structure? Used by the round trip
// check below, where flattening a table changes spacing but must not drop a
// word.
static BOOL SameContent(const WCHAR* a, const WCHAR* b) {
    if (!a || !b) return FALSE;

    for (;;) {
        while (*a && (iswspace(*a) || *a < 0x20 || (*a >= 0xFFF9 && *a <= 0xFFFC))) a++;
        while (*b && (iswspace(*b) || *b < 0x20 || (*b >= 0xFFF9 && *b <= 0xFFFC))) b++;

        if (*a != *b) return FALSE;
        if (!*a) return TRUE;
        a++;
        b++;
    }
}

// --docx-check <dir>
//
// The conformance harness. For every X.docx in the directory, convert it and
// check the assertions in the matching X.expect, one per line:
//
//     contains:some text     the converted RTF must contain this
//     absent:some text       it must not
//
// Reports a pass/fail count over the corpus rather than a bare "tests passed",
// and exits non-zero on any failure so a regression fails the build. The corpus
// is generated by tests/make_fixtures.py, so no binary documents are committed.
static int RunDocxCheck(int argc, WCHAR** argv) {
    if (argc < 3) {
        printf("usage: OpenNote.exe --docx-check <corpus-dir>\n");
        return 2;
    }

    // The whole corpus is listed before anything is written, and the round trip
    // output goes to a separate directory. Writing into the directory being
    // enumerated makes FindNextFile's behaviour undefined -- it silently
    // processed a different number of documents depending on the run.
    #define MAX_CORPUS 256
    static WCHAR names[MAX_CORPUS][MAX_PATH];
    int fileCount = 0;

    WCHAR pattern[MAX_PATH];
    swprintf_s(pattern, MAX_PATH, L"%s\\*.docx", argv[2]);

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fileCount >= MAX_CORPUS) break;
            wcscpy_s(names[fileCount++], MAX_PATH, fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    if (fileCount == 0) {
        wprintf(L"no .docx files in %s -- nothing to check\n", argv[2]);
        wprintf(L"generate the corpus with: py tests\\make_fixtures.py <dir>\n");
        return 0;   // a missing corpus is a skip, not a failure
    }

    WCHAR outDir[MAX_PATH];
    swprintf_s(outDir, MAX_PATH, L"%s\\out", argv[2]);
    CreateDirectoryW(outDir, NULL);

    int files = 0, checks = 0, failures = 0;

    // Fidelity is reported separately from pass/fail: a property that does not
    // survive a round trip is a measurement, and the point is that it is a
    // number which can get worse.
    int serCompared = 0, serLost = 0;
    int edCompared = 0, edLost = 0;

    for (int fi = 0; fi < fileCount; fi++) {
        const WCHAR* fileName = names[fi];
        files++;

        WCHAR docPath[MAX_PATH];
        swprintf_s(docPath, MAX_PATH, L"%s\\%s", argv[2], fileName);

        char* rtf = Docx_ReadToRtf(docPath);
        if (!rtf) {
            wprintf(L"FAIL  %s: %s\n", fileName, Docx_GetLastError());
            failures++;
            continue;
        }

        // Structural checks every converted document must satisfy.
        checks++;
        if (strncmp(rtf, "{\\rtf", 5) != 0) {
            wprintf(L"FAIL  %s: output is not RTF\n", fileName);
            failures++;
        }

        checks++;
        int depth = 0;
        BOOL balanced = TRUE;
        for (const char* c = rtf; *c; c++) {
            if (*c == '\\' && c[1]) { c++; continue; }   // escaped, not structural
            if (*c == '{') depth++;
            else if (*c == '}' && --depth < 0) { balanced = FALSE; break; }
        }
        if (!balanced || depth != 0) {
            wprintf(L"FAIL  %s: unbalanced RTF groups (depth %d)\n", fileName, depth);
            failures++;
        }

        // Per-document assertions.
        WCHAR expectPath[MAX_PATH];
        wcscpy_s(expectPath, MAX_PATH, docPath);
        WCHAR* dot = wcsrchr(expectPath, L'.');
        if (dot) wcscpy_s(dot, MAX_PATH - (dot - expectPath), L".expect");

        FILE* ef = NULL;
        if (_wfopen_s(&ef, expectPath, L"rb") == 0 && ef) {
            char line[512];
            while (fgets(line, sizeof(line), ef)) {
                char* nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                if (!line[0] || line[0] == '#') continue;

                BOOL wantPresent;
                const char* needle;
                if (strncmp(line, "contains:", 9) == 0) {
                    wantPresent = TRUE;  needle = line + 9;
                } else if (strncmp(line, "absent:", 7) == 0) {
                    wantPresent = FALSE; needle = line + 7;
                } else {
                    continue;
                }

                checks++;
                BOOL found = strstr(rtf, needle) != NULL;
                if (found != wantPresent) {
                    wprintf(L"FAIL  %s: expected %s \"%hs\"\n", fileName,
                            wantPresent ? L"to contain" : L"NOT to contain", needle);
                    failures++;
                }
            }
            fclose(ef);
        }

        // ------------------------------------------------------------------
        // The layout engine, over the same corpus. The invariant that matters
        // is that nothing is ever placed below the bottom margin: if that
        // holds, pagination is working, and if it stops holding there is no
        // page of a long document where looking at the screen would reliably
        // catch it.
        // ------------------------------------------------------------------
        {
            DocModel* lm = Docx_ReadToModel(docPath);
            if (lm) {
                LayoutResult* lr = Layout_BuildUpdating(lm, L"Calibri", 11.0f);

                checks++;
                if (!lr) {
                    wprintf(L"FAIL  %s: could not be laid out\n", fileName);
                    failures++;
                } else {
                    float pw = 0.0f, ph = 0.0f;
                    Layout_PageSize(lr, &pw, &ph);

                    int pages = Layout_PageCount(lr);

                    checks++;
                    if (pages < 1) {
                        wprintf(L"FAIL  %s: laid out to no pages at all\n", fileName);
                        failures++;
                    }

                    // One DIP of tolerance absorbs the rounding in converting
                    // twips to DIPs. It does not absorb a line of text.
                    float limit = ph - (lm->section.marginBottom / TWIPS_PER_DIP) + 1.0f;
                    for (int pg = 0; pg < pages; pg++) {
                        checks++;
                        float bottom = Layout_PageContentBottom(lr, pg);
                        if (bottom > limit) {
                            wprintf(L"FAIL  %s: page %d has content %.1f DIPs down, "
                                    L"past the %.1f margin\n",
                                    fileName, pg + 1, bottom, limit);
                            failures++;
                        }
                    }

                    // Every table cell in the model has to reach a page; a
                    // table silently dropped by the engine would otherwise
                    // only show up by looking.
                    checks++;
                    int laidCells = 0;
                    for (int pg = 0; pg < pages; pg++) laidCells += Layout_PageCellCount(lr, pg);
                    if (laidCells != Doc_CountCells(lm)) {
                        wprintf(L"FAIL  %s: %d cells in the model, %d laid out\n",
                                fileName, Doc_CountCells(lm), laidCells);
                        failures++;
                    }

                    Layout_Free(lr);
                }
                Doc_Free(lm);
            }
        }

        // ------------------------------------------------------------------
        // Round trips, measured against the document model.
        //
        // Two separate questions, and conflating them hides which half broke:
        //
        //   serializer  model -> .docx -> model, with no editor involved
        //   editor      model -> RTF -> the control -> model
        //
        // Doc_Compare reports how many properties did not survive, so the
        // result is a number that can get worse rather than a pass.
        // ------------------------------------------------------------------
        DocModel* source = Docx_ReadToModel(docPath);

        checks++;
        if (!source) {
            wprintf(L"FAIL  %s: could not be read into a model\n", fileName);
            failures++;
        } else {
            WCHAR outPath[MAX_PATH];
            swprintf_s(outPath, MAX_PATH, L"%s\\%s.out.docx", outDir, fileName);

            // --- serializer ---
            WCHAR serPath[MAX_PATH];
            swprintf_s(serPath, MAX_PATH, L"%s\\%s.model.docx", outDir, fileName);

            checks++;
            if (!Docx_WriteModel(source, serPath)) {
                wprintf(L"FAIL  %s: the model could not be written as .docx\n", fileName);
                failures++;
            } else {
                DocModel* back = Docx_ReadToModel(serPath);
                checks++;
                if (!back) {
                    wprintf(L"FAIL  %s: the written model could not be read back\n",
                            fileName);
                    failures++;
                } else {
                    DocDiff diff;
                    Doc_Compare(source, back, &diff);
                    serCompared += diff.compared;
                    serLost += diff.differences;

                    if (diff.differences) {
                        wprintf(L"LOSS  %s: %d of %d properties lost through .docx\n",
                                fileName, diff.differences, diff.compared);
                        for (int k = 0; k < diff.firstCount; k++) {
                            wprintf(L"        %hs\n", diff.first[k]);
                        }
                    }
                    Doc_Free(back);
                }
            }

            // --- editor ---
            Rich_EnsureLoaded();
            HWND rt = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
                                      WS_POPUP | ES_MULTILINE | ES_NOHIDESEL,
                                      0, 0, 100, 100, HWND_MESSAGE, NULL,
                                      GetModuleHandleW(NULL), NULL);
            checks++;
            if (!rt) {
                // Silently skipping here would let the editor path go unchecked
                // while the harness still reported success.
                wprintf(L"FAIL  %s: could not create a control for the round trip\n",
                        fileName);
                failures++;
            } else {
                RichOle_Attach(rt);
                SendMessageW(rt, EM_SETTEXTMODE, TM_RICHTEXT, 0);
                SendMessageW(rt, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);

                checks++;
                if (!Rich_SetRtf(rt, rtf)) {
                    wprintf(L"FAIL  %s: converted RTF was rejected by the control\n",
                            fileName);
                    failures++;
                } else if (!Docx_WriteFromEditorWith(rt, outPath, source)) {
                    // With the model the view was loaded from, the same way the
                    // application saves: the control cannot give a picture's
                    // bytes back, so they come from there.
                    wprintf(L"FAIL  %s: could not be written back as .docx\n", fileName);
                    failures++;
                } else {
                    DocModel* viaEditor = Docx_ReadToModel(outPath);
                    checks++;
                    if (!viaEditor) {
                        wprintf(L"FAIL  %s: the editor's .docx could not be read back\n",
                                fileName);
                        failures++;
                    } else {
                        DocDiff diff;
                        Doc_Compare(source, viaEditor, &diff);
                        edCompared += diff.compared;
                        edLost += diff.differences;

                        if (diff.differences) {
                            wprintf(L"LOSS  %s: %d of %d properties lost through the editor\n",
                                    fileName, diff.differences, diff.compared);
                            for (int k = 0; k < diff.firstCount; k++) {
                                wprintf(L"        %hs\n", diff.first[k]);
                            }
                        }

                        // Text is the floor: structure may be approximated by
                        // the editor, but a word must never go missing.
                        WCHAR* a = Doc_GetText(source);
                        WCHAR* b = Doc_GetText(viaEditor);
                        checks++;
                        if (!a || !b || !SameContent(a, b)) {
                            wprintf(L"FAIL  %s: text was lost through the editor\n",
                                    fileName);
                            failures++;
                        }
                        free(a);
                        free(b);

                        Doc_Free(viaEditor);
                    }
                }
                DestroyWindow(rt);
            }

            Doc_Free(source);
        }

        free(rtf);
    }

    wprintf(L"\ndocx conformance: %d/%d checks passed across %d documents\n",
            checks - failures, checks, files);
    wprintf(L"model fidelity:   %d/%d properties survive .docx -> model -> .docx\n",
            serCompared - serLost, serCompared);
    wprintf(L"editor fidelity:  %d/%d properties survive a load, edit and save\n",
            edCompared - edLost, edCompared);

    return failures ? 1 : 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    if (lpCmdLine && wcsstr(lpCmdLine, L"--selftest")) {
        return RunSelfTest();
    }

    // Diagnostic and conformance modes. Both are console tools living inside a
    // windowed binary, so they borrow the caller's console the same way
    // --selftest does.
    if (lpCmdLine && (wcsstr(lpCmdLine, L"--docx2rtf") ||
                      wcsstr(lpCmdLine, L"--docx-check") ||
                      wcsstr(lpCmdLine, L"--layout-report") ||
                      wcsstr(lpCmdLine, L"--export-pdf") ||
                      wcsstr(lpCmdLine, L"--pdf-info") ||
                      wcsstr(lpCmdLine, L"--pdf-fields") ||
                      wcsstr(lpCmdLine, L"--pdf-fill") ||
                      wcsstr(lpCmdLine, L"--pdf-stamp") ||
                      wcsstr(lpCmdLine, L"--pdf-sign") ||
                      wcsstr(lpCmdLine, L"--pdf-verify") ||
                      wcsstr(lpCmdLine, L"--timestamp-check"))) {
        BOOL attached = FALSE;
        FILE* out = NULL;
        if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL) {
            attached = AttachConsole(ATTACH_PARENT_PROCESS);
            if (attached) freopen_s(&out, "CONOUT$", "w", stdout);
        }

        int argc = 0;
        WCHAR** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int rc = 2;
        if (argv) {
            if (wcsstr(lpCmdLine, L"--docx-check"))         rc = RunDocxCheck(argc, argv);
            else if (wcsstr(lpCmdLine, L"--layout-report")) rc = RunLayoutReport(argc, argv);
            else if (wcsstr(lpCmdLine, L"--export-pdf"))    rc = RunExportPdf(argc, argv);
            else if (wcsstr(lpCmdLine, L"--pdf-info"))      rc = RunPdfInfo(argc, argv);
            else if (wcsstr(lpCmdLine, L"--pdf-fields"))    rc = RunPdfFields(argc, argv);
            else if (wcsstr(lpCmdLine, L"--pdf-fill"))      rc = RunPdfFill(argc, argv);
            else if (wcsstr(lpCmdLine, L"--pdf-stamp"))     rc = RunPdfStamp(argc, argv);
            else if (wcsstr(lpCmdLine, L"--pdf-verify"))    rc = RunPdfVerify(argc, argv);
            else if (wcsstr(lpCmdLine, L"--timestamp-check")) rc = RunTimestampCheck(argc, argv);
            else if (wcsstr(lpCmdLine, L"--pdf-sign"))      rc = RunPdfSign(argc, argv);
            else                                            rc = RunDocxToRtf(argc, argv);
            LocalFree(argv);
        }

        fflush(stdout);
        if (attached) FreeConsole();
        return rc;
    }

    // Initialize common controls
    INITCOMMONCONTROLSEX icex = {
        .dwSize = sizeof(icex),
        .dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES |
                 ICC_LISTVIEW_CLASSES | ICC_LINK_CLASS
    };
    InitCommonControlsEx(&icex);

    // ponytail: no Msftedit.dll load here. The RichEdit control has been unused since
    // the Scintilla port, but the library was still being loaded on startup and a load
    // failure aborted the application for nothing. It comes back deliberately in v0.5
    // when the rich text document type needs it -- see ROADMAP.md.

    // Initialize application
    if (!App_Initialize(hInstance)) {
        MessageBoxW(NULL, L"Failed to initialize application", APP_NAME, MB_ICONERROR);
        return 1;
    }

    // Show the main window
    ShowWindow(g_app->hMainWindow, nCmdShow);
    UpdateWindow(g_app->hMainWindow);

    // Process command line - open file if specified
    if (lpCmdLine && lpCmdLine[0]) {
        // Let Windows do the quoting rules. The hand-rolled version kept
        // whatever the caller left on the end -- a trailing space survived
        // into the file name, and the title bar showed it.
        WCHAR path[MAX_PATH];
        path[0] = L'\0';

        int fileArgc = 0;
        WCHAR** fileArgv = CommandLineToArgvW(GetCommandLineW(), &fileArgc);
        if (fileArgv) {
            if (fileArgc > 1) wcscpy_s(path, MAX_PATH, fileArgv[1]);
            LocalFree(fileArgv);
        }

        // Check if file exists
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            Document* doc = Document_CreateFromFile(path);
            if (doc) MainWindow_OpenDocument(doc);
        }
    }

    // Run message loop
    BOOL result = App_Run();

    // Cleanup
    App_Shutdown();

    return result ? 0 : 1;
}
