// The layout engine.
//
// DirectWrite lays out one paragraph at a time: given a width it shapes the
// text, breaks it into lines, justifies, falls back across fonts and reports
// exactly where every line sits. This file is the part above that -- taking
// those measured paragraphs and flowing them down a page, breaking a paragraph
// across a page boundary at a line, and placing tables.
//
// C++ rather than C only because DirectWrite's headers are C++ only; there is
// no C binding for it the way there is for the packaging and XML APIs. The
// interface it presents back to the rest of the project is plain C.

#include "supernote.h"
#include "layout/layout.h"
#include "layout/layout_internal.h"

#pragma comment(lib, "dwrite.lib")

// Padding between a cell's edge and its text.
static const float CELL_PAD_DIP = 4.0f;

// A paragraph flattened into one string plus the formatting spans over it.
struct FlatText {
    WCHAR*    text;
    UINT32    len;
    TextSpan* spans;
    int       spanCount;
};

// ---------------------------------------------------------------------------
// Growable arrays
// ---------------------------------------------------------------------------

static LaidPage* AddPage(LayoutResult* r) {
    if (r->pageCount == r->pageCap) {
        int cap = r->pageCap ? r->pageCap * 2 : 8;
        LaidPage* grown = (LaidPage*)realloc(r->pages, (size_t)cap * sizeof(LaidPage));
        if (!grown) return NULL;
        r->pages = grown;
        r->pageCap = cap;
    }
    LaidPage* p = &r->pages[r->pageCount++];
    memset(p, 0, sizeof(*p));
    return p;
}

static LaidText* AddText(LaidPage* page) {
    if (page->textCount == page->textCap) {
        int cap = page->textCap ? page->textCap * 2 : 16;
        LaidText* grown = (LaidText*)realloc(page->texts, (size_t)cap * sizeof(LaidText));
        if (!grown) return NULL;
        page->texts = grown;
        page->textCap = cap;
    }
    LaidText* t = &page->texts[page->textCount++];
    memset(t, 0, sizeof(*t));
    return t;
}

static LaidCell* AddCell(LaidPage* page) {
    if (page->cellCount == page->cellCap) {
        int cap = page->cellCap ? page->cellCap * 2 : 16;
        LaidCell* grown = (LaidCell*)realloc(page->cells, (size_t)cap * sizeof(LaidCell));
        if (!grown) return NULL;
        page->cells = grown;
        page->cellCap = cap;
    }
    LaidCell* c = &page->cells[page->cellCount++];
    memset(c, 0, sizeof(*c));
    return c;
}

// ---------------------------------------------------------------------------
// Flattening a paragraph
// ---------------------------------------------------------------------------

static void FlatFree(FlatText* f) {
    free(f->text);
    free(f->spans);
    memset(f, 0, sizeof(*f));
}

static BOOL FlattenPara(const DocPara* para, FlatText* out) {
    memset(out, 0, sizeof(*out));

    size_t cap = 256, len = 0;
    WCHAR* text = (WCHAR*)malloc(cap * sizeof(WCHAR));
    if (!text) return FALSE;

    int spanCap = 16, spanCount = 0;
    TextSpan* spans = (TextSpan*)malloc((size_t)spanCap * sizeof(TextSpan));
    if (!spans) {
        free(text);
        return FALSE;
    }

    for (const DocRun* r = para->runs; r; r = r->next) {
        const WCHAR* piece;
        size_t pieceLen;
        WCHAR one[2];

        if (r->tab) {
            one[0] = L'\t'; one[1] = 0; piece = one; pieceLen = 1;
        } else if (r->lineBreak) {
            // A hard break inside the paragraph, which DirectWrite treats as a
            // line separator rather than a new paragraph. That is right.
            one[0] = L'\n'; one[1] = 0; piece = one; pieceLen = 1;
        } else {
            piece = r->text;
            pieceLen = wcslen(r->text);
            if (!pieceLen) continue;
        }

        while (len + pieceLen + 1 > cap) {
            cap *= 2;
            WCHAR* g = (WCHAR*)realloc(text, cap * sizeof(WCHAR));
            if (!g) { free(text); free(spans); return FALSE; }
            text = g;
        }
        memcpy(text + len, piece, pieceLen * sizeof(WCHAR));

        if (spanCount == spanCap) {
            spanCap *= 2;
            TextSpan* g = (TextSpan*)realloc(spans, (size_t)spanCap * sizeof(TextSpan));
            if (!g) { free(text); free(spans); return FALSE; }
            spans = g;
        }
        spans[spanCount].start = (UINT32)len;
        spans[spanCount].len = (UINT32)pieceLen;
        spans[spanCount].props = r->props;
        spanCount++;

        len += pieceLen;
    }

    text[len] = L'\0';

    out->text = text;
    out->len = (UINT32)len;
    out->spans = spans;
    out->spanCount = spanCount;
    return TRUE;
}

