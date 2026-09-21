#include "supernote.h"
#include "res/resource.h"
#include "ui/toolbar.h"
#include "pdf/pdfview.h"
#include "pdf/pdfform.h"
#include "pdf/pdfsign.h"
#include <windowsx.h>
// ponytail: still here for the SCNotification dispatch below -- that is the
// control's notification protocol, not an editing operation. Every editing
// operation now goes through the Editor_* API. When the second document view
// lands in v0.5 this dispatch is the only part that needs a branch.
#include <Scintilla.h>
#include <shellapi.h>

// Window procedure forward declaration
static void OnCreate(HWND hwnd);
static void OnDestroy(HWND hwnd);
static LRESULT OnNotify(HWND hwnd, int idCtrl, LPNMHDR pnmh);

// System tray
static NOTIFYICONDATAW g_trayIcon = {0};
static BOOL g_trayIconVisible = FALSE;

static void TrayIcon_Add(HWND hwnd) {
    if (g_trayIconVisible) return;

    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_APP_TRAY_CALLBACK;
    g_trayIcon.hIcon = LoadIconW(g_app->hInstance, MAKEINTRESOURCEW(IDI_SUPERNOTE));
    wcscpy_s(g_trayIcon.szTip, 128, APP_NAME);

    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    g_trayIconVisible = TRUE;
}

static void TrayIcon_Remove(void) {
    if (!g_trayIconVisible) return;

    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayIconVisible = FALSE;
}

// Register window class
BOOL MainWindow_RegisterClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {
        .cbSize = sizeof(wc),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = MainWindow_WndProc,
        .hInstance = hInstance,
        .hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_SUPERNOTE)),
        .hCursor = LoadCursorW(NULL, IDC_ARROW),
        .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
        .lpszMenuName = NULL,
        .lpszClassName = APP_CLASS_NAME,
        .hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_SUPERNOTE))
    };

    return RegisterClassExW(&wc) != 0;
}

// Create main window
HWND MainWindow_Create(HINSTANCE hInstance) {
    // Create menu
    HMENU hMenu = MenuBar_Create();

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        APP_CLASS_NAME,
        APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600,
        NULL,
        hMenu,
        hInstance,
        NULL
    );

    return hwnd;
}

