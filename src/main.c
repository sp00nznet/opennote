#include "supernote.h"
#include "res/resource.h"
#include "sync/crypto.h"
#include "sync/oauth.h"
#include "ui/editor_rich.h"
#include "pdf/pdfread.h"

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
        { "pdf",    Pdf_SelfTest    },
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
        if (Pdf_RenderPage(pdf, index, 1000, &bytes, &len)) {
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
                      wcsstr(lpCmdLine, L"--pdf-info"))) {
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
