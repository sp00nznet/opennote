// What somebody brings to a form.
//
// Two tables and no cleverness: signatures as PNG blobs, and answers keyed by
// what the field was called. The only thing worth explaining is the key.
//
// Field names are a mess. The same question is `Full Name` in one form,
// `full_name` in the next and `FullName1` in the one after that, because
// whoever built each form named the box for themselves. So the key is the
// name with its case and punctuation taken out, and the display name is kept
// beside it -- which matches the three spellings above to each other without
// pretending to understand what any of them means.

#include "supernote.h"
#include "db/database.h"
#include "db/fill_repo.h"

#include <sqlite3.h>

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

static void NormaliseField(const WCHAR* field, char* out, size_t outBytes) {
    out[0] = '\0';
    if (!field) return;

    WCHAR folded[256];
    size_t n = 0;

    for (const WCHAR* c = field; *c && n + 1 < 256; c++) {
        if (*c >= L'A' && *c <= L'Z')      folded[n++] = (WCHAR)(*c - L'A' + L'a');
        else if (*c >= L'a' && *c <= L'z') folded[n++] = *c;
        else if (*c >= L'0' && *c <= L'9') folded[n++] = *c;
        // Everything else -- spaces, underscores, dots, brackets -- is how a
        // form designer wrote the same word, not part of the word.
    }
    folded[n] = L'\0';

    WideCharToMultiByte(CP_UTF8, 0, folded, -1, out, (int)outBytes, NULL, NULL);
}

static void ToUtf8(const WCHAR* text, char* out, size_t outBytes) {
    WideCharToMultiByte(CP_UTF8, 0, text ? text : L"", -1, out, (int)outBytes, NULL, NULL);
}

static void ToWide(const char* text, WCHAR* out, size_t outChars) {
    out[0] = L'\0';
    if (!text) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, (int)outChars);
}

// ---------------------------------------------------------------------------
// Signatures
// ---------------------------------------------------------------------------

int Fill_SaveSignature(const WCHAR* name, const BYTE* png, size_t len) {
    sqlite3* db = Database_GetHandle();
    if (!db || !png || len == 0 || len > 4 * 1024 * 1024) return 0;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "INSERT INTO signatures (name, png) VALUES (?, ?)";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    char nameUtf8[256];
    ToUtf8(name && name[0] ? name : L"Signature", nameUtf8, sizeof(nameUtf8));

    sqlite3_bind_text(stmt, 1, nameUtf8, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, png, (int)len, SQLITE_TRANSIENT);

    int id = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) id = (int)sqlite3_last_insert_rowid(db);

    sqlite3_finalize(stmt);
    return id;
}

int Fill_ListSignatures(SignatureInfo* out, int max) {
    sqlite3* db = Database_GetHandle();
    if (!db || !out || max <= 0) return 0;

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT id, name, created_at, length(png) FROM signatures ORDER BY id DESC";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    int count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        SignatureInfo* item = &out[count++];

        item->id = sqlite3_column_int(stmt, 0);
        ToWide((const char*)sqlite3_column_text(stmt, 1), item->name, 64);
        ToWide((const char*)sqlite3_column_text(stmt, 2), item->createdAt, 32);
        item->bytes = sqlite3_column_int(stmt, 3);
    }

    sqlite3_finalize(stmt);
    return count;
}

BYTE* Fill_ReadSignature(int id, size_t* lenOut) {
    if (lenOut) *lenOut = 0;

    sqlite3* db = Database_GetHandle();
    if (!db || id <= 0) return NULL;

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT png FROM signatures WHERE id = ?", -1,
                           &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_int(stmt, 1, id);

    BYTE* bytes = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int len = sqlite3_column_bytes(stmt, 0);

        if (blob && len > 0) {
            bytes = (BYTE*)malloc((size_t)len);
            if (bytes) {
                memcpy(bytes, blob, (size_t)len);
                if (lenOut) *lenOut = (size_t)len;
            }
        }
    }

    sqlite3_finalize(stmt);
    return bytes;
}

