#include "supernote.h"
#include "pdf/pdfform.h"
#include "res/resource.h"

// About dialog
void Dialogs_About(HWND hParent) {
    DialogBoxW(g_app->hInstance, MAKEINTRESOURCEW(IDD_ABOUT), hParent, AboutProc);
}

#define REPO_URL     L"https://github.com/sp00nznet/opennote"
#define RELEASES_URL L"https://github.com/sp00nznet/opennote/releases/latest"

// About dialog procedure
INT_PTR CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // The version comes from the one place that has it, rather than
            // being typed into the dialog and left behind at the next release.
            WCHAR version[64];
            swprintf_s(version, 64, L"Version %s", APP_VERSION);
            SetDlgItemTextW(hwnd, IDC_ABOUT_VERSION, version);
            return TRUE;
        }

        case WM_NOTIFY: {
            const NMHDR* note = (const NMHDR*)lParam;
            if (note->idFrom != IDC_ABOUT_LINKS) break;
            if (note->code != NM_CLICK && note->code != NM_RETURN) break;

            const NMLINK* link = (const NMLINK*)lParam;
            const WCHAR* url = (wcscmp(link->item.szID, L"releases") == 0)
                             ? RELEASES_URL : REPO_URL;
            ShellExecuteW(hwnd, L"open", url, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, LOWORD(wParam));
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// Go To Line dialog
BOOL Dialogs_GoToLine(HWND hParent, int* line) {
    return DialogBoxParamW(g_app->hInstance, MAKEINTRESOURCEW(IDD_GOTO), hParent, GoToLineProc, (LPARAM)line) == IDOK;
}

// Go To Line procedure
INT_PTR CALLBACK GoToLineProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int* pLine;

    switch (msg) {
        case WM_INITDIALOG:
            pLine = (int*)lParam;
            SetFocus(GetDlgItem(hwnd, IDC_GOTO_LINE));
            return FALSE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                    {
                        WCHAR buffer[32];
                        GetDlgItemTextW(hwnd, IDC_GOTO_LINE, buffer, 32);
                        *pLine = _wtoi(buffer);
                        if (*pLine > 0) {
                            EndDialog(hwnd, IDOK);
                        } else {
                            MessageBoxW(hwnd, L"Invalid line number.", APP_NAME, MB_ICONWARNING);
                        }
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Find/Replace modeless dialog procedure
static UINT WM_FINDREPLACE;
static FINDREPLACEW g_fr;
static WCHAR g_findBuffer[256];
static WCHAR g_replaceBuffer[256];

static LRESULT CALLBACK FindReplaceSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass;
    (void)dwRefData;

    if (msg == WM_FINDREPLACE) {
        if (g_fr.Flags & FR_DIALOGTERM) {
            g_app->hFindDialog = NULL;
            return 0;
        }

        Tab* tab = App_GetActiveTab();
        HWND hEditor = tab ? tab->hEditor : NULL;
        if (!hEditor) return 0;

        // Update search options
        wcscpy_s(g_app->findText, MAX_PATH, g_fr.lpstrFindWhat);
        wcscpy_s(g_app->replaceText, MAX_PATH, g_fr.lpstrReplaceWith);
        g_app->matchCase = (g_fr.Flags & FR_MATCHCASE) != 0;
        g_app->wholeWord = (g_fr.Flags & FR_WHOLEWORD) != 0;

        if (g_fr.Flags & FR_FINDNEXT) {
            BOOL forward = !(g_fr.Flags & FR_DOWN) ? FALSE : TRUE;
            Editor_FindText(hEditor, g_app->findText, g_app->matchCase, g_app->wholeWord, forward);
        } else if (g_fr.Flags & FR_REPLACE) {
            Editor_ReplaceText(hEditor, g_app->findText, g_app->replaceText, g_app->matchCase, g_app->wholeWord);
        } else if (g_fr.Flags & FR_REPLACEALL) {
            Editor_ReplaceAll(hEditor, g_app->findText, g_app->replaceText, g_app->matchCase, g_app->wholeWord);
        }

        return 0;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Find dialog
void Dialogs_Find(HWND hParent) {
    if (g_app->hFindDialog) {
        SetFocus(g_app->hFindDialog);
        return;
    }

    // Register message
    if (!WM_FINDREPLACE) {
        WM_FINDREPLACE = RegisterWindowMessageW(FINDMSGSTRINGW);
    }

    // Get selected text for find
    Tab* tab = App_GetActiveTab();
    if (tab && tab->hEditor) {
        WCHAR* selected = Editor_GetSelectedText(tab->hEditor);
        if (selected && selected[0] && wcslen(selected) < 256) {
            wcscpy_s(g_findBuffer, 256, selected);
        }
        free(selected);
    }

    // Initialize structure
    ZeroMemory(&g_fr, sizeof(g_fr));
    g_fr.lStructSize = sizeof(g_fr);
    g_fr.hwndOwner = hParent;
    g_fr.lpstrFindWhat = g_findBuffer;
    g_fr.lpstrReplaceWith = g_replaceBuffer;
    g_fr.wFindWhatLen = 256;
    g_fr.wReplaceWithLen = 256;
    g_fr.Flags = FR_DOWN;
    if (g_app->matchCase) g_fr.Flags |= FR_MATCHCASE;
    if (g_app->wholeWord) g_fr.Flags |= FR_WHOLEWORD;

    g_app->hFindDialog = FindTextW(&g_fr);

    // Subclass main window to receive find messages
    SetWindowSubclass(hParent, FindReplaceSubclass, 1, 0);
}

// Replace dialog
void Dialogs_Replace(HWND hParent) {
    if (g_app->hFindDialog) {
        SetFocus(g_app->hFindDialog);
        return;
    }

    // Register message
    if (!WM_FINDREPLACE) {
        WM_FINDREPLACE = RegisterWindowMessageW(FINDMSGSTRINGW);
    }

    // Get selected text for find
    Tab* tab = App_GetActiveTab();
    if (tab && tab->hEditor) {
        WCHAR* selected = Editor_GetSelectedText(tab->hEditor);
        if (selected && selected[0] && wcslen(selected) < 256) {
            wcscpy_s(g_findBuffer, 256, selected);
        }
        free(selected);
    }

    // Initialize structure
    ZeroMemory(&g_fr, sizeof(g_fr));
    g_fr.lStructSize = sizeof(g_fr);
    g_fr.hwndOwner = hParent;
    g_fr.lpstrFindWhat = g_findBuffer;
    g_fr.lpstrReplaceWith = g_replaceBuffer;
    g_fr.wFindWhatLen = 256;
    g_fr.wReplaceWithLen = 256;
    g_fr.Flags = FR_DOWN;
    if (g_app->matchCase) g_fr.Flags |= FR_MATCHCASE;
    if (g_app->wholeWord) g_fr.Flags |= FR_WHOLEWORD;

    g_app->hFindDialog = ReplaceTextW(&g_fr);

    // Subclass main window to receive find messages
    SetWindowSubclass(hParent, FindReplaceSubclass, 1, 0);
}

// Font dialog
BOOL Dialogs_Font(HWND hParent, LOGFONTW* font) {
    CHOOSEFONTW cf = {
        .lStructSize = sizeof(cf),
        .hwndOwner = hParent,
        .lpLogFont = font,
        .Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FIXEDPITCHONLY
    };

    return ChooseFontW(&cf);
}

// Open file dialog
BOOL Dialogs_OpenFile(HWND hParent, WCHAR* pathBuffer, int bufferSize) {
    pathBuffer[0] = L'\0';  // Must be initialized

    // Comprehensive file filter like Notepad++
    static const WCHAR filter[] =
        L"All Supported Files\0*.docx;*.rtf;*.txt;*.c;*.cpp;*.h;*.hpp;*.cs;*.java;*.py;*.js;*.ts;*.json;*.xml;*.html;*.htm;*.css;*.md;*.yaml;*.yml;*.ini;*.cfg;*.conf;*.log;*.sql;*.sh;*.bat;*.cmd;*.ps1;*.rb;*.php;*.go;*.rs;*.swift;*.kt;*.lua;*.pl;*.r;*.m;*.asm;*.s\0"
        L"Word Document (*.docx)\0*.docx\0"
        L"Rich Text Format (*.rtf)\0*.rtf\0"
        L"Text Files (*.txt)\0*.txt\0"
        L"C/C++ Files (*.c;*.cpp;*.h;*.hpp)\0*.c;*.cpp;*.h;*.hpp\0"
        L"C# Files (*.cs)\0*.cs\0"
        L"Java Files (*.java)\0*.java\0"
        L"Python Files (*.py)\0*.py\0"
        L"JavaScript/TypeScript (*.js;*.ts)\0*.js;*.ts\0"
        L"JSON Files (*.json)\0*.json\0"
        L"XML Files (*.xml)\0*.xml\0"
        L"HTML Files (*.html;*.htm)\0*.html;*.htm\0"
        L"CSS Files (*.css)\0*.css\0"
        L"Markdown Files (*.md)\0*.md\0"
        L"YAML Files (*.yaml;*.yml)\0*.yaml;*.yml\0"
        L"INI/Config Files (*.ini;*.cfg;*.conf)\0*.ini;*.cfg;*.conf\0"
        L"Log Files (*.log)\0*.log\0"
        L"SQL Files (*.sql)\0*.sql\0"
        L"Shell Scripts (*.sh;*.bat;*.cmd;*.ps1)\0*.sh;*.bat;*.cmd;*.ps1\0"
        L"Ruby Files (*.rb)\0*.rb\0"
        L"PHP Files (*.php)\0*.php\0"
        L"Go Files (*.go)\0*.go\0"
        L"Rust Files (*.rs)\0*.rs\0"
        L"All Files (*.*)\0*.*\0";

    OPENFILENAMEW ofn = {
        .lStructSize = sizeof(ofn),
        .hwndOwner = hParent,
        .lpstrFilter = filter,
        .nFilterIndex = 1,
        .lpstrFile = pathBuffer,
        .nMaxFile = bufferSize,
        .Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT
    };

    return GetOpenFileNameW(&ofn);
}

// Save file dialog
BOOL Dialogs_SaveFile(HWND hParent, WCHAR* pathBuffer, int bufferSize, const WCHAR* defaultName) {
    if (defaultName) {
        wcscpy_s(pathBuffer, bufferSize, defaultName);
    }

    // Determine default extension from filename if provided
    const WCHAR* defExt = L"txt";
    int filterIndex = 1;

    if (defaultName) {
        const WCHAR* ext = wcsrchr(defaultName, L'.');
        if (ext) {
            ext++;  // Skip the dot
            if (_wcsicmp(ext, L"c") == 0 || _wcsicmp(ext, L"cpp") == 0 ||
                _wcsicmp(ext, L"h") == 0 || _wcsicmp(ext, L"hpp") == 0) {
                filterIndex = 2;
                defExt = ext;
            } else if (_wcsicmp(ext, L"py") == 0) {
                filterIndex = 3;
                defExt = L"py";
            } else if (_wcsicmp(ext, L"js") == 0 || _wcsicmp(ext, L"ts") == 0) {
                filterIndex = 4;
                defExt = ext;
            } else if (_wcsicmp(ext, L"json") == 0) {
                filterIndex = 5;
                defExt = L"json";
            } else if (_wcsicmp(ext, L"html") == 0 || _wcsicmp(ext, L"htm") == 0) {
                filterIndex = 6;
                defExt = ext;
            } else if (_wcsicmp(ext, L"css") == 0) {
                filterIndex = 7;
                defExt = L"css";
            } else if (_wcsicmp(ext, L"md") == 0) {
                filterIndex = 8;
                defExt = L"md";
            } else if (_wcsicmp(ext, L"xml") == 0) {
                filterIndex = 9;
                defExt = L"xml";
            } else if (_wcsicmp(ext, L"sql") == 0) {
                filterIndex = 10;
                defExt = L"sql";
            } else if (_wcsicmp(ext, L"rtf") == 0) {
                filterIndex = 11;
                defExt = L"rtf";
            } else if (_wcsicmp(ext, L"docx") == 0) {
                filterIndex = 12;
                defExt = L"docx";
            }
        }
    }

    static const WCHAR filter[] =
        L"Text Files (*.txt)\0*.txt\0"
        L"C/C++ Files (*.c;*.cpp;*.h;*.hpp)\0*.c;*.cpp;*.h;*.hpp\0"
        L"Python Files (*.py)\0*.py\0"
        L"JavaScript/TypeScript (*.js;*.ts)\0*.js;*.ts\0"
        L"JSON Files (*.json)\0*.json\0"
        L"HTML Files (*.html;*.htm)\0*.html;*.htm\0"
        L"CSS Files (*.css)\0*.css\0"
        L"Markdown Files (*.md)\0*.md\0"
        L"XML Files (*.xml)\0*.xml\0"
        L"SQL Files (*.sql)\0*.sql\0"
        L"Rich Text Format (*.rtf)\0*.rtf\0"
        L"Word Document (*.docx)\0*.docx\0"
        L"All Files (*.*)\0*.*\0";

    OPENFILENAMEW ofn = {
        .lStructSize = sizeof(ofn),
        .hwndOwner = hParent,
        .lpstrFilter = filter,
        .nFilterIndex = filterIndex,
        .lpstrFile = pathBuffer,
        .nMaxFile = bufferSize,
        .lpstrDefExt = defExt,
        .Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST
    };

    return GetSaveFileNameW(&ofn);
}

// Save changes confirmation dialog
int Dialogs_SaveChanges(HWND hParent, const WCHAR* filename) {
    WCHAR message[MAX_PATH + 64];
    swprintf_s(message, sizeof(message)/sizeof(WCHAR),
               L"Do you want to save changes to %s?", filename ? filename : L"Untitled");

    return MessageBoxW(hParent, message, APP_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
}

// Notes Browser dialog
BOOL Dialogs_NotesBrowser(HWND hParent, int* noteId) {
    return DialogBoxParamW(g_app->hInstance, MAKEINTRESOURCEW(IDD_NOTES_BROWSER),
                           hParent, NotesBrowserProc, (LPARAM)noteId) == IDOK;
}

// Notes Browser procedure
// The notes browser has a sizing frame, and until now nothing moved when it
// was used: the search box kept its old width, the buttons stayed where the
// bottom edge used to be, and the list did not grow into the space. A dialog
// that can be resized has to lay itself out.
typedef struct { int id, x, y, w, h; } NotesPlace;

static void NotesBrowserLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    const int pad = 12;
    const int row = 24;        // one line of controls
    const int button = 26;
    const int syncWidth = 56;

    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width < 200 || height < 160) return;

    int listTop = pad + row + 10;
    int buttonY = height - pad - button;
    int listBottom = buttonY - 10;

    NotesPlace places[] = {
        // The search line: a label, a box that takes the width, Sync at the end.
        { IDC_STATIC_SEARCH, pad, pad + 5, 40, 16 },
        { IDC_NOTES_SEARCH,  pad + 44, pad, width - pad * 2 - 44 - syncWidth - 8, row },
        { IDC_NOTES_SYNC,    width - pad - syncWidth, pad, syncWidth, row },

        // The list fills what is left between the two.
        { IDC_NOTES_LIST,    pad, listTop, width - pad * 2, listBottom - listTop },

        // Making on the left, opening and closing on the right.
        { IDC_NOTES_NEW,     pad, buttonY, 70, button },
        { IDC_NOTES_RENAME,  pad + 76, buttonY, 64, button },
        { IDC_NOTES_DELETE,  pad + 146, buttonY, 64, button },
        { IDOK,              width - pad - 140, buttonY, 66, button },
        { IDCANCEL,          width - pad - 66, buttonY, 66, button },
    };

    int count = (int)(sizeof(places) / sizeof(places[0]));

    HDWP defer = BeginDeferWindowPos(count);
    if (!defer) return;

    for (int i = 0; i < count; i++) {
        HWND control = GetDlgItem(hwnd, places[i].id);
        if (!control) continue;

        defer = DeferWindowPos(defer, control, NULL, places[i].x, places[i].y,
                               places[i].w, places[i].h, SWP_NOZORDER);
        if (!defer) return;
    }

    EndDeferWindowPos(defer);

    // The title column takes whatever the other two do not.
    HWND list = GetDlgItem(hwnd, IDC_NOTES_LIST);
    if (list) {
        RECT lr;
        GetClientRect(list, &lr);

        int rest = (lr.right - lr.left) - 110 - 60 - 24;
        if (rest > 80) ListView_SetColumnWidth(list, 0, rest);
    }

    InvalidateRect(hwnd, NULL, TRUE);
}

INT_PTR CALLBACK NotesBrowserProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int* pNoteId;
    static HWND hList;
    static HWND hSearch;

    switch (msg) {
        case WM_SIZE:
            NotesBrowserLayout(hwnd);
            return TRUE;

        case WM_GETMINMAXINFO: {
            // Small enough to be useful, not small enough to be nonsense.
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = 420;
            mmi->ptMinTrackSize.y = 300;
            return TRUE;
        }

        case WM_INITDIALOG:
            pNoteId = (int*)lParam;
            hList = GetDlgItem(hwnd, IDC_NOTES_LIST);
            hSearch = GetDlgItem(hwnd, IDC_NOTES_SEARCH);

            // Set up list view columns
            {
                LVCOLUMNW col = {
                    .mask = LVCF_TEXT | LVCF_WIDTH,
                    .cx = 180,
                    .pszText = L"Title"
                };
                ListView_InsertColumn(hList, 0, &col);

                col.cx = 110;
                col.pszText = L"Updated";
                ListView_InsertColumn(hList, 1, &col);

                col.cx = 60;
                col.pszText = L"Size";
                ListView_InsertColumn(hList, 2, &col);
            }

            // Load notes
            {
                NoteListItem* items = NULL;
                int count = 0;
                Notes_GetList(&items, &count);

                for (int i = 0; i < count; i++) {
                    LVITEMW item = {
                        .mask = LVIF_TEXT | LVIF_PARAM,
                        .iItem = i,
                        .pszText = items[i].title,
                        .lParam = items[i].id
                    };
                    int idx = ListView_InsertItem(hList, &item);
                    ListView_SetItemText(hList, idx, 1, items[i].updatedAt);

                    // Format size
                    WCHAR sizeText[32];
                    if (items[i].contentSize < 1024) {
                        swprintf_s(sizeText, 32, L"%d B", items[i].contentSize);
                    } else if (items[i].contentSize < 1024 * 1024) {
                        swprintf_s(sizeText, 32, L"%.1f KB", items[i].contentSize / 1024.0);
                    } else {
                        swprintf_s(sizeText, 32, L"%.1f MB", items[i].contentSize / (1024.0 * 1024.0));
                    }
                    ListView_SetItemText(hList, idx, 2, sizeText);
                }

                Notes_FreeList(items);
            }

            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_NOTES_SEARCH:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        WCHAR query[256];
                        GetWindowTextW(hSearch, query, 256);

                        ListView_DeleteAllItems(hList);

                        NoteListItem* items = NULL;
                        int count = 0;

                        if (query[0]) {
                            Notes_Search(query, &items, &count);
                        } else {
                            Notes_GetList(&items, &count);
                        }

                        for (int i = 0; i < count; i++) {
                            LVITEMW item = {
                                .mask = LVIF_TEXT | LVIF_PARAM,
                                .iItem = i,
                                .pszText = items[i].title,
                                .lParam = items[i].id
                            };
                            int idx = ListView_InsertItem(hList, &item);
                            ListView_SetItemText(hList, idx, 1, items[i].updatedAt);

                            // Format size
                            WCHAR sizeText[32];
                            if (items[i].contentSize < 1024) {
                                swprintf_s(sizeText, 32, L"%d B", items[i].contentSize);
                            } else if (items[i].contentSize < 1024 * 1024) {
                                swprintf_s(sizeText, 32, L"%.1f KB", items[i].contentSize / 1024.0);
                            } else {
                                swprintf_s(sizeText, 32, L"%.1f MB", items[i].contentSize / (1024.0 * 1024.0));
                            }
                            ListView_SetItemText(hList, idx, 2, sizeText);
                        }

                        Notes_FreeList(items);
                    }
                    return TRUE;

                case IDC_NOTES_NEW:
                    {
                        int newId = Notes_Create(L"New Note", L"");
                        if (newId > 0) {
                            *pNoteId = newId;
                            EndDialog(hwnd, IDOK);
                        }
                    }
                    return TRUE;

                case IDC_NOTES_RENAME:
                    {
                        int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                        if (sel >= 0) {
                            LVITEMW item = { .mask = LVIF_PARAM | LVIF_TEXT, .iItem = sel };
                            WCHAR currentTitle[MAX_TITLE_LEN];
                            item.pszText = currentTitle;
                            item.cchTextMax = MAX_TITLE_LEN;
                            ListView_GetItem(hList, &item);

                            WCHAR newTitle[MAX_TITLE_LEN];
                            wcscpy_s(newTitle, MAX_TITLE_LEN, currentTitle);

                            if (Dialogs_InputBox(hwnd, L"Rename Note", L"Enter new title:", newTitle, MAX_TITLE_LEN)) {
                                if (newTitle[0] && wcscmp(newTitle, currentTitle) != 0) {
                                    Notes_SetTitle((int)item.lParam, newTitle);
                                    ListView_SetItemText(hList, sel, 0, newTitle);
                                }
                            }
                        }
                    }
                    return TRUE;

                case IDC_NOTES_DELETE:
                    {
                        // Count selected items
                        int selCount = ListView_GetSelectedCount(hList);
                        if (selCount > 0) {
                            WCHAR msg[128];
                            if (selCount == 1) {
                                wcscpy_s(msg, 128, L"Delete this note?");
                            } else {
                                swprintf_s(msg, 128, L"Delete %d selected notes?", selCount);
                            }

                            if (MessageBoxW(hwnd, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                                // Delete from end to start to avoid index shifting
                                int sel = -1;
                                int* toDelete = (int*)malloc(selCount * sizeof(int));
                                int* noteIds = (int*)malloc(selCount * sizeof(int));
                                int idx = 0;

                                while ((sel = ListView_GetNextItem(hList, sel, LVNI_SELECTED)) != -1) {
                                    LVITEMW item = { .mask = LVIF_PARAM, .iItem = sel };
                                    ListView_GetItem(hList, &item);
                                    toDelete[idx] = sel;
                                    noteIds[idx] = (int)item.lParam;
                                    idx++;
                                }

                                // Delete from end to start
                                for (int i = idx - 1; i >= 0; i--) {
                                    Notes_Delete(noteIds[i]);
                                    ListView_DeleteItem(hList, toDelete[i]);
                                }

                                free(toDelete);
                                free(noteIds);
                            }
                        }
                    }
                    return TRUE;

                case IDC_NOTES_SYNC:
                    {
                        // Check if user is signed in
                        if (!g_app->syncProvider || g_app->syncProvider[0] == L'\0') {
                            MessageBoxW(hwnd, L"Please sign in to a cloud account first.\n\nGo to Settings > Default Settings > Sync Accounts to connect.",
                                        APP_NAME, MB_ICONINFORMATION);
                        } else if (wcscmp(g_app->syncProvider, L"GitHub") == 0) {
                            // Sync with GitHub Gists
                            GitHubSync_SyncAll(hwnd);
                            // Refresh the notes list after sync
                            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_NOTES_SEARCH, EN_CHANGE), 0);
                        } else if (wcscmp(g_app->syncProvider, L"Google Drive") == 0) {
                            // Sync with Google Drive
                            GoogleSync_SyncAll(hwnd);
                            // Refresh the notes list after sync
                            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_NOTES_SEARCH, EN_CHANGE), 0);
                        }
                    }
                    return TRUE;

                case IDOK:
                    {
                        int selCount = ListView_GetSelectedCount(hList);
                        if (selCount == 1) {
                            // Single selection - return the note ID
                            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                            if (sel >= 0) {
                                LVITEMW item = { .mask = LVIF_PARAM, .iItem = sel };
                                ListView_GetItem(hList, &item);
                                *pNoteId = (int)item.lParam;
                                EndDialog(hwnd, IDOK);
                            }
                        } else if (selCount > 1) {
                            // Multi-selection - open all notes directly
                            int sel = -1;
                            BOOL first = TRUE;
                            while ((sel = ListView_GetNextItem(hList, sel, LVNI_SELECTED)) != -1) {
                                LVITEMW item = { .mask = LVIF_PARAM, .iItem = sel };
                                ListView_GetItem(hList, &item);
                                int noteId = (int)item.lParam;

                                // Open each note in a new tab
                                Document* doc = Document_CreateFromNote(noteId);
                                if (doc) {
                                    int tabIdx = App_CreateTab(doc->noteTitle);
                                    if (tabIdx >= 0 && g_app->tabs[tabIdx]) {
                                        Document_Destroy(g_app->tabs[tabIdx]->document);
                                        g_app->tabs[tabIdx]->document = doc;
                                        Document_Load(doc, g_app->tabs[tabIdx]->hEditor);
                                        TabControl_UpdateTabTitle(tabIdx);

                                        if (first) {
                                            *pNoteId = noteId;  // Return first for compatibility
                                            first = FALSE;
                                        }
                                    } else {
                                        Document_Destroy(doc);
                                    }
                                }
                            }
                            EndDialog(hwnd, IDOK);
                        }
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->idFrom == IDC_NOTES_LIST) {
                if (((LPNMHDR)lParam)->code == NM_DBLCLK) {
                    SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
                    return TRUE;
                }
            }
            break;

    }
    return FALSE;
}