// The spans covering [from, end), rebased to zero so a paragraph's tail can be
// laid out as its own layout when it is split across a page boundary.
static int RebaseSpans(const FlatText* f, UINT32 from, TextSpan* out, int outCap) {
    int n = 0;
    for (int i = 0; i < f->spanCount && n < outCap; i++) {
        UINT32 s = f->spans[i].start;
        UINT32 e = s + f->spans[i].len;
        if (e <= from) continue;
        if (s < from) s = from;

        out[n].start = s - from;
        out[n].len = e - s;
        out[n].props = f->spans[i].props;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Building a DirectWrite layout for one paragraph
// ---------------------------------------------------------------------------

struct Ctx {
    IDWriteFactory*    dwrite;
    IDWriteTextFormat* baseFormat;
    float              defaultSizeDip;
};

static void ApplySpans(IDWriteTextLayout* layout, const TextSpan* spans, int count,
                       const Ctx* ctx) {
    for (int i = 0; i < count; i++) {
        DWRITE_TEXT_RANGE range = { spans[i].start, spans[i].len };
        const CharProps* p = &spans[i].props;

        if (p->font[0]) layout->SetFontFamilyName(p->font, range);

        if (p->halfPoints > 0) {
            // Half-points to DIPs: one point is 96/72 DIPs.
            float dip = (p->halfPoints / 2.0f) * (96.0f / 72.0f);
            layout->SetFontSize(dip, range);
        }
        if (p->bold)      layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        if (p->italic)    layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (p->underline) layout->SetUnderline(TRUE, range);
        if (p->strike)    layout->SetStrikethrough(TRUE, range);

        // Super and subscript are OpenType features rather than layout
        // properties. A font without them renders at the normal position,
        // which is a graceful loss rather than text in the wrong place.
        if (p->superscript || p->subscript) {
            IDWriteTypography* typo = NULL;
            if (SUCCEEDED(ctx->dwrite->CreateTypography(&typo))) {
                DWRITE_FONT_FEATURE feat = {
                    p->superscript ? DWRITE_FONT_FEATURE_TAG_SUPERSCRIPT
                                   : DWRITE_FONT_FEATURE_TAG_SUBSCRIPT,
                    1
                };
                typo->AddFontFeature(feat);
                layout->SetTypography(typo, range);
                typo->Release();
            }
        }
    }
}

static BOOL CaptureColors(LaidText* t, const TextSpan* spans, int count) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (spans[i].props.hasColor) n++;
    }
    if (!n) return TRUE;

    t->colors = (ColorSpan*)calloc((size_t)n, sizeof(ColorSpan));
    if (!t->colors) return FALSE;

    int k = 0;
    for (int i = 0; i < count; i++) {
        if (!spans[i].props.hasColor) continue;
        t->colors[k].start = spans[i].start;
        t->colors[k].len = spans[i].len;
        t->colors[k].color = spans[i].props.color;
        k++;
    }
    t->colorCount = n;
    return TRUE;
}

static IDWriteTextLayout* MakeLayout(const Ctx* ctx, const WCHAR* text, UINT32 len,
                                     const TextSpan* spans, int spanCount,
                                     float width, const ParaProps* pp) {
    IDWriteTextLayout* layout = NULL;
    if (FAILED(ctx->dwrite->CreateTextLayout(text, len, ctx->baseFormat,
                                             width, 100000.0f, &layout))) {
        return NULL;
    }

    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (pp->align) {
        case ALIGN_CENTER:  align = DWRITE_TEXT_ALIGNMENT_CENTER;    break;
        case ALIGN_RIGHT:   align = DWRITE_TEXT_ALIGNMENT_TRAILING;  break;
        case ALIGN_JUSTIFY: align = DWRITE_TEXT_ALIGNMENT_JUSTIFIED; break;
        default: break;
    }
    layout->SetTextAlignment(align);

    if (pp->lineSpacing > 0) {
        // The model stores twentieths of a line; DirectWrite wants an absolute
        // height, taken here relative to the document's default size.
        float mult = pp->lineSpacing / 20.0f;
        float line = ctx->defaultSizeDip * 1.2f * mult;
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, line * 0.8f);
    }

    ApplySpans(layout, spans, spanCount, ctx);
    return layout;
}

