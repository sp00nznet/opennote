#include "supernote.h"
#include "ui/toolbar.h"
#include "res/resource.h"

// Initialize application
BOOL App_Initialize(HINSTANCE hInstance) {
    // Allocate state
    g_app = (AppState*)calloc(1, sizeof(AppState));
    if (!g_app) return FALSE;

    g_app->hInstance = hInstance;
    g_app->activeTab = -1;

    // Default settings
    g_app->wordWrap = FALSE;
    g_app->showStatusBar = TRUE;
    g_app->showFormatBar = TRUE;
    g_app->alwaysOnTop = FALSE;
    g_app->autoSaveSession = FALSE;
    g_app->autoRestoreSession = FALSE;
    g_app->tabSize = 4;
    g_app->zoomLevel = 100;
    g_app->wrapAround = TRUE;

    // Default font
    g_app->editorFont.lfHeight = -12;
    g_app->editorFont.lfWeight = FW_NORMAL;
    g_app->editorFont.lfCharSet = DEFAULT_CHARSET;
    g_app->editorFont.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(g_app->editorFont.lfFaceName, LF_FACESIZE, L"Consolas");
    g_app->hEditorFont = CreateFontIndirectW(&g_app->editorFont);

    // Get database path in AppData
    WCHAR appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        swprintf_s(g_app->dbPath, MAX_PATH, L"%s\\OpenNote", appData);
        CreateDirectoryW(g_app->dbPath, NULL);
        wcscat_s(g_app->dbPath, MAX_PATH, L"\\opennote.db");
    } else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) {
        swprintf_s(g_app->dbPath, MAX_PATH, L"%s\\OpenNote", appData);
        CreateDirectoryW(g_app->dbPath, NULL);
        wcscat_s(g_app->dbPath, MAX_PATH, L"\\opennote.db");
    } else {
        // Last resort: the temp directory. The old fallback was a relative
        // path, which means the working directory -- for an installed copy
        // that is Program Files, where the write fails or is quietly
        // redirected into a virtual store nothing else looks in.
        WCHAR temp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, temp)) {
            swprintf_s(g_app->dbPath, MAX_PATH, L"%sopennote.db", temp);
        } else {
            wcscpy_s(g_app->dbPath, MAX_PATH, L"opennote.db");
        }
    }

    // Open database
    if (!Database_Open(g_app->dbPath)) {
        MessageBoxW(NULL, L"Failed to open database", APP_NAME, MB_ICONWARNING);
        // Continue without database - file operations will still work
    } else {
        Database_Initialize();
    }

    // Load settings from database
    App_LoadSettings();

    // Register window class
    if (!MainWindow_RegisterClass(hInstance)) {
        return FALSE;
    }

    // Create main window
    g_app->hMainWindow = MainWindow_Create(hInstance);
    if (!g_app->hMainWindow) {
        return FALSE;
    }

    // Load accelerators
    g_app->hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDA_ACCEL));

    // Auto-restore session or create initial tab
    BOOL sessionRestored = FALSE;
    if (g_app->autoRestoreSession && Database_IsOpen()) {
        // Check if there's a saved session
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(Database_GetHandle(), "SELECT COUNT(*) FROM session_tabs", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0) {
                sqlite3_finalize(stmt);
                App_RestoreSession();
                sessionRestored = (g_app->tabCount > 0);
            } else {
                sqlite3_finalize(stmt);
            }
        }
    }

    if (!sessionRestored) {
        App_CreateTab(L"Untitled");
    }

    // Initialize spell checker (disabled for now - may cause issues)
    // Editor_InitSpellCheck();

    return TRUE;
}

