#include "supernote.h"

static sqlite3* g_db = NULL;
static char g_lastError[256] = {0};

// SQL to create schema
static const char* SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS notes ("
    "    id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    title           TEXT NOT NULL DEFAULT 'Untitled',"
    "    content         TEXT NOT NULL DEFAULT '',"
    "    content_hash    TEXT,"
    "    created_at      TEXT DEFAULT (datetime('now')),"
    "    updated_at      TEXT DEFAULT (datetime('now')),"
    "    is_pinned       INTEGER DEFAULT 0,"
    "    is_archived     INTEGER DEFAULT 0,"
    "    category_id     INTEGER"
    ");"

    "CREATE INDEX IF NOT EXISTS idx_notes_hash ON notes(content_hash);"

    "CREATE TABLE IF NOT EXISTS categories ("
    "    id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    name        TEXT NOT NULL UNIQUE,"
    "    color       TEXT"
    ");"

    "CREATE TABLE IF NOT EXISTS tags ("
    "    id      INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    name    TEXT NOT NULL UNIQUE"
    ");"

    "CREATE TABLE IF NOT EXISTS note_tags ("
    "    note_id     INTEGER NOT NULL,"
    "    tag_id      INTEGER NOT NULL,"
    "    PRIMARY KEY (note_id, tag_id),"
    "    FOREIGN KEY (note_id) REFERENCES notes(id) ON DELETE CASCADE,"
    "    FOREIGN KEY (tag_id) REFERENCES tags(id) ON DELETE CASCADE"
    ");"

    // A signature somebody drew, kept so they never have to draw it twice.
    // The bytes are a PNG with a transparent background, which is what goes
    // onto a page.
    "CREATE TABLE IF NOT EXISTS signatures ("
    "    id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    name        TEXT NOT NULL DEFAULT 'Signature',"
    "    png         BLOB NOT NULL,"
    "    created_at  TEXT DEFAULT (datetime('now'))"
    ");"

    // What somebody has already typed into a form, by the name of the field
    // they typed it into -- so the next form asking for a name, an address or
    // a date of birth can offer what the last one was told.
    "CREATE TABLE IF NOT EXISTS form_answers ("
    "    field       TEXT PRIMARY KEY,"   // the question, with case and punctuation out
    "    label       TEXT,"               // ...and how it was last written
    "    value       TEXT NOT NULL,"
    "    updated_at  TEXT DEFAULT (datetime('now'))"
    ");"

    "CREATE TABLE IF NOT EXISTS settings ("
    "    key     TEXT PRIMARY KEY,"
    "    value   TEXT"
    ");"

    "CREATE TABLE IF NOT EXISTS session_tabs ("
    "    tab_index   INTEGER PRIMARY KEY,"
    "    doc_type    INTEGER NOT NULL,"
    "    file_path   TEXT,"
    "    note_id     INTEGER,"
    "    title       TEXT,"
    "    content     TEXT,"
    "    is_modified INTEGER DEFAULT 0"
    ");"

    "CREATE VIRTUAL TABLE IF NOT EXISTS notes_fts USING fts5("
    "    title, content, content='notes', content_rowid='id'"
    ");"

    "CREATE TRIGGER IF NOT EXISTS notes_ai AFTER INSERT ON notes BEGIN"
    "    INSERT INTO notes_fts(rowid, title, content) VALUES (new.id, new.title, new.content);"
    "END;"

    "CREATE TRIGGER IF NOT EXISTS notes_ad AFTER DELETE ON notes BEGIN"
    "    INSERT INTO notes_fts(notes_fts, rowid, title, content) VALUES('delete', old.id, old.title, old.content);"
    "END;"

    "CREATE TRIGGER IF NOT EXISTS notes_au AFTER UPDATE ON notes BEGIN"
    "    INSERT INTO notes_fts(notes_fts, rowid, title, content) VALUES('delete', old.id, old.title, old.content);"
    "    INSERT INTO notes_fts(rowid, title, content) VALUES (new.id, new.title, new.content);"
    "END;"

    // Links table - stores word links between documents
    // source_type: 0=file, 1=note
    // target_type: 0=file, 1=note, 2=url
    "CREATE TABLE IF NOT EXISTS links ("
    "    id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    source_type     INTEGER NOT NULL,"
    "    source_path     TEXT,"
    "    source_note_id  INTEGER,"
    "    link_text       TEXT NOT NULL,"
    "    start_pos       INTEGER NOT NULL,"
    "    end_pos         INTEGER NOT NULL,"
    "    target_type     INTEGER NOT NULL,"
    "    target_path     TEXT,"
    "    target_note_id  INTEGER,"
    "    target_url      TEXT,"
    "    created_at      TEXT DEFAULT (datetime('now'))"
    ");"

    // Index for faster lookup
    "CREATE INDEX IF NOT EXISTS idx_links_source ON links(source_type, source_path, source_note_id);";

// Open database
BOOL Database_Open(const WCHAR* path) {
    if (g_db) {
        Database_Close();
    }

    // Convert path to UTF-8
    char pathUtf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, pathUtf8, sizeof(pathUtf8), NULL, NULL);

    int rc = sqlite3_open(pathUtf8, &g_db);
    if (rc != SQLITE_OK) {
        if (g_db) {
            strncpy_s(g_lastError, sizeof(g_lastError), sqlite3_errmsg(g_db), _TRUNCATE);
            sqlite3_close(g_db);
            g_db = NULL;
        }
        return FALSE;
    }

    // Enable foreign keys
    sqlite3_exec(g_db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);

    // Set busy timeout
    sqlite3_busy_timeout(g_db, 5000);

    return TRUE;
}