// Window procedure
LRESULT CALLBACK MainWindow_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;

        case WM_DESTROY:
            OnDestroy(hwnd);
            return 0;

        case WM_SIZE:
            MainWindow_OnSize(hwnd, (UINT)wParam, LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_CLOSE:
            MainWindow_OnClose(hwnd);
            return 0;

        case WM_QUERYENDSESSION:
            return MainWindow_OnQueryEndSession(hwnd);

        case WM_COMMAND:
            MainWindow_OnCommand(hwnd, LOWORD(wParam), (HWND)lParam, HIWORD(wParam));
            return 0;

        case WM_NOTIFY:
            return OnNotify(hwnd, (int)wParam, (LPNMHDR)lParam);

        case WM_DROPFILES:
            {
                HDROP hDrop = (HDROP)wParam;
                UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);

                for (UINT i = 0; i < fileCount; i++) {
                    WCHAR filePath[MAX_PATH];
                    if (DragQueryFileW(hDrop, i, filePath, MAX_PATH)) {
                        // Check if it's a file (not a directory)
                        DWORD attrs = GetFileAttributesW(filePath);
                        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                            // Open the file in a new tab
                            Document* doc = Document_CreateFromFile(filePath);
                            if (doc) {
                                int tabIdx = App_CreateTabEx(doc->title, doc->format);
                                if (tabIdx >= 0 && g_app->tabs[tabIdx]) {
                                    Document_Destroy(g_app->tabs[tabIdx]->document);
                                    g_app->tabs[tabIdx]->document = doc;
                                    Document_Load(doc, g_app->tabs[tabIdx]->hEditor);
                                    Editor_ApplyTheme(g_app->tabs[tabIdx]->hEditor, filePath);
                                    TabControl_UpdateTabTitle(tabIdx);
                                    App_AddRecentFile(filePath);
                                } else {
                                    Document_Destroy(doc);
                                }
                            }
                        }
                    }
                }

                DragFinish(hDrop);
            }
            return 0;

        case WM_PARENTNOTIFY:
            // Handle double-click on tab control empty area
            if (LOWORD(wParam) == WM_LBUTTONDOWN) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                HWND hChild = ChildWindowFromPoint(hwnd, pt);
                if (hChild == g_app->hTabControl) {
                    // Click is on tab control - check if on empty area
                    POINT ptTab = pt;
                    MapWindowPoints(hwnd, g_app->hTabControl, &ptTab, 1);
                    TCHITTESTINFO hti = { .pt = ptTab };
                    int hitTab = (int)SendMessageW(g_app->hTabControl, TCM_HITTEST, 0, (LPARAM)&hti);
                    if (hitTab == -1) {
                        // Check for double-click timing
                        static DWORD lastClick = 0;
                        static POINT lastPt = {0, 0};
                        DWORD now = GetTickCount();
                        if ((now - lastClick) <= GetDoubleClickTime() &&
                            abs(pt.x - lastPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                            abs(pt.y - lastPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK)) {
                            App_CreateTab(L"Untitled");
                            lastClick = 0;
                        } else {
                            lastClick = now;
                            lastPt = pt;
                        }
                    }
                }
            }
            break;

        case WM_SETFOCUS:
            // Forward focus to active editor
            {
                Tab* tab = App_GetActiveTab();
                if (tab && tab->hEditor) {
                    SetFocus(tab->hEditor);
                }
            }
            return 0;

        case WM_INITMENUPOPUP:
            {
                HMENU hMenu = (HMENU)wParam;
                int menuIndex = LOWORD(lParam);
                switch (menuIndex) {
                    case 0: MenuBar_UpdateFileMenu(hMenu); break;
                    case 1: MenuBar_UpdateEditMenu(hMenu); break;
                    case 2: MenuBar_UpdateFormatMenu(hMenu); break;
                    case 3: MenuBar_UpdateViewMenu(hMenu); break;
                    case 4: MenuBar_UpdateSettingsMenu(hMenu); break;
                }
            }
            return 0;

        case WM_CONTEXTMENU:
            {
                HWND hTarget = (HWND)wParam;
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                POINT pt = { x, y };

                // Check if right-click is on tab control
                if (hTarget == g_app->hTabControl) {
                    TabControl_OnRightClick(x, y);
                    return 0;
                }

                // Check if click is in tab control area (even if on empty space)
                RECT rcTab;
                GetWindowRect(g_app->hTabControl, &rcTab);
                if (PtInRect(&rcTab, pt)) {
                    TabControl_OnRightClick(x, y);
                    return 0;
                }

                // Check if right-click is on editor (by target or by position)
                Tab* tab = App_GetActiveTab();
                if (tab && tab->hEditor) {
                    RECT rcEditor;
                    GetWindowRect(tab->hEditor, &rcEditor);
                    if (hTarget == tab->hEditor || PtInRect(&rcEditor, pt)) {
                        MainWindow_ShowEditorContextMenu(hwnd, x, y);
                        return 0;
                    }
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_APP_UPDATE_STATUS:
            {
                Tab* tab = App_GetActiveTab();
                if (tab && tab->hEditor) {
                    int line, col;
                    Editor_GetCursorPos(tab->hEditor, &line, &col);
                    StatusBar_UpdatePosition(line, col);
                }
            }
            return 0;

        case WM_APP_TRAY_CALLBACK:
            if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                TrayIcon_Remove();
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            }
            return 0;

        case WM_APP_DOC_MODIFIED:
            {
                // Ask the control rather than assuming. This message is posted
                // from a change notification, and loading a file is a change:
                // the notification arrived after the load had already declared
                // the document clean, so every file was marked modified the
                // moment it was opened and asked to be saved on the way out.
                Tab* tab = App_GetActiveTab();
                if (tab && tab->document && tab->hEditor) {
                    BOOL modified = Editor_GetModified(tab->hEditor);
                    tab->document->modified = modified;

                    // A scratch tab with something in it gets a note behind
                    // it, so what was typed survives a restart.
                    if (modified) Document_BeginNote(tab->document, tab->hEditor);

                    TabControl_UpdateTabTitle(tab->index);
                    MainWindow_UpdateTitle();
                    StatusBar_UpdateModified(modified);
                }
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// New tab button handle
static HWND g_hNewTabBtn = NULL;

// Update position of the new tab button (call after adding/removing tabs)
void MainWindow_UpdateNewTabButton(void) {
    if (!g_hNewTabBtn || !g_app->hTabControl) return;

    int btnWidth = 24;
    int tabHeight = 24;
    int btnX = 4;

    int tabCount = (int)SendMessageW(g_app->hTabControl, TCM_GETITEMCOUNT, 0, 0);
    if (tabCount > 0) {
        RECT rcTab;
        if (SendMessageW(g_app->hTabControl, TCM_GETITEMRECT, tabCount - 1, (LPARAM)&rcTab)) {
            btnX = rcTab.right + 2;
        }
    }

    // Position button and ensure it's visible on top
    SetWindowPos(g_hNewTabBtn, HWND_TOP, btnX, 2, btnWidth, tabHeight - 4, SWP_SHOWWINDOW);
    InvalidateRect(g_hNewTabBtn, NULL, TRUE);
}

// Create child controls
static void OnCreate(HWND hwnd) {
    // Create tab control
    g_app->hTabControl = TabControl_Create(hwnd);

    // Create "+" button for new tab (initially hidden until properly positioned)
    g_hNewTabBtn = CreateWindowExW(
        0, L"BUTTON", L"+",
        WS_CHILD | BS_PUSHBUTTON,  // Not visible yet - will show after positioning
        0, 0, 24, 22,
        hwnd, (HMENU)(INT_PTR)IDC_NEW_TAB_BTN,
        g_app->hInstance, NULL
    );
    if (g_hNewTabBtn) {
        // Use same font as tab control
        SendMessageW(g_hNewTabBtn, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }

    // Formatting toolbar. Created hidden; it appears when a rich text tab is
    // active and takes its own strip of the window when it does.
    FormatBar_Create(hwnd);

    // Create status bar
    g_app->hStatusBar = StatusBar_Create(hwnd);
    StatusBar_Show(g_app->showStatusBar);

    // Set up status update timer
    SetTimer(hwnd, TIMER_STATUS_UPDATE, 100, NULL);
}

// Cleanup on destroy
static void OnDestroy(HWND hwnd) {
    (void)hwnd;
    TrayIcon_Remove();
    KillTimer(hwnd, TIMER_STATUS_UPDATE);
    PostQuitMessage(0);
}

// Handle link click - navigate to target tab
static void HandleLinkClick(int position) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->document) return;

    // Get link at this position
    Link* link = NULL;
    if (tab->document->type == DOC_TYPE_FILE) {
        link = Links_GetAtPosition(DOC_TYPE_FILE, tab->document->filePath, 0, position);
    } else if (tab->document->type == DOC_TYPE_NOTE) {
        link = Links_GetAtPosition(DOC_TYPE_NOTE, NULL, tab->document->noteId, position);
    }

    if (!link) return;

    // Handle URL links - open in browser
    if (link->targetType == LINK_TARGET_URL && link->targetURL[0]) {
        ShellExecuteW(NULL, L"open", link->targetURL, NULL, NULL, SW_SHOWNORMAL);
        Links_Free(link);
        return;
    }

    // Find the target tab for file/note links
    for (int i = 0; i < MAX_TABS; i++) {
        if (!g_app->tabs[i] || !g_app->tabs[i]->document) continue;

        BOOL match = FALSE;
        if (link->targetType == LINK_TARGET_FILE && g_app->tabs[i]->document->type == DOC_TYPE_FILE) {
            match = (_wcsicmp(g_app->tabs[i]->document->filePath, link->targetPath) == 0);
        } else if (link->targetType == LINK_TARGET_NOTE && g_app->tabs[i]->document->type == DOC_TYPE_NOTE) {
            match = (g_app->tabs[i]->document->noteId == link->targetNoteId);
        }

        if (match) {
            App_SetActiveTab(i);
            break;
        }
    }

    Links_Free(link);
}

// Handle WM_NOTIFY
static LRESULT OnNotify(HWND hwnd, int idCtrl, LPNMHDR pnmh) {
    // The formatting toolbar draws its own buttons -- see toolbar.c. This
    // value has to be returned from the window procedure; DWLP_MSGRESULT is
    // for dialog procedures and would be ignored here.
    if (pnmh->code == NM_CUSTOMDRAW && pnmh->hwndFrom == FormatBar_Handle()) {
        return FormatBar_OnCustomDraw((LPNMTBCUSTOMDRAW)pnmh);
    }

    // Toolbar tooltips
    if (pnmh->code == TTN_GETDISPINFOW) {
        FormatBar_OnGetTooltip((NMTTDISPINFOW*)pnmh);
        return 0;
    }
    (void)hwnd;
    (void)idCtrl;

    if (pnmh->hwndFrom == g_app->hTabControl) {
        switch (pnmh->code) {
            case TCN_SELCHANGE:
                TabControl_OnSelChange();
                break;
            case NM_RCLICK:
                {
                    POINT pt;
                    GetCursorPos(&pt);
                    TabControl_OnRightClick(pt.x, pt.y);
                }
                break;
            case NM_DBLCLK:
                {
                    // Double-click on empty area creates new tab
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(g_app->hTabControl, &pt);
                    TCHITTESTINFO hti = { .pt = pt };
                    int hitTab = (int)SendMessageW(g_app->hTabControl, TCM_HITTEST, 0, (LPARAM)&hti);
                    if (hitTab == -1) {
                        // Clicked on empty space, create new tab
                        App_CreateTab(L"Untitled");
                    }
                }
                break;
        }
    }

    // Scintilla notifications
    Tab* tab = App_GetActiveTab();
    if (tab && pnmh->hwndFrom == tab->hEditor) {
        SCNotification* scn = (SCNotification*)pnmh;

        switch (pnmh->code) {
            case SCN_MODIFIED:
                PostMessageW(hwnd, WM_APP_DOC_MODIFIED, 0, 0);
                PostMessageW(hwnd, WM_APP_UPDATE_STATUS, 0, 0);
                break;

            case SCN_UPDATEUI:
                PostMessageW(hwnd, WM_APP_UPDATE_STATUS, 0, 0);
                break;

            case SCN_INDICATORCLICK:
                // Check if click was on link indicator (indicator 8)
                if (Editor_HasIndicatorAt(tab->hEditor, scn->position, EDITOR_INDICATOR_LINK)) {
                    HandleLinkClick(scn->position);
                }
                break;
        }
    }

    // Rich text view notifications. EN_SELCHANGE is what keeps the formatting
    // toolbar showing the state of whatever the caret is now sitting in.
    if (pnmh->code == EN_CHANGE || pnmh->code == EN_SELCHANGE) {
        if (tab && pnmh->hwndFrom == tab->hEditor) {
            if (pnmh->code == EN_CHANGE) {
                PostMessageW(hwnd, WM_APP_DOC_MODIFIED, 0, 0);
            }
            FormatBar_SyncFromEditor(tab->hEditor);
            PostMessageW(hwnd, WM_APP_UPDATE_STATUS, 0, 0);
        }
    }

    return 0;
}

// Put a newly created document into a tab.
//
// Reuses the current tab when it is empty, unmodified and backed by the right
// view; otherwise opens a new one. The view check is the part that matters:
// an .rtf document dropped into a Scintilla tab would show the user its markup
// instead of the document. Four call sites had this block copied out, and only
// by having one of them does the check happen everywhere.
// Opening a document: reuse the tab when it is an untouched one of the right
// kind, otherwise make a new one. Startup and File > Open both come here --
// they used to have a copy each, and the copy at startup had never heard of a
// PDF.
void MainWindow_OpenDocument(Document* newDoc) {
    if (!newDoc) return;

    Tab* tab = App_GetActiveTab();
    Document* doc = tab ? tab->document : NULL;

    // A PDF always gets a tab of its own: there is no editor to reuse, and
    // the view owns the file rather than a copy of its contents.
    if (newDoc->format == FORMAT_PDF) {
        int idx = App_CreateTabEx(NULL, FORMAT_PLAIN);
        if (idx < 0) {
            Document_Destroy(newDoc);
            return;
        }

        Tab* fresh = g_app->tabs[idx];
        HWND view = PdfView_Create(g_app->hMainWindow, newDoc->filePath);
        if (!view) {
            MessageBoxW(g_app->hMainWindow,
                L"That PDF could not be opened.\n\n"
                L"It may be damaged, or protected with a password.",
                APP_NAME, MB_ICONWARNING);
            Document_Destroy(newDoc);
            return;
        }

        Document_Destroy(fresh->document);
        fresh->document = newDoc;
        fresh->hPdfView = view;
        ShowWindow(fresh->hEditor, SW_HIDE);

        RECT rc;
        GetClientRect(g_app->hMainWindow, &rc);
        MainWindow_OnSize(g_app->hMainWindow, SIZE_RESTORED, rc.right, rc.bottom);

        TabControl_UpdateTabTitle(idx);
        MainWindow_UpdateTitle();

        WCHAR status[320];
        PdfView_Describe(view, status, 64);

        // A signed document says so the moment it opens: nobody thinks to go
        // looking, and a signature nobody notices is a signature that may as
        // well not be there.
        PdfSignatureReport signature = {0};
        if (PdfForm_CheckSignature(newDoc->filePath, &signature) && signature.present) {
            WCHAR note[256];

            if (signature.intact && signature.trust == PDFTRUST_TRUSTED) {
                swprintf_s(note, 256, L" - signed by %s, unchanged, certificate trusted",
                           signature.signer[0] ? signature.signer : L"an unnamed signer");
            } else if (signature.intact) {
                // Intact but not trusted is the common case and the one worth
                // saying carefully: the bytes are fine, the signer is a claim.
                swprintf_s(note, 256, L" - signed by %s, unchanged - but %s",
                           signature.signer[0] ? signature.signer : L"an unnamed signer",
                           PdfSign_TrustSentence(signature.trust));
            } else {
                wcscpy_s(note, 256, L" - SIGNED, BUT THE BYTES DO NOT MATCH");
            }

            wcscat_s(status, 320, note);
        }

        StatusBar_SetMessage(status);
        return;
    }

    BOOL wantRich = FORMAT_IS_RICH(newDoc->format);
    BOOL viewMatches = tab && tab->hEditor &&
                       (Editor_IsRich(tab->hEditor) == wantRich);

    if (tab && doc && doc->isNew && !doc->modified && viewMatches) {
        Document_Destroy(doc);
        tab->document = newDoc;
        Document_Load(newDoc, tab->hEditor);
    } else {
        int idx = App_CreateTabEx(NULL, newDoc->format);
        if (idx < 0) {
            Document_Destroy(newDoc);
            return;
        }
        Document_Destroy(g_app->tabs[idx]->document);
        g_app->tabs[idx]->document = newDoc;
        Document_Load(newDoc, g_app->tabs[idx]->hEditor);
    }

    TabControl_UpdateTabTitle(g_app->activeTab);
    MainWindow_UpdateTitle();

    // The toolbar was last synced against whatever the tab held before; the
    // document that just loaded has its own fonts and alignment.
    Tab* active = App_GetActiveTab();
    if (active && active->hEditor) FormatBar_SyncFromEditor(active->hEditor);
}

// Handle WM_SIZE
void MainWindow_OnSize(HWND hwnd, UINT state, int cx, int cy) {
    (void)hwnd;
    if (state == SIZE_MINIMIZED) {
        if (g_app->minimizeToTray) {
            TrayIcon_Add(hwnd);
            ShowWindow(hwnd, SW_HIDE);
        }
        return;
    }

    // Position status bar
    if (g_app->hStatusBar && g_app->showStatusBar) {
        StatusBar_Resize(hwnd);
    }

    // Get status bar height
    int statusHeight = 0;
    if (g_app->hStatusBar && g_app->showStatusBar) {
        RECT rcStatus;
        GetWindowRect(g_app->hStatusBar, &rcStatus);
        statusHeight = rcStatus.bottom - rcStatus.top;
    }

    // Fixed tab control height and new tab button
    int tabHeight = 24;
    int btnWidth = 24;
    if (g_app->hTabControl) {
        SetWindowPos(g_app->hTabControl, NULL, 0, 0, cx, tabHeight, SWP_NOZORDER);
    }
    if (g_hNewTabBtn) {
        // Position button right after the last tab
        int btnX = 4;  // Default position if no tabs
        int tabCount = (int)SendMessageW(g_app->hTabControl, TCM_GETITEMCOUNT, 0, 0);
        if (tabCount > 0) {
            RECT rcTab;
            SendMessageW(g_app->hTabControl, TCM_GETITEMRECT, tabCount - 1, (LPARAM)&rcTab);
            btnX = rcTab.right + 2;
        }
        SetWindowPos(g_hNewTabBtn, HWND_TOP, btnX, 2, btnWidth, tabHeight - 4, 0);
    }

    // The toolbar reflects what the active tab is showing: a laid-out page has
    // no character formatting yet, so that half of the bar goes grey there too.
    Tab* tab = App_GetActiveTab();

    // A PDF tab has an editor underneath it holding nothing, so the bar is
    // told there is none: what is on screen cannot be typed into.
    HWND barEditor = (tab && !tab->hPdfView) ? tab->hEditor : NULL;
    FormatBar_UpdateVisibility(barEditor, tab && tab->hPageView);

    int barHeight = FormatBar_Height();
    if (barHeight > 0) {
        SetWindowPos(FormatBar_Handle(), NULL, 0, tabHeight, cx, barHeight, SWP_NOZORDER);
        FormatBar_Layout(cx);
    }

    // Position whichever view this tab is showing, in the same rectangle.
    if (tab) {
        int top = tabHeight + barHeight;
        HWND view = tab->hPdfView  ? tab->hPdfView
                  : tab->hPageView ? tab->hPageView
                                   : tab->hEditor;
        if (view) {
            SetWindowPos(view, NULL, 0, top, cx, cy - top - statusHeight, SWP_NOZORDER);
        }
    }
}

// Handle WM_CLOSE
void MainWindow_OnClose(HWND hwnd) {
    // If auto-save on exit is enabled, skip prompts - session will be saved
    if (!g_app->autoSaveSession) {
        // Check for unsaved changes in all tabs
        for (int i = 0; i < MAX_TABS; i++) {
            if (g_app->tabs[i] && g_app->tabs[i]->document && g_app->tabs[i]->document->modified) {
                App_SetActiveTab(i);
                int result = Dialogs_SaveChanges(hwnd, Document_GetTitle(g_app->tabs[i]->document));
                if (result == IDCANCEL) return;
                if (result == IDYES) {
                    if (!Document_Save(g_app->tabs[i]->document, g_app->tabs[i]->hEditor)) {
                        return;  // Save failed
                    }
                }
            }
        }
    }

    DestroyWindow(hwnd);
}

// Handle WM_QUERYENDSESSION
BOOL MainWindow_OnQueryEndSession(HWND hwnd) {
    // Check for unsaved changes
    for (int i = 0; i < MAX_TABS; i++) {
        if (g_app->tabs[i] && g_app->tabs[i]->document && g_app->tabs[i]->document->modified) {
            return FALSE;  // Block shutdown
        }
    }
    (void)hwnd;
    return TRUE;
}

// Handle WM_COMMAND
// Rich text formatting commands, from the toolbar and the Format menu alike.
// Returns TRUE when the command was one of them and has been dealt with.
//
// Every one of these is meaningless without a rich view, so the guard is here
// once rather than repeated per case.
static BOOL HandleRichFormatCommand(HWND hwnd, int id, HWND hEditor) {
    if (!hEditor || !Editor_IsRich(hEditor)) return FALSE;

    switch (id) {
        case IDM_FORMAT_BOLD:        Rich_ToggleEffect(hEditor, CFE_BOLD);        break;
        case IDM_FORMAT_ITALIC:      Rich_ToggleEffect(hEditor, CFE_ITALIC);      break;
        case IDM_FORMAT_UNDERLINE:   Rich_ToggleEffect(hEditor, CFE_UNDERLINE);   break;
        case IDM_FORMAT_STRIKE:      Rich_ToggleEffect(hEditor, CFE_STRIKEOUT);   break;
        case IDM_FORMAT_SUPERSCRIPT: Rich_ToggleEffect(hEditor, CFE_SUPERSCRIPT); break;
        case IDM_FORMAT_SUBSCRIPT:   Rich_ToggleEffect(hEditor, CFE_SUBSCRIPT);   break;

        case IDM_FORMAT_TEXTCOLOR:   Rich_ChooseColor(hEditor, hwnd);             break;

        case IDM_FORMAT_ALIGN_LEFT:    Rich_SetAlignment(hEditor, PFA_LEFT);    break;
        case IDM_FORMAT_ALIGN_CENTER:  Rich_SetAlignment(hEditor, PFA_CENTER);  break;
        case IDM_FORMAT_ALIGN_RIGHT:   Rich_SetAlignment(hEditor, PFA_RIGHT);   break;
        case IDM_FORMAT_ALIGN_JUSTIFY: Rich_SetAlignment(hEditor, PFA_JUSTIFY); break;

        case IDM_FORMAT_BULLETS:
            Rich_SetListStyle(hEditor,
                Rich_GetListStyle(hEditor) == PFN_BULLET ? 0 : PFN_BULLET);
            break;

        case IDM_FORMAT_NUMBERING: {
            WORD cur = Rich_GetListStyle(hEditor);
            BOOL numbered = (cur != 0 && cur != PFN_BULLET);
            Rich_SetListStyle(hEditor, numbered ? 0 : PFN_ARABIC);
            break;
        }

        case IDM_FORMAT_INDENT_MORE: Rich_Indent(hEditor, TRUE);  break;
        case IDM_FORMAT_INDENT_LESS: Rich_Indent(hEditor, FALSE); break;

        case IDM_FORMAT_LINESPACE_1:  Rich_SetLineSpacing(hEditor, 10); break;
        case IDM_FORMAT_LINESPACE_15: Rich_SetLineSpacing(hEditor, 15); break;
        case IDM_FORMAT_LINESPACE_2:  Rich_SetLineSpacing(hEditor, 20); break;

        case IDM_FORMAT_CLEAR:       Rich_ClearFormatting(hEditor);  break;

        case IDM_INSERT_PICTURE: {
            WCHAR path[MAX_PATH] = {0};
            static const WCHAR filter[] =
                L"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff)\0"
                L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0"
                L"All Files (*.*)\0*.*\0";
            OPENFILENAMEW ofn = {
                .lStructSize = sizeof(ofn),
                .hwndOwner = hwnd,
                .lpstrFilter = filter,
                .nFilterIndex = 1,
                .lpstrFile = path,
                .nMaxFile = MAX_PATH,
                .Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR
            };
            if (GetOpenFileNameW(&ofn)) {
                if (!Editor_IsRich(hEditor)) {
                    MessageBoxW(hwnd,
                        L"A plain text file has nowhere to keep a picture.\n\n"
                        L"Save it as .rtf or .docx first.",
                        APP_NAME, MB_ICONINFORMATION);
                } else if (!InsertPicture(hwnd, path)) {
                    MessageBoxW(hwnd,
                        L"That image could not be inserted.\n\n"
                        L"It could not be read as a picture.",
                        APP_NAME, MB_ICONWARNING);
                }
            }
            break;
        }

        default:
            return FALSE;
    }

    FormatBar_SyncFromEditor(hEditor);
    SetFocus(hEditor);
    return TRUE;
}

// Page margins, in thousandths of an inch, which is what PAGESETUPDLG uses
// when PSD_INTHOUSANDTHSOFINCHES is set. One inch all round is what WordPad
// defaulted to.
static RECT g_pageMargins = { 1000, 1000, 1000, 1000 };

static void ShowPageSetup(HWND hwnd) {
    PAGESETUPDLGW psd = {
        .lStructSize = sizeof(psd),
        .hwndOwner = hwnd,
        .Flags = PSD_INTHOUSANDTHSOFINCHES | PSD_MARGINS,
        .rtMargin = g_pageMargins
    };

    if (!PageSetupDlgW(&psd)) return;
    g_pageMargins = psd.rtMargin;

    // The layout engine, the printer and the .docx writer all read the page
    // off a new model, and a model captured from the editor has no page of
    // its own -- the control has no notion of one. Handing the setup to the
    // model layer is therefore the whole of applying it.
    //
    // Thousandths of an inch to twips, at 1440 twips to the inch.
    SectionProps page;
    Doc_GetPageDefaults(&page);
    page.marginLeft   = MulDiv(psd.rtMargin.left,   1440, 1000);
    page.marginTop    = MulDiv(psd.rtMargin.top,    1440, 1000);
    page.marginRight  = MulDiv(psd.rtMargin.right,  1440, 1000);
    page.marginBottom = MulDiv(psd.rtMargin.bottom, 1440, 1000);
    if (psd.ptPaperSize.x > 0 && psd.ptPaperSize.y > 0) {
        page.pageWidth  = MulDiv(psd.ptPaperSize.x, 1440, 1000);
        page.pageHeight = MulDiv(psd.ptPaperSize.y, 1440, 1000);
    }
    Doc_SetPageDefaults(&page);
}

// The printable area of a page, in twips, with the requested margins measured
// from the paper edge rather than from wherever the driver happens to start.
// Used by the control's own printing path, which works in these coordinates.
static BOOL PrintableRectTwips(HDC hDC, RECT* out) {
    int dpiX = GetDeviceCaps(hDC, LOGPIXELSX);
    int dpiY = GetDeviceCaps(hDC, LOGPIXELSY);
    if (dpiX <= 0 || dpiY <= 0) return FALSE;

    int offXTw = MulDiv(GetDeviceCaps(hDC, PHYSICALOFFSETX), 1440, dpiX);
    int offYTw = MulDiv(GetDeviceCaps(hDC, PHYSICALOFFSETY), 1440, dpiY);
    int pageWTw = MulDiv(GetDeviceCaps(hDC, PHYSICALWIDTH), 1440, dpiX);
    int pageHTw = MulDiv(GetDeviceCaps(hDC, PHYSICALHEIGHT), 1440, dpiY);

    RECT m = g_pageMargins;   // thousandths of an inch
    out->left   = MulDiv(m.left, 1440, 1000) - offXTw;
    out->top    = MulDiv(m.top,  1440, 1000) - offYTw;
    out->right  = pageWTw - MulDiv(m.right,  1440, 1000) - offXTw;
    out->bottom = pageHTw - MulDiv(m.bottom, 1440, 1000) - offYTw;

    if (out->left < 0) out->left = 0;
    if (out->top < 0) out->top = 0;

    return out->right > out->left && out->bottom > out->top;
}

// Does the document hold an embedded object -- in practice a picture? The
// control marks each one with U+FFFC in the text.
static BOOL RichHasPicture(HWND hEditor) {
    WCHAR* text = Editor_GetText(hEditor);
    if (!text) return FALSE;
    BOOL found = wcschr(text, 0xFFFC) != NULL;
    free(text);
    return found;
}

// Put the current rich document on a device: a printer, or the PDF printer
// with `outputFile` naming where to write. Returns pages produced.
//
// The layout engine does it, because the engine is what knows about paper,
// margins and tables and is what the preview draws -- the screen and the page
// cannot drift apart if one of them produces both.
//
// ponytail: a document holding a picture still prints through the control.
// The model carries no images until v0.9, and dropping a picture off a
// printout is a worse trade than losing the engine's pagination. Delete this
// fallback, and `Rich_PrintToDC` with it, when images land in the model.
static int PrintRichDocumentTo(HWND hwnd, HWND hEditor, const WCHAR* printerName,
                               HDC hDC, const WCHAR* title, const WCHAR* outputFile,
                               const DocModel* source) {
    // No printer name means Direct2D has nothing to address, so the control
    // prints instead -- as it does for a rich document holding a picture,
    // which the engine cannot draw from the model yet.
    if (Editor_IsRich(hEditor) &&
        (!printerName || !printerName[0] || RichHasPicture(hEditor))) {
        RECT rc;
        if (!hDC) return 0;
        if (!PrintableRectTwips(hDC, &rc)) {
            MessageBoxW(hwnd,
                L"Those margins leave no room to print on this paper size.\n\n"
                L"Reduce them in Page Setup.",
                APP_NAME, MB_ICONWARNING);
            return 0;
        }
        return Rich_PrintToDC(hEditor, hDC, &rc, title, outputFile);
    }

    // Either kind of document lays out: a rich one is captured, a plain one is
    // its text in the editor's font. The old plain path drew a single page
    // with DrawText and stopped there, so a long note printed its first page
    // and lost the rest.
    DocModel* model = NULL;

    if (Editor_IsRich(hEditor)) {
        model = DocView_CaptureWith(hEditor, source);
    } else {
        WCHAR* text = Editor_GetText(hEditor);
        CharProps props = {0};

        wcsncpy_s(props.font, LF_FACESIZE, g_app->editorFont.lfFaceName, _TRUNCATE);
        int points = -g_app->editorFont.lfHeight * 72 / 96;
        if (points < 6 || points > 72) points = 11;
        props.halfPoints = points * 2;

        model = Doc_FromText(text ? text : L"", &props);
        free(text);
    }

    if (!model) return 0;

    int pages = 0;
    LayoutPrint_ToPrinter(model, printerName, title, outputFile, &pages);
    Doc_Free(model);
    return pages;
}

// The printer the user chose, out of what PrintDlg filled in. Direct2D
// addresses a printer by name rather than by device context.
static const WCHAR* ChosenPrinter(const PRINTDLGW* pd) {
    if (!pd->hDevNames) return NULL;
    const DEVNAMES* dn = (const DEVNAMES*)GlobalLock(pd->hDevNames);
    if (!dn) return NULL;

    static WCHAR name[256];
    wcsncpy_s(name, 256, (const WCHAR*)dn + dn->wDeviceOffset, _TRUNCATE);
    GlobalUnlock(pd->hDevNames);
    return name;
}

// Print a rich text document across as many pages as it needs.
static void PrintRichDocument(HWND hwnd, HWND hEditor, Document* doc) {
    PRINTDLGW pd = {
        .lStructSize = sizeof(pd),
        .hwndOwner = hwnd,
        .Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION
    };
    if (!PrintDlgW(&pd)) return;

    int pages = PrintRichDocumentTo(hwnd, hEditor, ChosenPrinter(&pd), pd.hDC,
                                    doc ? Document_GetTitle(doc) : L"Document", NULL,
                                    doc ? doc->source : NULL);

    DeleteDC(pd.hDC);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);

    if (pages == 0) {
        MessageBoxW(hwnd, L"Nothing was printed.", APP_NAME, MB_ICONWARNING);
    }
}