// Shutdown application
void App_Shutdown(void) {
    if (!g_app) return;

    // Save session if auto-save is enabled
    if (g_app->autoSaveSession) {
        App_SaveSession();
    }

    // Save settings
    App_SaveSettings();

    // Close all tabs
    for (int i = 0; i < g_app->tabCount; i++) {
        if (g_app->tabs[i]) {
            if (g_app->tabs[i]->document) {
                Document_Destroy(g_app->tabs[i]->document);
            }
            if (g_app->tabs[i]->hEditor) {
                DestroyWindow(g_app->tabs[i]->hEditor);
            }
            free(g_app->tabs[i]);
        }
    }

    // Close find dialog
    if (g_app->hFindDialog) {
        DestroyWindow(g_app->hFindDialog);
    }

    // Shutdown spell checker
    Editor_ShutdownSpellCheck();

    // Close database
    Database_Close();

    // Delete font
    if (g_app->hEditorFont) {
        DeleteObject(g_app->hEditorFont);
    }

    free(g_app);
    g_app = NULL;
}

// Run message loop
BOOL App_Run(void) {
    MSG msg;

    while (GetMessageW(&msg, NULL, 0, 0)) {
        // Handle Find/Replace dialog messages
        if (g_app->hFindDialog && IsDialogMessageW(g_app->hFindDialog, &msg)) {
            continue;
        }

        // Handle Ctrl+Tab and Ctrl+Shift+Tab for tab navigation
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
            SHORT ctrlState = GetKeyState(VK_CONTROL);
            if (ctrlState & 0x8000) {
                SHORT shiftState = GetKeyState(VK_SHIFT);
                int newIndex;
                if (shiftState & 0x8000) {
                    // Ctrl+Shift+Tab - previous tab
                    newIndex = g_app->activeTab - 1;
                    if (newIndex < 0) newIndex = g_app->tabCount - 1;
                } else {
                    // Ctrl+Tab - next tab
                    newIndex = g_app->activeTab + 1;
                    if (newIndex >= g_app->tabCount) newIndex = 0;
                }
                if (newIndex >= 0 && newIndex < g_app->tabCount) {
                    App_SetActiveTab(newIndex);
                }
                continue;
            }
        }

        // Handle accelerators
        if (!TranslateAcceleratorW(g_app->hMainWindow, g_app->hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return TRUE;
}

// Load settings from database
void App_LoadSettings(void) {
    if (!Database_IsOpen()) return;

    sqlite3_stmt* stmt;
    const char* sql = "SELECT key, value FROM settings";

    if (sqlite3_prepare_v2(Database_GetHandle(), sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* key = (const char*)sqlite3_column_text(stmt, 0);
            const char* value = (const char*)sqlite3_column_text(stmt, 1);

            if (strcmp(key, "word_wrap") == 0) {
                g_app->wordWrap = atoi(value);
            } else if (strcmp(key, "show_statusbar") == 0) {
                g_app->showStatusBar = atoi(value);
            } else if (strcmp(key, "show_formatbar") == 0) {
                g_app->showFormatBar = atoi(value);
            } else if (strcmp(key, "always_on_top") == 0) {
                g_app->alwaysOnTop = atoi(value);
            } else if (strcmp(key, "auto_save_on_exit") == 0) {
                g_app->autoSaveSession = atoi(value);
            } else if (strcmp(key, "auto_restore_session") == 0) {
                g_app->autoRestoreSession = atoi(value);
            } else if (strcmp(key, "minimize_to_tray") == 0) {
                g_app->minimizeToTray = atoi(value);
            } else if (strcmp(key, "tab_size") == 0) {
                g_app->tabSize = atoi(value);
            } else if (strcmp(key, "zoom_level") == 0) {
                g_app->zoomLevel = atoi(value);
            } else if (strcmp(key, "font_name") == 0) {
                MultiByteToWideChar(CP_UTF8, 0, value, -1, g_app->editorFont.lfFaceName, LF_FACESIZE);
            } else if (strcmp(key, "font_size") == 0) {
                g_app->editorFont.lfHeight = -atoi(value);
            } else if (strcmp(key, "theme_index") == 0) {
                g_app->themeIndex = atoi(value);
            } else if (strncmp(key, "recent_", 7) == 0) {
                int idx = atoi(key + 7);
                if (idx >= 0 && idx < MAX_RECENT && value && value[0]) {
                    MultiByteToWideChar(CP_UTF8, 0, value, -1, g_app->recentFiles[idx], MAX_PATH);
                    if (idx >= g_app->recentCount) {
                        g_app->recentCount = idx + 1;
                    }
                }
            }
        }
        sqlite3_finalize(stmt);

        // Recreate font with loaded settings
        if (g_app->hEditorFont) {
            DeleteObject(g_app->hEditorFont);
        }
        g_app->hEditorFont = CreateFontIndirectW(&g_app->editorFont);
    }
}

// Save settings to database
void App_SaveSettings(void) {
    if (!Database_IsOpen()) return;

    Database_BeginTransaction();

    char sql[256];
    char value[256];

    // Save each setting
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('word_wrap', '%d')", g_app->wordWrap);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('show_statusbar', '%d')", g_app->showStatusBar);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('show_formatbar', '%d')", g_app->showFormatBar);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('always_on_top', '%d')", g_app->alwaysOnTop);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('auto_save_on_exit', '%d')", g_app->autoSaveSession);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('auto_restore_session', '%d')", g_app->autoRestoreSession);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('minimize_to_tray', '%d')", g_app->minimizeToTray);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('tab_size', '%d')", g_app->tabSize);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('zoom_level', '%d')", g_app->zoomLevel);
    Database_Execute(sql);

    WideCharToMultiByte(CP_UTF8, 0, g_app->editorFont.lfFaceName, -1, value, sizeof(value), NULL, NULL);
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('font_name', '%s')", value);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('font_size', '%d')", -g_app->editorFont.lfHeight);
    Database_Execute(sql);

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('theme_index', '%d')", g_app->themeIndex);
    Database_Execute(sql);

    // Save recent files
    for (int i = 0; i < MAX_RECENT; i++) {
        char key[32];
        snprintf(key, sizeof(key), "recent_%d", i);
        if (i < g_app->recentCount && g_app->recentFiles[i][0]) {
            WideCharToMultiByte(CP_UTF8, 0, g_app->recentFiles[i], -1, value, sizeof(value), NULL, NULL);
            snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('%s', '%s')", key, value);
        } else {
            snprintf(sql, sizeof(sql), "DELETE FROM settings WHERE key = '%s'", key);
        }
        Database_Execute(sql);
    }

    Database_CommitTransaction();
}

