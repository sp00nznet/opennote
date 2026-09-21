#ifndef RESOURCE_H
#define RESOURCE_H

// Icons
#define IDI_SUPERNOTE           100
#define IDI_DOCUMENT            101
#define IDI_NOTE                102

// Menus
#define IDM_MAINMENU            200

// File menu
#define IDM_FILE_NEW            1001
#define IDM_FILE_NEW_NOTE       1002
#define IDM_FILE_OPEN           1003
#define IDM_FILE_OPEN_NOTE      1004
#define IDM_FILE_SAVE           1005
#define IDM_FILE_SAVEAS         1006
#define IDM_FILE_SAVE_TO_NOTES  1007
#define IDM_FILE_EXPORT         1008
#define IDM_FILE_CLOSE_TAB      1009
#define IDM_FILE_PRINT          1010
#define IDM_FILE_PRINT_PREVIEW  1011
#define IDM_FILE_PRINT_SETUP    1012
#define IDM_FILE_EXIT           1013

// Edit menu
#define IDM_EDIT_UNDO           2001
#define IDM_EDIT_REDO           2002
#define IDM_EDIT_CUT            2003
#define IDM_EDIT_COPY           2004
#define IDM_EDIT_PASTE          2005
#define IDM_EDIT_DELETE         2006
#define IDM_EDIT_FIND           2007
#define IDM_EDIT_FIND_NEXT      2008
#define IDM_EDIT_FIND_PREV      2009
#define IDM_EDIT_REPLACE        2010
#define IDM_EDIT_GOTO           2011
#define IDM_EDIT_SELECT_ALL     2012
#define IDM_EDIT_TIME_DATE      2013
#define IDM_EDIT_FIND_IN_TABS   2014
#define IDM_EDIT_REPLACE_IN_TABS 2015

// Format menu
#define IDM_FORMAT_WORDWRAP     3001
#define IDM_FORMAT_FONT         3002
#define IDM_FORMAT_TABSIZE_2    3003
#define IDM_FORMAT_TABSIZE_4    3004
#define IDM_FORMAT_TABSIZE_8    3005

// View menu
#define IDM_VIEW_ZOOM_IN        4001
#define IDM_VIEW_ZOOM_OUT       4002
#define IDM_VIEW_ZOOM_RESET     4003
#define IDM_VIEW_STATUSBAR      4004
#define IDM_VIEW_NOTES_BROWSER  4005
#define IDM_VIEW_ALWAYS_ON_TOP  4006
#define IDM_VIEW_PREVIEW        4007
#define IDM_VIEW_TOOLBAR        4008

// Settings menu
#define IDM_SETTINGS_AUTOSAVE   5501
#define IDM_SETTINGS_RESTORE    5502
#define IDM_SETTINGS_DEFAULTS   5503
#define IDM_SETTINGS_FONT_8     5510
#define IDM_SETTINGS_FONT_9     5511
#define IDM_SETTINGS_FONT_10    5512
#define IDM_SETTINGS_FONT_11    5513
#define IDM_SETTINGS_FONT_12    5514
#define IDM_SETTINGS_FONT_14    5515
#define IDM_SETTINGS_FONT_16    5516
#define IDM_SETTINGS_FONT_18    5518
#define IDM_SETTINGS_FONT_20    5520

// Help menu
#define IDM_HELP_ABOUT          5001

// Accelerators
#define IDA_ACCEL               300

// Compare
#define IDM_COMPARE_BASE        7000  // Compare with tab 0 = 7000, tab 1 = 7001, etc.

// Link to tab commands (IDM_COMPARE_BASE + 100 + tab_index)
#define IDM_LINK_TAB_BASE       7100
#define IDM_LINK_URL            7150

// Spell check context menu
#define IDM_SPELL_SUGGESTION_BASE 7200  // Suggestions: 7200-7209
#define IDM_SPELL_ADD_DICT      7210
#define IDM_SPELL_IGNORE        7211
#define IDM_SPELL_CHECK_DOC     7212
#define IDM_SHELL_CMD           7220
#define IDM_SHELL_POWERSHELL    7221
#define IDM_SHELL_CMD_ADMIN     7222
#define IDM_SHELL_PS_ADMIN      7223

// Dialogs
#define IDD_ABOUT               400
#define IDD_COMPARE             405
#define IDD_GOTO                401
#define IDD_NOTES_BROWSER       402
#define IDD_FIND                403
#define IDD_REPLACE             404
#define IDD_DEFAULTS            406
#define IDD_FIND_IN_TABS        407
#define IDD_REPLACE_IN_TABS     408
#define IDD_INPUTBOX            409
#define IDD_SYNC_ACCOUNTS       411
#define IDD_SYNC_CREDENTIALS    412