// Create find dialog (legacy - now using Windows common dialogs)
HWND CreateFindDialog(HWND hParent, BOOL showReplace) {
    if (showReplace) {
        Dialogs_Replace(hParent);
    } else {
        Dialogs_Find(hParent);
    }
    return g_app->hFindDialog;
}

// Set find text
void FindDialog_SetFindText(const WCHAR* text) {
    if (text) {
        wcscpy_s(g_app->findText, MAX_PATH, text);
    }
}

// Defaults dialog
void Dialogs_Defaults(HWND hParent) {
    DialogBoxW(g_app->hInstance, MAKEINTRESOURCEW(IDD_DEFAULTS), hParent, DefaultsProc);
}

// Forward declaration for sync accounts dialog
void Dialogs_SyncAccounts(HWND hParent);

// Defaults dialog procedure
INT_PTR CALLBACK DefaultsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;

    switch (msg) {
        case WM_INITDIALOG:
            {
                // Font size combobox
                HWND hFontCombo = GetDlgItem(hwnd, IDC_DEFAULTS_FONTSIZE);
                int sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32};
                int currentSize = -g_app->editorFont.lfHeight;
                int fontSelectIndex = 4;

                for (int i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
                    WCHAR text[16];
                    swprintf_s(text, 16, L"%d", sizes[i]);
                    int idx = (int)SendMessageW(hFontCombo, CB_ADDSTRING, 0, (LPARAM)text);
                    SendMessageW(hFontCombo, CB_SETITEMDATA, idx, sizes[i]);
                    if (sizes[i] == currentSize) {
                        fontSelectIndex = idx;
                    }
                }
                SendMessageW(hFontCombo, CB_SETCURSEL, fontSelectIndex, 0);

                // Theme combobox
                HWND hThemeCombo = GetDlgItem(hwnd, IDC_DEFAULTS_THEME);
                if (hThemeCombo) {
                    int themeCount = Editor_GetThemeCount();
                    for (int i = 0; i < themeCount; i++) {
                        SendMessageW(hThemeCombo, CB_ADDSTRING, 0, (LPARAM)Editor_GetThemeName(i));
                    }
                    SendMessageW(hThemeCombo, CB_SETCURSEL, g_app->themeIndex, 0);
                }

                // Minimize to tray checkbox
                CheckDlgButton(hwnd, IDC_DEFAULTS_TRAY, g_app->minimizeToTray ? BST_CHECKED : BST_UNCHECKED);

                // Auto-save session checkbox
                CheckDlgButton(hwnd, IDC_DEFAULTS_AUTOSAVE, g_app->autoSaveSession ? BST_CHECKED : BST_UNCHECKED);

                // Auto-restore session checkbox
                CheckDlgButton(hwnd, IDC_DEFAULTS_AUTORESTORE, g_app->autoRestoreSession ? BST_CHECKED : BST_UNCHECKED);
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_DEFAULTS_RESTORE:
                    // Restore previous session
                    if (MessageBoxW(hwnd, L"Restore the previous session?\nThis will open all previously open tabs.",
                                    APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        EndDialog(hwnd, IDCANCEL);
                        App_RestoreSession();
                    }
                    return TRUE;

                case IDC_DEFAULTS_SYNC_ACCOUNTS:
                    Dialogs_SyncAccounts(hwnd);
                    return TRUE;

                case IDOK:
                    {
                        // Get font size
                        HWND hFontCombo = GetDlgItem(hwnd, IDC_DEFAULTS_FONTSIZE);
                        int fontSel = (int)SendMessageW(hFontCombo, CB_GETCURSEL, 0, 0);
                        if (fontSel != CB_ERR) {
                            int newSize = (int)SendMessageW(hFontCombo, CB_GETITEMDATA, fontSel, 0);
                            g_app->editorFont.lfHeight = -newSize;
                            if (g_app->hEditorFont) DeleteObject(g_app->hEditorFont);
                            g_app->hEditorFont = CreateFontIndirectW(&g_app->editorFont);
                        }

                        // Get theme
                        HWND hThemeCombo = GetDlgItem(hwnd, IDC_DEFAULTS_THEME);
                        int themeSel = (int)SendMessageW(hThemeCombo, CB_GETCURSEL, 0, 0);
                        BOOL themeChanged = (themeSel != CB_ERR && themeSel != g_app->themeIndex);
                        if (themeSel != CB_ERR) {
                            g_app->themeIndex = themeSel;
                        }

                        // Apply to all open editors
                        for (int i = 0; i < MAX_TABS; i++) {
                            if (g_app->tabs[i] && g_app->tabs[i]->hEditor) {
                                if (themeChanged) {
                                    // Apply theme (which also applies font)
                                    const WCHAR* filename = NULL;
                                    if (g_app->tabs[i]->document) {
                                        if (g_app->tabs[i]->document->type == DOC_TYPE_FILE) {
                                            filename = g_app->tabs[i]->document->filePath;
                                        } else {
                                            filename = g_app->tabs[i]->document->noteTitle;
                                        }
                                    }
                                    Editor_ApplyTheme(g_app->tabs[i]->hEditor, filename);
                                } else {
                                    // Just apply font
                                    Editor_SetFont(g_app->tabs[i]->hEditor, g_app->hEditorFont);
                                }
                            }
                        }

                        // Get minimize to tray setting
                        g_app->minimizeToTray = (IsDlgButtonChecked(hwnd, IDC_DEFAULTS_TRAY) == BST_CHECKED);

                        // Get auto-save session setting
                        g_app->autoSaveSession = (IsDlgButtonChecked(hwnd, IDC_DEFAULTS_AUTOSAVE) == BST_CHECKED);

                        // Get auto-restore session setting
                        g_app->autoRestoreSession = (IsDlgButtonChecked(hwnd, IDC_DEFAULTS_AUTORESTORE) == BST_CHECKED);

                        EndDialog(hwnd, IDOK);
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Find in Tabs dialog
void Dialogs_FindInTabs(HWND hParent) {
    DialogBoxW(g_app->hInstance, MAKEINTRESOURCEW(IDD_FIND_IN_TABS), hParent, FindInTabsProc);
}

// Helper to populate tabs list
static void PopulateTabsList(HWND hList) {
    // Set up list view columns
    LVCOLUMNW col = {
        .mask = LVCF_TEXT | LVCF_WIDTH,
        .cx = 200,
        .pszText = L"Tab"
    };
    ListView_InsertColumn(hList, 0, &col);

    // Add tabs
    for (int i = 0; i < MAX_TABS; i++) {
        if (g_app->tabs[i] && g_app->tabs[i]->document) {
            WCHAR title[MAX_TITLE_LEN + 8];
            swprintf_s(title, sizeof(title)/sizeof(WCHAR), L"%d. %s",
                      i + 1, Document_GetTitle(g_app->tabs[i]->document));

            LVITEMW item = {
                .mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE,
                .iItem = ListView_GetItemCount(hList),
                .pszText = title,
                .lParam = i,
                .state = LVIS_SELECTED,
                .stateMask = LVIS_SELECTED
            };
            ListView_InsertItem(hList, &item);
        }
    }

    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);

    // Check all items by default
    int count = ListView_GetItemCount(hList);
    for (int i = 0; i < count; i++) {
        ListView_SetCheckState(hList, i, TRUE);
    }
}

// Find in Tabs dialog procedure
INT_PTR CALLBACK FindInTabsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hList;
    static HWND hFindText;

    (void)lParam;

    switch (msg) {
        case WM_INITDIALOG:
            hList = GetDlgItem(hwnd, IDC_TABS_LIST);
            hFindText = GetDlgItem(hwnd, IDC_FIND_IN_TABS_TEXT);

            PopulateTabsList(hList);

            // Set initial text from current find text
            if (g_app->findText[0]) {
                SetWindowTextW(hFindText, g_app->findText);
            }

            // Set checkboxes
            CheckDlgButton(hwnd, IDC_MATCH_CASE, g_app->matchCase ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_WHOLE_WORD, g_app->wholeWord ? BST_CHECKED : BST_UNCHECKED);

            SetFocus(hFindText);
            return FALSE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SELECT_ALL_TABS:
                    {
                        int count = ListView_GetItemCount(hList);
                        BOOL allChecked = TRUE;
                        for (int i = 0; i < count; i++) {
                            if (!ListView_GetCheckState(hList, i)) {
                                allChecked = FALSE;
                                break;
                            }
                        }
                        for (int i = 0; i < count; i++) {
                            ListView_SetCheckState(hList, i, !allChecked);
                        }
                    }
                    return TRUE;

                case IDC_FIND_ALL_TABS:
                    {
                        WCHAR findText[256];
                        GetWindowTextW(hFindText, findText, 256);
                        if (!findText[0]) {
                            MessageBoxW(hwnd, L"Please enter text to find.", APP_NAME, MB_ICONWARNING);
                            return TRUE;
                        }

                        wcscpy_s(g_app->findText, MAX_PATH, findText);
                        g_app->matchCase = IsDlgButtonChecked(hwnd, IDC_MATCH_CASE) == BST_CHECKED;
                        g_app->wholeWord = IsDlgButtonChecked(hwnd, IDC_WHOLE_WORD) == BST_CHECKED;

                        int foundCount = 0;
                        int tabsSearched = 0;
                        WCHAR firstFoundTab[MAX_TITLE_LEN] = {0};

                        // Search in selected tabs
                        int count = ListView_GetItemCount(hList);
                        for (int i = 0; i < count; i++) {
                            if (ListView_GetCheckState(hList, i)) {
                                LVITEMW item = { .mask = LVIF_PARAM, .iItem = i };
                                ListView_GetItem(hList, &item);
                                int tabIndex = (int)item.lParam;

                                Tab* tab = g_app->tabs[tabIndex];
                                if (tab && tab->hEditor) {
                                    tabsSearched++;

                                    // Count occurrences in this tab
                                    WCHAR* text = Editor_GetText(tab->hEditor);
                                    if (text) {
                                        WCHAR* search = text;
                                        while ((search = wcsstr(search, findText)) != NULL) {
                                            if (foundCount == 0 && firstFoundTab[0] == 0) {
                                                wcscpy_s(firstFoundTab, MAX_TITLE_LEN,
                                                        Document_GetTitle(tab->document));
                                            }
                                            foundCount++;
                                            search++;
                                        }
                                        free(text);
                                    }
                                }
                            }
                        }

                        WCHAR resultMsg[256];
                        if (foundCount > 0) {
                            swprintf_s(resultMsg, 256, L"Found %d occurrence(s) in %d tab(s).\nFirst match in: %s",
                                      foundCount, tabsSearched, firstFoundTab);
                        } else {
                            swprintf_s(resultMsg, 256, L"No matches found in %d tab(s).", tabsSearched);
                        }
                        MessageBoxW(hwnd, resultMsg, APP_NAME, MB_ICONINFORMATION);
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Replace in Tabs dialog
void Dialogs_ReplaceInTabs(HWND hParent) {
    DialogBoxW(g_app->hInstance, MAKEINTRESOURCEW(IDD_REPLACE_IN_TABS), hParent, ReplaceInTabsProc);
}

// Replace in Tabs dialog procedure
INT_PTR CALLBACK ReplaceInTabsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hList;
    static HWND hFindText;
    static HWND hReplaceText;

    (void)lParam;

    switch (msg) {
        case WM_INITDIALOG:
            hList = GetDlgItem(hwnd, IDC_TABS_LIST);
            hFindText = GetDlgItem(hwnd, IDC_FIND_IN_TABS_TEXT);
            hReplaceText = GetDlgItem(hwnd, IDC_REPLACE_IN_TABS_TEXT);

            PopulateTabsList(hList);

            // Set initial text
            if (g_app->findText[0]) {
                SetWindowTextW(hFindText, g_app->findText);
            }
            if (g_app->replaceText[0]) {
                SetWindowTextW(hReplaceText, g_app->replaceText);
            }

            // Set checkboxes
            CheckDlgButton(hwnd, IDC_MATCH_CASE, g_app->matchCase ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_WHOLE_WORD, g_app->wholeWord ? BST_CHECKED : BST_UNCHECKED);

            SetFocus(hFindText);
            return FALSE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SELECT_ALL_TABS:
                    {
                        int count = ListView_GetItemCount(hList);
                        BOOL allChecked = TRUE;
                        for (int i = 0; i < count; i++) {
                            if (!ListView_GetCheckState(hList, i)) {
                                allChecked = FALSE;
                                break;
                            }
                        }
                        for (int i = 0; i < count; i++) {
                            ListView_SetCheckState(hList, i, !allChecked);
                        }
                    }
                    return TRUE;

                case IDC_REPLACE_ALL_TABS:
                    {
                        WCHAR findText[256];
                        WCHAR replaceText[256];
                        GetWindowTextW(hFindText, findText, 256);
                        GetWindowTextW(hReplaceText, replaceText, 256);

                        if (!findText[0]) {
                            MessageBoxW(hwnd, L"Please enter text to find.", APP_NAME, MB_ICONWARNING);
                            return TRUE;
                        }

                        wcscpy_s(g_app->findText, MAX_PATH, findText);
                        wcscpy_s(g_app->replaceText, MAX_PATH, replaceText);
                        g_app->matchCase = IsDlgButtonChecked(hwnd, IDC_MATCH_CASE) == BST_CHECKED;
                        g_app->wholeWord = IsDlgButtonChecked(hwnd, IDC_WHOLE_WORD) == BST_CHECKED;

                        int replacements = 0;
                        int tabsModified = 0;

                        // Replace in selected tabs
                        int count = ListView_GetItemCount(hList);
                        for (int i = 0; i < count; i++) {
                            if (ListView_GetCheckState(hList, i)) {
                                LVITEMW item = { .mask = LVIF_PARAM, .iItem = i };
                                ListView_GetItem(hList, &item);
                                int tabIndex = (int)item.lParam;

                                Tab* tab = g_app->tabs[tabIndex];
                                if (tab && tab->hEditor) {
                                    int tabReplacements = Editor_ReplaceAll(tab->hEditor, findText,
                                                                           replaceText, g_app->matchCase,
                                                                           g_app->wholeWord);
                                    if (tabReplacements > 0) {
                                        replacements += tabReplacements;
                                        tabsModified++;
                                        if (tab->document) {
                                            tab->document->modified = TRUE;
                                            TabControl_UpdateTabTitle(tabIndex);
                                        }
                                    }
                                }
                            }
                        }

                        WCHAR resultMsg[256];
                        if (replacements > 0) {
                            swprintf_s(resultMsg, 256, L"Replaced %d occurrence(s) in %d tab(s).",
                                      replacements, tabsModified);
                        } else {
                            swprintf_s(resultMsg, 256, L"No matches found.");
                        }
                        MessageBoxW(hwnd, resultMsg, APP_NAME, MB_ICONINFORMATION);

                        MainWindow_UpdateTitle();
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Compare window class name
#define COMPARE_CLASS_NAME L"OpenNoteCompareWindow"

// Compare window data
typedef struct {
    HWND hEditor1;
    HWND hEditor2;
    HWND hLabel1;
    HWND hLabel2;
    WCHAR title1[MAX_TITLE_LEN];
    WCHAR title2[MAX_TITLE_LEN];
} CompareWindowData;

// Compare window procedure
static LRESULT CALLBACK CompareWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CompareWindowData* data = (CompareWindowData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE:
            {
                CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
                data = (CompareWindowData*)cs->lpCreateParams;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);

                // Create labels
                data->hLabel1 = CreateWindowExW(0, L"STATIC", data->title1,
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    0, 0, 100, 20, hwnd, NULL, g_app->hInstance, NULL);
                data->hLabel2 = CreateWindowExW(0, L"STATIC", data->title2,
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    100, 0, 100, 20, hwnd, NULL, g_app->hInstance, NULL);

                // Create read-only editors
                data->hEditor1 = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, NULL,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                    0, 20, 100, 100, hwnd, NULL, g_app->hInstance, NULL);
                data->hEditor2 = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, NULL,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                    100, 20, 100, 100, hwnd, NULL, g_app->hInstance, NULL);

                // Set font
                SendMessageW(data->hLabel1, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
                SendMessageW(data->hLabel2, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
                if (g_app->hEditorFont) {
                    Editor_SetFont(data->hEditor1, g_app->hEditorFont);
                    Editor_SetFont(data->hEditor2, g_app->hEditorFont);
                }
            }
            return 0;

        case WM_SIZE:
            {
                int cx = LOWORD(lParam);
                int cy = HIWORD(lParam);
                int half = cx / 2;
                int labelHeight = 24;
                int gap = 4;

                SetWindowPos(data->hLabel1, NULL, 0, 0, half - gap, labelHeight, SWP_NOZORDER);
                SetWindowPos(data->hLabel2, NULL, half + gap, 0, half - gap, labelHeight, SWP_NOZORDER);
                SetWindowPos(data->hEditor1, NULL, 0, labelHeight, half - gap, cy - labelHeight, SWP_NOZORDER);
                SetWindowPos(data->hEditor2, NULL, half + gap, labelHeight, half - gap, cy - labelHeight, SWP_NOZORDER);
            }
            return 0;

        case WM_DESTROY:
            free(data);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Register compare window class
static BOOL RegisterCompareClass(void) {
    static BOOL registered = FALSE;
    if (registered) return TRUE;

    WNDCLASSEXW wc = {
        .cbSize = sizeof(wc),
        .lpfnWndProc = CompareWndProc,
        .hInstance = g_app->hInstance,
        .hCursor = LoadCursorW(NULL, IDC_ARROW),
        .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1),
        .lpszClassName = COMPARE_CLASS_NAME
    };

    registered = (RegisterClassExW(&wc) != 0);
    return registered;
}

// Show compare window
void Dialogs_Compare(HWND hParent, int tab1Index, int tab2Index) {
    if (!RegisterCompareClass()) return;

    Tab* tab1 = g_app->tabs[tab1Index];
    Tab* tab2 = g_app->tabs[tab2Index];
    if (!tab1 || !tab2 || !tab1->document || !tab2->document) return;

    // Allocate window data
    CompareWindowData* data = (CompareWindowData*)calloc(1, sizeof(CompareWindowData));
    if (!data) return;

    wcscpy_s(data->title1, MAX_TITLE_LEN, Document_GetTitle(tab1->document));
    wcscpy_s(data->title2, MAX_TITLE_LEN, Document_GetTitle(tab2->document));

    // Create window title
    WCHAR windowTitle[MAX_TITLE_LEN * 2 + 32];
    swprintf_s(windowTitle, sizeof(windowTitle)/sizeof(WCHAR),
               L"Compare: %s vs %s", data->title1, data->title2);

    // Get parent window position for centering
    RECT rcParent;
    GetWindowRect(hParent, &rcParent);
    int width = 1000;
    int height = 600;
    int x = rcParent.left + (rcParent.right - rcParent.left - width) / 2;
    int y = rcParent.top + (rcParent.bottom - rcParent.top - height) / 2;

    HWND hCompare = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW,
        COMPARE_CLASS_NAME,
        windowTitle,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, width, height,
        NULL,  // No parent - independent window
        NULL,
        g_app->hInstance,
        data
    );

    if (hCompare && data->hEditor1 && data->hEditor2) {
        // Copy content from tabs to compare editors
        WCHAR* text1 = Editor_GetText(tab1->hEditor);
        WCHAR* text2 = Editor_GetText(tab2->hEditor);

        if (text1) {
            Editor_SetText(data->hEditor1, text1);
            free(text1);
        }
        if (text2) {
            Editor_SetText(data->hEditor2, text2);
            free(text2);
        }
    }
}

// Input box dialog data
typedef struct {
    const WCHAR* title;
    const WCHAR* prompt;
    WCHAR* buffer;
    int bufferSize;
} InputBoxData;

// Input box dialog procedure
static INT_PTR CALLBACK InputBoxProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static InputBoxData* data;

    switch (msg) {
        case WM_INITDIALOG:
            data = (InputBoxData*)lParam;
            SetWindowTextW(hwnd, data->title);
            SetDlgItemTextW(hwnd, IDC_INPUTBOX_PROMPT, data->prompt);
            SetDlgItemTextW(hwnd, IDC_INPUTBOX_TEXT, data->buffer);
            SendDlgItemMessageW(hwnd, IDC_INPUTBOX_TEXT, EM_SETSEL, 0, -1);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                    GetDlgItemTextW(hwnd, IDC_INPUTBOX_TEXT, data->buffer, data->bufferSize);
                    EndDialog(hwnd, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Simple input box dialog
// ---------------------------------------------------------------------------
// Comments
//
// A comment is not in the document's text, so there is nowhere on the page to
// show it yet -- this is the list of them, with what each one is about, and
// the way to take one off. The bubble in the margin arrives with the review
// pane; the round trip through the file is what mattered first.
// ---------------------------------------------------------------------------

typedef struct {
    DocModel* doc;
    BOOL      changed;
} CommentsData;

static void CommentsFill(HWND hwnd, CommentsData* data) {
    HWND list = GetDlgItem(hwnd, IDC_COMMENTS_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);

    for (const DocComment* c = data->doc->comments; c; c = c->next) {
        WCHAR* text = Doc_CommentText(c);

        // The date as the file states it is ISO 8601; the day is enough here.
        WCHAR day[16] = L"";
        wcsncpy_s(day, 16, c->date, 10);

        WCHAR line[512];
        swprintf_s(line, 512, L"%s%s%s\t%s",
                   c->author[0] ? c->author : L"Author",
                   day[0] ? L", " : L"", day,
                   text ? text : L"");
        free(text);

        int at = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line);
        SendMessageW(list, LB_SETITEMDATA, at, (LPARAM)c->id);
    }

    EnableWindow(GetDlgItem(hwnd, IDC_COMMENTS_DELETE),
                 data->doc->comments != NULL);
}

static INT_PTR CALLBACK CommentsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CommentsData* data = (CommentsData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_INITDIALOG: {
            data = (CommentsData*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);

            // One tab stop, so the author column lines up and the comment
            // itself starts where the eye expects it.
            int tabs[1] = { 90 };
            SendDlgItemMessageW(hwnd, IDC_COMMENTS_LIST, LB_SETTABSTOPS, 1, (LPARAM)tabs);

            CommentsFill(hwnd, data);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_COMMENTS_DELETE: {
                    HWND list = GetDlgItem(hwnd, IDC_COMMENTS_LIST);
                    int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
                    if (sel == LB_ERR) return TRUE;

                    int id = (int)SendMessageW(list, LB_GETITEMDATA, sel, 0);
                    Doc_DeleteComment(data->doc, id);
                    data->changed = TRUE;
                    CommentsFill(hwnd, data);
                    return TRUE;
                }

                case IDCANCEL:
                case IDOK:
                    EndDialog(hwnd, data->changed);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwnd, data ? data->changed : FALSE);
            return TRUE;
    }

    return FALSE;
}

BOOL Dialogs_Comments(HWND hParent, DocModel* doc) {
    if (!doc) return FALSE;

    CommentsData data = { doc, FALSE };
    DialogBoxParamW(g_app->hInstance, MAKEINTRESOURCEW(IDD_COMMENTS),
                    hParent, CommentsProc, (LPARAM)&data);
    return data.changed;
}

// ---------------------------------------------------------------------------
// Filling in a PDF form
//
// The fields as the file states them, each with what is in it. Filling one in
// changes nothing on disk: the file is written when Save As is pressed, and
// it is written as an incremental update, so what was there stays there.
// ---------------------------------------------------------------------------

typedef struct {
    PdfForm* form;
    WCHAR    savedTo[MAX_PATH];
} PdfFormData;

static void PdfFormFill(HWND hwnd, PdfFormData* data) {
    HWND list = GetDlgItem(hwnd, IDC_PDF_FORM_LIST);
    ListView_DeleteAllItems(list);

    int count = PdfForm_FieldCount(data->form);
    for (int i = 0; i < count; i++) {
        LVITEMW item = {0};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.lParam = i;
        item.pszText = (WCHAR*)PdfForm_FieldName(data->form, i);

        int at = ListView_InsertItem(list, &item);
        if (at < 0) continue;

        PdfFieldKind kind = PdfForm_FieldKind(data->form, i);

        // A tick box shows as a tick rather than as the name of whatever its
        // "on" state happens to be called in this form.
        const WCHAR* shown = PdfForm_FieldValue(data->form, i);
        if (kind == PDF_FIELD_CHECKBOX) {
            shown = PdfForm_FieldChecked(data->form, i) ? L"Yes" : L"No";
        }

        const WCHAR* kindName = kind == PDF_FIELD_TEXT     ? L"text"
                              : kind == PDF_FIELD_CHECKBOX ? L"tick"
                              : kind == PDF_FIELD_CHOICE   ? L"list"
                                                           : L"other";

        ListView_SetItemText(list, at, 1, (WCHAR*)shown);
        ListView_SetItemText(list, at, 2, (WCHAR*)kindName);
    }
}

static void PdfFormEditSelected(HWND hwnd, PdfFormData* data) {
    HWND list = GetDlgItem(hwnd, IDC_PDF_FORM_LIST);
    int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0) return;

    PdfFieldKind kind = PdfForm_FieldKind(data->form, selected);

    // A tick box has two states and no question to ask: it turns over.
    if (kind == PDF_FIELD_CHECKBOX) {
        PdfForm_SetFieldChecked(data->form, selected,
                                !PdfForm_FieldChecked(data->form, selected));
        PdfFormFill(hwnd, data);
        return;
    }

    if (kind != PDF_FIELD_TEXT && kind != PDF_FIELD_CHOICE) {
        MessageBoxW(hwnd,
            L"This one cannot be filled in here.\n\n"
            L"Buttons that do something when they are clicked are listed, not filled.",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }

    // A choice list says what it will take, because typing something it does
    // not offer is a value the form will not accept.
    WCHAR prompt[1024];
    swprintf_s(prompt, 1024, L"%s:", PdfForm_FieldName(data->form, selected));

    int options = PdfForm_FieldOptionCount(data->form, selected);
    for (int i = 0; i < options && wcslen(prompt) < 900; i++) {
        wcscat_s(prompt, 1024, i == 0 ? L"    (" : L", ");
        wcscat_s(prompt, 1024, PdfForm_FieldOption(data->form, selected, i));
        if (i == options - 1) wcscat_s(prompt, 1024, L")");
    }

    WCHAR value[1024];
    wcsncpy_s(value, 1024, PdfForm_FieldValue(data->form, selected), _TRUNCATE);

    if (!Dialogs_InputBox(hwnd, L"Fill in", prompt, value, 1024)) return;

    PdfForm_SetFieldValue(data->form, selected, value);
    PdfFormFill(hwnd, data);
}

static void PdfFormSave(HWND hwnd, PdfFormData* data) {
    WCHAR path[MAX_PATH] = {0};
    if (!Dialogs_SaveFile(hwnd, path, MAX_PATH, L"filled.pdf")) return;

    // A name with no extension gets the one it is going to be.
    if (!wcsrchr(path, L'.')) wcscat_s(path, MAX_PATH, L".pdf");

    if (!PdfForm_Save(data->form, path)) {
        MessageBoxW(hwnd, L"The filled form could not be written.",
                    APP_NAME, MB_ICONWARNING);
        return;
    }

    wcsncpy_s(data->savedTo, MAX_PATH, path, _TRUNCATE);
    EndDialog(hwnd, IDOK);
}

static INT_PTR CALLBACK PdfFormProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PdfFormData* data = (PdfFormData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_INITDIALOG: {
            data = (PdfFormData*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);

            HWND list = GetDlgItem(hwnd, IDC_PDF_FORM_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);

            LVCOLUMNW column = {0};
            column.mask = LVCF_TEXT | LVCF_WIDTH;

            column.cx = 150;
            column.pszText = (WCHAR*)L"Field";
            ListView_InsertColumn(list, 0, &column);

            column.cx = 280;
            column.pszText = (WCHAR*)L"Value";
            ListView_InsertColumn(list, 1, &column);

            column.cx = 60;
            column.pszText = (WCHAR*)L"Kind";
            ListView_InsertColumn(list, 2, &column);

            PdfFormFill(hwnd, data);
            return TRUE;
        }

        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->idFrom == IDC_PDF_FORM_LIST &&
                ((LPNMHDR)lParam)->code == NM_DBLCLK) {
                PdfFormEditSelected(hwnd, data);
                return TRUE;
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_PDF_FORM_EDIT:
                    PdfFormEditSelected(hwnd, data);
                    return TRUE;

                case IDC_PDF_FORM_SAVE:
                    PdfFormSave(hwnd, data);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }

    return FALSE;
}

BOOL Dialogs_PdfForm(HWND hParent, const WCHAR* pdfPath, WCHAR* savedTo, size_t savedChars) {
    if (savedTo && savedChars) savedTo[0] = L'\0';

    const WCHAR* why = NULL;
    PdfForm* form = PdfForm_Open(pdfPath, &why);
    if (!form) {
        MessageBoxW(hParent, why ? why : L"This PDF's form could not be read.",
                    APP_NAME, MB_ICONINFORMATION);
        return FALSE;
    }

    PdfFormData data = {0};
    data.form = form;

    INT_PTR result = DialogBoxParamW(g_app->hInstance, MAKEINTRESOURCEW(IDD_PDF_FORM),
                                     hParent, PdfFormProc, (LPARAM)&data);
    PdfForm_Close(form);

    if (result != IDOK) return FALSE;

    if (savedTo && savedChars) wcsncpy_s(savedTo, savedChars, data.savedTo, _TRUNCATE);
    return TRUE;
}

BOOL Dialogs_InputBox(HWND hParent, const WCHAR* title, const WCHAR* prompt, WCHAR* buffer, int bufferSize) {
    InputBoxData data = {
        .title = title,
        .prompt = prompt,
        .buffer = buffer,
        .bufferSize = bufferSize
    };

    return DialogBoxParamW(g_app->hInstance, MAKEINTRESOURCEW(IDD_INPUTBOX), hParent, InputBoxProc, (LPARAM)&data) == IDOK;
}

// Print Preview data
// Sync Accounts dialog procedure
// ---------------------------------------------------------------------------
// Cloud credentials
//
// OpenNote ships no API keys: not a client secret, which the build refuses, and
// not a client id, which CI never passes. Sync therefore needs an OAuth
// application the user registered for themselves -- and this is where an
// installed copy is told about it, so that does not mean building from source.
//
// What is entered here goes into the settings table: client ids as they are,
// because they are public identifiers, and Google's secret wrapped with DPAPI
// like an access token.
// ---------------------------------------------------------------------------

#define CREDS_GUIDE_URL \
    L"https://github.com/sp00nznet/opennote/blob/main/docs/cloud-sync-setup.md"

static void SetDlgItemUtf8(HWND hwnd, int id, const char* text) {
    WCHAR wide[1024] = {0};
    if (text && text[0]) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 1024);
    }
    SetDlgItemTextW(hwnd, id, wide);
}

static void GetDlgItemUtf8(HWND hwnd, int id, char* out, size_t outSize) {
    WCHAR wide[1024] = {0};
    GetDlgItemTextW(hwnd, id, wide, 1024);

    // Credentials pasted out of a browser bring whitespace with them.
    WCHAR* start = wide;
    while (*start == L' ' || *start == L'\t') start++;

    size_t len = wcslen(start);
    while (len > 0 && (start[len - 1] == L' ' || start[len - 1] == L'\t')) {
        start[--len] = L'\0';
    }

    out[0] = '\0';
    WideCharToMultiByte(CP_UTF8, 0, start, -1, out, (int)outSize, NULL, NULL);
}

static INT_PTR CALLBACK SyncCredentialsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;

    switch (msg) {
        case WM_INITDIALOG: {
            OAuthCredentials creds;
            OAuth_LoadCredentials(&creds);

            SetDlgItemUtf8(hwnd, IDC_CREDS_GITHUB_ID, creds.githubClientId);
            SetDlgItemUtf8(hwnd, IDC_CREDS_GOOGLE_ID, creds.googleClientId);
            SetDlgItemUtf8(hwnd, IDC_CREDS_GOOGLE_SECRET, creds.googleSecret);

            SecureZeroMemory(&creds, sizeof(creds));
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_CREDS_HELP:
                    ShellExecuteW(hwnd, L"open", CREDS_GUIDE_URL, NULL, NULL, SW_SHOWNORMAL);
                    return TRUE;

                case IDC_CREDS_CLEAR:
                    if (MessageBoxW(hwnd,
                            L"Remove the credentials stored on this machine?\n\n"
                            L"Any account already signed in stays signed in until you "
                            L"sign out; this only forgets the application they were "
                            L"obtained through.",
                            APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        SetDlgItemTextW(hwnd, IDC_CREDS_GITHUB_ID, L"");
                        SetDlgItemTextW(hwnd, IDC_CREDS_GOOGLE_ID, L"");
                        SetDlgItemTextW(hwnd, IDC_CREDS_GOOGLE_SECRET, L"");
                    }
                    return TRUE;

                case IDOK: {
                    OAuthCredentials creds;
                    memset(&creds, 0, sizeof(creds));

                    GetDlgItemUtf8(hwnd, IDC_CREDS_GITHUB_ID,
                                   creds.githubClientId, sizeof(creds.githubClientId));
                    GetDlgItemUtf8(hwnd, IDC_CREDS_GOOGLE_ID,
                                   creds.googleClientId, sizeof(creds.googleClientId));
                    GetDlgItemUtf8(hwnd, IDC_CREDS_GOOGLE_SECRET,
                                   creds.googleSecret, sizeof(creds.googleSecret));

                    // Half a Google credential cannot sign anybody in, and
                    // finding that out at the consent screen is worse than
                    // being told here.
                    if (creds.googleClientId[0] && !creds.googleSecret[0]) {
                        MessageBoxW(hwnd,
                            L"Google needs both the client ID and the client secret "
                            L"from your own Cloud project.\n\n"
                            L"They are on the same page in the Google Cloud console, "
                            L"under Credentials.",
                            APP_NAME, MB_ICONWARNING);
                        SecureZeroMemory(&creds, sizeof(creds));
                        return TRUE;
                    }

                    BOOL saved = OAuth_SaveCredentials(&creds);
                    SecureZeroMemory(&creds, sizeof(creds));

                    if (!saved) {
                        MessageBoxW(hwnd, L"The credentials could not be stored.",
                                    APP_NAME, MB_ICONERROR);
                        return TRUE;
                    }

                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }

    return FALSE;
}

void Dialogs_SyncCredentials(HWND hParent) {
    DialogBoxW(g_app->hInstance, MAKEINTRESOURCEW(IDD_SYNC_CREDENTIALS), hParent,
               SyncCredentialsProc);
}

// Sign-in is only possible with an OAuth application to sign in through, so
// the buttons follow the credentials rather than being offered and then
// failing with an explanation.
static void SyncAccountsRefresh(HWND hwnd) {
    BOOL signedIn = g_app->syncProvider && g_app->syncProvider[0] != L'\0';
    BOOL haveGitHub = OAuth_HasGitHubCredentials();
    BOOL haveGoogle = OAuth_HasGoogleCredentials();

    EnableWindow(GetDlgItem(hwnd, IDC_SYNC_SIGNOUT), signedIn);
    EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GITHUB), !signedIn && haveGitHub);
    EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GOOGLE), !signedIn && haveGoogle);

    if (signedIn) {
        WCHAR status[256];
        swprintf_s(status, 256, L"Signed in with %s", g_app->syncProvider);
        SetDlgItemTextW(hwnd, IDC_SYNC_STATUS, status);
    } else if (!haveGitHub && !haveGoogle) {
        SetDlgItemTextW(hwnd, IDC_SYNC_STATUS,
                        L"No credentials yet. OpenNote ships none -- register your "
                        L"own app and enter it under Credentials.");
    } else {
        SetDlgItemTextW(hwnd, IDC_SYNC_STATUS, L"Not signed in");
    }
}

