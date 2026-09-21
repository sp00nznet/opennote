# OpenNote Architecture

## Overview

OpenNote is a Win32 application written in C, using the Windows API for UI and SQLite for data persistence.

## Project Structure

```
OpenNote/
├── CMakeLists.txt          # Build configuration
├── include/
│   └── supernote.h         # Main header with common includes
├── src/
│   ├── main.c              # Entry point (WinMain)
│   ├── app.c/h             # Application state and lifecycle
│   ├── ui/
│   │   ├── mainwindow.c/h  # Main window and message handling
│   │   ├── tabcontrol.c/h  # Tab management and switching
│   │   ├── editor.c/h      # Scintilla editor wrapper
│   │   ├── editor_rich.c/h # RichEdit view: the rich text document
│   │   ├── pageview.cpp/h  # Print preview: the laid-out pages, drawn with Direct2D
│   │   ├── menubar.c/h     # Menu creation and updates
│   │   ├── statusbar.c/h   # Status bar updates
│   │   └── dialogs.c/h     # Find, Replace, Go To, Notes Browser, etc.
│   ├── layout/
│   │   ├── layout.cpp/h    # The layout engine: document model -> pages
│   │   └── layoutprint.cpp/h # Printing those pages, and export to PDF
│   ├── core/
│   │   ├── doctree.c/h     # The document tree: sections, paragraphs, runs, tables
│   │   ├── docx.c/h        # .docx reading and writing (ECMA-376)
│   │   ├── document.c/h    # Document model (title, path, content, modified state)
│   │   ├── fileio.c/h      # File reading/writing with encoding detection
│   │   └── search.c/h      # Find and replace engine
│   └── db/
│       ├── database.c/h    # SQLite initialization and migrations
│       ├── notes_repo.c/h  # Notes CRUD operations
│       ├── links_repo.c/h  # Document links (file, note, URL)
│       └── settings.c/h    # Settings key-value store
├── res/
│   ├── resource.h          # Resource IDs
│   ├── supernote.rc        # Resources (menus, dialogs, icons)
│   ├── icon.ico            # Application icon
│   └── manifest.xml        # DPI awareness and visual styles
└── lib/sqlite/
    ├── sqlite3.c           # SQLite amalgamation
    └── sqlite3.h
```

## Core Components

### Application State (`src/app.c`)

The `AppState` struct holds all global application state:

```c
typedef struct {
    HINSTANCE hInstance;        // Application instance
    HWND hMainWindow;           // Main window handle
    HWND hTabControl;           // Tab control handle
    HWND hStatusBar;            // Status bar handle

    Document** documents;       // Array of open documents
    int documentCount;          // Number of documents
    int activeTab;              // Currently active tab index

    sqlite3* db;                // Database connection

    // Settings
    BOOL wordWrap;
    BOOL autoSave;
    BOOL minimizeToTray;
    int fontSize;
    int zoomLevel;
} AppState;
```

### Document Model (`src/core/document.c`)

Each open document is represented by:

```c
typedef struct {
    HWND hEditor;               // Scintilla editor control
    WCHAR* filePath;            // NULL for unsaved files
    WCHAR* title;               // Display title
    int noteId;                 // Note ID if from database, -1 otherwise
    DocumentType type;          // DOC_TYPE_FILE or DOC_TYPE_NOTE
    BOOL isModified;            // Has unsaved changes
    int encoding;               // UTF-8, UTF-16, ANSI
} Document;
```

### Main Window (`src/ui/mainwindow.c`)

Handles:
- Window creation and message loop
- Menu commands (`WM_COMMAND`)
- Tab notifications (`WM_NOTIFY`)
- System tray integration
- Context menus

### Tab Control (`src/ui/tabcontrol.c`)

Manages:
- Creating/closing tabs
- Tab switching
- Tab context menu
- "+" button for new tabs

### Editor (`src/ui/editor.c`)

Wraps Scintilla editor control:
- Text manipulation (get/set content)
- Selection handling
- Syntax highlighting setup
- Zoom control

## Database Schema

### Notes Table

```sql
CREATE TABLE notes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    content TEXT NOT NULL,
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now')),
    is_pinned INTEGER DEFAULT 0,
    is_archived INTEGER DEFAULT 0
);

-- Full-text search index
CREATE VIRTUAL TABLE notes_fts USING fts5(
    title,
    content,
    content='notes',
    content_rowid='id'
);
```

### Links Table

```sql
CREATE TABLE links (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_type INTEGER NOT NULL,     -- 0=file, 1=note
    source_path TEXT,                 -- File path if source is file
    source_note_id INTEGER,           -- Note ID if source is note
    link_text TEXT NOT NULL,          -- Display text
    start_pos INTEGER,                -- Position in source
    end_pos INTEGER,
    target_type INTEGER NOT NULL,     -- 0=file, 1=note, 2=url
    target_path TEXT,                 -- File path if target is file
    target_note_id INTEGER,           -- Note ID if target is note
    target_url TEXT,                  -- URL if target is URL
    created_at TEXT DEFAULT (datetime('now'))
);
```

### Session Tables

```sql
CREATE TABLE session_tabs (
    tab_index INTEGER PRIMARY KEY,
    doc_type INTEGER,           -- 0=file, 1=note
    file_path TEXT,
    note_id INTEGER,
    title TEXT,
    content TEXT,
    is_modified INTEGER
);

CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT
);
```

## Message Flow

1. **User Action** (keyboard/mouse)
2. **Windows Message** (`WM_COMMAND`, `WM_NOTIFY`, etc.)
3. **MainWindow_WndProc** dispatches to handler
4. **Handler** updates state and UI
5. **UI Update** (status bar, menu state, title)

## Key Patterns

### Resource IDs

All resource IDs are defined in `res/resource.h`:
- `IDM_*` - Menu commands
- `IDD_*` - Dialog templates
- `IDC_*` - Dialog controls
- `IDI_*` - Icons

### Error Handling

Functions return success/failure indicators:
- `BOOL` functions return `TRUE` on success
- Pointer functions return `NULL` on failure
- Database functions check `sqlite3_step()` return codes

### Memory Management

- Use `malloc`/`free` for dynamic allocation
- Wide strings (`WCHAR*`) for all text
- SQLite manages its own memory

## Extending OpenNote

### Adding a Menu Command

1. Add `IDM_*` constant in `res/resource.h`
2. Add menu item in `res/supernote.rc`
3. Handle in `MainWindow_OnCommand()`
4. Add keyboard shortcut in accelerator table if needed

### Adding a Dialog

1. Add `IDD_*` and `IDC_*` constants in `res/resource.h`
2. Create dialog template in `res/supernote.rc`
3. Add dialog procedure in `src/ui/dialogs.c`
4. Call `DialogBox()` or `CreateDialog()` from menu handler

### Adding a Database Table

1. Add CREATE TABLE in `Database_Initialize()`
2. Add migration in `Database_Migrate()` for existing databases
3. Create repository functions in new or existing `*_repo.c` file