// Create a new tab
int App_CreateTab(const WCHAR* title) {
    return App_CreateTabEx(title, FORMAT_PLAIN);
}

int App_CreateTabEx(const WCHAR* title, DocumentFormat format) {
    if (g_app->tabCount >= MAX_TABS) return -1;

    // Allocate tab
    Tab* tab = (Tab*)calloc(1, sizeof(Tab));
    if (!tab) return -1;

    // Create document
    tab->document = Document_Create();
    if (!tab->document) {
        free(tab);
        return -1;
    }

    tab->document->format = format;

    // Auto-create note in database for persistence. A rich document is not
    // backed by a note: the notes table stores text, and storing RTF markup in
    // it would put braces and control words into full-text search results.
    if (Database_IsOpen() && !FORMAT_IS_RICH(format)) {
        const WCHAR* noteTitle = title ? title : L"Untitled";
        int noteId = Notes_Create(noteTitle, L"");
        if (noteId > 0) {
            tab->document->noteId = noteId;
            tab->document->type = DOC_TYPE_NOTE;
            wcscpy_s(tab->document->noteTitle, MAX_TITLE_LEN, noteTitle);
        }
    }

    // Add to tab control
    int index = TabControl_AddTab(title ? title : L"Untitled");
    if (index < 0) {
        Document_Destroy(tab->document);
        free(tab);
        return -1;
    }

    // Create the view the format calls for.
    tab->hEditor = FORMAT_IS_RICH(format)
        ? Editor_CreateRich(g_app->hMainWindow)
        : Editor_Create(g_app->hMainWindow);
    if (!tab->hEditor) {
        TabControl_RemoveTab(index);
        Document_Destroy(tab->document);
        free(tab);
        return -1;
    }

    // Apply user settings. The editor font, word wrap and tab size are plain
    // text concerns -- a rich document carries its own fonts, and forcing the
    // code font over the whole thing would flatten every document it opened.
    if (!FORMAT_IS_RICH(format)) {
        Editor_SetFont(tab->hEditor, g_app->hEditorFont);
        Editor_SetWordWrap(tab->hEditor, g_app->wordWrap);
        Editor_SetTabSize(tab->hEditor, g_app->tabSize);
    }
    Editor_SetZoom(tab->hEditor, g_app->zoomLevel);

    tab->index = index;
    g_app->tabs[index] = tab;
    g_app->tabCount++;

    // Set as active
    App_SetActiveTab(index);

    // Update new tab button position
    MainWindow_UpdateNewTabButton();

    return index;
}