static float LayoutHeight(IDWriteTextLayout* layout) {
    DWRITE_TEXT_METRICS m = {};
    if (FAILED(layout->GetMetrics(&m))) return 0.0f;
    return m.height;
}

// How many characters fit within `avail` height, at a line boundary. Returns 0
// when not even the first line fits.
static UINT32 SplitOffset(IDWriteTextLayout* layout, float avail, float* usedHeight) {
    UINT32 count = 0;
    layout->GetLineMetrics(NULL, 0, &count);
    if (!count) return 0;

    DWRITE_LINE_METRICS* lines =
        (DWRITE_LINE_METRICS*)calloc(count, sizeof(DWRITE_LINE_METRICS));
    if (!lines) return 0;

    UINT32 actual = 0;
    if (FAILED(layout->GetLineMetrics(lines, count, &actual))) {
        free(lines);
        return 0;
    }

    float h = 0.0f;
    UINT32 chars = 0;
    UINT32 fitted = 0;

    for (UINT32 i = 0; i < actual; i++) {
        if (h + lines[i].height > avail) break;
        h += lines[i].height;
        chars += lines[i].length;
        fitted++;
    }

    free(lines);

    if (usedHeight) *usedHeight = h;
    return fitted ? chars : 0;
}

// ---------------------------------------------------------------------------
// Flowing
// ---------------------------------------------------------------------------

struct Flow {
    LayoutResult* result;
    LaidPage*     page;
    float         y;              // pen position, DIPs from the page top
    float         contentWidth;
    float         bottom;         // nothing may be placed beyond this
    Ctx           ctx;
};

static void NewPage(Flow* f) {
    f->page = AddPage(f->result);
    f->y = f->result->marginTop;
}

static float SpaceLeft(const Flow* f) {
    return f->bottom - f->y;
}

static LaidText* Place(Flow* f, IDWriteTextLayout* layout, float x, float width,
                       const DocPara* para, BOOL isCellText,
                       const TextSpan* spans, int spanCount) {
    LaidText* t = AddText(f->page);
    if (!t) {
        layout->Release();
        return NULL;
    }
    t->layout = layout;
    t->x = x;
    t->y = f->y;
    t->width = width;
    t->height = LayoutHeight(layout);
    t->para = para;
    t->isCellText = isCellText;
    CaptureColors(t, spans, spanCount);
    return t;
}