// Export to PDF: the printing path above, pointed at the PDF printer Windows
// has shipped since Windows 10, with the output file named so it writes there
// instead of asking. There is no PDF library here and there does not need to
// be.
static void ExportToPdf(HWND hwnd, HWND hEditor, Document* doc) {
    WCHAR path[MAX_PATH] = {0};

    const WCHAR* title = doc ? Document_GetTitle(doc) : NULL;
    if (title) {
        wcsncpy_s(path, MAX_PATH, title, _TRUNCATE);
        WCHAR* dot = wcsrchr(path, L'.');
        if (dot) *dot = L'\0';
        wcsncat_s(path, MAX_PATH, L".pdf", _TRUNCATE);
    }

    OPENFILENAMEW ofn = {
        .lStructSize = sizeof(ofn),
        .hwndOwner = hwnd,
        .lpstrFilter = L"PDF Document (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0",
        .lpstrFile = path,
        .nMaxFile = MAX_PATH,
        .lpstrDefExt = L"pdf",
        .Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
    };
    if (!GetSaveFileNameW(&ofn)) return;

    // A DC for the PDF printer as well as its name: the engine addresses the
    // printer by name, and the picture fallback still needs a device context.
    HDC hDC = CreateDCW(L"WINSPOOL", LAYOUTPRINT_PDF_DEVICE, NULL, NULL);
    if (!hDC) {
        MessageBoxW(hwnd,
            L"Windows' PDF printer is not available.\n\n"
            L"It ships as \"Microsoft Print to PDF\"; if it has been removed, "
            L"add it again under Printers & scanners.",
            APP_NAME, MB_ICONWARNING);
        return;
    }

    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    int pages = PrintRichDocumentTo(hwnd, hEditor, LAYOUTPRINT_PDF_DEVICE, hDC,
                                    doc ? Document_GetTitle(doc) : L"Document", path,
                                    doc ? doc->source : NULL);
    SetCursor(old);
    if (hDC) DeleteDC(hDC);

    if (pages > 0) {
        StatusBar_SetMessage(L"Exported to PDF");
        App_AddRecentFile(path);
    } else {
        MessageBoxW(hwnd, L"The PDF could not be written.", APP_NAME, MB_ICONERROR);
    }
}