// Dialog controls
#define IDC_STATIC              -1
#define IDC_GOTO_LINE           1101
#define IDC_NOTES_LIST          1102
#define IDC_NOTES_SEARCH        1103
#define IDC_NOTES_NEW           1104
#define IDC_NOTES_DELETE        1105
#define IDC_FIND_TEXT           1106
#define IDC_REPLACE_TEXT        1107
#define IDC_FIND_NEXT           1108
#define IDC_FIND_PREV           1109
#define IDC_REPLACE_ONE         1110
#define IDC_REPLACE_ALL         1111
#define IDC_MATCH_CASE          1112
#define IDC_WHOLE_WORD          1113
#define IDC_WRAP_AROUND         1114
#define IDC_DEFAULTS_FONTSIZE   1115
#define IDC_DEFAULTS_THEME      1122
#define IDC_TABS_LIST           1116
#define IDC_FIND_IN_TABS_TEXT   1117
#define IDC_REPLACE_IN_TABS_TEXT 1118
#define IDC_SELECT_ALL_TABS     1119
#define IDC_FIND_ALL_TABS       1120
#define IDC_REPLACE_ALL_TABS    1121
#define IDC_HIGHLIGHT_PERSIST   1123
#define IDC_INPUTBOX_TEXT       1124
#define IDC_INPUTBOX_PROMPT     1125
#define IDC_DEFAULTS_TRAY       1126
#define IDC_NOTES_RENAME        1132
#define IDC_NEW_TAB_BTN         1133
#define IDC_NOTES_SYNC          1134
#define IDC_DEFAULTS_AUTOSAVE   1135
#define IDC_DEFAULTS_RESTORE    1136
#define IDC_DEFAULTS_AUTORESTORE 1142
#define IDC_DEFAULTS_SYNC_ACCOUNTS 1137
#define IDC_SYNC_GITHUB         1138
#define IDC_SYNC_GOOGLE         1139
#define IDC_SYNC_STATUS         1140
#define IDC_SYNC_SIGNOUT        1141
#define IDC_SYNC_CREDENTIALS    1142

// Cloud credentials: the OAuth application the user registered themselves
#define IDC_CREDS_GITHUB_ID     1143
#define IDC_CREDS_GOOGLE_ID     1144
#define IDC_CREDS_GOOGLE_SECRET 1145
#define IDC_CREDS_HELP          1146
#define IDC_CREDS_CLEAR         1147
#define IDC_ABOUT_NAME          1148
#define IDC_ABOUT_VERSION       1149
#define IDC_ABOUT_LINKS         1150

// Status bar parts
#define SB_PART_MESSAGE         0
#define SB_PART_LINE            1
#define SB_PART_COLUMN          2
#define SB_PART_ENCODING        3
#define SB_PART_MODIFIED        4

// Timer IDs
#define TIMER_AUTOSAVE          1
#define TIMER_STATUS_UPDATE     2

// Custom messages
#define WM_APP_UPDATE_STATUS    (WM_APP + 1)
#define WM_APP_TAB_CHANGED      (WM_APP + 2)
#define WM_APP_DOC_MODIFIED     (WM_APP + 3)
#define WM_APP_TRAY_CALLBACK    (WM_APP + 4)

#endif // RESOURCE_H

// Rich text formatting (v0.5 -- the WordPad replacement)
#define IDM_FORMAT_BOLD          7300
#define IDM_FORMAT_ITALIC        7301
#define IDM_FORMAT_UNDERLINE     7302
#define IDM_FORMAT_STRIKE        7303
#define IDM_FORMAT_TEXTCOLOR     7304
#define IDM_FORMAT_ALIGN_LEFT    7305
#define IDM_FORMAT_ALIGN_CENTER  7306
#define IDM_FORMAT_ALIGN_RIGHT   7307
#define IDM_FORMAT_ALIGN_JUSTIFY 7308
#define IDM_FORMAT_BULLETS       7309
#define IDM_FORMAT_NUMBERING     7310
#define IDM_FORMAT_INDENT_MORE   7311
#define IDM_FORMAT_INDENT_LESS   7312
#define IDM_FORMAT_CLEAR         7313
#define IDM_FORMAT_PARAGRAPH     7314
#define IDM_FORMAT_SUPERSCRIPT   7315
#define IDM_FORMAT_SUBSCRIPT     7316
#define IDM_FORMAT_HIGHLIGHT     7317
#define IDM_FORMAT_LINESPACE_1   7318
#define IDM_FORMAT_LINESPACE_15  7319
#define IDM_FORMAT_LINESPACE_2   7320

#define IDM_FILE_NEW_RICH        7330
#define IDM_FILE_PAGE_SETUP      7331
#define IDM_INSERT_PICTURE       7332
#define IDM_FILE_EXPORT_PDF      7333
#define IDM_VIEW_PAGE_LAYOUT     7334

// Toolbar child controls
#define IDC_TOOLBAR              7340
#define IDC_FONT_COMBO           7341
#define IDC_SIZE_COMBO           7342
