#ifndef DIALOGS_H
#define DIALOGS_H

// Dialog functions
void Dialogs_About(HWND hParent);
BOOL Dialogs_GoToLine(HWND hParent, int* line);
void Dialogs_Find(HWND hParent);
void Dialogs_Replace(HWND hParent);
BOOL Dialogs_Font(HWND hParent, LOGFONTW* font);
BOOL Dialogs_NotesBrowser(HWND hParent, int* noteId);
void Dialogs_Compare(HWND hParent, int tab1Index, int tab2Index);
void Dialogs_Defaults(HWND hParent);

// The OAuth application the user registered for themselves. Nothing ships with
// OpenNote, so this is how an installed copy is given credentials.
void Dialogs_SyncCredentials(HWND hParent);
void Dialogs_FindInTabs(HWND hParent);
void Dialogs_ReplaceInTabs(HWND hParent);

// Open/Save dialogs
BOOL Dialogs_OpenFile(HWND hParent, WCHAR* pathBuffer, int bufferSize);
BOOL Dialogs_SaveFile(HWND hParent, WCHAR* pathBuffer, int bufferSize, const WCHAR* defaultName);

// Confirmation dialogs
int Dialogs_SaveChanges(HWND hParent, const WCHAR* filename);  // Returns IDYES, IDNO, or IDCANCEL

// Input dialog
// What this machine has kept about the person using it, and a way to take it
// back.
void Dialogs_RememberedAnswers(HWND hParent);

// Choosing a signature to put on a page: one that was drawn before, one drawn
// now, or a picture from disk. Gives back the PNG bytes, which the caller
// frees. NULL when the user backs out.
BYTE* Dialogs_ChooseSignature(HWND hParent, size_t* lenOut);

// The form in a PDF: its fields, and filling them in. `savedTo` is given the
// file that was written, when one was.
BOOL Dialogs_PdfForm(HWND hParent, const WCHAR* pdfPath, WCHAR* savedTo, size_t savedChars);

// The document's comments: who said what, and a way to remove one. Returns
// TRUE when something was deleted, which is when the document has changed.
BOOL Dialogs_Comments(HWND hParent, DocModel* doc);

BOOL Dialogs_InputBox(HWND hParent, const WCHAR* title, const WCHAR* prompt, WCHAR* buffer, int bufferSize);

// Markdown preview
void Dialogs_MarkdownPreview(HWND hParent, HWND hEditor);

// Dialog procedures
INT_PTR CALLBACK GoToLineProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK NotesBrowserProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK DefaultsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK FindInTabsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK ReplaceInTabsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Find/Replace modeless dialog
HWND CreateFindDialog(HWND hParent, BOOL showReplace);
void FindDialog_SetFindText(const WCHAR* text);

#endif // DIALOGS_H
