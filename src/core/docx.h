#ifndef DOCX_H
#define DOCX_H

// .docx support -- ECMA-376 WordprocessingML.
//
// A .docx is an Open Packaging Conventions container: a zip of XML parts with a
// relationship graph over them. Windows ships an API for exactly that shape
// (msopc.dll, IOpcFactory) and an XML pull reader (xmllite.dll, IXmlReader), so
// neither the container nor the parser is code this project has to own.
//
// ponytail: reading converts WordprocessingML to RTF and hands it to the rich
// text view that already exists, rather than to a layout engine that does not.
// It reuses everything and the ceiling is the one already documented for that
// view -- weak tables, no page boundaries. The DirectWrite engine that lifts it
// is v0.7; this file feeds that engine just as well when it arrives.

// Does this path look like a .docx? Extension only -- the content check happens
// when it is opened, where a failure can be reported.
BOOL Docx_IsDocxPath(const WCHAR* path);

// Read a .docx into a document model. Caller frees with Doc_Free().
// Returns NULL if the file is not a readable WordprocessingML package.
DocModel* Docx_ReadToModel(const WCHAR* path);

// Read a .docx and produce RTF for the view. Convenience over
// Docx_ReadToModel + DocRtf_Emit. Caller frees with free().
char* Docx_ReadToRtf(const WCHAR* path);

// Write a model out as a .docx.
BOOL Docx_WriteModel(const DocModel* doc, const WCHAR* path);

// Capture a rich text editor window into a model and write that.
BOOL Docx_WriteFromEditor(HWND hRichEdit, const WCHAR* path);

// Human-readable reason the last read or write failed, for the message box.
const WCHAR* Docx_GetLastError(void);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL Docx_SelfTest(char* failure, size_t failureSize);

#endif // DOCX_H