static INT_PTR CALLBACK SyncAccountsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;

    switch (msg) {
        case WM_INITDIALOG:
            {
                // Check for saved tokens if not already signed in
                if (!g_app->syncProvider || g_app->syncProvider[0] == L'\0') {
                    OAuthToken token;
                    if (OAuth_LoadToken("github", &token) && token.isAuthenticated) {
                        if (g_app->syncProvider) free(g_app->syncProvider);
                        g_app->syncProvider = _wcsdup(L"GitHub");
                    } else if (OAuth_LoadToken("google", &token) && token.isAuthenticated) {
                        if (g_app->syncProvider) free(g_app->syncProvider);
                        g_app->syncProvider = _wcsdup(L"Google Drive");
                    }
                }

                SyncAccountsRefresh(hwnd);
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SYNC_GITHUB:
                    {
                        OAuthToken token;
                        if (OAuth_GitHubLogin(hwnd, &token)) {
                            // Success - update UI
                            if (g_app->syncProvider) free(g_app->syncProvider);
                            g_app->syncProvider = _wcsdup(L"GitHub");
                            SetDlgItemTextW(hwnd, IDC_SYNC_STATUS, L"Signed in with GitHub");
                            EnableWindow(GetDlgItem(hwnd, IDC_SYNC_SIGNOUT), TRUE);
                            EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GITHUB), FALSE);
                            EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GOOGLE), FALSE);
                            MessageBoxW(hwnd, L"Successfully signed in with GitHub!", APP_NAME, MB_ICONINFORMATION);
                        }
                    }
                    return TRUE;

                case IDC_SYNC_GOOGLE:
                    {
                        OAuthToken token;
                        if (OAuth_GoogleLogin(hwnd, &token)) {
                            // Success - update UI
                            if (g_app->syncProvider) free(g_app->syncProvider);
                            g_app->syncProvider = _wcsdup(L"Google Drive");
                            SetDlgItemTextW(hwnd, IDC_SYNC_STATUS, L"Signed in with Google Drive");
                            EnableWindow(GetDlgItem(hwnd, IDC_SYNC_SIGNOUT), TRUE);
                            EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GITHUB), FALSE);
                            EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GOOGLE), FALSE);
                            MessageBoxW(hwnd, L"Successfully signed in with Google Drive!", APP_NAME, MB_ICONINFORMATION);
                        }
                    }
                    return TRUE;

                case IDC_SYNC_CREDENTIALS:
                    Dialogs_SyncCredentials(hwnd);
                    SyncAccountsRefresh(hwnd);
                    return TRUE;

                case IDC_SYNC_SIGNOUT:
                    if (MessageBoxW(hwnd, L"Sign out from cloud sync?", APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        // Clear tokens
                        if (g_app->syncProvider) {
                            if (wcscmp(g_app->syncProvider, L"GitHub") == 0) {
                                OAuth_GitHubLogout();
                            } else if (wcscmp(g_app->syncProvider, L"Google Drive") == 0) {
                                OAuth_GoogleLogout();
                            }
                            free(g_app->syncProvider);
                            g_app->syncProvider = NULL;
                        }
                        SetDlgItemTextW(hwnd, IDC_SYNC_STATUS, L"Not signed in");
                        EnableWindow(GetDlgItem(hwnd, IDC_SYNC_SIGNOUT), FALSE);
                        EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GITHUB), TRUE);
                        EnableWindow(GetDlgItem(hwnd, IDC_SYNC_GOOGLE), TRUE);
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Show sync accounts dialog
void Dialogs_SyncAccounts(HWND hParent) {
    DialogBoxW(g_app->hInstance, MAKEINTRESOURCEW(IDD_SYNC_ACCOUNTS), hParent, SyncAccountsProc);
}