// Close a tab
void App_CloseTab(int index) {
    if (index < 0 || index >= MAX_TABS || !g_app->tabs[index]) return;

    Tab* tab = g_app->tabs[index];

    // Anything edited in the laid-out view belongs to the document before it
    // is asked whether to save.
    if (tab->hPageView) PageView_Apply(tab->hPageView);

    // Check for unsaved changes
    if (tab->document && tab->document->modified) {
        int result = Dialogs_SaveChanges(g_app->hMainWindow, Document_GetTitle(tab->document));
        if (result == IDCANCEL) return;
        if (result == IDYES) {
            if (!Document_Save(tab->document, tab->hEditor)) {
                return;  // Save failed, don't close
            }
        }
    }

    // Hide both views
    ShowWindow(tab->hEditor, SW_HIDE);
    if (tab->hPageView) ShowWindow(tab->hPageView, SW_HIDE);
    if (tab->hPdfView)  ShowWindow(tab->hPdfView, SW_HIDE);

    // Cleanup
    if (tab->document) Document_Destroy(tab->document);
    if (tab->hPdfView) DestroyWindow(tab->hPdfView);
    if (tab->hPageView) DestroyWindow(tab->hPageView);
    if (tab->hEditor) DestroyWindow(tab->hEditor);
    free(tab);
    g_app->tabs[index] = NULL;

    // Remove from tab control
    TabControl_RemoveTab(index);
    g_app->tabCount--;

    // Adjust indices
    for (int i = index; i < MAX_TABS - 1; i++) {
        g_app->tabs[i] = g_app->tabs[i + 1];
        if (g_app->tabs[i]) {
            g_app->tabs[i]->index = i;
        }
    }
    g_app->tabs[MAX_TABS - 1] = NULL;

    // If no tabs left, create a new one
    if (g_app->tabCount == 0) {
        App_CreateTab(L"Untitled");
    } else {
        // Select appropriate tab
        int newIndex = (index >= g_app->tabCount) ? g_app->tabCount - 1 : index;
        App_SetActiveTab(newIndex);
    }

    // Update new tab button position
    MainWindow_UpdateNewTabButton();
}