// Switch the active tab between its text control and the laid-out view.
//
// The control stays the document's home: the laid-out view works on a copy and
// hands it back on the way out. That is what makes the two views agree without
// either of them having to watch the other.
static void TogglePageLayout(HWND hwnd) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->hEditor) return;

    if (tab->hPageView) {
        PageView_Apply(tab->hPageView);
        DestroyWindow(tab->hPageView);
        tab->hPageView = NULL;

        ShowWindow(tab->hEditor, SW_SHOW);
        SetFocus(tab->hEditor);
        StatusBar_SetMessage(L"");
    } else {
        HWND view = PageView_Create(hwnd, tab->hEditor,
                                    tab->document ? tab->document->source : NULL,
                                    tab->document ? Document_GetTitle(tab->document) : NULL);
        if (!view) {
            MessageBoxW(hwnd, L"The page layout view could not be prepared.",
                        APP_NAME, MB_ICONWARNING);
            return;
        }

        tab->hPageView = view;
        ShowWindow(tab->hEditor, SW_HIDE);
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    MainWindow_OnSize(hwnd, SIZE_RESTORED, rc.right, rc.bottom);
}

// The laid-out view holds edits that have not reached the control yet, and
// saving, printing and exporting all read the control.
static void FlushPageLayout(void) {
    Tab* tab = App_GetActiveTab();
    if (tab && tab->hPageView) PageView_Apply(tab->hPageView);
}

// Which paragraph the user is looking at: the one the page view's caret is in
// when that is the view, and otherwise the one the text control's selection
// starts in -- counted the way the capture counts them, by the paragraph marks
// before it.
static int ActiveParagraphIndex(Tab* tab) {
    if (tab->hPageView) {
        int index = 0;
        if (PageView_CaretPara(tab->hPageView, &index)) return index;
    }

    CHARRANGE sel = {0};
    SendMessageW(tab->hEditor, EM_EXGETSEL, 0, (LPARAM)&sel);
    if (sel.cpMin <= 0) return 0;

    TEXTRANGEW range;
    WCHAR* buffer = (WCHAR*)calloc((size_t)sel.cpMin + 1, sizeof(WCHAR));
    if (!buffer) return 0;

    range.chrg.cpMin = 0;
    range.chrg.cpMax = sel.cpMin;
    range.lpstrText = buffer;
    SendMessageW(tab->hEditor, EM_GETTEXTRANGE, 0, (LPARAM)&range);

    int paragraphs = 0;
    for (const WCHAR* c = buffer; *c; c++) {
        if (*c == L'\r' || *c == L'\n') paragraphs++;
    }
    free(buffer);
    return paragraphs;
}

