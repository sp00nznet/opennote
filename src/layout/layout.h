#ifndef LAYOUT_H
#define LAYOUT_H

// The layout engine: DocModel -> pages.
//
// Windows supplies the hard half. IDWriteTextLayout does shaping, line
// breaking, justification, bidi and font fallback for one paragraph given a
// width. What the engine does is the part above that -- measuring those
// paragraphs, flowing them down a page, breaking one across a page boundary at
// a line, and placing tables.
//
// Deliberately free of any window and of Direct2D. The engine answers "where
// does everything go"; drawing it is somebody else's job. That is what lets
// one layout serve the screen, the printer and eventually a PDF, and it is
// what makes the engine checkable without a screen.
//
// This header is the C-visible half. DirectWrite's headers are C++ only, so
// the structures live in layout_internal.h, which only the engine and whatever
// renders it include.

typedef struct LayoutResult LayoutResult;

#ifdef __cplusplus
extern "C" {
#endif

// Lay a model out. `defaultFont` and `defaultSizePt` apply to runs that do not
// name their own. Returns NULL if DirectWrite is unavailable.
LayoutResult* Layout_Build(const DocModel* doc, const WCHAR* defaultFont,
                           float defaultSizePt);

void Layout_Free(LayoutResult* result);

int   Layout_PageCount(const LayoutResult* r);
void  Layout_PageSize(const LayoutResult* r, float* widthDip, float* heightDip);

// Bottom edge of the last thing placed on a page, in DIPs from the page top.
// Diagnostic -- the engine does not need it, but a check can assert with it.
float Layout_PageContentBottom(const LayoutResult* r, int pageIndex);

// Total number of laid-out text pieces on a page. A paragraph split across a
// page boundary counts once per page it appears on.
int   Layout_PageTextCount(const LayoutResult* r, int pageIndex);
int   Layout_PageCellCount(const LayoutResult* r, int pageIndex);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL Layout_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // LAYOUT_H