BOOL Fill_DeleteSignature(int id) {
    sqlite3* db = Database_GetHandle();
    if (!db || id <= 0) return FALSE;

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM signatures WHERE id = ?", -1,
                           &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_int(stmt, 1, id);
    BOOL ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

BOOL Fill_RememberAnswer(const WCHAR* field, const WCHAR* value) {
    sqlite3* db = Database_GetHandle();
    if (!db || !field || !field[0] || !value || !value[0]) return FALSE;

    char key[256];
    NormaliseField(field, key, sizeof(key));
    if (!key[0]) return FALSE;

    char fieldUtf8[256], valueUtf8[1536];
    ToUtf8(field, fieldUtf8, sizeof(fieldUtf8));
    ToUtf8(value, valueUtf8, sizeof(valueUtf8));

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "INSERT INTO form_answers (field, label, value, updated_at) "
        "VALUES (?, ?, ?, datetime('now')) "
        "ON CONFLICT(field) DO UPDATE SET value = excluded.value, "
        "label = excluded.label, updated_at = datetime('now')";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return FALSE;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, fieldUtf8, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, valueUtf8, -1, SQLITE_TRANSIENT);

    BOOL ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

BOOL Fill_RecallAnswer(const WCHAR* field, WCHAR* out, size_t outChars) {
    if (out && outChars) out[0] = L'\0';

    sqlite3* db = Database_GetHandle();
    if (!db || !field || !field[0] || !out) return FALSE;

    char key[256];
    NormaliseField(field, key, sizeof(key));
    if (!key[0]) return FALSE;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT value FROM form_answers WHERE field = ?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return FALSE;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    BOOL found = FALSE;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ToWide((const char*)sqlite3_column_text(stmt, 0), out, outChars);
        found = out[0] != L'\0';
    }

    sqlite3_finalize(stmt);
    return found;
}

int Fill_ListAnswers(RememberedAnswer* out, int max) {
    sqlite3* db = Database_GetHandle();
    if (!db || !out || max <= 0) return 0;

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT field, label, value, updated_at FROM form_answers "
        "ORDER BY updated_at DESC";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    int count = 0;
    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        RememberedAnswer* item = &out[count++];

        // The label is how the question was last written; the key is what it
        // is matched on, and is shown only when nothing wrote a label.
        const char* label = (const char*)sqlite3_column_text(stmt, 1);
        ToWide(label && label[0] ? label : (const char*)sqlite3_column_text(stmt, 0),
               item->field, 128);

        ToWide((const char*)sqlite3_column_text(stmt, 2), item->value, 512);
        ToWide((const char*)sqlite3_column_text(stmt, 3), item->updatedAt, 32);
    }

    sqlite3_finalize(stmt);
    return count;
}

