#include "supernote.h"
#include "res/resource.h"

// Which view a file wants, decided by extension.
//
// .rtf and .docx open in the rich view. Legacy binary .doc is a different
// format entirely ([MS-DOC] over [MS-CFB]) and is not claimed here -- opening
// one and showing the user nothing would be worse than not offering.
DocumentFormat Document_FormatForPath(const WCHAR* path) {
    if (!path) return FORMAT_PLAIN;

    const WCHAR* ext = wcsrchr(path, L'.');
    if (!ext) return FORMAT_PLAIN;

    if (_wcsicmp(ext, L".rtf") == 0)  return FORMAT_RTF;
    if (_wcsicmp(ext, L".docx") == 0) return FORMAT_DOCX;
    if (_wcsicmp(ext, L".pdf") == 0)  return FORMAT_PDF;
    return FORMAT_PLAIN;
}

// Create new document
Document* Document_Create(void) {
    Document* doc = (Document*)calloc(1, sizeof(Document));
    if (!doc) return NULL;

    doc->type = DOC_TYPE_FILE;
    doc->format = FORMAT_PLAIN;
    doc->encoding = ENCODING_UTF8;
    doc->isNew = TRUE;
    doc->modified = FALSE;
    wcscpy_s(doc->title, MAX_TITLE_LEN, L"Untitled");

    return doc;
}

// Create document from file
Document* Document_CreateFromFile(const WCHAR* path) {
    Document* doc = Document_Create();
    if (!doc) return NULL;

    doc->type = DOC_TYPE_FILE;
    doc->format = Document_FormatForPath(path);
    doc->isNew = FALSE;
    wcscpy_s(doc->filePath, MAX_PATH, path);
    Document_UpdateTitle(doc);

    // Also create a note entry so it appears in Notes Browser. For an RTF file
    // the markup itself is not what anyone wants to full-text search, so the
    // note gets the text and the file keeps the formatting.
    if (Database_IsOpen() && !FORMAT_IS_RICH(doc->format)) {
        // Read file content to store in database
        TextEncoding encoding = ENCODING_UTF8;
        WCHAR* content = FileIO_ReadFile(path, &encoding);
        int noteId = Notes_Create(doc->title, content ? content : L"");
        if (content) free(content);
        if (noteId > 0) {
            doc->noteId = noteId;
        }
    }

    return doc;
}

// Create document from note
Document* Document_CreateFromNote(int noteId) {
    Document* doc = Document_Create();
    if (!doc) return NULL;

    doc->type = DOC_TYPE_NOTE;
    doc->noteId = noteId;
    doc->isNew = FALSE;

    // Get title from database
    Notes_GetTitle(noteId, doc->noteTitle, MAX_TITLE_LEN);
    Document_UpdateTitle(doc);

    return doc;
}

// Destroy document
void Document_Destroy(Document* doc) {
    if (doc) {
        Doc_Free(doc->source);
        free(doc);
    }
}

