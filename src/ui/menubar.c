#include "supernote.h"
#include "res/resource.h"

// Create menu bar
HMENU MenuBar_Create(void) {
    HMENU hMenu = CreateMenu();
    if (!hMenu) return NULL;

    // File menu
    HMENU hFileMenu = CreatePopupMenu();
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_NEW, L"&New\tCtrl+N");
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_NEW_RICH, L"New &Rich Text Document");
    AppendMenuW(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_OPEN_NOTE, L"&Browse Notes...");
    AppendMenuW(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_SAVEAS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(hFileMenu, MF_SEPARATOR, 0, NULL);
    // Recent Files submenu
    HMENU hRecentMenu = CreatePopupMenu();
    AppendMenuW(hRecentMenu, MF_STRING | MF_GRAYED, 0, L"(No recent files)");
    AppendMenuW(hFileMenu, MF_POPUP, (UINT_PTR)hRecentMenu, L"Recent &Files");
    AppendMenuW(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_CLOSE_TAB, L"&Close Tab\tCtrl+W");
    AppendMenuW(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_PAGE_SETUP, L"Page Set&up...");
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_PRINT, L"&Print...\tCtrl+P");
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_PRINT_PREVIEW, L"Print Pre&view...");
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_EXPORT_PDF, L"Export to P&DF...");
    AppendMenuW(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFileMenu, MF_STRING, IDM_FILE_EXIT, L"E&xit");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, L"&File");

    // Edit menu
    HMENU hEditMenu = CreatePopupMenu();
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_CUT, L"Cu&t\tCtrl+X");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_PASTE, L"&Paste\tCtrl+V");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_DELETE, L"De&lete\tDel");
    AppendMenuW(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_FIND, L"&Find...\tCtrl+F");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_FIND_NEXT, L"Find &Next\tF3");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_FIND_PREV, L"Find Pre&vious\tShift+F3");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_REPLACE, L"R&eplace...\tCtrl+H");
    AppendMenuW(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_FIND_IN_TABS, L"Find in All &Tabs...");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_REPLACE_IN_TABS, L"Replace in All Ta&bs...");
    AppendMenuW(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_GOTO, L"&Go To...\tCtrl+G");
    AppendMenuW(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_SELECT_ALL, L"Select &All\tCtrl+A");
    AppendMenuW(hEditMenu, MF_STRING, IDM_EDIT_TIME_DATE, L"Time/&Date\tF5");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hEditMenu, L"&Edit");

    // Format menu
    HMENU hFormatMenu = CreatePopupMenu();
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_WORDWRAP, L"&Word Wrap");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_FONT, L"&Font...");
    AppendMenuW(hFormatMenu, MF_SEPARATOR, 0, NULL);
    HMENU hTabSizeMenu = CreatePopupMenu();
    AppendMenuW(hTabSizeMenu, MF_STRING, IDM_FORMAT_TABSIZE_2, L"2 spaces");
    AppendMenuW(hTabSizeMenu, MF_STRING, IDM_FORMAT_TABSIZE_4, L"4 spaces");
    AppendMenuW(hTabSizeMenu, MF_STRING, IDM_FORMAT_TABSIZE_8, L"8 spaces");
    AppendMenuW(hFormatMenu, MF_POPUP, (UINT_PTR)hTabSizeMenu, L"&Tab Size");

    // Rich text formatting. Present always, greyed unless a rich text tab is
    // active -- hiding and rebuilding the menu would make the items jump
    // around depending on which tab you came from.
    AppendMenuW(hFormatMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_BOLD, L"&Bold\tCtrl+B");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_ITALIC, L"&Italic\tCtrl+I");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_UNDERLINE, L"&Underline\tCtrl+U");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_STRIKE, L"Stri&kethrough");
    AppendMenuW(hFormatMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_SUPERSCRIPT, L"Su&perscript");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_SUBSCRIPT, L"Su&bscript");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_TEXTCOLOR, L"Text &Colour...");
    AppendMenuW(hFormatMenu, MF_SEPARATOR, 0, NULL);

    HMENU hAlignMenu = CreatePopupMenu();
    AppendMenuW(hAlignMenu, MF_STRING, IDM_FORMAT_ALIGN_LEFT, L"&Left");
    AppendMenuW(hAlignMenu, MF_STRING, IDM_FORMAT_ALIGN_CENTER, L"&Centre");
    AppendMenuW(hAlignMenu, MF_STRING, IDM_FORMAT_ALIGN_RIGHT, L"&Right");
    AppendMenuW(hAlignMenu, MF_STRING, IDM_FORMAT_ALIGN_JUSTIFY, L"&Justify");
    AppendMenuW(hFormatMenu, MF_POPUP, (UINT_PTR)hAlignMenu, L"&Alignment");

    HMENU hSpaceMenu = CreatePopupMenu();
    AppendMenuW(hSpaceMenu, MF_STRING, IDM_FORMAT_LINESPACE_1, L"&Single");
    AppendMenuW(hSpaceMenu, MF_STRING, IDM_FORMAT_LINESPACE_15, L"1.&5 lines");
    AppendMenuW(hSpaceMenu, MF_STRING, IDM_FORMAT_LINESPACE_2, L"&Double");
    AppendMenuW(hFormatMenu, MF_POPUP, (UINT_PTR)hSpaceMenu, L"Line &Spacing");

    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_BULLETS, L"Bulle&ted List");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_NUMBERING, L"&Numbered List");
    AppendMenuW(hFormatMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_INDENT_MORE, L"Increase Inden&t");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_INDENT_LESS, L"&Decrease Indent");
    AppendMenuW(hFormatMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFormatMenu, MF_STRING, IDM_INSERT_PICTURE, L"Insert Pict&ure...");
    AppendMenuW(hFormatMenu, MF_STRING, IDM_FORMAT_CLEAR, L"Clear F&ormatting");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFormatMenu, L"F&ormat");

    // View menu
    HMENU hViewMenu = CreatePopupMenu();
    HMENU hZoomMenu = CreatePopupMenu();
    AppendMenuW(hZoomMenu, MF_STRING, IDM_VIEW_ZOOM_IN, L"Zoom &In\tCtrl++");
    AppendMenuW(hZoomMenu, MF_STRING, IDM_VIEW_ZOOM_OUT, L"Zoom &Out\tCtrl+-");
    AppendMenuW(hZoomMenu, MF_STRING, IDM_VIEW_ZOOM_RESET, L"&Reset Zoom\tCtrl+0");
    AppendMenuW(hViewMenu, MF_POPUP, (UINT_PTR)hZoomMenu, L"&Zoom");
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hViewMenu, MF_STRING, IDM_VIEW_PAGE_LAYOUT, L"&Page Layout	Ctrl+Shift+L");
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hViewMenu, MF_STRING, IDM_VIEW_TOOLBAR, L"&Toolbar");
    AppendMenuW(hViewMenu, MF_STRING, IDM_VIEW_STATUSBAR, L"&Status Bar");
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hViewMenu, MF_STRING, IDM_VIEW_NOTES_BROWSER, L"&Notes Browser...");
    AppendMenuW(hViewMenu, MF_STRING, IDM_VIEW_PREVIEW, L"&Markdown Preview\tF6");
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hViewMenu, MF_STRING, IDM_VIEW_ALWAYS_ON_TOP, L"&Always on Top");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, L"&View");

    // Settings menu
    HMENU hSettingsMenu = CreatePopupMenu();
    AppendMenuW(hSettingsMenu, MF_STRING, IDM_SETTINGS_DEFAULTS, L"&Default Settings...");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSettingsMenu, L"&Settings");

    // Help menu
    HMENU hHelpMenu = CreatePopupMenu();
    AppendMenuW(hHelpMenu, MF_STRING, IDM_HELP_GUIDE, L"&Getting Started");
    AppendMenuW(hHelpMenu, MF_STRING, IDM_HELP_CONTROLS, L"&Controls");
    AppendMenuW(hHelpMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hHelpMenu, MF_STRING, IDM_HELP_ABOUT, L"&About opennote");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelpMenu, L"&Help");

    return hMenu;
}