// Place a paragraph, splitting it across pages at line boundaries when it does
// not fit. A paragraph taller than a whole page is split as many times as it
// takes rather than being dropped or allowed to overflow.
static void PlacePara(Flow* f, const DocPara* para, float x, float width,
                      BOOL isCellText) {
    FlatText flat;
    if (!FlattenPara(para, &flat)) return;

    // A left indent moves the whole paragraph in and narrows it; the margin is
    // where indentation is measured from, so it cannot be folded into the
    // page margin.
    //
    // ponytail: `indentFirst` is not applied. DirectWrite has no first-line
    // indent -- a layout is one rectangle -- so it needs the first line laid
    // out separately and stitched to the rest. Worth it when body text with
    // indented first lines shows up; a hanging indent already works, because
    // that is what the list marker uses.
    float indent = para->props.indentLeft / TWIPS_PER_DIP;
    if (indent > 0.0f) {
        x += indent;
        width -= indent;
        if (width < 1.0f) width = 1.0f;
    }

    f->y += para->props.spaceBefore / TWIPS_PER_DIP;

    // A list marker sits in the hanging indent as its own piece of text, which
    // is simpler and more faithful than trying to make one layout do both.
    if (para->props.list != LIST_NONE && !isCellText) {
        const WCHAR* marker = (para->props.list == LIST_BULLET) ? L"\x2022" : L"1.";
        TextSpan span = {};
        span.start = 0;
        span.len = (UINT32)wcslen(marker);
        if (para->runs) span.props = para->runs->props;

        ParaProps plain = para->props;
        plain.align = ALIGN_LEFT;

        IDWriteTextLayout* m = MakeLayout(&f->ctx, marker, span.len, &span, 1, 24.0f, &plain);
        if (m) {
            LaidText* t = AddText(f->page);
            if (t) {
                t->layout = m;
                t->x = x - 24.0f;
                t->y = f->y;
                t->width = 24.0f;
                t->height = LayoutHeight(m);
                t->para = para;
            } else {
                m->Release();
            }
        }
    }

    UINT32 from = 0;
    int guard = 0;

    for (;;) {
        if (++guard > 10000) break;   // no paragraph needs this many pages

        TextSpan tail[512];
        int tailCount = RebaseSpans(&flat, from, tail, 512);

        UINT32 remaining = flat.len - from;
        IDWriteTextLayout* layout = MakeLayout(&f->ctx, flat.text + from, remaining,
                                               tail, tailCount, width, &para->props);
        if (!layout) break;

        float height = LayoutHeight(layout);

        if (height <= SpaceLeft(f) || remaining == 0) {
            Place(f, layout, x, width, para, isCellText, tail, tailCount);
            f->y += height;
            break;
        }

        // Does not fit. Find the last line boundary that does.
        float used = 0.0f;
        UINT32 fit = SplitOffset(layout, SpaceLeft(f), &used);
        layout->Release();

        if (fit == 0) {
            // Nothing fits here. A fresh page gives it the best chance; if the
            // pen is already at the top of one then a single line is taller
            // than the page, and it is placed anyway rather than looping.
            if (f->y > f->result->marginTop) {
                NewPage(f);
                continue;
            }

            IDWriteTextLayout* forced = MakeLayout(&f->ctx, flat.text + from, remaining,
                                                   tail, tailCount, width, &para->props);
            if (forced) {
                float h = LayoutHeight(forced);
                Place(f, forced, x, width, para, isCellText, tail, tailCount);
                f->y += h;
            }
            break;
        }

        IDWriteTextLayout* head = MakeLayout(&f->ctx, flat.text + from, fit,
                                             tail, tailCount, width, &para->props);
        if (head) Place(f, head, x, width, para, isCellText, tail, tailCount);

        from += fit;
        NewPage(f);
    }

    f->y += para->props.spaceAfter / TWIPS_PER_DIP;
    FlatFree(&flat);
}

// Measure a cell's paragraphs at a given width without placing anything.
static float MeasureCell(Flow* f, const DocCell* cell, float width) {
    float total = 0.0f;
    if (width < 1.0f) width = 1.0f;

    for (const DocPara* p = cell->paras; p; p = p->next) {
        FlatText flat;
        if (!FlattenPara(p, &flat)) continue;

        IDWriteTextLayout* layout = MakeLayout(&f->ctx, flat.text, flat.len,
                                               flat.spans, flat.spanCount,
                                               width, &p->props);
        if (layout) {
            total += LayoutHeight(layout);
            layout->Release();
        }
        FlatFree(&flat);
    }
    return total;
}

