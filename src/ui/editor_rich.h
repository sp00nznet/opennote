#ifndef EDITOR_RICH_H
#define EDITOR_RICH_H

// The rich text view: a RichEdit 4.1 control from Msftedit.dll, which is what
// WordPad was built on and which reads and writes RTF natively.
//
// These functions are the RichEdit half of the Editor_* API. Callers outside
// editor.c should not reach for them directly -- Editor_* dispatches here when
// the HWND is a rich view. See ROADMAP.md v0.5.

// Loads Msftedit.dll and registers the control class. Rich_Create does this
// itself; callers that create a RichEdit window directly must call it first or
// CreateWindowEx fails with an unregistered class.
BOOL Rich_EnsureLoaded(void);

HWND Rich_Create(HWND hParent);

// Text
void   Rich_SetText(HWND h, const WCHAR* text);
WCHAR* Rich_GetText(HWND h);            // Caller must free
int    Rich_GetTextLength(HWND h);

// RTF. The string forms back note storage; the file forms back .rtf documents.
BOOL   Rich_LoadRtfFile(HWND h, const WCHAR* path);
BOOL   Rich_SaveRtfFile(HWND h, const WCHAR* path);
BOOL   Rich_SetRtf(HWND h, const char* rtf);
char*  Rich_GetRtf(HWND h);             // Caller must free. UTF-8/ASCII RTF.

// Selection and position
void   Rich_GetSelection(HWND h, int* start, int* end);
void   Rich_SetSelection(HWND h, int start, int end);
void   Rich_ReplaceSelection(HWND h, const WCHAR* text);
WCHAR* Rich_GetSelectedText(HWND h);    // Caller must free
void   Rich_ReplaceRange(HWND h, int start, int end, const WCHAR* text);
WCHAR* Rich_GetWordAt(HWND h, int pos, int* wordStart, int* wordEnd);
int    Rich_GetPositionFromPoint(HWND h, int x, int y);

// Lines
void   Rich_GetCursorPos(HWND h, int* line, int* column);
void   Rich_GotoLine(HWND h, int line);
int    Rich_GetLineCount(HWND h);
int    Rich_GetCurrentLine(HWND h);

// History and clipboard
BOOL   Rich_CanUndo(HWND h);
BOOL   Rich_CanRedo(HWND h);
void   Rich_Undo(HWND h);
void   Rich_Redo(HWND h);
void   Rich_Cut(HWND h);
void   Rich_Copy(HWND h);
void   Rich_Paste(HWND h);

// Appearance
void   Rich_SetWordWrap(HWND h, BOOL wrap);
void   Rich_SetFont(HWND h, HFONT hFont);
void   Rich_SetZoom(HWND h, int zoomPercent);

// Modification state
BOOL   Rich_GetModified(HWND h);
void   Rich_SetModified(HWND h, BOOL modified);

// Find and replace
int    Rich_FindText(HWND h, const WCHAR* text, BOOL matchCase, BOOL wholeWord, BOOL forward);
int    Rich_ReplaceText(HWND h, const WCHAR* findText, const WCHAR* replaceText, BOOL matchCase, BOOL wholeWord);
int    Rich_ReplaceAll(HWND h, const WCHAR* findText, const WCHAR* replaceText, BOOL matchCase, BOOL wholeWord);

// ---------------------------------------------------------------------------
// Character and paragraph formatting -- the part a plain text view has no
// answer for, and the reason this view exists.
// ---------------------------------------------------------------------------

// Toggle an effect over the selection. CFE_BOLD, CFE_ITALIC, CFE_UNDERLINE,
// CFE_STRIKEOUT, CFE_SUBSCRIPT, CFE_SUPERSCRIPT.
void Rich_ToggleEffect(HWND h, DWORD effect);
BOOL Rich_HasEffect(HWND h, DWORD effect);

void Rich_SetFontName(HWND h, const WCHAR* name);
void Rich_SetFontSize(HWND h, int points);
void Rich_SetTextColor(HWND h, COLORREF color);
void Rich_SetHighlightColor(HWND h, COLORREF color, BOOL none);

// Reports what is under the caret, for the toolbar to reflect.
BOOL Rich_GetFontName(HWND h, WCHAR* out, size_t outChars);
int  Rich_GetFontSize(HWND h);

// PFA_LEFT, PFA_CENTER, PFA_RIGHT, PFA_JUSTIFY
void Rich_SetAlignment(HWND h, WORD alignment);
WORD Rich_GetAlignment(HWND h);

// PFN_BULLET, or 0 for none. RichEdit 4.1 also accepts PFN_ARABIC and friends.
void Rich_SetListStyle(HWND h, WORD numbering);
WORD Rich_GetListStyle(HWND h);

// Indent the selected paragraphs by one step, or back by one.
void Rich_Indent(HWND h, BOOL increase);

// Line spacing in multiples of single: 10 = single, 15 = 1.5, 20 = double.
void Rich_SetLineSpacing(HWND h, int spacingTenths);

// Common dialogs, applied to the selection.
void Rich_ChooseFont(HWND h, HWND hOwner);
void Rich_ChooseColor(HWND h, HWND hOwner);

// Drop every character and paragraph attribute from the selection.
void Rich_ClearFormatting(HWND h);

// Insert a picture from a file at the caret.
BOOL Rich_InsertPicture(HWND h, const WCHAR* path);

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------

// Render the document to a printer DC across as many pages as it takes.
// `rcPageTwips` is the printable area in twips. `outputFile` names the file a
// print-to-file device writes to, and is NULL for a real printer. Returns
// pages printed.
int Rich_PrintToDC(HWND h, HDC hDC, const RECT* rcPageTwips, const WCHAR* docTitle,
                   const WCHAR* outputFile);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL Rich_SelfTest(char* failure, size_t failureSize);

#endif // EDITOR_RICH_H