// Close database
void Database_Close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

// Check if database is open
BOOL Database_IsOpen(void) {
    return g_db != NULL;
}

// Initialize database schema
BOOL Database_Initialize(void) {
    if (!g_db) return FALSE;

    char* errMsg = NULL;
    int rc = sqlite3_exec(g_db, SCHEMA_SQL, NULL, NULL, &errMsg);

    if (rc != SQLITE_OK) {
        if (errMsg) {
            strncpy_s(g_lastError, sizeof(g_lastError), errMsg, _TRUNCATE);
            sqlite3_free(errMsg);
        }
        return FALSE;
    }

    // Migration: Add target_url column to links table if it doesn't exist
    sqlite3_exec(g_db, "ALTER TABLE links ADD COLUMN target_url TEXT;", NULL, NULL, NULL);

    // Migration: Add content_hash column to notes table if it doesn't exist
    sqlite3_exec(g_db, "ALTER TABLE notes ADD COLUMN content_hash TEXT;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_notes_hash ON notes(content_hash);", NULL, NULL, NULL);

    // Migration: Add gist_id and last_synced columns for GitHub sync
    sqlite3_exec(g_db, "ALTER TABLE notes ADD COLUMN gist_id TEXT;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE notes ADD COLUMN last_synced TEXT;", NULL, NULL, NULL);

    // Migration: Add drive_file_id column for Google Drive sync
    sqlite3_exec(g_db, "ALTER TABLE notes ADD COLUMN drive_file_id TEXT;", NULL, NULL, NULL);

    return TRUE;
}

// Get current schema version
int Database_GetVersion(void) {
    if (!g_db) return -1;

    sqlite3_stmt* stmt;
    int version = 0;

    if (sqlite3_prepare_v2(g_db, "SELECT value FROM settings WHERE key = 'schema_version'", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = atoi((const char*)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }

    return version;
}

// Set schema version
BOOL Database_SetVersion(int version) {
    if (!g_db) return FALSE;

    char sql[128];
    snprintf(sql, sizeof(sql),
             "INSERT OR REPLACE INTO settings (key, value) VALUES ('schema_version', '%d')",
             version);

    return Database_Execute(sql);
}

// Run migrations
BOOL Database_Migrate(void) {
    // Currently no migrations needed
    Database_SetVersion(1);
    return TRUE;
}

// Get database handle
sqlite3* Database_GetHandle(void) {
    return g_db;
}

// Execute SQL statement
BOOL Database_Execute(const char* sql) {
    if (!g_db || !sql) return FALSE;

    char* errMsg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errMsg);

    if (rc != SQLITE_OK) {
        if (errMsg) {
            strncpy_s(g_lastError, sizeof(g_lastError), errMsg, _TRUNCATE);
            sqlite3_free(errMsg);
        }
        return FALSE;
    }

    return TRUE;
}

// Begin transaction
BOOL Database_BeginTransaction(void) {
    return Database_Execute("BEGIN TRANSACTION");
}

// Commit transaction
BOOL Database_CommitTransaction(void) {
    return Database_Execute("COMMIT");
}

// Rollback transaction
BOOL Database_RollbackTransaction(void) {
    return Database_Execute("ROLLBACK");
}

// Get last error message
const char* Database_GetLastError(void) {
    if (g_db) {
        return sqlite3_errmsg(g_db);
    }
    return g_lastError[0] ? g_lastError : "Unknown error";
}

// ---------------------------------------------------------------------------
// Settings key/value access
//
// Bound parameters, not formatted SQL. Every caller here handles values that
// came from somewhere else -- an OAuth provider, a file, a dialog -- and a
// quote in any of them would otherwise change the statement.
// ---------------------------------------------------------------------------

BOOL Database_SetSetting(const char* key, const char* value) {
    if (!g_db || !key || !value) return FALSE;

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);

    BOOL ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

BOOL Database_GetSetting(const char* key, char* valueOut, size_t valueSize) {
    if (!g_db || !key || !valueOut || valueSize == 0) return FALSE;

    valueOut[0] = '\0';

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db,
            "SELECT value FROM settings WHERE key = ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    BOOL found = FALSE;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = (const char*)sqlite3_column_text(stmt, 0);
        if (value && value[0]) {
            strncpy_s(valueOut, valueSize, value, _TRUNCATE);
            found = TRUE;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

BOOL Database_DeleteSetting(const char* key) {
    if (!g_db || !key) return FALSE;

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db,
            "DELETE FROM settings WHERE key = ?", -1, &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    BOOL ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}