static void PlaceTable(Flow* f, const DocBlock* block) {
    int colCount = 0;
    for (const DocRow* r = block->table.rows; r; r = r->next) {
        int n = 0;
        for (const DocCell* c = r->cells; c; c = c->next) n++;
        if (n > colCount) colCount = n;
    }
    if (!colCount) return;

    // Column widths from the grid when the document has one, equal columns
    // when it does not. Scaled down if the table is wider than the page, so it
    // cannot run off the edge.
    float widths[32] = {};
    if (block->table.gridCount > 0) {
        float prev = 0.0f, total = 0.0f;
        for (int i = 0; i < block->table.gridCount && i < 32; i++) {
            float edge = block->table.gridEdges[i] / TWIPS_PER_DIP;
            widths[i] = edge - prev;
            prev = edge;
            total += widths[i];
        }
        if (total > f->contentWidth && total > 0.0f) {
            float scale = f->contentWidth / total;
            for (int i = 0; i < block->table.gridCount && i < 32; i++) widths[i] *= scale;
        }
    } else {
        for (int i = 0; i < colCount && i < 32; i++) widths[i] = f->contentWidth / colCount;
    }

    for (const DocRow* row = block->table.rows; row; row = row->next) {
        float rowHeight = 0.0f;
        int col = 0;
        for (const DocCell* c = row->cells; c && col < 32; c = c->next, col++) {
            float w = widths[col] > 0 ? widths[col] : f->contentWidth / colCount;
            float h = MeasureCell(f, c, w - 2 * CELL_PAD_DIP);
            if (h > rowHeight) rowHeight = h;
        }
        rowHeight += 2 * CELL_PAD_DIP;

        // ponytail: a row moves to the next page whole rather than splitting
        // across the boundary. Splitting means re-measuring every cell against
        // the height left and carrying the remainder, which is real work for a
        // case a letter or a report rarely hits. A row taller than a page is
        // placed anyway rather than looping forever.
        if (rowHeight > SpaceLeft(f) && f->y > f->result->marginTop) {
            NewPage(f);
        }

        float x = f->result->marginLeft;
        float rowTop = f->y;

        col = 0;
        for (const DocCell* c = row->cells; c && col < 32; c = c->next, col++) {
            float w = widths[col] > 0 ? widths[col] : f->contentWidth / colCount;

            LaidCell* box = AddCell(f->page);
            if (box) {
                box->x = x;
                box->y = rowTop;
                box->width = w;
                box->height = rowHeight;
            }

            float saveY = f->y;
            f->y = rowTop + CELL_PAD_DIP;
            for (const DocPara* p = c->paras; p; p = p->next) {
                PlacePara(f, p, x + CELL_PAD_DIP, w - 2 * CELL_PAD_DIP, TRUE);
            }
            f->y = saveY;

            x += w;
        }

        f->y = rowTop + rowHeight;
    }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

extern "C" LayoutResult* Layout_Build(const DocModel* doc, const WCHAR* defaultFont,
                                      float defaultSizePt) {
    if (!doc) return NULL;

    LayoutResult* r = (LayoutResult*)calloc(1, sizeof(LayoutResult));
    if (!r) return NULL;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&r->dwrite))) {
        free(r);
        return NULL;
    }

    if (!defaultFont || !defaultFont[0]) defaultFont = L"Calibri";
    if (defaultSizePt <= 0.0f) defaultSizePt = 11.0f;

    Ctx ctx = {};
    ctx.dwrite = r->dwrite;
    ctx.defaultSizeDip = defaultSizePt * (96.0f / 72.0f);

    if (FAILED(r->dwrite->CreateTextFormat(defaultFont, NULL,
                                           DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL,
                                           ctx.defaultSizeDip, L"", &ctx.baseFormat))) {
        r->dwrite->Release();
        free(r);
        return NULL;
    }

    r->pageWidth    = doc->section.pageWidth  / TWIPS_PER_DIP;
    r->pageHeight   = doc->section.pageHeight / TWIPS_PER_DIP;
    r->marginLeft   = doc->section.marginLeft   / TWIPS_PER_DIP;
    r->marginRight  = doc->section.marginRight  / TWIPS_PER_DIP;
    r->marginTop    = doc->section.marginTop    / TWIPS_PER_DIP;
    r->marginBottom = doc->section.marginBottom / TWIPS_PER_DIP;

    Flow f = {};
    f.result = r;
    f.ctx = ctx;
    f.contentWidth = r->pageWidth - r->marginLeft - r->marginRight;
    f.bottom = r->pageHeight - r->marginBottom;
    if (f.contentWidth < 1.0f) f.contentWidth = 1.0f;

    NewPage(&f);

    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) {
                PlacePara(&f, p, r->marginLeft, f.contentWidth, FALSE);
            }
        } else {
            PlaceTable(&f, b);
        }
    }

    ctx.baseFormat->Release();
    return r;
}

extern "C" void Layout_Free(LayoutResult* r) {
    if (!r) return;

    for (int i = 0; i < r->pageCount; i++) {
        LaidPage* p = &r->pages[i];
        for (int j = 0; j < p->textCount; j++) {
            if (p->texts[j].layout) p->texts[j].layout->Release();
            free(p->texts[j].colors);
        }
        free(p->texts);
        free(p->cells);
    }
    free(r->pages);

    if (r->dwrite) r->dwrite->Release();
    free(r);
}

extern "C" int Layout_PageCount(const LayoutResult* r) {
    return r ? r->pageCount : 0;
}

extern "C" void Layout_PageSize(const LayoutResult* r, float* w, float* h) {
    if (!r) return;
    if (w) *w = r->pageWidth;
    if (h) *h = r->pageHeight;
}

extern "C" int Layout_PageTextCount(const LayoutResult* r, int i) {
    if (!r || i < 0 || i >= r->pageCount) return 0;
    return r->pages[i].textCount;
}