// ============================================================================
// Markdown Preview - Opens in default browser
// ============================================================================

// Simple markdown to HTML conversion
static WCHAR* MarkdownToHtml(const WCHAR* markdown) {
    if (!markdown) return NULL;

    // Allocate buffer (generous size for HTML tags)
    size_t mdLen = wcslen(markdown);
    size_t bufSize = mdLen * 4 + 4096;  // Extra space for HTML wrapper and tags
    WCHAR* html = (WCHAR*)malloc(bufSize * sizeof(WCHAR));
    if (!html) return NULL;

    // HTML header with styling
    wcscpy_s(html, bufSize,
        L"<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        L"<style>"
        L"body { font-family: 'Segoe UI', Arial, sans-serif; padding: 20px; margin: 0; "
        L"       background: #ffffff; color: #333; line-height: 1.6; }"
        L"img { max-width: 100%; height: auto; display: block; margin: 10px 0; }"
        L"pre, code { background: #f4f4f4; padding: 2px 6px; border-radius: 3px; "
        L"           font-family: Consolas, monospace; }"
        L"pre { padding: 10px; overflow-x: auto; }"
        L"h1, h2, h3 { color: #2c3e50; border-bottom: 1px solid #eee; padding-bottom: 5px; }"
        L"a { color: #3498db; }"
        L"blockquote { border-left: 4px solid #3498db; margin: 10px 0; padding-left: 15px; color: #666; }"
        L"hr { border: none; border-top: 1px solid #eee; margin: 20px 0; }"
        L"</style></head><body>");

    WCHAR* out = html + wcslen(html);
    const WCHAR* p = markdown;
    BOOL inCodeBlock = FALSE;
    BOOL lineStart = TRUE;

    while (*p) {
        // Code block (```)
        if (lineStart && wcsncmp(p, L"```", 3) == 0) {
            if (inCodeBlock) {
                wcscpy_s(out, bufSize - (out - html), L"</code></pre>");
                out += wcslen(L"</code></pre>");
                inCodeBlock = FALSE;
            } else {
                wcscpy_s(out, bufSize - (out - html), L"<pre><code>");
                out += wcslen(L"<pre><code>");
                inCodeBlock = TRUE;
            }
            p += 3;
            // Skip to end of line
            while (*p && *p != L'\n') p++;
            if (*p == L'\n') p++;
            lineStart = TRUE;
            continue;
        }

        // Inside code block - escape HTML
        if (inCodeBlock) {
            if (*p == L'<') {
                wcscpy_s(out, bufSize - (out - html), L"&lt;");
                out += 4;
            } else if (*p == L'>') {
                wcscpy_s(out, bufSize - (out - html), L"&gt;");
                out += 4;
            } else if (*p == L'\n') {
                *out++ = L'\n';
                lineStart = TRUE;
            } else {
                *out++ = *p;
                lineStart = FALSE;
            }
            p++;
            continue;
        }

        // Headers at line start
        if (lineStart) {
            int level = 0;
            const WCHAR* hp = p;
            while (*hp == L'#' && level < 6) { hp++; level++; }
            if (level > 0 && (*hp == L' ' || *hp == L'\t')) {
                hp++;  // Skip space after #
                swprintf_s(out, bufSize - (out - html), L"<h%d>", level);
                out += wcslen(out);
                // Copy header text
                while (*hp && *hp != L'\n') {
                    *out++ = *hp++;
                }
                swprintf_s(out, bufSize - (out - html), L"</h%d>", level);
                out += wcslen(out);
                p = hp;
                if (*p == L'\n') {
                    p++;
                    lineStart = TRUE;
                }
                continue;
            }
        }

        // Image: ![alt](url)
        if (*p == L'!' && *(p+1) == L'[') {
            const WCHAR* altStart = p + 2;
            const WCHAR* altEnd = wcschr(altStart, L']');
            if (altEnd && *(altEnd+1) == L'(') {
                const WCHAR* urlStart = altEnd + 2;
                const WCHAR* urlEnd = wcschr(urlStart, L')');
                if (urlEnd) {
                    // Extract alt text
                    WCHAR alt[256] = {0};
                    int altLen = (int)(altEnd - altStart);
                    if (altLen > 255) altLen = 255;
                    wcsncpy_s(alt, 256, altStart, altLen);

                    // Extract URL
                    WCHAR url[1024] = {0};
                    int urlLen = (int)(urlEnd - urlStart);
                    if (urlLen > 1023) urlLen = 1023;
                    wcsncpy_s(url, 1024, urlStart, urlLen);

                    // Generate img tag
                    swprintf_s(out, bufSize - (out - html), L"<img src=\"%s\" alt=\"%s\">", url, alt);
                    out += wcslen(out);
                    p = urlEnd + 1;
                    lineStart = FALSE;
                    continue;
                }
            }
        }

        // Link: [text](url)
        if (*p == L'[') {
            const WCHAR* textStart = p + 1;
            const WCHAR* textEnd = wcschr(textStart, L']');
            if (textEnd && *(textEnd+1) == L'(') {
                const WCHAR* urlStart = textEnd + 2;
                const WCHAR* urlEnd = wcschr(urlStart, L')');
                if (urlEnd) {
                    WCHAR text[256] = {0};
                    int textLen = (int)(textEnd - textStart);
                    if (textLen > 255) textLen = 255;
                    wcsncpy_s(text, 256, textStart, textLen);

                    WCHAR url[1024] = {0};
                    int urlLen = (int)(urlEnd - urlStart);
                    if (urlLen > 1023) urlLen = 1023;
                    wcsncpy_s(url, 1024, urlStart, urlLen);

                    swprintf_s(out, bufSize - (out - html), L"<a href=\"%s\" target=\"_blank\">%s</a>", url, text);
                    out += wcslen(out);
                    p = urlEnd + 1;
                    lineStart = FALSE;
                    continue;
                }
            }
        }

        // Bold: **text** or __text__
        if ((*p == L'*' && *(p+1) == L'*') || (*p == L'_' && *(p+1) == L'_')) {
            WCHAR marker = *p;
            const WCHAR* textStart = p + 2;
            const WCHAR* textEnd = wcsstr(textStart, (marker == L'*') ? L"**" : L"__");
            if (textEnd) {
                wcscpy_s(out, bufSize - (out - html), L"<strong>");
                out += 8;
                while (textStart < textEnd) {
                    *out++ = *textStart++;
                }
                wcscpy_s(out, bufSize - (out - html), L"</strong>");
                out += 9;
                p = textEnd + 2;
                lineStart = FALSE;
                continue;
            }
        }

        // Italic: *text* or _text_
        if ((*p == L'*' || *p == L'_') && *(p+1) != *p) {
            WCHAR marker = *p;
            const WCHAR* textStart = p + 1;
            const WCHAR* textEnd = wcschr(textStart, marker);
            if (textEnd && textEnd > textStart) {
                wcscpy_s(out, bufSize - (out - html), L"<em>");
                out += 4;
                while (textStart < textEnd) {
                    *out++ = *textStart++;
                }
                wcscpy_s(out, bufSize - (out - html), L"</em>");
                out += 5;
                p = textEnd + 1;
                lineStart = FALSE;
                continue;
            }
        }

        // Horizontal rule
        if (lineStart && (wcsncmp(p, L"---", 3) == 0 || wcsncmp(p, L"***", 3) == 0)) {
            wcscpy_s(out, bufSize - (out - html), L"<hr>");
            out += 4;
            p += 3;
            while (*p && *p != L'\n') p++;
            if (*p == L'\n') p++;
            lineStart = TRUE;
            continue;
        }

        // Blockquote
        if (lineStart && *p == L'>') {
            p++;
            if (*p == L' ') p++;
            wcscpy_s(out, bufSize - (out - html), L"<blockquote>");
            out += 12;
            while (*p && *p != L'\n') {
                *out++ = *p++;
            }
            wcscpy_s(out, bufSize - (out - html), L"</blockquote>");
            out += 13;
            if (*p == L'\n') {
                p++;
                lineStart = TRUE;
            }
            continue;
        }

        // Line breaks
        if (*p == L'\n') {
            // Double newline = paragraph
            if (*(p+1) == L'\n') {
                wcscpy_s(out, bufSize - (out - html), L"<br><br>");
                out += 8;
                p += 2;
            } else {
                wcscpy_s(out, bufSize - (out - html), L"<br>");
                out += 4;
                p++;
            }
            lineStart = TRUE;
            continue;
        }

        // Escape HTML special chars
        if (*p == L'<') {
            wcscpy_s(out, bufSize - (out - html), L"&lt;");
            out += 4;
        } else if (*p == L'>') {
            wcscpy_s(out, bufSize - (out - html), L"&gt;");
            out += 4;
        } else if (*p == L'&') {
            wcscpy_s(out, bufSize - (out - html), L"&amp;");
            out += 5;
        } else {
            *out++ = *p;
        }

        lineStart = FALSE;
        p++;
    }

    // Close code block if still open
    if (inCodeBlock) {
        wcscpy_s(out, bufSize - (out - html), L"</code></pre>");
        out += wcslen(L"</code></pre>");
    }

    // HTML footer
    wcscpy_s(out, bufSize - (out - html), L"</body></html>");
    out += wcslen(L"</body></html>");
    *out = L'\0';

    return html;
}

