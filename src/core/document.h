#ifndef DOCUMENT_H
#define DOCUMENT_H

// Document structure
struct Document {
    DocumentType type;
    DocumentFormat format;   // FORMAT_PLAIN or FORMAT_RTF -- picks the view
    TextEncoding encoding;

    // File-based document
    WCHAR filePath[MAX_PATH];

    // Note-based document
    int noteId;
    WCHAR noteTitle[MAX_TITLE_LEN];

    // The model this document was read from, when it came from a format that
    // has one. The view cannot hold everything a file states -- a picture's
    // bytes, a style's name -- so the original is kept beside it and consulted
    // when the document is written back out.
    DocModel* source;

    // State
    BOOL modified;
    BOOL isNew;  // Never saved

    // Display title
    WCHAR title[MAX_TITLE_LEN];
};

// Document creation/destruction
Document* Document_Create(void);
Document* Document_CreateFromFile(const WCHAR* path);
Document* Document_CreateFromNote(int noteId);
void Document_Destroy(Document* doc);

// Document operations
BOOL Document_Save(Document* doc, HWND hEditor);
BOOL Document_SaveAs(Document* doc, HWND hEditor, const WCHAR* path);
BOOL Document_Load(Document* doc, HWND hEditor);

// Properties
const WCHAR* Document_GetTitle(Document* doc);
void Document_UpdateTitle(Document* doc);
BOOL Document_IsModified(Document* doc);
void Document_SetModified(Document* doc, BOOL modified);

// Which format a path implies, from its extension. Used to decide which view
// a tab needs before the document is loaded into it.
DocumentFormat Document_FormatForPath(const WCHAR* path);

// Type checking
BOOL Document_IsFile(Document* doc);
BOOL Document_IsNote(Document* doc);
BOOL Document_IsNew(Document* doc);

#endif // DOCUMENT_H