extern "C" int Layout_PageCellCount(const LayoutResult* r, int i) {
    if (!r || i < 0 || i >= r->pageCount) return 0;
    return r->pages[i].cellCount;
}

extern "C" float Layout_PageContentBottom(const LayoutResult* r, int i) {
    if (!r || i < 0 || i >= r->pageCount) return 0.0f;

    const LaidPage* page = &r->pages[i];
    float bottom = 0.0f;

    for (int j = 0; j < page->textCount; j++) {
        float b = page->texts[j].y + page->texts[j].height;
        if (b > bottom) bottom = b;
    }
    for (int j = 0; j < page->cellCount; j++) {
        float b = page->cells[j].y + page->cells[j].height;
        if (b > bottom) bottom = b;
    }
    return bottom;
}

// ---------------------------------------------------------------------------
// Self-check. Run with: OpenNote.exe --selftest
//
// The engine produces geometry and nothing else, which is exactly what makes
// it checkable without a screen. The invariant that matters most is that
// nothing is ever placed below the bottom margin -- if that holds, pagination
// is working, and if it does not, no amount of looking at the screen would
// reliably catch the page where it stopped holding.
// ---------------------------------------------------------------------------

static DocPara* AddTextPara(DocModel* doc, const WCHAR* text, const CharProps* props) {
    DocPara* p = Doc_AddPara(doc);
    if (p) Doc_AddRun(p, text, -1, props);
    return p;
}