// Update File menu state
void MenuBar_UpdateFileMenu(HMENU hMenu) {
    Tab* tab = App_GetActiveTab();
    Document* doc = tab ? tab->document : NULL;

    // Enable/disable based on document state
    EnableMenuItem(hMenu, IDM_FILE_SAVE, MF_BYCOMMAND | (doc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_FILE_SAVEAS, MF_BYCOMMAND | (doc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_FILE_CLOSE_TAB, MF_BYCOMMAND | (g_app->tabCount > 0 ? MF_ENABLED : MF_GRAYED));

    // Database operations
    EnableMenuItem(hMenu, IDM_FILE_OPEN_NOTE, MF_BYCOMMAND | (Database_IsOpen() ? MF_ENABLED : MF_GRAYED));

    // Update Recent Files submenu (find it by position)
    // Position: New(0), Sep(1), Open(2), BrowseNotes(3), Sep(4),
    // Save(5), SaveAs(6), Sep(7), RecentFiles(8)
    HMENU hRecentMenu = GetSubMenu(hMenu, 8);
    if (hRecentMenu) {
        MenuBar_UpdateRecentFiles(hRecentMenu);
    }
}

// Update Edit menu state
void MenuBar_UpdateEditMenu(HMENU hMenu) {
    Tab* tab = App_GetActiveTab();
    HWND hEditor = tab ? tab->hEditor : NULL;

    BOOL hasSelection = FALSE;
    if (hEditor) {
        int start, end;
        Editor_GetSelection(hEditor, &start, &end);
        hasSelection = (start != end);
    }

    BOOL canPaste = IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);

    EnableMenuItem(hMenu, IDM_EDIT_UNDO, MF_BYCOMMAND | (hEditor && Editor_CanUndo(hEditor) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_REDO, MF_BYCOMMAND | (hEditor && Editor_CanRedo(hEditor) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_CUT, MF_BYCOMMAND | (hasSelection ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_COPY, MF_BYCOMMAND | (hasSelection ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_PASTE, MF_BYCOMMAND | (hEditor && canPaste ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_DELETE, MF_BYCOMMAND | (hasSelection ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_FIND, MF_BYCOMMAND | (hEditor ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_FIND_NEXT, MF_BYCOMMAND | (hEditor && g_app->findText[0] ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_FIND_PREV, MF_BYCOMMAND | (hEditor && g_app->findText[0] ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_REPLACE, MF_BYCOMMAND | (hEditor ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_GOTO, MF_BYCOMMAND | (hEditor ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_SELECT_ALL, MF_BYCOMMAND | (hEditor ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, IDM_EDIT_TIME_DATE, MF_BYCOMMAND | (hEditor ? MF_ENABLED : MF_GRAYED));
}

// Update Format menu state
void MenuBar_UpdateFormatMenu(HMENU hMenu) {
    CheckMenuItem(hMenu, IDM_FORMAT_WORDWRAP, MF_BYCOMMAND | (g_app->wordWrap ? MF_CHECKED : MF_UNCHECKED));

    // Tab size
    CheckMenuItem(hMenu, IDM_FORMAT_TABSIZE_2, MF_BYCOMMAND | (g_app->tabSize == 2 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, IDM_FORMAT_TABSIZE_4, MF_BYCOMMAND | (g_app->tabSize == 4 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, IDM_FORMAT_TABSIZE_8, MF_BYCOMMAND | (g_app->tabSize == 8 ? MF_CHECKED : MF_UNCHECKED));

    // The rich text items only mean something in a rich text tab.
    Tab* tab = App_GetActiveTab();
    HWND hEditor = tab ? tab->hEditor : NULL;
    BOOL rich = Editor_IsRich(hEditor);

    static const int RICH_ITEMS[] = {
        IDM_FORMAT_BOLD, IDM_FORMAT_ITALIC, IDM_FORMAT_UNDERLINE, IDM_FORMAT_STRIKE,
        IDM_FORMAT_SUPERSCRIPT, IDM_FORMAT_SUBSCRIPT, IDM_FORMAT_TEXTCOLOR,
        IDM_FORMAT_ALIGN_LEFT, IDM_FORMAT_ALIGN_CENTER, IDM_FORMAT_ALIGN_RIGHT,
        IDM_FORMAT_ALIGN_JUSTIFY, IDM_FORMAT_LINESPACE_1, IDM_FORMAT_LINESPACE_15,
        IDM_FORMAT_LINESPACE_2, IDM_FORMAT_BULLETS, IDM_FORMAT_NUMBERING,
        IDM_FORMAT_INDENT_MORE, IDM_FORMAT_INDENT_LESS, IDM_INSERT_PICTURE,
        IDM_FORMAT_CLEAR,
    };
    for (int i = 0; i < (int)(sizeof(RICH_ITEMS) / sizeof(RICH_ITEMS[0])); i++) {
        EnableMenuItem(hMenu, RICH_ITEMS[i],
                       MF_BYCOMMAND | (rich ? MF_ENABLED : MF_GRAYED));
    }

    // Conversely, tab size and word wrap belong to the plain text view.
    EnableMenuItem(hMenu, IDM_FORMAT_TABSIZE_2, MF_BYCOMMAND | (rich ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(hMenu, IDM_FORMAT_TABSIZE_4, MF_BYCOMMAND | (rich ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(hMenu, IDM_FORMAT_TABSIZE_8, MF_BYCOMMAND | (rich ? MF_GRAYED : MF_ENABLED));
    // The rich view always wraps to the window -- see Rich_SetWordWrap.
    EnableMenuItem(hMenu, IDM_FORMAT_WORDWRAP, MF_BYCOMMAND | (rich ? MF_GRAYED : MF_ENABLED));

    if (rich) {
        CheckMenuItem(hMenu, IDM_FORMAT_BOLD,      MF_BYCOMMAND | (Rich_HasEffect(hEditor, CFE_BOLD)      ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_FORMAT_ITALIC,    MF_BYCOMMAND | (Rich_HasEffect(hEditor, CFE_ITALIC)    ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_FORMAT_UNDERLINE, MF_BYCOMMAND | (Rich_HasEffect(hEditor, CFE_UNDERLINE) ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_FORMAT_STRIKE,    MF_BYCOMMAND | (Rich_HasEffect(hEditor, CFE_STRIKEOUT) ? MF_CHECKED : MF_UNCHECKED));

        WORD align = Rich_GetAlignment(hEditor);
        CheckMenuItem(hMenu, IDM_FORMAT_ALIGN_LEFT,    MF_BYCOMMAND | (align == PFA_LEFT    ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_FORMAT_ALIGN_CENTER,  MF_BYCOMMAND | (align == PFA_CENTER  ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_FORMAT_ALIGN_RIGHT,   MF_BYCOMMAND | (align == PFA_RIGHT   ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_FORMAT_ALIGN_JUSTIFY, MF_BYCOMMAND | (align == PFA_JUSTIFY ? MF_CHECKED : MF_UNCHECKED));
    }
}

// Update View menu state
void MenuBar_UpdateViewMenu(HMENU hMenu) {
    CheckMenuItem(hMenu, IDM_VIEW_TOOLBAR, MF_BYCOMMAND | (g_app->showFormatBar ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, IDM_VIEW_STATUSBAR, MF_BYCOMMAND | (g_app->showStatusBar ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, IDM_VIEW_ALWAYS_ON_TOP, MF_BYCOMMAND | (g_app->alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));

    // Enable notes browser only if database is open
    EnableMenuItem(hMenu, IDM_VIEW_NOTES_BROWSER, MF_BYCOMMAND | (Database_IsOpen() ? MF_ENABLED : MF_GRAYED));
}

// Update Settings menu state
void MenuBar_UpdateSettingsMenu(HMENU hMenu) {
    (void)hMenu;  // Settings are now in the Defaults dialog
}

// Update recent files menu
void MenuBar_UpdateRecentFiles(HMENU hMenu) {
    // Remove existing recent items
    while (DeleteMenu(hMenu, 0, MF_BYPOSITION)) {}

    if (g_app->recentCount == 0) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"(No recent files)");
        return;
    }

    for (int i = 0; i < g_app->recentCount; i++) {
        WCHAR menuText[MAX_PATH + 4];
        swprintf_s(menuText, sizeof(menuText)/sizeof(WCHAR), L"&%d %s", i + 1, g_app->recentFiles[i]);
        AppendMenuW(hMenu, MF_STRING, 6000 + i, menuText);
    }
}