// A comment on the paragraph the user is in. The document is captured, the
// comment added to the model and the views put back in step -- the same shape
// as every other command that changes something the control cannot hold.
static void NewComment(HWND hwnd) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->document || !tab->hEditor) return;

    if (!Editor_IsRich(tab->hEditor)) {
        MessageBoxW(hwnd, L"A plain text file has nowhere to keep a comment.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }

    WCHAR text[512] = L"";
    if (!Dialogs_InputBox(hwnd, L"New Comment",
                          L"What do you want to say about this paragraph?",
                          text, 512)) {
        return;
    }
    if (!text[0]) return;

    int paragraph = ActiveParagraphIndex(tab);

    FlushPageLayout();

    Document* doc = tab->document;
    DocModel* model = DocView_CaptureWith(tab->hEditor, doc->source);
    if (!model) return;

    // Who is saying it: the Windows account, which is the only name this
    // program knows and the one Word would use too.
    WCHAR author[64] = L"";
    DWORD size = 64;
    if (!GetUserNameW(author, &size) || !author[0]) wcscpy_s(author, 64, L"Author");

    WCHAR initials[16] = L"";
    initials[0] = author[0];
    initials[1] = L'\0';

    DocComment* comment = Doc_AddComment(model, author, initials, text);
    DocPara* para = Doc_ParaAt(model, paragraph);
    if (!comment || !para) {
        Doc_Free(model);
        return;
    }
    Doc_MarkComment(para, comment->id);

    Doc_Free(doc->source);
    doc->source = model;
    doc->modified = TRUE;

    if (tab->hPageView) {
        TogglePageLayout(hwnd);
        TogglePageLayout(hwnd);
    }

    TabControl_UpdateTabTitle(tab->index);
    MainWindow_UpdateTitle();
    StatusBar_UpdateModified(TRUE);

    WCHAR message[128];
    swprintf_s(message, 128, L"Comment added to paragraph %d", paragraph + 1);
    StatusBar_SetMessage(message);
}

// The list of them, which is also where one can be taken off.
static void ShowComments(HWND hwnd) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->document || !tab->hEditor) return;

    FlushPageLayout();

    Document* doc = tab->document;
    DocModel* model = DocView_CaptureWith(tab->hEditor, doc->source);
    if (!model) return;

    if (Doc_CountComments(model) == 0) {
        Doc_Free(model);
        MessageBoxW(hwnd, L"This document has no comments.", APP_NAME, MB_ICONINFORMATION);
        return;
    }

    BOOL changed = Dialogs_Comments(hwnd, model);

    Doc_Free(doc->source);
    doc->source = model;

    if (changed) {
        doc->modified = TRUE;
        if (tab->hPageView) {
            TogglePageLayout(hwnd);
            TogglePageLayout(hwnd);
        }
        TabControl_UpdateTabTitle(tab->index);
        MainWindow_UpdateTitle();
        StatusBar_UpdateModified(TRUE);
    }
}

// A picture goes into the document rather than into the view.
//
// It used to go straight into the RichEdit control, which meant two things:
// only a bitmap would load, because that is all LoadImage reads, and the
// picture existed nowhere but the control -- so saving to .docx wrote a
// document with no picture in it. The control cannot hold a picture's bytes,
// which is the same wall page numbers, comments and tracked changes all ran
// into, and the same road out: capture the document, change the model, hand
// it back to both views.
static BOOL InsertPicture(HWND hwnd, const WCHAR* path) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->document || !tab->hEditor) return FALSE;

    // A picture wider than the text is laid out wider than the paper, so the
    // page it is going on decides how big it may be.
    SectionProps page;
    Doc_GetPageDefaults(&page);
    if (tab->document->source) page = tab->document->source->section;

    // 635 EMU to the twip.
    int maxWidthEmu = (page.pageWidth - page.marginLeft - page.marginRight) * 635;
    int paragraph = ActiveParagraphIndex(tab);

    FlushPageLayout();

    Document* doc = tab->document;
    DocModel* model = DocView_CaptureWith(tab->hEditor, doc->source);
    if (!model) return FALSE;

    // On a line of its own, after the paragraph the caret is in: a picture
    // dropped into the middle of a sentence is a harder question than it
    // looks, and this is what everyone expects anyway.
    DocPara* at = Doc_ParaAt(model, paragraph);
    DocPara* into = Doc_InsertParaBefore(model, at ? at->next : NULL);
    if (!into) into = Doc_AddPara(model);

    if (!into || !Doc_AddImageFromFile(into, path, maxWidthEmu)) {
        Doc_Free(model);
        return FALSE;
    }

    char* rtf = DocRtf_Emit(model);
    if (rtf) {
        Rich_SetRtf(tab->hEditor, rtf);
        free(rtf);
    }

    Doc_Free(doc->source);
    doc->source = model;
    doc->modified = TRUE;

    if (tab->hPageView) {
        TogglePageLayout(hwnd);
        TogglePageLayout(hwnd);
    }

    TabControl_UpdateTabTitle(tab->index);
    MainWindow_UpdateTitle();
    StatusBar_UpdateModified(TRUE);
    StatusBar_SetMessage(L"Picture inserted");
    return TRUE;
}

// What the Insert menu's field commands have in common: the document is
// captured, something is added to the model, and the views are put back in
// step with it.
//
// The model is the only place any of this can live. A page number in a footer,
// a table of contents that knows what page a heading is on -- the control has
// nowhere to keep either, which is why they are added here and shown by the
// layout engine.
typedef enum { INSERT_PAGE_NUMBERS, INSERT_TOC, INSERT_DATE } InsertKind;

static void InsertField(HWND hwnd, InsertKind kind) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->document || !tab->hEditor) return;

    if (!Editor_IsRich(tab->hEditor)) {
        MessageBoxW(hwnd,
            L"This belongs to a rich text document.\n\n"
            L"Save the file as .rtf or .docx first, or start one with "
            L"File > New Rich Text Document.",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }

    FlushPageLayout();

    Document* doc = tab->document;
    DocModel* model = DocView_CaptureWith(tab->hEditor, doc->source);
    if (!model) return;

    WCHAR message[128] = L"";
    BOOL textChanged = FALSE;

    switch (kind) {
        case INSERT_PAGE_NUMBERS:
            Doc_InsertPageNumbers(model);
            wcscpy_s(message, 128, L"Page numbers added to the footer");
            break;

        case INSERT_TOC: {
            int entries = Doc_InsertTableOfContents(model);
            if (entries == 0) {
                Doc_Free(model);
                MessageBoxW(hwnd,
                    L"A table of contents is built from the document's headings, "
                    L"and this one has none.\n\n"
                    L"Give the headings a heading style first.",
                    APP_NAME, MB_ICONINFORMATION);
                return;
            }
            swprintf_s(message, 128, L"Table of contents: %d entr%s",
                       entries, entries == 1 ? L"y" : L"ies");
            textChanged = TRUE;
            break;
        }

        case INSERT_DATE: {
            // At the end, which is where a letter's date goes and the only
            // place the control can be asked about without a caret in the
            // model. The page view inserts where the caret is.
            DocPara* last = Doc_ParaAt(model, Doc_CountParas(model) - 1);
            if (!last) {
                Doc_Free(model);
                return;
            }
            CharProps props = model->defaultRun;
            Doc_AddFieldRun(last, L" DATE \\@ \"d MMMM yyyy\" ", L"", &props);
            Doc_UpdateFields(model);
            wcscpy_s(message, 128, L"Date field added");
            textChanged = TRUE;
            break;
        }
    }

    if (textChanged) {
        char* rtf = DocRtf_Emit(model);
        if (rtf) {
            Rich_SetRtf(tab->hEditor, rtf);
            free(rtf);
        }
    }

    Doc_Free(doc->source);
    doc->source = model;
    doc->modified = TRUE;

    if (tab->hPageView) {
        TogglePageLayout(hwnd);
        TogglePageLayout(hwnd);
    }

    TabControl_UpdateTabTitle(tab->index);
    MainWindow_UpdateTitle();
    StatusBar_UpdateModified(TRUE);
    StatusBar_SetMessage(message);
}