extern "C" BOOL Layout_SelfTest(char* failure, size_t failureSize) {
    DocModel* doc = NULL;
    LayoutResult* r = NULL;

    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        if (r) Layout_Free(r); \
        if (doc) Doc_Free(doc); \
        return FALSE; \
    } while (0)

    CharProps plain = {};

    // --- a short document occupies one page -------------------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");

    AddTextPara(doc, L"A short document.", &plain);

    r = Layout_Build(doc, L"Calibri", 11.0f);
    if (!r) FAIL("Layout_Build returned nothing -- is DirectWrite available?");

    if (Layout_PageCount(r) != 1) FAIL("a one-line document did not occupy one page");
    if (Layout_PageTextCount(r, 0) != 1) FAIL("the paragraph was not placed");

    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(r, &pw, &ph);
    // Letter paper is 12240 x 15840 twips, which is 816 x 1056 DIPs.
    if (pw < 815.0f || pw > 817.0f)   FAIL("page width is not Letter");
    if (ph < 1055.0f || ph > 1057.0f) FAIL("page height is not Letter");

    Layout_Free(r); r = NULL;
    Doc_Free(doc); doc = NULL;

    // --- a long document paginates, and stays inside its margins ----------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");

    for (int i = 0; i < 400; i++) {
        AddTextPara(doc, L"Paragraph of body text used to fill enough pages that "
                         L"pagination has to do something.", &plain);
    }

    r = Layout_Build(doc, L"Calibri", 11.0f);
    if (!r) FAIL("Layout_Build failed on a long document");

    int pages = Layout_PageCount(r);
    if (pages < 2) FAIL("400 paragraphs did not paginate");

    // The invariant. A tolerance of a DIP absorbs the rounding in converting
    // twips, not a line of text.
    float bottomLimit = 1056.0f - (1440.0f / TWIPS_PER_DIP);
    for (int i = 0; i < pages; i++) {
        if (Layout_PageContentBottom(r, i) > bottomLimit + 1.0f) {
            FAIL("content was placed below the bottom margin");
        }
    }

    // Every page except possibly the last should have been filled reasonably;
    // a page holding one paragraph when many were queued means the flow gave
    // up early rather than continuing.
    for (int i = 0; i < pages - 1; i++) {
        if (Layout_PageTextCount(r, i) < 2) FAIL("a page was left almost empty");
    }

    Layout_Free(r); r = NULL;
    Doc_Free(doc); doc = NULL;

    // --- one paragraph longer than a page is split, not dropped -----------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        for (int i = 0; i < 600; i++) {
            Doc_AddRun(p, L"This single paragraph is deliberately long enough that "
                          L"it cannot fit on one page and must be broken across "
                          L"several. ", -1, &plain);
        }
    }

    r = Layout_Build(doc, L"Calibri", 11.0f);
    if (!r) FAIL("Layout_Build failed on one very long paragraph");

    pages = Layout_PageCount(r);
    if (pages < 2) FAIL("a paragraph longer than a page was not split");

    for (int i = 0; i < pages; i++) {
        if (Layout_PageTextCount(r, i) < 1) FAIL("a page in a split paragraph is empty");
        if (Layout_PageContentBottom(r, i) > bottomLimit + 1.0f) {
            FAIL("a split paragraph overflowed the bottom margin");
        }
    }

    Layout_Free(r); r = NULL;
    Doc_Free(doc); doc = NULL;

    // --- tables get cell boxes, and honour their grid ---------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocBlock* t = Doc_AddTable(doc);
        t->table.gridEdges[0] = 3000;
        t->table.gridEdges[1] = 6000;
        t->table.gridEdges[2] = 9000;
        t->table.gridCount = 3;

        for (int rIdx = 0; rIdx < 3; rIdx++) {
            DocRow* row = Doc_AddRow(t);
            for (int c = 0; c < 3; c++) {
                DocPara* cp = Doc_AddCellPara(Doc_AddCell(row));
                Doc_AddRun(cp, L"Cell", -1, &plain);
            }
        }
    }

    r = Layout_Build(doc, L"Calibri", 11.0f);
    if (!r) FAIL("Layout_Build failed on a table");

    if (Layout_PageCellCount(r, 0) != 9) FAIL("a 3x3 table did not produce nine cells");
    if (Layout_PageTextCount(r, 0) != 9) FAIL("cell text was not placed");

    Layout_Free(r); r = NULL;
    Doc_Free(doc); doc = NULL;

    // --- a left indent narrows the paragraph ------------------------------
    //
    // Checked by its consequence rather than by reading a coordinate back: the
    // same words in a narrower column take more lines, so the content reaches
    // further down the page. Enough words that the difference cannot land on
    // the same line count by luck -- height is quantised by lines.
    {
        float plainBottom = 0.0f, indentedBottom = 0.0f;

        for (int pass = 0; pass < 2; pass++) {
            doc = Doc_New();
            if (!doc) FAIL("could not allocate a model");

            DocPara* p = Doc_AddPara(doc);
            if (!p) FAIL("could not build the indent case");
            for (int i = 0; i < 8; i++) {
                Doc_AddRun(p, L"An indented paragraph occupies a narrower column than "
                              L"the same words set flush to the margin, so it takes "
                              L"more lines to say them. ", -1, &plain);
            }
            if (pass == 1) p->props.indentLeft = 2880;   // two inches

            r = Layout_Build(doc, L"Calibri", 11.0f);
            if (!r) FAIL("Layout_Build failed on the indent case");

            if (pass == 0) plainBottom = Layout_PageContentBottom(r, 0);
            else           indentedBottom = Layout_PageContentBottom(r, 0);

            Layout_Free(r); r = NULL;
            Doc_Free(doc); doc = NULL;
        }

        if (indentedBottom <= plainBottom) FAIL("a left indent did not narrow the paragraph");
    }

    // --- Page Setup reaches the engine ------------------------------------
    //
    // A model captured from the editor has no page of its own, so the page
    // defaults are the only route from the Page Setup dialog to paper size.
    {
        SectionProps saved, a4 = {};
        Doc_GetPageDefaults(&saved);

        a4 = saved;
        a4.pageWidth  = 11906;   // A4, in twips
        a4.pageHeight = 16838;
        Doc_SetPageDefaults(&a4);

        doc = Doc_New();
        BOOL built = doc != NULL;
        r = built ? Layout_Build(doc, L"Calibri", 11.0f) : NULL;

        float w = 0.0f, h = 0.0f;
        if (r) Layout_PageSize(r, &w, &h);

        Layout_Free(r); r = NULL;
        Doc_Free(doc); doc = NULL;
        Doc_SetPageDefaults(&saved);

        if (!built || w <= 0.0f) FAIL("Layout_Build failed on A4 paper");
        // 11906 twips is 793.7 DIPs; 16838 is 1122.5.
        if (w < 793.0f || w > 794.5f)    FAIL("the page defaults did not set the page width");
        if (h < 1122.0f || h > 1123.5f)  FAIL("the page defaults did not set the page height");
    }

    // --- an empty document still produces a page --------------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");

    r = Layout_Build(doc, L"Calibri", 11.0f);
    if (!r) FAIL("Layout_Build failed on an empty document");
    if (Layout_PageCount(r) != 1) FAIL("an empty document did not produce one page");

    Layout_Free(r); r = NULL;
    Doc_Free(doc); doc = NULL;

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