BOOL Fill_ForgetAnswer(const WCHAR* field) {
    sqlite3* db = Database_GetHandle();
    if (!db || !field || !field[0]) return FALSE;

    char key[256];
    NormaliseField(field, key, sizeof(key));
    if (!key[0]) return FALSE;

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM form_answers WHERE field = ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    BOOL ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

BOOL Fill_ForgetAllAnswers(void) {
    sqlite3* db = Database_GetHandle();
    if (!db) return FALSE;

    return sqlite3_exec(db, "DELETE FROM form_answers", NULL, NULL, NULL) == SQLITE_OK;
}

// ---------------------------------------------------------------------------
// Self-check
//
// Against a database of its own in the temp directory, which is deleted
// afterwards: a self-check that wrote to somebody's real notes would be a
// bug of its own.
// ---------------------------------------------------------------------------

BOOL Fill_SelfTest(char* failure, size_t failureSize) {
    WCHAR temp[MAX_PATH];
    WCHAR path[MAX_PATH];

    GetTempPathW(MAX_PATH, temp);
    swprintf_s(path, MAX_PATH, L"%sopennote-selftest-fill.db", temp);
    DeleteFileW(path);

    BOOL wasOpen = Database_IsOpen();
    if (wasOpen) Database_Close();

    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        Database_Close(); \
        DeleteFileW(path); \
        return FALSE; \
    } while (0)

    if (!Database_Open(path)) FAIL("a database for the check could not be opened");

    // Opening a file is not the same as it having tables in it.
    if (!Database_Initialize()) FAIL("the check's database has no schema in it");

    // --- signatures ---
    const BYTE picture[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4 };

    int id = Fill_SaveSignature(L"Test signature", picture, sizeof(picture));
    if (id <= 0) FAIL("a signature would not save");

    SignatureInfo listed[MAX_SIGNATURES];
    if (Fill_ListSignatures(listed, MAX_SIGNATURES) != 1) FAIL("the signature was not listed");
    if (wcscmp(listed[0].name, L"Test signature") != 0) FAIL("the signature lost its name");
    if (listed[0].bytes != (int)sizeof(picture)) FAIL("the signature changed size");

    size_t len = 0;
    BYTE* read = Fill_ReadSignature(id, &len);
    BOOL same = read && len == sizeof(picture) && memcmp(read, picture, len) == 0;
    free(read);
    if (!same) FAIL("the signature came back different");

    if (!Fill_DeleteSignature(id)) FAIL("the signature would not delete");
    if (Fill_ListSignatures(listed, MAX_SIGNATURES) != 0) FAIL("the deleted signature is still there");

    // --- answers ---
    if (!Fill_RememberAnswer(L"Full Name", L"Grace Hopper")) FAIL("an answer would not save");

    WCHAR recalled[256];
    if (!Fill_RecallAnswer(L"Full Name", recalled, 256) ||
        wcscmp(recalled, L"Grace Hopper") != 0) {
        FAIL("the answer did not come back");
    }

    // The point of the whole thing: the same question, spelled the way three
    // different form designers spell it.
    if (!Fill_RecallAnswer(L"full_name", recalled, 256) ||
        wcscmp(recalled, L"Grace Hopper") != 0) {
        FAIL("full_name did not match Full Name");
    }
    if (!Fill_RecallAnswer(L"FullName", recalled, 256)) FAIL("FullName did not match");
    if (!Fill_RecallAnswer(L"FULL NAME.", recalled, 256)) FAIL("punctuation and case were not ignored");

    // ...and a different question is a different question.
    if (Fill_RecallAnswer(L"full name of employer", recalled, 256)) {
        FAIL("a longer name matched a shorter one");
    }
    if (Fill_RecallAnswer(L"address", recalled, 256)) FAIL("something never answered came back");

    // Answering again replaces rather than piling up.
    if (!Fill_RememberAnswer(L"full_name", L"Grace Brewster Hopper")) FAIL("the answer would not update");

    RememberedAnswer answers[MAX_ANSWERS];
    int count = Fill_ListAnswers(answers, MAX_ANSWERS);
    if (count != 1) FAIL("the same question was remembered twice");
    if (wcscmp(answers[0].value, L"Grace Brewster Hopper") != 0) FAIL("the newer answer did not win");

    // The name is remembered as it was last written, not as the squashed key.
    if (wcscmp(answers[0].field, L"full_name") != 0) FAIL("the question was not remembered as asked");

    if (!Fill_ForgetAnswer(L"Full Name")) FAIL("an answer would not be forgotten");
    if (Fill_RecallAnswer(L"full_name", recalled, 256)) FAIL("a forgotten answer came back");

    Fill_RememberAnswer(L"one", L"1");
    Fill_RememberAnswer(L"two", L"2");
    if (!Fill_ForgetAllAnswers()) FAIL("forgetting everything failed");
    if (Fill_ListAnswers(answers, MAX_ANSWERS) != 0) FAIL("something survived forgetting everything");

    Database_Close();
    DeleteFileW(path);

    // Whatever was open before stays the caller's business; the check does
    // not put it back, because nothing else runs after it.
    (void)wasOpen;

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