// Accept or reject every tracked change in the document.
//
// The document is captured first, exactly as saving captures it, so anything
// typed since it was opened is part of what gets resolved rather than thrown
// away. The result becomes the document's model and the view is rebuilt from
// it: accepting changes nothing on screen, rejecting brings deleted text back
// and takes inserted text away.
static void ResolveRevisions(HWND hwnd, BOOL accept) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->document || !tab->hEditor) return;

    if (!Editor_IsRich(tab->hEditor)) {
        MessageBoxW(hwnd, L"A plain text file cannot carry tracked changes.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }

    FlushPageLayout();

    Document* doc = tab->document;
    DocModel* model = DocView_CaptureWith(tab->hEditor, doc->source);
    if (!model) return;

    int count = Doc_CountRevisions(model);
    if (count == 0) {
        Doc_Free(model);
        MessageBoxW(hwnd, L"This document has no tracked changes.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }

    if (accept) Doc_AcceptRevisions(model);
    else        Doc_RejectRevisions(model);

    char* rtf = DocRtf_Emit(model);
    if (rtf) {
        Rich_SetRtf(tab->hEditor, rtf);
        free(rtf);
    }

    Doc_Free(doc->source);
    doc->source = model;
    doc->modified = TRUE;

    // The laid-out view is a view of the model, so it is rebuilt from the new
    // one rather than left showing the old.
    if (tab->hPageView) {
        TogglePageLayout(hwnd);
        TogglePageLayout(hwnd);
    }

    TabControl_UpdateTabTitle(tab->index);
    MainWindow_UpdateTitle();
    StatusBar_UpdateModified(TRUE);

    WCHAR message[128];
    swprintf_s(message, 128, L"%d tracked change%s %s",
               count, count == 1 ? L"" : L"s", accept ? L"accepted" : L"rejected");
    StatusBar_SetMessage(message);
}

// A PDF is shown, not edited. The commands that would change a document, or
// lay one out, or print one, all mean "the document in this tab" -- and this
// tab is holding somebody else's file that opennote cannot yet write.
//
// Saying so once, here, beats each command finding out for itself.
static BOOL RefusedForPdf(HWND hwnd, int id) {
    Tab* tab = App_GetActiveTab();
    if (!tab || !tab->hPdfView) return FALSE;

    switch (id) {
        case IDM_FILE_SAVE:
        case IDM_FILE_SAVEAS:
        case IDM_FILE_PRINT:
        case IDM_FILE_PRINT_PREVIEW:
        case IDM_FILE_EXPORT_PDF:
        case IDM_FILE_PAGE_SETUP:
            // ...but not the three commands that mean something here:
            // filling in the form, and the two kinds of signing.
        case IDM_VIEW_PAGE_LAYOUT:
        case IDM_INSERT_PICTURE:
        case IDM_INSERT_PAGE_NUMBERS:
        case IDM_INSERT_TOC:
        case IDM_INSERT_DATE_FIELD:
        case IDM_REVIEW_NEW_COMMENT:
        case IDM_REVIEW_COMMENTS:
        case IDM_REVIEW_ACCEPT_ALL:
        case IDM_REVIEW_REJECT_ALL:
            MessageBoxW(hwnd,
                L"This PDF can be read but not changed.\n\n"
                L"Filling in a form and signing one are the next thing along -- "
                L"see the roadmap.",
                APP_NAME, MB_ICONINFORMATION);
            return TRUE;
    }

    return FALSE;
}

void MainWindow_OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify) {
    if (RefusedForPdf(hwnd, id)) return;

    (void)hwndCtl;
    (void)codeNotify;

    Tab* tab = App_GetActiveTab();
    Document* doc = tab ? tab->document : NULL;
    HWND hEditor = tab ? tab->hEditor : NULL;

    // The formatting toolbar's own combos, then the formatting commands.
    if (FormatBar_OnCommand(hEditor, id, (int)codeNotify, hwndCtl)) return;
    if (HandleRichFormatCommand(hwnd, id, hEditor)) return;

    switch (id) {
        // File menu
        case IDM_FILE_NEW:
        case IDC_NEW_TAB_BTN:
            App_CreateTab(L"Untitled");
            break;

        case IDM_FILE_NEW_RICH:
            App_CreateTabEx(L"Untitled", FORMAT_RTF);
            break;

        case IDM_FILE_OPEN:
            {
                WCHAR path[MAX_PATH] = {0};
                if (Dialogs_OpenFile(hwnd, path, MAX_PATH)) {
                    Document* newDoc = Document_CreateFromFile(path);
                    if (newDoc) {
                        // If current tab is empty and unmodified, reuse it
                        MainWindow_OpenDocument(newDoc);
                        App_AddRecentFile(path);
                    }
                }
            }
            break;

        case IDM_FILE_OPEN_NOTE:
            {
                int noteId = 0;
                if (Dialogs_NotesBrowser(hwnd, &noteId) && noteId > 0) {
                    Document* newDoc = Document_CreateFromNote(noteId);
                    if (newDoc) {
                        MainWindow_OpenDocument(newDoc);
                    }
                }
            }
            break;

        case IDM_FILE_SAVE:
            FlushPageLayout();
            if (doc && hEditor) {
                Document_Save(doc, hEditor);
                TabControl_UpdateTabTitle(tab->index);
                MainWindow_UpdateTitle();
                StatusBar_UpdateModified(doc->modified);
            }
            break;

        case IDM_FILE_SAVEAS:
            FlushPageLayout();
            if (doc && hEditor) {
                WCHAR path[MAX_PATH] = {0};
                if (Dialogs_SaveFile(hwnd, path, MAX_PATH, Document_GetTitle(doc))) {
                    Document_SaveAs(doc, hEditor, path);
                    TabControl_UpdateTabTitle(tab->index);
                    MainWindow_UpdateTitle();
                    StatusBar_UpdateModified(doc->modified);
                    App_AddRecentFile(path);
                }
            }
            break;

        case IDM_FILE_EXPORT:
            // Export a database note to a file
            if (doc && hEditor && doc->type == DOC_TYPE_NOTE) {
                WCHAR path[MAX_PATH] = {0};
                if (Dialogs_SaveFile(hwnd, path, MAX_PATH, doc->noteTitle)) {
                    WCHAR* content = Editor_GetText(hEditor);
                    if (content) {
                        if (FileIO_WriteFile(path, content, ENCODING_UTF8)) {
                            MessageBoxW(hwnd, L"Note exported successfully!", APP_NAME, MB_ICONINFORMATION);
                            App_AddRecentFile(path);
                        } else {
                            MessageBoxW(hwnd, L"Failed to export note.", APP_NAME, MB_ICONERROR);
                        }
                        free(content);
                    }
                }
            }
            break;

        case IDM_FILE_CLOSE_TAB:
            if (g_app->tabCount > 0) {
                App_CloseTab(g_app->activeTab);
            }
            break;

        case IDM_FILE_PAGE_SETUP:
            ShowPageSetup(hwnd);
            break;

        case IDM_FILE_PRINT:
            FlushPageLayout();
            // Every document prints through the layout engine, plain text
            // included: what stood here before drew one page with DrawText and
            // stopped, so a long note printed its first page and lost the rest.
            if (hEditor) PrintRichDocument(hwnd, hEditor, doc);
            break;

        case IDM_FILE_EXPORT_PDF:
            // Only the rich view: exporting a plain text file to PDF is a
            // thing nobody asks for, and the layout engine has nothing to lay
            // out from Scintilla.
            FlushPageLayout();
            if (hEditor) ExportToPdf(hwnd, hEditor, doc);
            break;

        case IDM_FILE_PRINT_PREVIEW:
        case IDM_VIEW_PAGE_LAYOUT:
            // A preview of the laid-out pages and the place they can be edited
            // are the same thing once the engine exists. A text file gets the
            // same treatment: it is one paragraph per line, which is all
            // printing a note from Notepad has ever been.
            TogglePageLayout(hwnd);
            break;

        case IDM_FILE_EXIT:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            break;

        // Edit menu
        case IDM_EDIT_UNDO:
            if (hEditor) Editor_Undo(hEditor);
            break;

        case IDM_EDIT_REDO:
            if (hEditor) Editor_Redo(hEditor);
            break;

        case IDM_EDIT_CUT:
            if (hEditor) Editor_Cut(hEditor);
            break;

        case IDM_EDIT_COPY:
            if (hEditor) Editor_Copy(hEditor);
            break;

        case IDM_EDIT_PASTE:
            if (hEditor) Editor_Paste(hEditor);
            break;

        case IDM_EDIT_DELETE:
            if (hEditor) SendMessageW(hEditor, WM_CLEAR, 0, 0);
            break;

        case IDM_EDIT_FIND:
            Dialogs_Find(hwnd);
            break;

        case IDM_EDIT_FIND_NEXT:
            if (hEditor && g_app->findText[0]) {
                Editor_FindText(hEditor, g_app->findText, g_app->matchCase, g_app->wholeWord, TRUE);
            }
            break;

        case IDM_EDIT_FIND_PREV:
            if (hEditor && g_app->findText[0]) {
                Editor_FindText(hEditor, g_app->findText, g_app->matchCase, g_app->wholeWord, FALSE);
            }
            break;

        case IDM_EDIT_REPLACE:
            Dialogs_Replace(hwnd);
            break;

        case IDM_EDIT_FIND_IN_TABS:
            Dialogs_FindInTabs(hwnd);
            break;

        case IDM_EDIT_REPLACE_IN_TABS:
            Dialogs_ReplaceInTabs(hwnd);
            break;

        case IDM_EDIT_GOTO:
            {
                int line = 0;
                if (Dialogs_GoToLine(hwnd, &line) && hEditor) {
                    Editor_GotoLine(hEditor, line);
                }
            }
            break;

        case IDM_EDIT_SELECT_ALL:
            if (hEditor) {
                SendMessageW(hEditor, EM_SETSEL, 0, -1);
            }
            break;

        case IDM_EDIT_TIME_DATE:
            if (hEditor) {
                Editor_InsertTimeDate(hEditor);
            }
            break;

        // Format menu
        case IDM_FORMAT_WORDWRAP:
            g_app->wordWrap = !g_app->wordWrap;
            for (int i = 0; i < MAX_TABS; i++) {
                if (g_app->tabs[i] && g_app->tabs[i]->hEditor) {
                    Editor_SetWordWrap(g_app->tabs[i]->hEditor, g_app->wordWrap);
                }
            }
            break;

        case IDM_FORMAT_FONT:
            if (Dialogs_Font(hwnd, &g_app->editorFont)) {
                if (g_app->hEditorFont) DeleteObject(g_app->hEditorFont);
                g_app->hEditorFont = CreateFontIndirectW(&g_app->editorFont);
                for (int i = 0; i < MAX_TABS; i++) {
                    if (g_app->tabs[i] && g_app->tabs[i]->hEditor) {
                        Editor_SetFont(g_app->tabs[i]->hEditor, g_app->hEditorFont);
                    }
                }
            }
            break;

        case IDM_FORMAT_TABSIZE_2:
            g_app->tabSize = 2;
            for (int i = 0; i < MAX_TABS; i++) {
                if (g_app->tabs[i] && g_app->tabs[i]->hEditor) {
                    Editor_SetTabSize(g_app->tabs[i]->hEditor, g_app->tabSize);
                }
            }
            break;

        case IDM_FORMAT_TABSIZE_4:
            g_app->tabSize = 4;
            for (int i = 0; i < MAX_TABS; i++) {
                if (g_app->tabs[i] && g_app->tabs[i]->hEditor) {
                    Editor_SetTabSize(g_app->tabs[i]->hEditor, g_app->tabSize);
                }
            }
            break;

        case IDM_FORMAT_TABSIZE_8:
            g_app->tabSize = 8;
            for (int i = 0; i < MAX_TABS; i++) {
                if (g_app->tabs[i] && g_app->tabs[i]->hEditor) {
                    Editor_SetTabSize(g_app->tabs[i]->hEditor, g_app->tabSize);
                }
            }
            break;

        // View menu
        case IDM_VIEW_ZOOM_IN:
            g_app->zoomLevel = min(500, g_app->zoomLevel + 10);
            if (hEditor) Editor_SetZoom(hEditor, g_app->zoomLevel);
            break;

        case IDM_VIEW_ZOOM_OUT:
            g_app->zoomLevel = max(10, g_app->zoomLevel - 10);
            if (hEditor) Editor_SetZoom(hEditor, g_app->zoomLevel);
            break;

        case IDM_VIEW_ZOOM_RESET:
            g_app->zoomLevel = 100;
            if (hEditor) Editor_SetZoom(hEditor, g_app->zoomLevel);
            break;

        case IDM_VIEW_TOOLBAR:
            g_app->showFormatBar = !g_app->showFormatBar;
            {
                Tab* active = App_GetActiveTab();
                FormatBar_UpdateVisibility(hEditor, active && active->hPageView);
            }
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                MainWindow_OnSize(hwnd, SIZE_RESTORED, rc.right, rc.bottom);
            }
            break;

        case IDM_VIEW_STATUSBAR:
            g_app->showStatusBar = !g_app->showStatusBar;
            StatusBar_Show(g_app->showStatusBar);
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                MainWindow_OnSize(hwnd, SIZE_RESTORED, rc.right, rc.bottom);
            }
            break;

        case IDM_VIEW_NOTES_BROWSER:
            {
                int noteId = 0;
                if (Dialogs_NotesBrowser(hwnd, &noteId) && noteId > 0) {
                    SendMessageW(hwnd, WM_COMMAND, IDM_FILE_OPEN_NOTE, 0);
                }
            }
            break;

        case IDM_VIEW_ALWAYS_ON_TOP:
            g_app->alwaysOnTop = !g_app->alwaysOnTop;
            SetWindowPos(hwnd, g_app->alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            break;

        case IDM_VIEW_PREVIEW:
            {
                Tab* tab = App_GetActiveTab();
                if (tab && tab->hEditor) {
                    Dialogs_MarkdownPreview(hwnd, tab->hEditor);
                }
            }
            break;

        case IDM_SETTINGS_ANSWERS:
            Dialogs_RememberedAnswers(hwnd);
            break;

        // Settings menu
        case IDM_SETTINGS_DEFAULTS:
            Dialogs_Defaults(hwnd);
            break;

        case IDM_FILE_SIGN_CERT: {
            // The other kind of signature: not a picture of one, but a claim
            // that these bytes have not changed since the holder of a private
            // key saw them.
            Tab* tab = App_GetActiveTab();

            WCHAR pdfPath[MAX_PATH];
            if (!tab || !tab->hPdfView || !PdfView_Path(tab->hPdfView, pdfPath, MAX_PATH)) {
                MessageBoxW(hwnd,
                    L"Open a PDF first.\n\n"
                    L"This signs it with a certificate you hold the private key for.",
                    APP_NAME, MB_ICONINFORMATION);
                break;
            }

            PdfCertificate certificate = PdfSign_ChooseCertificate(hwnd);
            if (!certificate) break;

            WCHAR who[256] = L"";
            PdfSign_SubjectName(certificate, who, 256);

            const WCHAR* why = NULL;
            PdfForm* form = PdfForm_Open(pdfPath, &why);
            if (!form) {
                PdfSign_ReleaseCertificate(certificate);
                MessageBoxW(hwnd, why ? why : L"This PDF could not be opened for signing.",
                            APP_NAME, MB_ICONINFORMATION);
                break;
            }

            WCHAR saveTo[MAX_PATH] = {0};
            if (!Dialogs_SaveFile(hwnd, saveTo, MAX_PATH, L"signed.pdf")) {
                PdfForm_Close(form);
                PdfSign_ReleaseCertificate(certificate);
                break;
            }
            if (!wcsrchr(saveTo, L'.')) wcscat_s(saveTo, MAX_PATH, L".pdf");

            PdfForm_SignWithCertificate(form, certificate, who, L"Signed with opennote");
            BOOL ok = PdfForm_Save(form, saveTo);

            PdfForm_Close(form);
            PdfSign_ReleaseCertificate(certificate);

            if (!ok) {
                MessageBoxW(hwnd,
                    L"The signature could not be written.\n\n"
                    L"The certificate may not have a private key this program can use.",
                    APP_NAME, MB_ICONWARNING);
                break;
            }

            Document* signedDoc = Document_CreateFromFile(saveTo);
            if (signedDoc) MainWindow_OpenDocument(signedDoc);

            WCHAR message[320];
            swprintf_s(message, 320, L"Signed by %s", who[0] ? who : L"the chosen certificate");
            StatusBar_SetMessage(message);
            break;
        }

        case IDM_FILE_SIGN_PDF: {
            // A signature is a picture of one. If there is a picture kept
            // already it is offered; if there is not, the pad opens and one is
            // drawn -- which is the difference between "sign this PDF" meaning
            // "find a scanner" and meaning "sign it".
            Tab* tab = App_GetActiveTab();
            if (!tab || !tab->hPdfView) {
                MessageBoxW(hwnd,
                    L"Open a PDF first.\n\n"
                    L"This puts a picture of a signature on one of its pages.",
                    APP_NAME, MB_ICONINFORMATION);
                break;
            }

            size_t len = 0;
            BYTE* png = Dialogs_ChooseSignature(hwnd, &len);
            if (!png) break;

            PdfView_BeginStamp(tab->hPdfView, png, len);
            free(png);
            break;
        }


        case IDM_FILE_FILL_FORM: {
            // The form belongs to the file rather than to anything drawn, so
            // this works on the PDF the active tab is showing.
            Tab* tab = App_GetActiveTab();

            WCHAR pdfPath[MAX_PATH];
            if (!tab || !tab->hPdfView || !PdfView_Path(tab->hPdfView, pdfPath, MAX_PATH)) {
                MessageBoxW(hwnd,
                    L"Open a PDF first.\n\n"
                    L"File > Open reads one, and this fills in the form it carries.",
                    APP_NAME, MB_ICONINFORMATION);
                break;
            }

            WCHAR savedTo[MAX_PATH] = {0};
            if (!Dialogs_PdfForm(hwnd, pdfPath, savedTo, MAX_PATH)) break;
            if (!savedTo[0]) break;

            // Show what was written: a filled form nobody can see is a filled
            // form nobody trusts.
            Document* filled = Document_CreateFromFile(savedTo);
            if (filled) MainWindow_OpenDocument(filled);
            break;
        }

        // Insert menu
        case IDM_INSERT_PAGE_NUMBERS:
            InsertField(hwnd, INSERT_PAGE_NUMBERS);
            break;

        case IDM_INSERT_TOC:
            InsertField(hwnd, INSERT_TOC);
            break;

        case IDM_INSERT_DATE_FIELD:
            InsertField(hwnd, INSERT_DATE);
            break;

        // Review menu
        case IDM_REVIEW_NEW_COMMENT:
            NewComment(hwnd);
            break;

        case IDM_REVIEW_COMMENTS:
            ShowComments(hwnd);
            break;

        case IDM_REVIEW_ACCEPT_ALL:
            ResolveRevisions(hwnd, TRUE);
            break;

        case IDM_REVIEW_REJECT_ALL:
            ResolveRevisions(hwnd, FALSE);
            break;

        // Help menu
        //
        // The documentation lives in the repository rather than in a help file
        // nobody would ship: one executable stays one executable.
        case IDM_HELP_GUIDE:
            ShellExecuteW(hwnd, L"open",
                          L"https://github.com/sp00nznet/opennote/blob/main/docs/getting-started.md",
                          NULL, NULL, SW_SHOWNORMAL);
            break;

        case IDM_HELP_CONTROLS:
            ShellExecuteW(hwnd, L"open",
                          L"https://github.com/sp00nznet/opennote/blob/main/docs/controls.md",
                          NULL, NULL, SW_SHOWNORMAL);
            break;

        case IDM_HELP_ABOUT:
            Dialogs_About(hwnd);
            break;

        default:
            // Handle recent files (IDs 6000-6009)
            if (id >= 6000 && id < 6000 + MAX_RECENT) {
                int index = id - 6000;
                if (index < g_app->recentCount && g_app->recentFiles[index][0]) {
                    WCHAR* path = g_app->recentFiles[index];
                    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
                        Document* newDoc = Document_CreateFromFile(path);
                        if (newDoc) {
                            MainWindow_OpenDocument(newDoc);
                            App_AddRecentFile(path);
                        }
                    } else {
                        MessageBoxW(hwnd, L"File not found.", APP_NAME, MB_ICONWARNING);
                    }
                }
            }
            break;
    }
}