// Set active tab
void App_SetActiveTab(int index) {
    if (index < 0 || index >= MAX_TABS || !g_app->tabs[index]) return;

    // Hide whichever view the previous tab was showing.
    if (g_app->activeTab >= 0 && g_app->activeTab < MAX_TABS && g_app->tabs[g_app->activeTab]) {
        Tab* previous = g_app->tabs[g_app->activeTab];
        ShowWindow(previous->hEditor, SW_HIDE);
        if (previous->hPageView) ShowWindow(previous->hPageView, SW_HIDE);
        if (previous->hPdfView)  ShowWindow(previous->hPdfView, SW_HIDE);
    }

    g_app->activeTab = index;
    TabControl_SetActiveTab(index);

    // Show this tab's, which may be the laid-out one.
    Tab* tab = g_app->tabs[index];
    if (tab && tab->hPdfView) {
        ShowWindow(tab->hPdfView, SW_SHOW);
        SetFocus(tab->hPdfView);

        RECT rc;
        GetClientRect(g_app->hMainWindow, &rc);
        SendMessageW(g_app->hMainWindow, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    } else if (tab && tab->hPageView) {
        ShowWindow(tab->hPageView, SW_SHOW);
        SetFocus(tab->hPageView);

        RECT rc;
        GetClientRect(g_app->hMainWindow, &rc);
        SendMessageW(g_app->hMainWindow, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    } else if (tab && tab->hEditor) {
        ShowWindow(tab->hEditor, SW_SHOW);
        SetFocus(tab->hEditor);

        // The page count belongs to the tab that was showing pages.
        StatusBar_SetMessage(L"");

        // Trigger resize to position editor correctly
        RECT rc;
        GetClientRect(g_app->hMainWindow, &rc);
        SendMessageW(g_app->hMainWindow, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    }

    // Update UI. The formatting toolbar shows or hides with the view kind via
    // WM_SIZE above; this makes its buttons reflect the new caret position.
    if (tab && tab->hEditor) FormatBar_SyncFromEditor(tab->hEditor);
    MainWindow_UpdateTitle();
    MainWindow_UpdateMenuState();

    // Update status bar
    if (tab && tab->hEditor) {
        int line, col;
        Editor_GetCursorPos(tab->hEditor, &line, &col);
        StatusBar_UpdatePosition(line, col);
        StatusBar_UpdateModified(tab->document ? tab->document->modified : FALSE);
        if (tab->document) {
            StatusBar_UpdateEncoding(tab->document->encoding);
        }
    }
}

// Get active tab
Tab* App_GetActiveTab(void) {
    if (g_app->activeTab >= 0 && g_app->activeTab < MAX_TABS) {
        return g_app->tabs[g_app->activeTab];
    }
    return NULL;
}

// Get active document
Document* App_GetActiveDocument(void) {
    Tab* tab = App_GetActiveTab();
    return tab ? tab->document : NULL;
}

// Add recent file
void App_AddRecentFile(const WCHAR* path) {
    if (!path || !path[0]) return;

    // Check if already in list
    for (int i = 0; i < g_app->recentCount; i++) {
        if (_wcsicmp(g_app->recentFiles[i], path) == 0) {
            // Move to top
            WCHAR temp[MAX_PATH];
            wcscpy_s(temp, MAX_PATH, g_app->recentFiles[i]);
            for (int j = i; j > 0; j--) {
                wcscpy_s(g_app->recentFiles[j], MAX_PATH, g_app->recentFiles[j - 1]);
            }
            wcscpy_s(g_app->recentFiles[0], MAX_PATH, temp);
            return;
        }
    }

    // Add to top
    for (int i = g_app->recentCount; i > 0; i--) {
        if (i < MAX_RECENT) {
            wcscpy_s(g_app->recentFiles[i], MAX_PATH, g_app->recentFiles[i - 1]);
        }
    }
    wcscpy_s(g_app->recentFiles[0], MAX_PATH, path);

    if (g_app->recentCount < MAX_RECENT) {
        g_app->recentCount++;
    }
}

// Update recent files menu (placeholder - implemented in menubar.c)
void App_UpdateRecentMenu(void) {
    // Implemented in menubar.c
}

// Save current session to database
void App_SaveSession(void) {
    if (!Database_IsOpen()) return;

    Database_BeginTransaction();

    // Clear existing session
    Database_Execute("DELETE FROM session_tabs");

    // Save each open tab
    for (int i = 0; i < MAX_TABS; i++) {
        Tab* tab = g_app->tabs[i];
        if (!tab || !tab->document) continue;

        Document* doc = tab->document;
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO session_tabs (tab_index, doc_type, file_path, note_id, title, content, is_modified) VALUES (?, ?, ?, ?, ?, ?, ?)";

        if (sqlite3_prepare_v2(Database_GetHandle(), sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, i);
            sqlite3_bind_int(stmt, 2, doc->type);

            // File path
            if (doc->type == DOC_TYPE_FILE && doc->filePath[0]) {
                char pathUtf8[MAX_PATH * 3];
                WideCharToMultiByte(CP_UTF8, 0, doc->filePath, -1, pathUtf8, sizeof(pathUtf8), NULL, NULL);
                sqlite3_bind_text(stmt, 3, pathUtf8, -1, SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, 3);
            }

            // Note ID
            if (doc->type == DOC_TYPE_NOTE) {
                sqlite3_bind_int(stmt, 4, doc->noteId);
            } else {
                sqlite3_bind_null(stmt, 4);
            }

            // Title
            char titleUtf8[MAX_TITLE_LEN * 3];
            WideCharToMultiByte(CP_UTF8, 0, doc->title, -1, titleUtf8, sizeof(titleUtf8), NULL, NULL);
            sqlite3_bind_text(stmt, 5, titleUtf8, -1, SQLITE_TRANSIENT);

            // Content - always save as backup in case file is deleted later
            if (tab->hEditor) {
                WCHAR* content = Editor_GetText(tab->hEditor);
                if (content) {
                    int contentLen = (int)wcslen(content);
                    int bufSize = contentLen * 3 + 1;
                    char* contentUtf8 = (char*)malloc(bufSize);
                    if (contentUtf8) {
                        WideCharToMultiByte(CP_UTF8, 0, content, -1, contentUtf8, bufSize, NULL, NULL);
                        sqlite3_bind_text(stmt, 6, contentUtf8, -1, SQLITE_TRANSIENT);
                        free(contentUtf8);
                    } else {
                        sqlite3_bind_null(stmt, 6);
                    }
                    free(content);
                } else {
                    sqlite3_bind_null(stmt, 6);
                }
            } else {
                sqlite3_bind_null(stmt, 6);
            }

            sqlite3_bind_int(stmt, 7, doc->modified ? 1 : 0);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Save active tab index
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO settings (key, value) VALUES ('session_active_tab', '%d')", g_app->activeTab);
    Database_Execute(sql);

    Database_CommitTransaction();
}

// Restore previous session from database
void App_RestoreSession(void) {
    if (!Database_IsOpen()) {
        MessageBoxW(g_app->hMainWindow, L"Database not available.", APP_NAME, MB_ICONWARNING);
        return;
    }

    // Check if session_tabs table has any data
    sqlite3_stmt* countStmt;
    int sessionCount = 0;
    if (sqlite3_prepare_v2(Database_GetHandle(), "SELECT COUNT(*) FROM session_tabs", -1, &countStmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            sessionCount = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
    }

    if (sessionCount == 0) {
        MessageBoxW(g_app->hMainWindow, L"No saved session found.", APP_NAME, MB_ICONINFORMATION);
        return;
    }

    // Close all current tabs first (but don't let it auto-create new ones)
    // Mark all as unmodified to avoid prompts
    for (int i = 0; i < MAX_TABS; i++) {
        if (g_app->tabs[i] && g_app->tabs[i]->document) {
            g_app->tabs[i]->document->modified = FALSE;
        }
    }

    // Close tabs from end to start to avoid index shifting issues
    for (int i = MAX_TABS - 1; i >= 0; i--) {
        if (g_app->tabs[i]) {
            Tab* tab = g_app->tabs[i];
            ShowWindow(tab->hEditor, SW_HIDE);
            if (tab->document) Document_Destroy(tab->document);
            if (tab->hEditor) DestroyWindow(tab->hEditor);
            free(tab);
            g_app->tabs[i] = NULL;
        }
    }
    g_app->tabCount = 0;
    g_app->activeTab = -1;

    // Clear tab control
    SendMessageW(g_app->hTabControl, TCM_DELETEALLITEMS, 0, 0);

    sqlite3_stmt* stmt;
    const char* sql = "SELECT tab_index, doc_type, file_path, note_id, title, content, is_modified FROM session_tabs ORDER BY tab_index";

    if (sqlite3_prepare_v2(Database_GetHandle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        App_CreateTab(L"Untitled");  // Fallback
        return;
    }

    int tabsRestored = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int docType = sqlite3_column_int(stmt, 1);
        const char* filePath = (const char*)sqlite3_column_text(stmt, 2);
        int noteId = sqlite3_column_int(stmt, 3);
        const char* title = (const char*)sqlite3_column_text(stmt, 4);
        const char* content = (const char*)sqlite3_column_text(stmt, 5);
        int isModified = sqlite3_column_int(stmt, 6);

        Document* doc = NULL;

        if (docType == DOC_TYPE_NOTE && noteId > 0) {
            doc = Document_CreateFromNote(noteId);
        } else if (docType == DOC_TYPE_FILE && filePath && filePath[0]) {
            WCHAR filePathW[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, filePath, -1, filePathW, MAX_PATH);
            if (GetFileAttributesW(filePathW) != INVALID_FILE_ATTRIBUTES) {
                doc = Document_CreateFromFile(filePathW);
            }
        }

        // If we couldn't restore from file/note, create from saved content
        if (!doc) {
            doc = Document_Create();
            if (doc && title) {
                MultiByteToWideChar(CP_UTF8, 0, title, -1, doc->title, MAX_TITLE_LEN);
            }
        }

        if (doc) {
            WCHAR titleW[MAX_TITLE_LEN];
            if (title) {
                MultiByteToWideChar(CP_UTF8, 0, title, -1, titleW, MAX_TITLE_LEN);
            } else {
                wcscpy_s(titleW, MAX_TITLE_LEN, L"Untitled");
            }

            int idx = App_CreateTab(titleW);
            if (idx >= 0 && g_app->tabs[idx]) {
                Document_Destroy(g_app->tabs[idx]->document);
                g_app->tabs[idx]->document = doc;

                // Load content from file/note first
                BOOL contentLoaded = FALSE;
                if (doc->type == DOC_TYPE_NOTE || (doc->type == DOC_TYPE_FILE && doc->filePath[0])) {
                    contentLoaded = Document_Load(doc, g_app->tabs[idx]->hEditor);
                }

                // Restore saved content if:
                // - Content wasn't loaded from file/note, OR
                // - Document was modified (use the modified version)
                if (content && content[0] && (!contentLoaded || isModified)) {
                    int len = MultiByteToWideChar(CP_UTF8, 0, content, -1, NULL, 0);
                    if (len > 0) {
                        WCHAR* contentW = (WCHAR*)malloc(len * sizeof(WCHAR));
                        if (contentW) {
                            MultiByteToWideChar(CP_UTF8, 0, content, -1, contentW, len);
                            Editor_SetText(g_app->tabs[idx]->hEditor, contentW);
                            free(contentW);
                        }
                    }
                } else if (!contentLoaded && (!content || !content[0])) {
                    // No content available - set empty text to avoid garbage
                    Editor_SetText(g_app->tabs[idx]->hEditor, L"");
                }

                doc->modified = isModified;
                TabControl_UpdateTabTitle(idx);
                tabsRestored++;
            } else {
                Document_Destroy(doc);
            }
        }
    }

    sqlite3_finalize(stmt);

    // Restore active tab
    int activeTab = 0;
    if (sqlite3_prepare_v2(Database_GetHandle(), "SELECT value FROM settings WHERE key = 'session_active_tab'", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            activeTab = atoi((const char*)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }

    if (tabsRestored == 0) {
        App_CreateTab(L"Untitled");
    } else if (activeTab >= 0 && activeTab < g_app->tabCount) {
        App_SetActiveTab(activeTab);
    }

    MainWindow_UpdateTitle();
}
