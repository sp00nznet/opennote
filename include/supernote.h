#ifndef SUPERNOTE_H
#define SUPERNOTE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
// RichEdit, declared at the Windows 8 level before richedit.h. That exposes
// CHARFORMAT2W, PARAFORMAT2 and EM_SETTEXTMODE for the view in editor_rich.c,
// and additionally TABLEROWPARMS / EM_GETTABLEPARMS, which is how
// doctree_view.c recovers a table's real column widths instead of guessing.
//
// Safe here because this targets Windows 10 and 11, where the control behind
// MSFTEDIT_CLASS is RichEdit 8 regardless of the 4.1-era class name.
#define _RICHEDIT_VER 0x0800
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sqlite3.h"

// Application info
#define APP_NAME        L"OpenNote"
#define APP_VERSION     L"0.7.0"
#define APP_CLASS_NAME  L"OpenNoteMainWindow"

// Limits
#define MAX_TABS        64
#define MAX_PATH_LEN    32768
#define MAX_TITLE_LEN   256
#define MAX_RECENT      10

// Document types
typedef enum {
    DOC_TYPE_FILE,      // Regular file from filesystem
    DOC_TYPE_NOTE       // Database note
} DocumentType;

// How a document's content is stored, and therefore which view edits it.
// FORMAT_PLAIN is the Scintilla view; FORMAT_RTF is the RichEdit one.
typedef enum {
    FORMAT_PLAIN,
    FORMAT_RTF,
    FORMAT_DOCX
} DocumentFormat;

// Both rich formats are edited in the same view; only their storage differs.
#define FORMAT_IS_RICH(f) ((f) == FORMAT_RTF || (f) == FORMAT_DOCX)

// Document encoding
typedef enum {
    ENCODING_UTF8,
    ENCODING_UTF16_LE,
    ENCODING_UTF16_BE,
    ENCODING_ANSI
} TextEncoding;

// Forward declarations
typedef struct Document Document;
typedef struct Tab Tab;
typedef struct AppState AppState;

// Include component headers
#include "app.h"
#include "ui/mainwindow.h"
#include "ui/tabcontrol.h"
#include "ui/editor.h"
#include "ui/editor_rich.h"
#include "ui/menubar.h"
#include "ui/statusbar.h"
#include "ui/dialogs.h"
#include "core/document.h"
#include "core/strbuf.h"
#include "core/doctree.h"
#include "core/doctree_rtf.h"
#include "core/doctree_view.h"
#include "core/docx.h"
#include "core/fileio.h"
#include "core/search.h"
#include "db/database.h"
#include "db/notes_repo.h"
#include "db/links_repo.h"
#include "sync/oauth.h"
#include "sync/github_sync.h"
#include "sync/google_sync.h"

// Global application state
extern AppState* g_app;

#endif // SUPERNOTE_H
