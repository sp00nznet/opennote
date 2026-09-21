#ifndef LAYOUT_INTERNAL_H
#define LAYOUT_INTERNAL_H

// The engine's structures. DirectWrite's headers are C++ only, so this is
// included by the engine and by whatever renders its output -- never by the
// C half of the project.

#include <dwrite.h>
#include "core/doctree.h"

#define TWIPS_PER_DIP 15.0f

// A stretch of a paragraph's text carrying one set of character properties.
// This is the shape IDWriteTextLayout wants: one string plus ranges over it.
struct TextSpan {
    UINT32    start;
    UINT32    len;
    CharProps props;
};

// Colour is a drawing effect rather than a layout property, so it is carried
// beside the layout for whoever draws it. Keeping D2D brushes out of the
// engine is the reason it has no Direct2D dependency at all.
struct ColorSpan {
    UINT32   start;
    UINT32   len;
    COLORREF color;
};

// One laid-out piece of text at a position on a page. A paragraph too tall for
// the space left is split into several of these across pages, which is why a
// laid-out piece and the paragraph it came from are not the same thing.
struct LaidText {
    IDWriteTextLayout* layout;    // owned
    float x, y;                   // top-left, DIPs from the page corner
    float width, height;
    const DocPara*     para;      // what it came from; not owned
    BOOL               isCellText;

    ColorSpan*         colors;    // owned; empty when the text is all default
    int                colorCount;
};

// A table cell's box on a page, for drawing its borders.
struct LaidCell {
    float x, y, width, height;
};

struct LaidPage {
    LaidText* texts;
    int       textCount, textCap;

    LaidCell* cells;
    int       cellCount, cellCap;
};

struct LayoutResult {
    LaidPage* pages;
    int       pageCount, pageCap;

    float pageWidth, pageHeight;          // DIPs
    float marginLeft, marginTop;
    float marginRight, marginBottom;

    IDWriteFactory* dwrite;               // owned
};

#endif // LAYOUT_INTERNAL_H
