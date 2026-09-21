#ifndef APP_H
#define APP_H

// Application state structure
struct AppState {
    HINSTANCE hInstance;
    HWND hMainWindow;
    HWND hTabControl;
    HWND hStatusBar;
    HACCEL hAccel;

    // Database
    sqlite3* db;
    WCHAR dbPath[MAX_PATH];

    // Tabs
    Tab* tabs[MAX_TABS];
    int tabCount;
    int activeTab;

    // Settings
    BOOL wordWrap;
    BOOL showStatusBar;
    BOOL showFormatBar;
    BOOL alwaysOnTop;
    BOOL autoSaveSession;   // Save session on exit
    BOOL autoRestoreSession; // Restore session on startup
    BOOL minimizeToTray;    // Minimize to system tray
    int tabSize;

    // Cloud sync
    WCHAR* syncProvider;    // "GitHub" or "Google Drive" or NULL
    int zoomLevel;      // 100 = 100%
    int themeIndex;     // Editor color theme
    LOGFONTW editorFont;
    HFONT hEditorFont;

    // Find/Replace state
    HWND hFindDialog;
    WCHAR findText[MAX_PATH];
    WCHAR replaceText[MAX_PATH];
    BOOL matchCase;
    BOOL wholeWord;
    BOOL wrapAround;

    // Recent files
    WCHAR recentFiles[MAX_RECENT][MAX_PATH];
    int recentCount;
};

// Application functions
BOOL App_Initialize(HINSTANCE hInstance);
void App_Shutdown(void);
BOOL App_Run(void);

// Settings
void App_LoadSettings(void);
void App_SaveSettings(void);

// Tab management. App_CreateTab makes a plain text tab; App_CreateTabEx picks
// the view from the format, which is how .rtf files land in the rich editor.
int App_CreateTab(const WCHAR* title);
int App_CreateTabEx(const WCHAR* title, DocumentFormat format);
void App_CloseTab(int index);
void App_SetActiveTab(int index);
Tab* App_GetActiveTab(void);
Document* App_GetActiveDocument(void);

// Recent files
void App_AddRecentFile(const WCHAR* path);
void App_UpdateRecentMenu(void);

// Session management
void App_SaveSession(void);
void App_RestoreSession(void);

#endif // APP_H