// Save document
BOOL Document_Save(Document* doc, HWND hEditor) {
    if (!doc || !hEditor) return FALSE;

    // An RTF document is written by the control, not by FileIO: passing it
    // through Editor_GetText would save the plain text and throw away every
    // bit of formatting the user just applied.
    if (FORMAT_IS_RICH(doc->format) && doc->type == DOC_TYPE_FILE) {
        if (doc->isNew || !doc->filePath[0]) {
            WCHAR path[MAX_PATH] = {0};
            if (Dialogs_SaveFile(g_app->hMainWindow, path, MAX_PATH, doc->title)) {
                return Document_SaveAs(doc, hEditor, path);
            }
            return FALSE;
        }

        BOOL saved = (doc->format == FORMAT_DOCX)
            ? Docx_WriteFromEditorWith(hEditor, doc->filePath, doc->source)
            : Rich_SaveRtfFile(hEditor, doc->filePath);

        if (!saved) {
            MessageBoxW(g_app->hMainWindow, Docx_GetLastError(), APP_NAME, MB_ICONERROR);
            return FALSE;
        }

        doc->modified = FALSE;
        Editor_SetModified(hEditor, FALSE);

        // Keep the searchable copy in the notes database in step, as text.
        if (doc->noteId > 0 && Database_IsOpen()) {
            WCHAR* text = Editor_GetText(hEditor);
            if (text) {
                Notes_Update(doc->noteId, doc->title, text);
                free(text);
            }
        }
        return TRUE;
    }

    WCHAR* content = Editor_GetText(hEditor);
    if (!content) return FALSE;

    BOOL result = FALSE;

    if (doc->type == DOC_TYPE_NOTE) {
        // Save to database
        result = Notes_Update(doc->noteId, doc->noteTitle, content);
    } else {
        // File document
        if (doc->isNew || !doc->filePath[0]) {
            // Need Save As
            free(content);
            WCHAR path[MAX_PATH] = {0};
            if (Dialogs_SaveFile(g_app->hMainWindow, path, MAX_PATH, doc->title)) {
                return Document_SaveAs(doc, hEditor, path);
            }
            return FALSE;
        }

        // Save to existing path
        result = FileIO_WriteFile(doc->filePath, content, doc->encoding);

        // Also update note in database if we have a noteId
        if (result && doc->noteId > 0 && Database_IsOpen()) {
            Notes_Update(doc->noteId, doc->title, content);
        }
    }

    free(content);

    if (result) {
        doc->modified = FALSE;
        Editor_SetModified(hEditor, FALSE);
    }

    return result;
}

// Save document as
BOOL Document_SaveAs(Document* doc, HWND hEditor, const WCHAR* path) {
    if (!doc || !hEditor || !path) return FALSE;

    // Saving a rich document under a name that is not .rtf would quietly drop
    // its formatting, so the format follows the view rather than the extension.
    if (Editor_IsRich(hEditor)) {
        // The name chosen in the Save As dialog decides the storage format --
        // that is the one place the user says which they want.
        DocumentFormat want = Document_FormatForPath(path);
        if (!FORMAT_IS_RICH(want)) want = FORMAT_RTF;

        BOOL saved = (want == FORMAT_DOCX)
            ? Docx_WriteFromEditorWith(hEditor, path, doc->source)
            : Rich_SaveRtfFile(hEditor, path);

        if (!saved) {
            MessageBoxW(g_app->hMainWindow, Docx_GetLastError(), APP_NAME, MB_ICONERROR);
            return FALSE;
        }

        doc->type = DOC_TYPE_FILE;
        doc->format = want;
        doc->isNew = FALSE;
        doc->modified = FALSE;
        wcscpy_s(doc->filePath, MAX_PATH, path);
        Document_UpdateTitle(doc);
        Editor_SetModified(hEditor, FALSE);

        if (Database_IsOpen()) {
            WCHAR* text = Editor_GetText(hEditor);
            if (text) {
                if (doc->noteId > 0) Notes_Update(doc->noteId, doc->title, text);
                else                 doc->noteId = Notes_Create(doc->title, text);
                free(text);
            }
        }
        return TRUE;
    }

    WCHAR* content = Editor_GetText(hEditor);
    if (!content) return FALSE;

    BOOL result = FileIO_WriteFile(path, content, doc->encoding);

    if (result) {
        doc->type = DOC_TYPE_FILE;
        doc->isNew = FALSE;
        doc->modified = FALSE;
        wcscpy_s(doc->filePath, MAX_PATH, path);
        Document_UpdateTitle(doc);
        Editor_SetModified(hEditor, FALSE);
        // Update syntax highlighting for new file extension
        Editor_SetLexerFromExtension(hEditor, path);

        // Create/update note in database
        if (Database_IsOpen()) {
            if (doc->noteId > 0) {
                Notes_Update(doc->noteId, doc->title, content);
            } else {
                doc->noteId = Notes_Create(doc->title, content);
            }
        }
    }

    free(content);
    return result;
}

