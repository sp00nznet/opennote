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

// The same, for a document whose fields have to be answered: the date, the
// time, a cross-reference, a page number. Laying the document out is what
// reveals where everything landed, so this lays it out, writes the answers
// into the model and lays it out once more when they changed the text.
//
// Anything that shows or prints a document uses this; Layout_Build stays the
// one that does not touch what it is given.
LayoutResult* Layout_BuildUpdating(DocModel* doc, const WCHAR* defaultFont,
                                   float defaultSizePt);

// Write the answers to page-dependent fields into the model. Returns how many
// changed, which is how a caller knows whether to lay it out again.
int Layout_UpdateFields(const LayoutResult* r, DocModel* doc);

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
int   Layout_PageImageCount(const LayoutResult* r, int pageIndex);

// ---------------------------------------------------------------------------
// Geometry: where a position is, and what is under a point
//
// The other half of laying a document out. A page of text is only an editor
// once a click can name a character and a character can name a place on the
// page -- and both of those are questions about geometry, which is why they
// live with the engine rather than with the window that draws it.
// ---------------------------------------------------------------------------

typedef struct {
    const DocPara* para;
    unsigned       offset;    // into the paragraph's text
} LayoutPos;

typedef struct {
    float x, y, width, height;   // page DIPs, from the paper corner
} LayoutRect;

// What is under a point on a page, in page DIPs. Answers the nearest position
// when the point is in a margin, because a click below the last line of a page
// means the end of it rather than nothing.
BOOL Layout_HitTest(const LayoutResult* r, int page, float x, float y, LayoutPos* out);

// Where a position is: which page it landed on, and the caret rectangle.
BOOL Layout_PosRect(const LayoutResult* r, LayoutPos pos, int* pageOut, LayoutRect* out);

// The rectangles covering [a, b) on one page, for drawing a selection.
// Returns how many were written.
int Layout_RangeRects(const LayoutResult* r, int page, LayoutPos a, LayoutPos b,
                      LayoutRect* out, int cap);

// Document order: negative, zero or positive.
int Layout_ComparePos(const LayoutResult* r, LayoutPos a, LayoutPos b);

// One line up (delta < 0) or down (delta > 0), keeping the horizontal place --
// including across a page boundary, which is where a document stops being one
// long column and starts being pages.
BOOL Layout_MoveLine(const LayoutResult* r, LayoutPos pos, int delta, LayoutPos* out);

// The start or end of the line a position is on, which is what Home and End
// mean once text is wrapped: the line, not the paragraph.
BOOL Layout_LineEdge(const LayoutResult* r, LayoutPos pos, BOOL end, LayoutPos* out);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL Layout_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // LAYOUT_H