// Show markdown preview - writes HTML to temp file and opens in default browser
void Dialogs_MarkdownPreview(HWND hParent, HWND hEditor) {
    (void)hParent;

    // Get text from editor
    WCHAR* text = Editor_GetText(hEditor);
    if (!text || !text[0]) {
        free(text);
        MessageBoxW(hParent, L"No content to preview.", APP_NAME, MB_ICONINFORMATION);
        return;
    }

    // Convert markdown to HTML
    WCHAR* html = MarkdownToHtml(text);
    free(text);

    if (!html) {
        MessageBoxW(hParent, L"Failed to convert markdown.", APP_NAME, MB_ICONERROR);
        return;
    }

    // Get temp directory
    WCHAR tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);

    // One file, overwritten each time, rather than a new one per preview: the
    // old name carried a tick count, so every preview ever taken was still
    // sitting in the temp directory.
    WCHAR tempFile[MAX_PATH];
    swprintf_s(tempFile, MAX_PATH, L"%sopennote-preview.html", tempPath);

    // Write HTML to file (UTF-8 with BOM for browser compatibility)
    HANDLE hFile = CreateFileW(tempFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        free(html);
        MessageBoxW(hParent, L"Failed to create preview file.", APP_NAME, MB_ICONERROR);
        return;
    }

    // Write UTF-8 BOM
    BYTE bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written;
    WriteFile(hFile, bom, 3, &written, NULL);

    // Convert HTML to UTF-8 and write
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, html, -1, NULL, 0, NULL, NULL);
    if (utf8Len > 0) {
        char* utf8 = (char*)malloc(utf8Len);
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, html, -1, utf8, utf8Len, NULL, NULL);
            WriteFile(hFile, utf8, utf8Len - 1, &written, NULL);  // -1 to exclude null terminator
            free(utf8);
        }
    }

    CloseHandle(hFile);
    free(html);

    // Open in default browser
    HINSTANCE result = ShellExecuteW(NULL, L"open", tempFile, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        MessageBoxW(hParent, L"Failed to open preview in browser.", APP_NAME, MB_ICONERROR);
        DeleteFileW(tempFile);
    }

    // Note: We leave the temp file for the browser to read.
    // It will be cleaned up on next Windows restart or by temp file cleanup.
}
