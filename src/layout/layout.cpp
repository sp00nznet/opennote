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

static LaidImage* AddImage(LaidPage* page) {
    if (page->imageCount == page->imageCap) {
        int cap = page->imageCap ? page->imageCap * 2 : 8;
        LaidImage* grown = (LaidImage*)realloc(page->images, (size_t)cap * sizeof(LaidImage));
        if (!grown) return NULL;
        page->images = grown;
        page->imageCap = cap;
    }
    LaidImage* i = &page->images[page->imageCount++];
    memset(i, 0, sizeof(*i));
    return i;
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

        if (r->image) {
            // A picture is one character wide in the text, so every offset
            // either side of it still means what it meant in the model. A
            // space is what goes there: it draws nothing, and the picture is
            // placed over it once the line is laid out.
            one[0] = L' '; one[1] = 0; piece = one; pieceLen = 1;
        } else if (r->pageBreak) {
            // The break itself is handled before the paragraph is laid out;
            // here it is one character of nothing, so offsets still line up.
            one[0] = L' '; one[1] = 0; piece = one; pieceLen = 1;
        } else if (r->tab) {
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

// How a paragraph's lines fall against the space left on the page. The line
// is the unit a paragraph can be broken at -- never inside one -- so this is
// what pagination actually decides with.
struct LineFit {
    UINT32 fitted;         // lines that fit in the space left
    UINT32 total;          // lines in the whole paragraph
    UINT32 chars;          // characters in the fitted lines
    UINT32 charsMinusOne;  // ...and in one line fewer, for widow control
    float  height;         // height of the fitted lines
};

static BOOL MeasureLines(IDWriteTextLayout* layout, float avail, LineFit* out) {
    memset(out, 0, sizeof(*out));

    UINT32 count = 0;
    layout->GetLineMetrics(NULL, 0, &count);
    if (!count) return FALSE;

    DWRITE_LINE_METRICS* lines =
        (DWRITE_LINE_METRICS*)calloc(count, sizeof(DWRITE_LINE_METRICS));
    if (!lines) return FALSE;

    UINT32 actual = 0;
    if (FAILED(layout->GetLineMetrics(lines, count, &actual))) {
        free(lines);
        return FALSE;
    }

    float h = 0.0f;
    UINT32 chars = 0, previous = 0;

    for (UINT32 i = 0; i < actual; i++) {
        if (h + lines[i].height > avail) break;
        previous = chars;
        h += lines[i].height;
        chars += lines[i].length;
        out->fitted++;
    }

    out->total = actual;
    out->chars = chars;
    out->charsMinusOne = previous;
    out->height = h;
    free(lines);
    return TRUE;
}

// ---------------------------------------------------------------------------
// List markers
//
// numbering.xml says how a list counts; the counting itself happens here,
// because it depends on the order paragraphs are laid out in and on nothing
// else. A level restarts when a shallower one moves on, which is what makes
// 1, 1.1, 1.2, 2, 2.1 come out right.
// ---------------------------------------------------------------------------

struct Numbering {
    int          listId;        // the list being counted, 0 for none
    int          counters[9];
    DocNumFormat formats[9];    // what each level was last seen counting in
    BOOL         seen[9];
};

static void NumberToText(int value, DocNumFormat fmt, WCHAR* out, size_t outChars) {
    if (value < 1) value = 1;

    switch (fmt) {
        case NUMFMT_LOWER_LETTER:
        case NUMFMT_UPPER_LETTER: {
            // a..z, then aa, bb, cc -- which is what Word does, rather than
            // counting in base 26.
            WCHAR base = (fmt == NUMFMT_LOWER_LETTER) ? L'a' : L'A';
            int repeats = ((value - 1) / 26) + 1;
            WCHAR letter = (WCHAR)(base + ((value - 1) % 26));
            size_t n = 0;
            while (n < outChars - 1 && n < (size_t)repeats) out[n++] = letter;
            out[n] = L'\0';
            break;
        }

        case NUMFMT_LOWER_ROMAN:
        case NUMFMT_UPPER_ROMAN: {
            static const int values[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
            static const WCHAR* upper[] = { L"M", L"CM", L"D", L"CD", L"C", L"XC", L"L",
                                            L"XL", L"X", L"IX", L"V", L"IV", L"I" };
            static const WCHAR* lower[] = { L"m", L"cm", L"d", L"cd", L"c", L"xc", L"l",
                                            L"xl", L"x", L"ix", L"v", L"iv", L"i" };
            const WCHAR** digits = (fmt == NUMFMT_UPPER_ROMAN) ? upper : lower;

            out[0] = L'\0';
            int left = value;
            for (int i = 0; i < 13 && left > 0; i++) {
                while (left >= values[i]) {
                    wcscat_s(out, outChars, digits[i]);
                    left -= values[i];
                }
            }
            break;
        }

        default:
            swprintf_s(out, outChars, L"%d", value);
            break;
    }
}

// Advance the counters for a paragraph and write out its marker.
static void NextMarker(Numbering* n, const ParaProps* props, WCHAR* out, size_t outChars) {
    int level = props->listLevel;
    if (level < 0) level = 0;
    if (level > 8) level = 8;

    int listId = props->listId > 0 ? props->listId : 1;
    if (listId != n->listId) {
        memset(n, 0, sizeof(*n));
        n->listId = listId;
    }

    n->formats[level] = props->numFormat;
    n->seen[level] = TRUE;

    // A deeper level starts again every time it is entered.
    for (int i = level + 1; i < 9; i++) {
        n->counters[i] = 0;
        n->seen[i] = FALSE;
    }
    n->counters[level]++;

    if (props->numFormat == NUMFMT_BULLET) {
        // The three bullets Word cycles through by level.
        static const WCHAR* bullets[] = { L"\x2022", L"\x25E6", L"\x25AA" };
        wcscpy_s(out, outChars, bullets[level % 3]);
        return;
    }

    // "%1.%2)" and the like: each %N is the counter for that level, in that
    // level's own format.
    const WCHAR* pattern = props->listText[0] ? props->listText : L"%1.";
    WCHAR fallback[8];
    if (!props->listText[0]) {
        swprintf_s(fallback, 8, L"%%%d.", level + 1);
        pattern = fallback;
    }

    size_t at = 0;
    for (const WCHAR* p = pattern; *p && at < outChars - 1; p++) {
        if (*p == L'%' && p[1] >= L'1' && p[1] <= L'9') {
            int which = p[1] - L'1';
            p++;

            WCHAR number[32];
            NumberToText(n->counters[which],
                         n->seen[which] ? n->formats[which] : NUMFMT_DECIMAL,
                         number, 32);

            for (const WCHAR* q = number; *q && at < outChars - 1; q++) out[at++] = *q;
        } else {
            out[at++] = *p;
        }
    }
    out[at] = L'\0';
}

// ---------------------------------------------------------------------------
// Flowing
// ---------------------------------------------------------------------------

struct Flow {
    LayoutResult* result;
    LaidPage*     page;
    float         y;              // pen position, DIPs from the page top
    float         contentWidth;   // one column's worth
    float         bottom;         // nothing may be placed beyond this
    Ctx           ctx;
    Numbering     numbering;

    // Columns. A one-column page is the same code with the loop running once,
    // which is why there is no separate path for it.
    int   columns;
    int   column;                 // 0-based, left to right
    float columnLeft;             // where this column starts, DIPs
    float columnGap;
};

// Where a column begins across the page.
static float ColumnLeft(const Flow* f, int column) {
    return f->result->marginLeft + column * (f->contentWidth + f->columnGap);
}

static void NewPage(Flow* f) {
    f->page = AddPage(f->result);
    f->y = f->result->marginTop;
    f->column = 0;
    f->columnLeft = ColumnLeft(f, 0);
}

// The next place text can go: the column beside this one, or the top of a new
// page when this was the last column. Everything that used to start a page now
// goes through here, so a two-column document fills both columns of a page
// before it reaches for another sheet.
static void NextColumn(Flow* f) {
    if (f->column + 1 < f->columns) {
        f->column++;
        f->columnLeft = ColumnLeft(f, f->column);
        f->y = f->result->marginTop;
        return;
    }
    NewPage(f);
}

static float SpaceLeft(const Flow* f) {
    return f->bottom - f->y;
}

static LaidText* Place(Flow* f, IDWriteTextLayout* layout, float x, float width,
                       const DocPara* para, BOOL isCellText,
                       const TextSpan* spans, int spanCount,
                       UINT32 textStart, UINT32 textLen) {
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
    t->textStart = textStart;
    t->textLen = textLen;
    CaptureColors(t, spans, spanCount);
    return t;
}

// Put a paragraph's pictures on the page, over the spaces standing in for
// them. Answers how far past the line's own bottom the tallest one reaches,
// which is what the paragraph has to make room for.
//
// ponytail: a picture takes a line to itself in effect -- text on the same
// line is not pushed aside, it is drawn over. Flowing text around a picture
// needs a DirectWrite inline object and a custom renderer to go with it, and
// a picture in a document is almost always alone on its line.
static float PlaceImages(Flow* f, const DocPara* para, const LaidText* piece) {
    if (!piece || !piece->layout) return 0.0f;

    float overflow = 0.0f;
    unsigned at = 0;

    for (const DocRun* r = para->runs; r; r = r->next) {
        unsigned length = 1;
        if (!r->image && !r->tab && !r->lineBreak) {
            length = r->text ? (unsigned)wcslen(r->text) : 0;
        }
        if (!length) continue;

        unsigned start = at;
        at += length;

        if (!r->image) continue;
        if (start < piece->textStart || start >= piece->textStart + piece->textLen) continue;

        float px = 0.0f, py = 0.0f;
        DWRITE_HIT_TEST_METRICS metrics = {};
        if (FAILED(piece->layout->HitTestTextPosition(start - piece->textStart, FALSE,
                                                      &px, &py, &metrics))) {
            continue;
        }

        LaidImage* laid = AddImage(f->page);
        if (!laid) continue;

        // 914400 EMU to the inch, 96 DIPs to the inch.
        laid->image = r->image;
        laid->width = r->image->widthEmu / 9525.0f;
        laid->height = r->image->heightEmu / 9525.0f;
        laid->x = piece->x + px;
        laid->y = piece->y + py;

        if (laid->width <= 0.0f) laid->width = 96.0f;
        if (laid->height <= 0.0f) laid->height = 96.0f;

        // Keep a picture inside the text column rather than off the page.
        float roomRight = f->result->pageWidth - f->result->marginRight - laid->x;
        if (roomRight > 8.0f && laid->width > roomRight) {
            laid->height *= roomRight / laid->width;
            laid->width = roomRight;
        }

        float past = laid->height - metrics.height;
        if (past > overflow) overflow = past;
    }

    return overflow;
}

// Place a paragraph, splitting it across pages at line boundaries when it does
// not fit. A paragraph taller than a whole page is split as many times as it
// takes rather than being dropped or allowed to overflow.
static void PlacePara(Flow* f, const DocPara* para, float x, float width,
                      BOOL isCellText) {
    FlatText flat;
    if (!FlattenPara(para, &flat)) return;

    // A page break before this paragraph, whether the document said so in the
    // paragraph's properties or put a break run at the front of it. Both mean
    // the same thing and both are common; neither does anything at the top of
    // a page, where a second break would leave a blank one.
    BOOL breakBefore = para->props.pageBreakBefore;
    if (!breakBefore && para->runs && para->runs->pageBreak) breakBefore = TRUE;

    if (breakBefore && !isCellText && f->y > f->result->marginTop) {
        NewPage(f);
    }

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
        WCHAR marker[64];
        NextMarker(&f->numbering, &para->props, marker, 64);

        TextSpan span = {};
        span.start = 0;
        span.len = (UINT32)wcslen(marker);
        if (para->runs) span.props = para->runs->props;

        ParaProps plain = para->props;
        plain.align = ALIGN_LEFT;

        // Where the first line starts, which is where the marker belongs: a
        // hanging indent exists precisely to leave room for it.
        float hang = para->props.indentFirst < 0
                   ? -para->props.indentFirst / TWIPS_PER_DIP
                   : 24.0f;
        if (hang < 12.0f) hang = 12.0f;

        IDWriteTextLayout* m = MakeLayout(&f->ctx, marker, span.len, &span, 1, hang, &plain);
        if (m) {
            LaidText* t = AddText(f->page);
            if (t) {
                t->layout = m;
                t->x = x - hang;
                t->y = f->y;
                t->width = hang;
                t->height = LayoutHeight(m);
                t->para = para;
                t->isMarker = TRUE;
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
            LaidText* piece = Place(f, layout, x, width, para, isCellText,
                                    tail, tailCount, from, remaining);
            f->y += height + PlaceImages(f, para, piece);
            break;
        }

        // Does not fit. Break at the last line that does -- and then think
        // again about where that leaves the paragraph.
        LineFit fit = {};
        BOOL measured = MeasureLines(layout, SpaceLeft(f), &fit);
        layout->Release();
        if (!measured) break;

        UINT32 take = fit.chars;

        // Widows and orphans. A paragraph does not leave a single line alone
        // at the bottom of one page or the top of the next: either two lines
        // go with it, or the whole paragraph moves to the next page. This is
        // the one place pagination is about how a page reads rather than about
        // what fits on it.
        if (fit.total >= 2) {
            BOOL orphan = fit.fitted < 2;                      // one line left behind
            BOOL widow  = (fit.total - fit.fitted) < 2;        // one line carried over

            if (orphan || widow) {
                if (!orphan && fit.fitted >= 3) {
                    take = fit.charsMinusOne;                  // carry two lines instead
                } else if (f->y > f->result->marginTop) {
                    NextColumn(f);                             // move the paragraph whole
                    continue;
                }
                // Already at the top of a page: a line that fills a page on its
                // own has nowhere better to go, so it stays where it falls.
            }
        }

        if (take == 0) {
            // Nothing fits here. A fresh page gives it the best chance; if the
            // pen is already at the top of one then a single line is taller
            // than the page, and it is placed anyway rather than looping.
            if (f->y > f->result->marginTop) {
                NextColumn(f);
                continue;
            }

            IDWriteTextLayout* forced = MakeLayout(&f->ctx, flat.text + from, remaining,
                                                   tail, tailCount, width, &para->props);
            if (forced) {
                float h = LayoutHeight(forced);
                Place(f, forced, x, width, para, isCellText, tail, tailCount, from, remaining);
                f->y += h;
            }
            break;
        }

        IDWriteTextLayout* head = MakeLayout(&f->ctx, flat.text + from, take,
                                             tail, tailCount, width, &para->props);
        if (head) {
            LaidText* piece = Place(f, head, x, width, para, isCellText,
                                    tail, tailCount, from, take);
            PlaceImages(f, para, piece);
        }

        from += take;
        NextColumn(f);
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
            NextColumn(f);
        }

        float x = f->columnLeft;
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
// Headers and footers
//
// The same paragraphs as anything else, laid out into the margins rather than
// into the text area -- and onto every page, which is why they happen after
// the document has been flowed and the pages are known.
//
// ponytail: the same header on every page. A different first page, or
// different left and right pages, is a section problem, and a document has
// one section here.
// ---------------------------------------------------------------------------

static void PlaceMargins(Flow* f, const DocModel* doc) {
    if (!doc->header && !doc->footer) return;

    LayoutResult* r = f->result;
    float width = r->pageWidth - r->marginLeft - r->marginRight;
    if (width < 1.0f) return;

    float headerTop = (doc->headerFromTop > 0 ? doc->headerFromTop : 720) / TWIPS_PER_DIP;
    float footerUp = (doc->footerFromBottom > 0 ? doc->footerFromBottom : 720) / TWIPS_PER_DIP;

    int pages = r->pageCount;
    for (int p = 0; p < pages; p++) {
        f->page = &r->pages[p];
        f->columnLeft = r->marginLeft;
        f->contentWidth = width;

        // Everything placed from here on is margin furniture rather than the
        // document's own text, which is what the checks measure.
        int before = f->page->textCount;

        // The header hangs from the top of the paper, above the text.
        if (doc->header) {
            f->y = headerTop;
            f->bottom = r->marginTop;      // it may not grow into the text
            for (const DocPara* para = doc->header; para; para = para->next) {
                PlacePara(f, para, r->marginLeft, width, TRUE);
            }
        }

        // The footer sits above the bottom edge. Its height is not known
        // until it is laid out, so it is placed from the top of the space it
        // is allowed and left there: a footer taller than the bottom margin
        // grows downwards, off the paper, which is what Word does too.
        if (doc->footer) {
            f->y = r->pageHeight - footerUp;
            f->bottom = r->pageHeight;
            for (const DocPara* para = doc->footer; para; para = para->next) {
                PlacePara(f, para, r->marginLeft, width, TRUE);
            }
        }

        for (int i = before; i < f->page->textCount; i++) {
            f->page->texts[i].isMargin = TRUE;
        }
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

    // Half an inch between tab stops, which is what every word processor
    // defaults to and what the ruler draws. DirectWrite's own default is four
    // times the font size, which is neither.
    //
    // ponytail: one spacing for the whole document. Per-paragraph stops are a
    // `w:tabs` list the model does not carry yet -- that is document fidelity,
    // and it arrives with the rest of it in v0.9.
    ctx.baseFormat->SetIncrementalTabStop(48.0f);

    r->doc = doc;
    r->pageWidth    = doc->section.pageWidth  / TWIPS_PER_DIP;
    r->pageHeight   = doc->section.pageHeight / TWIPS_PER_DIP;
    r->marginLeft   = doc->section.marginLeft   / TWIPS_PER_DIP;
    r->marginRight  = doc->section.marginRight  / TWIPS_PER_DIP;
    r->marginTop    = doc->section.marginTop    / TWIPS_PER_DIP;
    r->marginBottom = doc->section.marginBottom / TWIPS_PER_DIP;

    Flow f = {};
    f.result = r;
    f.ctx = ctx;
    f.bottom = r->pageHeight - r->marginBottom;

    f.columns = doc->section.columns > 0 ? doc->section.columns : 1;
    if (f.columns > 8) f.columns = 8;
    f.columnGap = doc->section.columnSpace / TWIPS_PER_DIP;

    float textWidth = r->pageWidth - r->marginLeft - r->marginRight;
    f.contentWidth = (textWidth - f.columnGap * (f.columns - 1)) / f.columns;
    if (f.contentWidth < 1.0f) f.contentWidth = 1.0f;

    NewPage(&f);

    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) {
                PlacePara(&f, p, f.columnLeft, f.contentWidth, FALSE);
            }
        } else {
            PlaceTable(&f, b);
        }
    }

    // Every page is known now, so the margins can be filled in.
    PlaceMargins(&f, doc);

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
        free(p->images);
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

extern "C" int Layout_PageImageCount(const LayoutResult* r, int i) {
    if (!r || i < 0 || i >= r->pageCount) return 0;
    return r->pages[i].imageCount;
}

extern "C" float Layout_PageContentBottom(const LayoutResult* r, int i) {
    if (!r || i < 0 || i >= r->pageCount) return 0.0f;

    const LaidPage* page = &r->pages[i];
    float bottom = 0.0f;

    for (int j = 0; j < page->textCount; j++) {
        if (page->texts[j].isMargin) continue;   // a footer belongs below it
        float b = page->texts[j].y + page->texts[j].height;
        if (b > bottom) bottom = b;
    }
    for (int j = 0; j < page->cellCount; j++) {
        float b = page->cells[j].y + page->cells[j].height;
        if (b > bottom) bottom = b;
    }
    for (int j = 0; j < page->imageCount; j++) {
        float b = page->images[j].y + page->images[j].height;
        if (b > bottom) bottom = b;
    }
    return bottom;
}

// ---------------------------------------------------------------------------
// Geometry
//
// Hit testing, caret rectangles and selection rectangles. DirectWrite answers
// all three for one laid-out piece; the work here is deciding which piece a
// question is about, which is the same problem pagination created -- one
// paragraph can be three pieces on two pages.
// ---------------------------------------------------------------------------

// The piece holding `offset` in `para`. A position on the seam between two
// pieces belongs to the earlier one, so a caret at a page break sits at the
// bottom of the page it was typed on rather than jumping to the next.
static const LaidText* PieceFor(const LayoutResult* r, const DocPara* para,
                                unsigned offset, int* pageOut) {
    const LaidText* best = NULL;
    int bestPage = 0;

    for (int p = 0; p < r->pageCount; p++) {
        const LaidPage* page = &r->pages[p];
        for (int i = 0; i < page->textCount; i++) {
            const LaidText* t = &page->texts[i];
            if (t->isMarker || t->isMargin || t->para != para) continue;

            if (offset >= t->textStart && offset <= t->textStart + t->textLen) {
                best = t;
                bestPage = p;
                if (offset < t->textStart + t->textLen) {
                    if (pageOut) *pageOut = bestPage;
                    return t;
                }
            } else if (!best) {
                best = t;
                bestPage = p;
            }
        }
    }

    if (best && pageOut) *pageOut = bestPage;
    return best;
}

extern "C" int Layout_ComparePos(const LayoutResult* r, LayoutPos a, LayoutPos b) {
    if (!r || !a.para || !b.para) return 0;
    if (a.para == b.para) {
        if (a.offset < b.offset) return -1;
        return a.offset > b.offset ? 1 : 0;
    }

    int ia = Doc_ParaIndexOf(r->doc, a.para);
    int ib = Doc_ParaIndexOf(r->doc, b.para);
    if (ia < 0 || ib < 0) return 0;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

extern "C" BOOL Layout_HitTest(const LayoutResult* r, int page, float x, float y,
                               LayoutPos* out) {
    if (!r || !out || page < 0 || page >= r->pageCount) return FALSE;

    const LaidPage* p = &r->pages[page];
    const LaidText* hit = NULL;
    float bestDistance = 0.0f;

    for (int i = 0; i < p->textCount; i++) {
        const LaidText* t = &p->texts[i];
        if (t->isMarker || t->isMargin) continue;

        if (y >= t->y && y <= t->y + t->height &&
            x >= t->x && x <= t->x + t->width) {
            hit = t;
            break;
        }

        // Not inside anything: remember the nearest, measured from the middle
        // of the piece, so a click in a margin lands on the line beside it and
        // a click below the text lands at the end of the page.
        float dx = 0.0f;
        if (x < t->x) dx = t->x - x;
        else if (x > t->x + t->width) dx = x - (t->x + t->width);

        float dy = 0.0f;
        if (y < t->y) dy = t->y - y;
        else if (y > t->y + t->height) dy = y - (t->y + t->height);

        float distance = dy * 4.0f + dx;   // a line away counts for more than a gap beside
        if (!hit || distance < bestDistance) {
            bestDistance = distance;
            hit = t;
        }
    }

    if (!hit || !hit->layout) return FALSE;

    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics = {};
    if (FAILED(hit->layout->HitTestPoint(x - hit->x, y - hit->y,
                                         &trailing, &inside, &metrics))) {
        return FALSE;
    }

    unsigned offset = metrics.textPosition + (trailing ? metrics.length : 0);
    if (offset > hit->textLen) offset = hit->textLen;

    out->para = hit->para;
    out->offset = hit->textStart + offset;
    return TRUE;
}

extern "C" BOOL Layout_PosRect(const LayoutResult* r, LayoutPos pos, int* pageOut,
                               LayoutRect* out) {
    if (!r || !pos.para || !out) return FALSE;

    int page = 0;
    const LaidText* t = PieceFor(r, pos.para, pos.offset, &page);
    if (!t || !t->layout) return FALSE;

    unsigned local = pos.offset >= t->textStart ? pos.offset - t->textStart : 0;
    if (local > t->textLen) local = t->textLen;

    float px = 0.0f, py = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics = {};
    if (FAILED(t->layout->HitTestTextPosition(local, FALSE, &px, &py, &metrics))) {
        return FALSE;
    }

    out->x = t->x + px;
    out->y = t->y + py;
    out->width = 1.0f;
    out->height = metrics.height > 0.0f ? metrics.height : t->height;

    if (pageOut) *pageOut = page;
    return TRUE;
}

extern "C" int Layout_RangeRects(const LayoutResult* r, int page, LayoutPos a, LayoutPos b,
                                 LayoutRect* out, int cap) {
    if (!r || !out || cap <= 0 || page < 0 || page >= r->pageCount) return 0;
    if (Layout_ComparePos(r, a, b) > 0) {
        LayoutPos swap = a;
        a = b;
        b = swap;
    }

    const LaidPage* p = &r->pages[page];
    int written = 0;

    for (int i = 0; i < p->textCount && written < cap; i++) {
        const LaidText* t = &p->texts[i];
        if (t->isMarker || t->isMargin || !t->layout) continue;

        // What part of this piece the selection covers, in the piece's own
        // offsets. A piece belonging to a paragraph strictly inside the range
        // is covered whole.
        unsigned from = 0, to = t->textLen;

        int cmpStart = Layout_ComparePos(r, a, LayoutPos{ t->para, t->textStart + t->textLen });
        int cmpEnd = Layout_ComparePos(r, b, LayoutPos{ t->para, t->textStart });
        if (cmpStart > 0 || cmpEnd < 0) continue;      // entirely before or after

        if (a.para == t->para && a.offset > t->textStart) from = a.offset - t->textStart;
        if (b.para == t->para && b.offset < t->textStart + t->textLen) {
            to = b.offset > t->textStart ? b.offset - t->textStart : 0;
        }
        if (to <= from) continue;

        UINT32 count = 0;
        DWRITE_HIT_TEST_METRICS hits[64] = {};
        // 64 rectangles is more lines than a page holds, so the buffer cannot
        // be the thing that is short.
        if (FAILED(t->layout->HitTestTextRange(from, to - from, t->x, t->y,
                                               hits, 64, &count))) {
            continue;
        }

        for (UINT32 h = 0; h < count && written < cap; h++) {
            out[written].x = hits[h].left;
            out[written].y = hits[h].top;
            out[written].width = hits[h].width;
            out[written].height = hits[h].height;
            written++;
        }
    }

    return written;
}

extern "C" BOOL Layout_MoveLine(const LayoutResult* r, LayoutPos pos, int delta,
                                LayoutPos* out) {
    if (!r || !out || delta == 0) return FALSE;

    int page = 0;
    LayoutRect caret = {};
    if (!Layout_PosRect(r, pos, &page, &caret)) return FALSE;

    float x = caret.x;
    float y = delta > 0 ? caret.y + caret.height * 1.5f
                        : caret.y - caret.height * 0.5f;

    // Past the top or the bottom of the page, the line above or below is on
    // the page before or after -- a document is pages, not one long column.
    if (y < r->marginTop && delta < 0) {
        if (page == 0) return FALSE;
        page--;
        y = Layout_PageContentBottom(r, page) - caret.height * 0.5f;
    } else if (y > r->pageHeight - r->marginBottom && delta > 0) {
        if (page + 1 >= r->pageCount) return FALSE;
        page++;
        y = r->marginTop + caret.height * 0.5f;
    }

    return Layout_HitTest(r, page, x, y, out);
}

extern "C" BOOL Layout_LineEdge(const LayoutResult* r, LayoutPos pos, BOOL end,
                                LayoutPos* out) {
    if (!r || !out) return FALSE;

    int page = 0;
    LayoutRect caret = {};
    if (!Layout_PosRect(r, pos, &page, &caret)) return FALSE;

    // Far off the line in the wanted direction: DirectWrite answers with the
    // nearest position on that line, which is exactly the line's edge.
    float x = end ? r->pageWidth * 2.0f : -r->pageWidth;
    return Layout_HitTest(r, page, x, caret.y + caret.height * 0.5f, out);
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

// How many lines a laid-out piece came out as. Only the self-check needs it,
// which is why it is here rather than in the engine's interface.
static UINT32 PieceLines(const LaidText* t) {
    if (!t->layout) return 0;
    UINT32 count = 0;
    t->layout->GetLineMetrics(NULL, 0, &count);
    return count;
}

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

    // --- a position and a point agree about where a character is ----------
    //
    // The round trip that makes the page an editor: ask where a position is,
    // click there, and get the same position back. If these two ever disagree
    // the caret lands somewhere other than where it was clicked, and no amount
    // of looking at the screen tells you by how much.
    {
        doc = Doc_New();
        if (!doc) FAIL("could not allocate a model");

        for (int i = 0; i < 120; i++) {
            AddTextPara(doc, L"A line of body text long enough to wrap onto a second "
                             L"line when it is laid out on Letter paper.", &plain);
        }

        r = Layout_Build(doc, L"Calibri", 11.0f);
        if (!r) FAIL("Layout_Build failed on the geometry case");
        if (Layout_PageCount(r) < 2) FAIL("the geometry case did not paginate");

        for (int i = 0; i < Doc_CountParas(doc); i += 17) {
            DocPara* para = Doc_ParaAt(doc, i);
            unsigned len = Doc_ParaLength(para);

            for (unsigned off = 0; off <= len; off += 13) {
                LayoutPos pos = { para, off };
                LayoutRect caret = {};
                int page = -1;

                if (!Layout_PosRect(r, pos, &page, &caret)) FAIL("a position had no place on a page");
                if (page < 0 || page >= Layout_PageCount(r)) FAIL("a position landed on no page");

                LayoutPos back = {};
                if (!Layout_HitTest(r, page, caret.x + 0.5f, caret.y + caret.height * 0.5f, &back)) {
                    FAIL("hit testing the caret's own point found nothing");
                }
                if (back.para != para) FAIL("a point over a paragraph named a different one");
                if (back.offset != off) FAIL("a point over a character named a different one");
            }
        }

        // Document order, which is what tells a selection which end is which.
        LayoutPos first = { Doc_ParaAt(doc, 0), 0 };
        LayoutPos later = { Doc_ParaAt(doc, 5), 3 };
        if (Layout_ComparePos(r, first, later) >= 0) FAIL("paragraph order came out backwards");
        if (Layout_ComparePos(r, later, first) <= 0) FAIL("paragraph order is not symmetric");
        if (Layout_ComparePos(r, later, later) != 0) FAIL("a position did not equal itself");

        // Down a line and back up is where it started.
        LayoutPos start = { Doc_ParaAt(doc, 3), 4 };
        LayoutPos down = {}, up = {};
        if (!Layout_MoveLine(r, start, 1, &down)) FAIL("moving down a line failed");
        if (Layout_ComparePos(r, start, down) >= 0) FAIL("moving down did not move forwards");
        if (!Layout_MoveLine(r, down, -1, &up)) FAIL("moving back up a line failed");
        if (up.para != start.para) FAIL("down then up left a different paragraph");

        // Home and end of a wrapped line stay inside the paragraph.
        LayoutPos home = {}, end = {};
        if (!Layout_LineEdge(r, start, FALSE, &home)) FAIL("finding the start of a line failed");
        if (!Layout_LineEdge(r, start, TRUE, &end)) FAIL("finding the end of a line failed");
        if (home.para != start.para || end.para != start.para) FAIL("a line edge left the paragraph");
        if (Layout_ComparePos(r, home, end) >= 0) FAIL("a line ends before it starts");

        // A selection has something to draw.
        LayoutRect rects[32] = {};
        LayoutPos selEnd = { Doc_ParaAt(doc, 0), Doc_ParaLength(Doc_ParaAt(doc, 0)) };
        int n = Layout_RangeRects(r, 0, first, selEnd, rects, 32);
        if (n < 1) FAIL("a selection over a whole paragraph produced no rectangles");
        for (int i = 0; i < n; i++) {
            if (rects[i].width <= 0.0f || rects[i].height <= 0.0f) FAIL("a selection rectangle is empty");
        }

        // ...and a selection that is not on this page produces nothing.
        LayoutPos lastPara = { Doc_ParaAt(doc, Doc_CountParas(doc) - 1), 0 };
        LayoutPos lastEnd = { lastPara.para, Doc_ParaLength(Doc_ParaAt(doc, Doc_CountParas(doc) - 1)) };
        if (Layout_RangeRects(r, 0, lastPara, lastEnd, rects, 32) != 0) {
            FAIL("a selection on the last page drew on the first");
        }

        Layout_Free(r); r = NULL;
        Doc_Free(doc); doc = NULL;
    }

    // --- no widows and no orphans -----------------------------------------
    //
    // Paragraphs of every length from three lines to a dozen, so page
    // boundaries land in every possible place inside one. A paragraph that
    // gets broken must leave at least two lines on each side of the break;
    // one that cannot moves to the next page whole instead.
    {
        doc = Doc_New();
        if (!doc) FAIL("could not allocate a model");

        for (int i = 0; i < 60; i++) {
            DocPara* p = Doc_AddPara(doc);
            if (!p) FAIL("could not build the widow and orphan case");
            for (int j = 0; j < 2 + (i % 7); j++) {
                Doc_AddRun(p, L"A sentence of body text, long enough to take a line and "
                              L"a half of a Letter page all by itself. ", -1, &plain);
            }
        }

        r = Layout_Build(doc, L"Calibri", 11.0f);
        if (!r) FAIL("Layout_Build failed on the widow and orphan case");
        if (Layout_PageCount(r) < 3) FAIL("the widow and orphan case did not paginate");

        int split = 0;
        for (int pg = 0; pg < r->pageCount; pg++) {
            for (int i = 0; i < r->pages[pg].textCount; i++) {
                const LaidText* t = &r->pages[pg].texts[i];
                if (t->isMarker) continue;

                unsigned whole = Doc_ParaLength(t->para);
                BOOL isPiece = (t->textStart > 0) || (t->textLen < whole);
                if (!isPiece) continue;

                split++;
                if (PieceLines(t) < 2) FAIL("a broken paragraph left a single line alone");
            }
        }

        if (split < 2) FAIL("no paragraph was broken, so the rule was never tested");

        Layout_Free(r); r = NULL;
        Doc_Free(doc); doc = NULL;
    }

    // --- lists count, and count the way the document said ------------------
    {
        Numbering n = {};
        ParaProps item = {};
        WCHAR marker[64];

        #define MARKER(fmt_, level_, text_) do {                       item.list = LIST_NUMBER;                                    item.listId = 1;                                            item.numFormat = (fmt_);                                    item.listLevel = (level_);                                  wcscpy_s(item.listText, 24, (text_));                       NextMarker(&n, &item, marker, 64);                      } while (0)

        MARKER(NUMFMT_DECIMAL, 0, L"%1.");
        if (wcscmp(marker, L"1.") != 0) FAIL("the first item of a list is not 1.");
        MARKER(NUMFMT_DECIMAL, 0, L"%1.");
        if (wcscmp(marker, L"2.") != 0) FAIL("the second item of a list is not 2.");

        // A deeper level starts again, and its marker can name both levels.
        MARKER(NUMFMT_LOWER_LETTER, 1, L"%2)");
        if (wcscmp(marker, L"a)") != 0) FAIL("a nested level did not start at a");
        MARKER(NUMFMT_LOWER_LETTER, 1, L"%2)");
        if (wcscmp(marker, L"b)") != 0) FAIL("a nested level did not continue to b");

        // ...and coming back out continues where the outer level left off.
        MARKER(NUMFMT_DECIMAL, 0, L"%1.");
        if (wcscmp(marker, L"3.") != 0) FAIL("the outer level did not continue at 3");

        // Re-entering the nested level starts it over rather than continuing.
        MARKER(NUMFMT_LOWER_LETTER, 1, L"%2)");
        if (wcscmp(marker, L"a)") != 0) FAIL("a nested level did not restart under a new parent");

        // A marker naming both levels.
        MARKER(NUMFMT_DECIMAL, 1, L"%1.%2");
        if (wcscmp(marker, L"3.2") != 0) FAIL("a multi-level marker did not name both levels");

        // A different list counts separately.
        item.listId = 2;
        item.numFormat = NUMFMT_UPPER_ROMAN;
        item.listLevel = 0;
        wcscpy_s(item.listText, 24, L"%1.");
        NextMarker(&n, &item, marker, 64);
        if (wcscmp(marker, L"I.") != 0) FAIL("a second list did not start again at one");
        NextMarker(&n, &item, marker, 64);
        if (wcscmp(marker, L"II.") != 0) FAIL("roman numerals did not reach II");

        // Letters run out at z and double, which is what Word does.
        item.listId = 3;
        item.numFormat = NUMFMT_LOWER_LETTER;
        wcscpy_s(item.listText, 24, L"%1.");
        for (int i = 0; i < 27; i++) NextMarker(&n, &item, marker, 64);
        if (wcscmp(marker, L"aa.") != 0) FAIL("the 27th lettered item is not aa");

        // A bullet does not count at all, and changes with the level.
        item.listId = 4;
        item.list = LIST_BULLET;
        item.numFormat = NUMFMT_BULLET;
        item.listLevel = 0;
        item.listText[0] = 0;
        NextMarker(&n, &item, marker, 64);
        if (wcscmp(marker, L"\x2022") != 0) FAIL("a bullet is not a bullet");
        item.listLevel = 1;
        NextMarker(&n, &item, marker, 64);
        if (wcscmp(marker, L"\x25E6") != 0) FAIL("a nested bullet did not change shape");

        #undef MARKER
    }

    // --- a picture takes up the room it says it does ----------------------
    {
        doc = Doc_New();
        if (!doc) FAIL("could not allocate a model");

        // The bytes are never decoded to lay a picture out -- only its stated
        // size matters here -- so four of them will do.
        static const BYTE fake[4] = { 1, 2, 3, 4 };

        DocPara* p = Doc_AddPara(doc);
        if (!p) FAIL("could not build the picture case");
        Doc_AddRun(p, L"Before", -1, &plain);
        if (!Doc_AddImageRun(p, fake, sizeof(fake), L"image/png", 914400, 457200)) {
            FAIL("a picture could not be added to the model");
        }
        AddTextPara(doc, L"After", &plain);

        r = Layout_Build(doc, L"Calibri", 11.0f);
        if (!r) FAIL("Layout_Build failed on a document with a picture");

        if (Layout_PageImageCount(r, 0) != 1) FAIL("a picture was not placed on the page");

        const LaidImage* laid = &r->pages[0].images[0];
        // An inch is 96 DIPs; half an inch is 48.
        if (laid->width < 95.0f || laid->width > 97.0f)  FAIL("a picture is not the width it stated");
        if (laid->height < 47.0f || laid->height > 49.0f) FAIL("a picture is not the height it stated");
        if (laid->x < r->marginLeft) FAIL("a picture was placed outside the left margin");
        if (laid->image->len != sizeof(fake)) FAIL("the laid-out picture is not the one in the model");

        // The paragraph after it has to clear the picture rather than run
        // through it.
        float pictureBottom = laid->y + laid->height;
        BOOL clear = TRUE;
        for (int i = 0; i < r->pages[0].textCount; i++) {
            const LaidText* t = &r->pages[0].texts[i];
            if (t->para == p) continue;
            if (t->y < pictureBottom - 1.0f) clear = FALSE;
        }
        if (!clear) FAIL("the paragraph after a picture was drawn over it");

        Layout_Free(r); r = NULL;
        Doc_Free(doc); doc = NULL;
    }

    // --- columns fill across the page before reaching for another ---------
    {
        doc = Doc_New();
        if (!doc) FAIL("could not allocate a model");
        doc->section.columns = 2;
        doc->section.columnSpace = 480;

        for (int i = 0; i < 120; i++) {
            AddTextPara(doc, L"A paragraph of body text, filling a column so that "
                             L"the next one has to be used.", &plain);
        }

        r = Layout_Build(doc, L"Calibri", 11.0f);
        if (!r) FAIL("Layout_Build failed on a two-column document");

        // Two columns means two bands of text across the page, and the right
        // one starts past the middle.
        float middle = r->pageWidth / 2.0f;
        BOOL leftBand = FALSE, rightBand = FALSE;
        for (int i = 0; i < r->pages[0].textCount; i++) {
            const LaidText* t = &r->pages[0].texts[i];
            if (t->x < middle)  leftBand = TRUE;
            if (t->x >= middle) rightBand = TRUE;
            if (t->x + t->width > r->pageWidth - r->marginRight + 1.0f) {
                FAIL("a column ran past the right margin");
            }
        }
        if (!leftBand || !rightBand) FAIL("a two-column page used only one column");

        // A column is about half the width of the text area, not the whole of
        // it -- two columns hold the same amount of text as one, arranged
        // differently, so counting pages proves nothing.
        float textWidth = r->pageWidth - r->marginLeft - r->marginRight;
        for (int i = 0; i < r->pages[0].textCount; i++) {
            const LaidText* t = &r->pages[0].texts[i];
            if (t->width > textWidth * 0.6f) FAIL("a column is as wide as the page");
        }

        Layout_Free(r); r = NULL;
        Doc_Free(doc); doc = NULL;
    }

    // --- a page break starts a page ---------------------------------------
    {
        doc = Doc_New();
        if (!doc) FAIL("could not allocate a model");

        AddTextPara(doc, L"First page.", &plain);

        DocPara* second = AddTextPara(doc, L"Second page, because of a property.", &plain);
        if (!second) FAIL("could not build the page break case");
        second->props.pageBreakBefore = TRUE;

        DocPara* third = Doc_AddPara(doc);
        if (!third) FAIL("could not build the page break case");
        DocRun* brk = Doc_AddRun(third, L"", 0, &plain);
        if (!brk) FAIL("could not add a break run");
        brk->pageBreak = TRUE;
        Doc_AddRun(third, L"Third page, because of a break run.", -1, &plain);

        r = Layout_Build(doc, L"Calibri", 11.0f);
        if (!r) FAIL("Layout_Build failed on a document with page breaks");
        if (Layout_PageCount(r) != 3) FAIL("page breaks did not produce three pages");

        Layout_Free(r); r = NULL;
        Doc_Free(doc); doc = NULL;
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