// Update window title
void MainWindow_UpdateTitle(void) {
    WCHAR title[MAX_PATH + 64];
    Document* doc = App_GetActiveDocument();

    if (doc) {
        if (doc->modified) {
            swprintf_s(title, sizeof(title)/sizeof(WCHAR), L"*%s - %s", Document_GetTitle(doc), APP_NAME);
        } else {
            swprintf_s(title, sizeof(title)/sizeof(WCHAR), L"%s - %s", Document_GetTitle(doc), APP_NAME);
        }
    } else {
        wcscpy_s(title, sizeof(title)/sizeof(WCHAR), APP_NAME);
    }

    SetWindowTextW(g_app->hMainWindow, title);
}

// Update menu state
void MainWindow_UpdateMenuState(void) {
    // Menu state is updated in WM_INITMENUPOPUP handlers in menubar.c
}


// Show editor context menu
void MainWindow_ShowEditorContextMenu(HWND hwnd, int x, int y) {
    Tab* tab = App_GetActiveTab();
    HWND hEditor = tab ? tab->hEditor : NULL;
    if (!hEditor) return;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // Check selection state
    int start, end;
    Editor_GetSelection(hEditor, &start, &end);
    BOOL hasSelection = (start != end);
    BOOL canPaste = IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);

    // Get position under cursor for spell check (x,y are screen coords, function handles conversion)
    int clickPos = Editor_GetPositionFromPoint(hEditor, x, y);

    // Check if there's a spell error at this position (indicator 9)
    BOOL hasSpellError = Editor_HasIndicatorAt(hEditor, clickPos, EDITOR_INDICATOR_SPELL);

    // Spell suggestions at top if there's an error
    WCHAR* misspelledWord = NULL;
    WCHAR** suggestions = NULL;
    int suggestionCount = 0;
    int spellWordStart = 0, spellWordEnd = 0;

    if (hasSpellError) {
        misspelledWord = Editor_GetWordAt(hEditor, clickPos, &spellWordStart, &spellWordEnd);
        if (misspelledWord) {
            suggestions = Editor_GetSpellSuggestions(misspelledWord, &suggestionCount);

            if (suggestionCount > 0) {
                for (int i = 0; i < suggestionCount && i < 5; i++) {
                    AppendMenuW(hMenu, MF_STRING, IDM_SPELL_SUGGESTION_BASE + i, suggestions[i]);
                }
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            }

            // Add to dictionary / Ignore options
            WCHAR addText[MAX_TITLE_LEN];
            swprintf_s(addText, MAX_TITLE_LEN, L"Add \"%s\" to Dictionary", misspelledWord);
            AppendMenuW(hMenu, MF_STRING, IDM_SPELL_ADD_DICT, addText);
            AppendMenuW(hMenu, MF_STRING, IDM_SPELL_IGNORE, L"Ignore");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        }
    }

    // Standard edit items
    AppendMenuW(hMenu, MF_STRING | (Editor_CanUndo(hEditor) ? 0 : MF_GRAYED), IDM_EDIT_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(hMenu, MF_STRING | (Editor_CanRedo(hEditor) ? 0 : MF_GRAYED), IDM_EDIT_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), IDM_EDIT_CUT, L"Cu&t\tCtrl+X");
    AppendMenuW(hMenu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), IDM_EDIT_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(hMenu, MF_STRING | (canPaste ? 0 : MF_GRAYED), IDM_EDIT_PASTE, L"&Paste\tCtrl+V");
    AppendMenuW(hMenu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), IDM_EDIT_DELETE, L"&Delete\tDel");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EDIT_SELECT_ALL, L"Select &All\tCtrl+A");

    // Spell check document option
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_SPELL_CHECK_DOC, L"Check &Spelling");

    // Link submenu - only if there's a selection
    if (hasSelection) {
        HMENU hLinkMenu = CreatePopupMenu();
        int linkItems = 0;

        // Add tabs as link targets
        for (int i = 0; i < MAX_TABS; i++) {
            if (i != g_app->activeTab && g_app->tabs[i] && g_app->tabs[i]->document) {
                WCHAR title[MAX_TITLE_LEN + 8];
                swprintf_s(title, sizeof(title)/sizeof(WCHAR), L"&%d. %s", linkItems + 1,
                          Document_GetTitle(g_app->tabs[i]->document));
                AppendMenuW(hLinkMenu, MF_STRING, IDM_LINK_TAB_BASE + i, title);
                linkItems++;
            }
        }

        // Add URL option
        if (linkItems > 0) {
            AppendMenuW(hLinkMenu, MF_SEPARATOR, 0, NULL);
        }
        AppendMenuW(hLinkMenu, MF_STRING, IDM_LINK_URL, L"&URL...");

        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hLinkMenu, L"&Link to");

        // Send to Shell submenu
        HMENU hShellMenu = CreatePopupMenu();
        AppendMenuW(hShellMenu, MF_STRING, IDM_SHELL_CMD, L"Run in &CMD");
        AppendMenuW(hShellMenu, MF_STRING, IDM_SHELL_CMD_ADMIN, L"Run in CMD (Admin)");
        AppendMenuW(hShellMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hShellMenu, MF_STRING, IDM_SHELL_POWERSHELL, L"Run in &PowerShell");
        AppendMenuW(hShellMenu, MF_STRING, IDM_SHELL_PS_ADMIN, L"Run in PowerShell (Admin)");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hShellMenu, L"&Send to Shell");
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);

    if (cmd) {
        // Handle spell suggestion commands
        if (cmd >= IDM_SPELL_SUGGESTION_BASE && cmd < IDM_SPELL_SUGGESTION_BASE + 10) {
            int suggIdx = cmd - IDM_SPELL_SUGGESTION_BASE;
            if (suggIdx < suggestionCount && suggestions[suggIdx]) {
                // Replace the misspelled word with suggestion
                Editor_ReplaceRange(hEditor, spellWordStart, spellWordEnd, suggestions[suggIdx]);

                // Re-check spelling
                Editor_CheckSpelling(hEditor);
            }
        }
        // Handle add to dictionary
        else if (cmd == IDM_SPELL_ADD_DICT) {
            if (misspelledWord) {
                Editor_AddWordToDictionary(misspelledWord);
                Editor_CheckSpelling(hEditor);
            }
        }
        // Handle ignore
        else if (cmd == IDM_SPELL_IGNORE) {
            // Clear indicator for this word only
            Editor_ClearSpellIndicatorRange(hEditor, spellWordStart, spellWordEnd);
        }
        // Handle spell check document
        else if (cmd == IDM_SPELL_CHECK_DOC) {
            Editor_CheckSpelling(hEditor);
        }
        // Handle link to URL
        else if (cmd == IDM_LINK_URL) {
            WCHAR* selected = Editor_GetSelectedText(hEditor);
            if (selected && tab->document) {
                // Prompt for URL
                WCHAR url[1024] = L"https://";
                if (Dialogs_InputBox(hwnd, L"Link to URL", L"Enter URL:", url, 1024)) {
                    Document* srcDoc = tab->document;

                    // Create the link in database (target_type = 2 for URL)
                    int linkId = Links_CreateURL(
                        srcDoc->type,
                        srcDoc->type == DOC_TYPE_FILE ? srcDoc->filePath : NULL,
                        srcDoc->type == DOC_TYPE_NOTE ? srcDoc->noteId : 0,
                        selected,
                        start, end,
                        url
                    );

                    if (linkId > 0) {
                        Editor_AddLinkIndicator(hEditor, start, end);
                        WCHAR msg[512];
                        swprintf_s(msg, 512, L"Link created: \"%s\" now links to:\n%s",
                                  selected, url);
                        MessageBoxW(hwnd, msg, L"Link Created", MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(hwnd, L"Failed to create link.", APP_NAME, MB_ICONERROR);
                    }
                }
                free(selected);
            }
        }
        // Handle link to tab commands
        else if (cmd >= IDM_LINK_TAB_BASE && cmd < IDM_LINK_TAB_BASE + MAX_TABS) {
            int targetTab = cmd - IDM_LINK_TAB_BASE;
            if (g_app->tabs[targetTab] && g_app->tabs[targetTab]->document && tab->document) {
                WCHAR* selected = Editor_GetSelectedText(hEditor);
                if (selected) {
                    Document* srcDoc = tab->document;
                    Document* tgtDoc = g_app->tabs[targetTab]->document;

                    // Create the link in database
                    int linkId = Links_Create(
                        srcDoc->type,
                        srcDoc->type == DOC_TYPE_FILE ? srcDoc->filePath : NULL,
                        srcDoc->type == DOC_TYPE_NOTE ? srcDoc->noteId : 0,
                        selected,
                        start, end,
                        tgtDoc->type,
                        tgtDoc->type == DOC_TYPE_FILE ? tgtDoc->filePath : NULL,
                        tgtDoc->type == DOC_TYPE_NOTE ? tgtDoc->noteId : 0
                    );

                    if (linkId > 0) {
                        // Add visual indicator
                        Editor_AddLinkIndicator(hEditor, start, end);

                        WCHAR msg[512];
                        swprintf_s(msg, 512, L"Link created: \"%s\" now links to \"%s\"\n\nClick on the linked text to navigate.",
                                  selected, Document_GetTitle(tgtDoc));
                        MessageBoxW(hwnd, msg, L"Link Created", MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(hwnd, L"Failed to create link.", APP_NAME, MB_ICONERROR);
                    }

                    free(selected);
                }
            }
        }
        // Handle shell commands
        else if (cmd == IDM_SHELL_CMD || cmd == IDM_SHELL_POWERSHELL ||
                 cmd == IDM_SHELL_CMD_ADMIN || cmd == IDM_SHELL_PS_ADMIN) {
            WCHAR* selected = Editor_GetSelectedText(hEditor);
            if (selected) {
                // Trim whitespace
                WCHAR* trimmed = selected;
                while (*trimmed == L' ' || *trimmed == L'\t' || *trimmed == L'\r' || *trimmed == L'\n') trimmed++;
                WCHAR* endTrim = trimmed + wcslen(trimmed) - 1;
                while (endTrim > trimmed && (*endTrim == L' ' || *endTrim == L'\t' || *endTrim == L'\r' || *endTrim == L'\n')) {
                    *endTrim = L'\0';
                    endTrim--;
                }

                if (*trimmed) {
                    BOOL isAdmin = (cmd == IDM_SHELL_CMD_ADMIN || cmd == IDM_SHELL_PS_ADMIN);
                    BOOL isPowerShell = (cmd == IDM_SHELL_POWERSHELL || cmd == IDM_SHELL_PS_ADMIN);

                    if (isAdmin) {
                        // Use ShellExecuteEx with runas for admin elevation
                        WCHAR params[MAX_PATH * 2];
                        if (isPowerShell) {
                            swprintf_s(params, MAX_PATH * 2, L"-NoExit -Command \"& '%s'\"", trimmed);
                        } else {
                            swprintf_s(params, MAX_PATH * 2, L"/c \"%s\" & pause", trimmed);
                        }

                        SHELLEXECUTEINFOW sei = {
                            .cbSize = sizeof(sei),
                            .fMask = SEE_MASK_NOCLOSEPROCESS,
                            .hwnd = hwnd,
                            .lpVerb = L"runas",
                            .lpFile = isPowerShell ? L"powershell.exe" : L"cmd.exe",
                            .lpParameters = params,
                            .nShow = SW_SHOWNORMAL
                        };

                        if (!ShellExecuteExW(&sei)) {
                            DWORD err = GetLastError();
                            if (err != ERROR_CANCELLED) {
                                MessageBoxW(hwnd, L"Failed to execute command as administrator.", APP_NAME, MB_ICONERROR);
                            }
                        } else if (sei.hProcess) {
                            CloseHandle(sei.hProcess);
                        }
                    } else {
                        // Regular execution
                        WCHAR cmdLine[MAX_PATH * 2];
                        if (isPowerShell) {
                            swprintf_s(cmdLine, MAX_PATH * 2, L"powershell.exe -NoExit -Command \"& '%s'\"", trimmed);
                        } else {
                            swprintf_s(cmdLine, MAX_PATH * 2, L"cmd.exe /c \"%s\" & pause", trimmed);
                        }

                        STARTUPINFOW si = { .cb = sizeof(si) };
                        PROCESS_INFORMATION pi = {0};

                        if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                           CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                        } else {
                            MessageBoxW(hwnd, L"Failed to execute command.", APP_NAME, MB_ICONERROR);
                        }
                    }
                }
                free(selected);
            }
        }
        else {
            SendMessageW(hwnd, WM_COMMAND, cmd, 0);
        }
    }

    // Cleanup
    if (suggestions) {
        Editor_FreeSpellSuggestions(suggestions, suggestionCount);
    }
    free(misspelledWord);
    DestroyMenu(hMenu);
}