// Load document
BOOL Document_Load(Document* doc, HWND hEditor) {
    if (!doc || !hEditor) return FALSE;

    // A PDF is shown by a view of its own, which opens the file itself. There
    // is nothing for the editor to hold.
    if (doc->format == FORMAT_PDF) {
        doc->modified = FALSE;
        return TRUE;
    }

    if (doc->type == DOC_TYPE_NOTE) {
        // Load from database
        WCHAR* content = Notes_GetContent(doc->noteId);
        if (content) {
            Editor_SetText(hEditor, content);
            free(content);
            doc->modified = FALSE;
            // Try to set lexer based on note title extension
            Editor_SetLexerFromExtension(hEditor, doc->noteTitle);
            // Refresh links from database
            Editor_RefreshLinks(hEditor, doc);
            return TRUE;
        }
        return FALSE;
    }

    // Load from file
    if (!doc->filePath[0]) return FALSE;

    if (doc->format == FORMAT_RTF) {
        if (!Rich_LoadRtfFile(hEditor, doc->filePath)) return FALSE;
        doc->modified = FALSE;
        return TRUE;
    }

    if (doc->format == FORMAT_DOCX) {
        // WordprocessingML is converted to RTF and handed to the same view --
        // see docx.h for why.
        // Read once into a model, kept; the view gets RTF made from it.
        DocModel* model = Docx_ReadToModel(doc->filePath);
        if (!model) {
            MessageBoxW(g_app->hMainWindow, Docx_GetLastError(), APP_NAME, MB_ICONWARNING);
            return FALSE;
        }

        char* rtf = DocRtf_Emit(model);
        if (!rtf) {
            Doc_Free(model);
            return FALSE;
        }

        BOOL ok = Rich_SetRtf(hEditor, rtf);
        free(rtf);
        if (!ok) {
            Doc_Free(model);
            return FALSE;
        }

        Doc_Free(doc->source);
        doc->source = model;
        doc->modified = FALSE;
        return TRUE;
    }

    TextEncoding encoding = ENCODING_UTF8;
    WCHAR* content = FileIO_ReadFile(doc->filePath, &encoding);
    if (content) {
        doc->encoding = encoding;
        Editor_SetText(hEditor, content);
        free(content);
        doc->modified = FALSE;
        // Set syntax highlighting based on file extension
        Editor_SetLexerFromExtension(hEditor, doc->filePath);
        // Refresh links from database
        Editor_RefreshLinks(hEditor, doc);
        return TRUE;
    }

    return FALSE;
}

// Get display title
const WCHAR* Document_GetTitle(Document* doc) {
    if (!doc) return L"Untitled";
    return doc->title;
}

// Update title based on state
void Document_UpdateTitle(Document* doc) {
    if (!doc) return;

    if (doc->type == DOC_TYPE_NOTE) {
        wcscpy_s(doc->title, MAX_TITLE_LEN, doc->noteTitle);
    } else if (doc->filePath[0]) {
        // Extract filename from path
        const WCHAR* filename = wcsrchr(doc->filePath, L'\\');
        if (filename) {
            wcscpy_s(doc->title, MAX_TITLE_LEN, filename + 1);
        } else {
            wcscpy_s(doc->title, MAX_TITLE_LEN, doc->filePath);
        }
    } else {
        wcscpy_s(doc->title, MAX_TITLE_LEN, L"Untitled");
    }
}

// Check if modified
BOOL Document_IsModified(Document* doc) {
    return doc ? doc->modified : FALSE;
}

// Set modified state
void Document_SetModified(Document* doc, BOOL modified) {
    if (doc) {
        doc->modified = modified;
    }
}

// Type checking
BOOL Document_IsFile(Document* doc) {
    return doc && doc->type == DOC_TYPE_FILE;
}

BOOL Document_IsNote(Document* doc) {
    return doc && doc->type == DOC_TYPE_NOTE;
}

BOOL Document_IsNew(Document* doc) {
    return doc ? doc->isNew : TRUE;
}
