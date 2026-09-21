// The document model: construction, teardown, and the comparison the
// round-trip harness uses to measure what a save-and-reopen lost.

#include "supernote.h"
#include "core/doctree.h"

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

// Letter paper with one inch margins, matching what the .docx writer emits
// when a document does not say otherwise. Page Setup replaces it.
static SectionProps g_pageDefaults = {
    12240, 15840,
    1440, 1440, 1440, 1440
};

void Doc_SetPageDefaults(const SectionProps* page) {
    if (page) g_pageDefaults = *page;
}

void Doc_GetPageDefaults(SectionProps* out) {
    if (out) *out = g_pageDefaults;
}

DocModel* Doc_New(void) {
    DocModel* doc = (DocModel*)calloc(1, sizeof(DocModel));
    if (!doc) return NULL;

    doc->section = g_pageDefaults;
    return doc;
}

static void FreeRuns(DocRun* run) {
    while (run) {
        DocRun* next = run->next;
        free(run->text);
        free(run);
        run = next;
    }
}

static void FreeParas(DocPara* para) {
    while (para) {
        DocPara* next = para->next;
        FreeRuns(para->runs);
        free(para);
        para = next;
    }
}

static void FreeRows(DocRow* row) {
    while (row) {
        DocRow* nextRow = row->next;
        DocCell* cell = row->cells;
        while (cell) {
            DocCell* nextCell = cell->next;
            FreeParas(cell->paras);
            free(cell);
            cell = nextCell;
        }
        free(row);
        row = nextRow;
    }
}

void Doc_Free(DocModel* doc) {
    if (!doc) return;

    DocBlock* block = doc->blocks;
    while (block) {
        DocBlock* next = block->next;
        if (block->kind == BLOCK_PARA) FreeParas(block->para);
        else                           FreeRows(block->table.rows);
        free(block);
        block = next;
    }

    DocStyle* style = doc->styles;
    while (style) {
        DocStyle* next = style->next;
        free(style);
        style = next;
    }

    free(doc);
}

// Append to a singly linked list without walking it from the head every time.
#define APPEND(headField, node, type)                 \
    do {                                              \
        if (!(headField)) { (headField) = (node); }   \
        else {                                        \
            type* tail = (headField);                 \
            while (tail->next) tail = tail->next;     \
            tail->next = (node);                      \
        }                                             \
    } while (0)

static DocBlock* AddBlock(DocModel* doc, DocBlockKind kind) {
    if (!doc) return NULL;
    DocBlock* block = (DocBlock*)calloc(1, sizeof(DocBlock));
    if (!block) return NULL;
    block->kind = kind;
    APPEND(doc->blocks, block, DocBlock);
    return block;
}

DocPara* Doc_AddPara(DocModel* doc) {
    DocBlock* block = AddBlock(doc, BLOCK_PARA);
    if (!block) return NULL;

    DocPara* para = (DocPara*)calloc(1, sizeof(DocPara));
    if (!para) return NULL;
    block->para = para;
    return para;
}

DocBlock* Doc_AddTable(DocModel* doc) {
    return AddBlock(doc, BLOCK_TABLE);
}

DocRow* Doc_AddRow(DocBlock* table) {
    if (!table || table->kind != BLOCK_TABLE) return NULL;
    DocRow* row = (DocRow*)calloc(1, sizeof(DocRow));
    if (!row) return NULL;
    APPEND(table->table.rows, row, DocRow);
    return row;
}

DocCell* Doc_AddCell(DocRow* row) {
    if (!row) return NULL;
    DocCell* cell = (DocCell*)calloc(1, sizeof(DocCell));
    if (!cell) return NULL;
    APPEND(row->cells, cell, DocCell);
    return cell;
}

DocPara* Doc_AddCellPara(DocCell* cell) {
    if (!cell) return NULL;
    DocPara* para = (DocPara*)calloc(1, sizeof(DocPara));
    if (!para) return NULL;
    APPEND(cell->paras, para, DocPara);
    return para;
}

DocRun* Doc_AddRun(DocPara* para, const WCHAR* text, int len, const CharProps* props) {
    if (!para) return NULL;

    DocRun* run = (DocRun*)calloc(1, sizeof(DocRun));
    if (!run) return NULL;

    if (props) run->props = *props;

    if (len < 0) len = text ? (int)wcslen(text) : 0;
    run->text = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (!run->text) {
        free(run);
        return NULL;
    }
    if (len && text) memcpy(run->text, text, (size_t)len * sizeof(WCHAR));
    run->text[len] = L'\0';

    APPEND(para->runs, run, DocRun);
    return run;
}

// ---------------------------------------------------------------------------
// Named styles
//
// A style states some properties and inherits the rest from the style it is
// based on, which inherits in turn, down to the document defaults. Resolving
// that chain is the whole of styles.xml as far as this project is concerned:
// paragraphs carry resolved properties, and the table is kept only so the
// names survive being written back out.
//
// ponytail: "stated" means "not zero". A style cannot therefore turn bold off
// again once a style it is based on turned it on -- `w:b w:val="0"` reads as
// silence. Word documents do that rarely; a per-property "was it stated" bit
// on every property is the fix when one turns up that matters.
// ---------------------------------------------------------------------------

DocStyle* Doc_AddStyle(DocModel* doc, const WCHAR* id) {
    if (!doc || !id || !id[0]) return NULL;

    DocStyle* existing = Doc_FindStyle(doc, id);
    if (existing) return existing;

    DocStyle* style = (DocStyle*)calloc(1, sizeof(DocStyle));
    if (!style) return NULL;

    wcsncpy_s(style->id, 64, id, _TRUNCATE);
    APPEND(doc->styles, style, DocStyle);
    return style;
}

DocStyle* Doc_FindStyle(const DocModel* doc, const WCHAR* id) {
    if (!doc || !id || !id[0]) return NULL;
    for (DocStyle* s = doc->styles; s; s = s->next) {
        if (_wcsicmp(s->id, id) == 0) return s;
    }
    return NULL;
}

static void MergeCharProps(CharProps* into, const CharProps* from) {
    if (from->bold)        into->bold = TRUE;
    if (from->italic)      into->italic = TRUE;
    if (from->underline)   into->underline = TRUE;
    if (from->strike)      into->strike = TRUE;
    if (from->superscript) into->superscript = TRUE;
    if (from->subscript)   into->subscript = TRUE;
    if (from->halfPoints)  into->halfPoints = from->halfPoints;
    if (from->hasColor) {
        into->hasColor = TRUE;
        into->color = from->color;
    }
    if (from->font[0]) wcsncpy_s(into->font, LF_FACESIZE, from->font, _TRUNCATE);
}

static void MergeParaProps(ParaProps* into, const ParaProps* from) {
    if (from->align)        into->align = from->align;
    if (from->indentLeft)   into->indentLeft = from->indentLeft;
    if (from->indentFirst)  into->indentFirst = from->indentFirst;
    if (from->spaceBefore)  into->spaceBefore = from->spaceBefore;
    if (from->spaceAfter)   into->spaceAfter = from->spaceAfter;
    if (from->lineSpacing)  into->lineSpacing = from->lineSpacing;
    if (from->headingLevel) into->headingLevel = from->headingLevel;
    if (from->list != LIST_NONE) {
        into->list = from->list;
        into->listLevel = from->listLevel;
        into->numFormat = from->numFormat;
        if (from->listText[0]) wcsncpy_s(into->listText, 24, from->listText, _TRUNCATE);
    }
}

// Apply `id` and everything it is based on, root first, over what is already
// in `paraOut` and `runOut`.
static void ApplyStyleChain(const DocModel* doc, const WCHAR* id,
                            ParaProps* paraOut, CharProps* runOut, int depth) {
    if (depth > 16) return;     // a style based on itself, which Word tolerates

    const DocStyle* style = Doc_FindStyle(doc, id);
    if (!style) return;

    if (style->basedOn[0]) {
        ApplyStyleChain(doc, style->basedOn, paraOut, runOut, depth + 1);
    }

    if (paraOut) MergeParaProps(paraOut, &style->para);
    if (runOut)  MergeCharProps(runOut, &style->run);
}

void Doc_ResolveStyle(const DocModel* doc, const WCHAR* id,
                      ParaProps* paraOut, CharProps* runOut) {
    if (!doc) return;

    if (paraOut) *paraOut = doc->defaultPara;
    if (runOut)  *runOut = doc->defaultRun;

    if (id && id[0]) {
        ApplyStyleChain(doc, id, paraOut, runOut, 0);
        if (paraOut) wcsncpy_s(paraOut->style, 64, id, _TRUNCATE);
    }
}

// ---------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------

typedef void (*ParaVisitor)(const DocPara* para, void* ctx);

static void VisitParas(const DocModel* doc, ParaVisitor fn, void* ctx) {
    if (!doc) return;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) fn(p, ctx);
        } else {
            for (const DocRow* r = b->table.rows; r; r = r->next) {
                for (const DocCell* c = r->cells; c; c = c->next) {
                    for (const DocPara* p = c->paras; p; p = p->next) fn(p, ctx);
                }
            }
        }
    }
}

static void CountParaFn(const DocPara* para, void* ctx) {
    (void)para;
    (*(int*)ctx)++;
}

static void CountRunFn(const DocPara* para, void* ctx) {
    for (const DocRun* r = para->runs; r; r = r->next) (*(int*)ctx)++;
}

int Doc_CountParas(const DocModel* doc) {
    int n = 0;
    VisitParas(doc, CountParaFn, &n);
    return n;
}

int Doc_CountRuns(const DocModel* doc) {
    int n = 0;
    VisitParas(doc, CountRunFn, &n);
    return n;
}

int Doc_CountTables(const DocModel* doc) {
    int n = 0;
    if (!doc) return 0;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_TABLE) n++;
    }
    return n;
}

int Doc_CountCells(const DocModel* doc) {
    int n = 0;
    if (!doc) return 0;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind != BLOCK_TABLE) continue;
        for (const DocRow* r = b->table.rows; r; r = r->next) {
            for (const DocCell* c = r->cells; c; c = c->next) n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

typedef struct {
    WCHAR* buf;
    size_t len, cap;
} TextAcc;

static void AccAdd(TextAcc* acc, const WCHAR* s, size_t n) {
    if (acc->len + n + 2 > acc->cap) {
        size_t want = acc->cap ? acc->cap * 2 : 4096;
        while (want < acc->len + n + 2) want *= 2;
        WCHAR* grown = (WCHAR*)realloc(acc->buf, want * sizeof(WCHAR));
        if (!grown) return;
        acc->buf = grown;
        acc->cap = want;
    }
    if (n) memcpy(acc->buf + acc->len, s, n * sizeof(WCHAR));
    acc->len += n;
    acc->buf[acc->len] = L'\0';
}

static void TextParaFn(const DocPara* para, void* ctx) {
    TextAcc* acc = (TextAcc*)ctx;
    for (const DocRun* r = para->runs; r; r = r->next) {
        if (r->tab)            AccAdd(acc, L"\t", 1);
        else if (r->lineBreak) AccAdd(acc, L"\n", 1);
        else                   AccAdd(acc, r->text, wcslen(r->text));
    }
    AccAdd(acc, L"\n", 1);
}

WCHAR* Doc_GetText(const DocModel* doc) {
    TextAcc acc = {0};
    AccAdd(&acc, L"", 0);
    VisitParas(doc, TextParaFn, &acc);
    return acc.buf;
}

// ---------------------------------------------------------------------------
// Comparison
//
// The point is a number that can get worse. Every property that a save and
// reopen failed to preserve is counted, and the first few are described so a
// regression says what broke rather than only that something did.
// ---------------------------------------------------------------------------

static void DiffNote(DocDiff* d, const char* fmt, ...) {
    d->differences++;
    if (d->firstCount >= DOC_DIFF_MAX) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->first[d->firstCount], sizeof(d->first[0]), fmt, ap);
    va_end(ap);
    d->firstCount++;
}

#define CMP(cond, what, ...)                        \
    do {                                            \
        d->compared++;                              \
        if (!(cond)) DiffNote(d, what, __VA_ARGS__);    \
    } while (0)

// Fidelity is measured as: did anything the source *stated* fail to survive?
//
// A source that never specified a font and comes back saying Calibri has not
// lost anything -- the editor resolved a default. Counting that as loss would
// bury the real losses in noise, and make the number impossible to drive down.
// So an unspecified property in the source is not compared; a specified one
// that changed or went missing is.

static void CompareChar(const CharProps* a, const CharProps* b, int paraIdx, int at,
                        DocDiff* d) {
    CMP(a->bold == b->bold,               "para %d char %d: bold %d -> %d", paraIdx, at, a->bold, b->bold);
    CMP(a->italic == b->italic,           "para %d char %d: italic %d -> %d", paraIdx, at, a->italic, b->italic);
    CMP(a->underline == b->underline,     "para %d char %d: underline %d -> %d", paraIdx, at, a->underline, b->underline);
    CMP(a->strike == b->strike,           "para %d char %d: strike %d -> %d", paraIdx, at, a->strike, b->strike);
    CMP(a->superscript == b->superscript, "para %d char %d: superscript %d -> %d", paraIdx, at, a->superscript, b->superscript);
    CMP(a->subscript == b->subscript,     "para %d char %d: subscript %d -> %d", paraIdx, at, a->subscript, b->subscript);

    // Size and font: only when the source said so.
    if (a->halfPoints) {
        CMP(a->halfPoints == b->halfPoints, "para %d char %d: size %d -> %d",
            paraIdx, at, a->halfPoints, b->halfPoints);
    }
    if (a->font[0]) {
        // A font the source named must survive. The editor may substitute for
        // characters the face cannot render, which is font fallback rather
        // than loss, so a *different* face is only reported when the source
        // asked for one and got nothing.
        CMP(b->font[0] != L'\0', "para %d char %d: font \"%ls\" dropped",
            paraIdx, at, a->font);
    }
    if (a->hasColor) {
        CMP(b->hasColor && a->color == b->color, "para %d char %d: colour %06X -> %06X",
            paraIdx, at, (unsigned)a->color, (unsigned)b->color);
    }
}

static void CompareParaProps(const ParaProps* a, const ParaProps* b, int idx, DocDiff* d) {
    CMP(a->align == b->align,             "para %d: alignment %d -> %d", idx, a->align, b->align);
    CMP(a->list == b->list,               "para %d: list kind %d -> %d", idx, a->list, b->list);
    CMP(a->headingLevel == b->headingLevel, "para %d: heading level %d -> %d", idx, a->headingLevel, b->headingLevel);

    // Indents and spacing are only asserted where the source set them; a
    // document that said nothing has nothing to lose.
    if (a->list != LIST_NONE) {
        CMP(a->numFormat == b->numFormat, "para %d: number format %d -> %d", idx, a->numFormat, b->numFormat);
        CMP(a->listLevel == b->listLevel, "para %d: list level %d -> %d", idx, a->listLevel, b->listLevel);
        CMP(wcscmp(a->listText, b->listText) == 0, "para %d: list marker changed", idx);
    }
    if (a->style[0]) CMP(_wcsicmp(a->style, b->style) == 0, "para %d: style changed", idx);
    if (a->indentLeft)  CMP(a->indentLeft == b->indentLeft,   "para %d: left indent %d -> %d", idx, a->indentLeft, b->indentLeft);
    if (a->spaceBefore) CMP(a->spaceBefore == b->spaceBefore, "para %d: space before %d -> %d", idx, a->spaceBefore, b->spaceBefore);
    if (a->spaceAfter)  CMP(a->spaceAfter == b->spaceAfter,   "para %d: space after %d -> %d", idx, a->spaceAfter, b->spaceAfter);
}

// A paragraph flattened to one entry per character, so that comparison does
// not depend on where run boundaries happen to fall. A round trip may split or
// merge runs freely -- the editor does, when a font cannot render a character
// -- and none of that is a change to the document.
#define MAX_FLAT 8192

typedef struct {
    WCHAR     text[MAX_FLAT];
    CharProps props[MAX_FLAT];
    int       len;
} FlatPara;

static void Flatten(const DocPara* para, FlatPara* out) {
    out->len = 0;
    for (const DocRun* r = para->runs; r && out->len < MAX_FLAT; r = r->next) {
        if (r->tab || r->lineBreak) {
            out->text[out->len] = r->tab ? L'\t' : L'\n';
            out->props[out->len] = r->props;
            out->len++;
            continue;
        }
        for (const WCHAR* c = r->text; *c && out->len < MAX_FLAT; c++) {
            out->text[out->len] = *c;
            out->props[out->len] = r->props;
            out->len++;
        }
    }
}

static void CompareParas(const DocPara* pa, const DocPara* pb, int idx, DocDiff* d) {
    CompareParaProps(&pa->props, &pb->props, idx, d);

    static FlatPara fa, fb;
    Flatten(pa, &fa);
    Flatten(pb, &fb);

    d->compared++;
    if (fa.len != fb.len ||
        wcsncmp(fa.text, fb.text, (size_t)fa.len) != 0) {
        DiffNote(d, "para %d: text changed", idx);
        return;   // positions no longer line up; per-character comparison would be noise
    }

    for (int i = 0; i < fa.len; i++) {
        CompareChar(&fa.props[i], &fb.props[i], idx, i, d);
    }
}

typedef struct {
    const DocPara* paras[4096];
    int            count;
} ParaList;

static void CollectFn(const DocPara* para, void* ctx) {
    ParaList* list = (ParaList*)ctx;
    if (list->count < 4096) list->paras[list->count++] = para;
}

void Doc_Compare(const DocModel* a, const DocModel* b, DocDiff* d) {
    memset(d, 0, sizeof(*d));
    if (!a || !b) {
        DiffNote(d, "a document is missing");
        return;
    }

    d->compared++;
    if (Doc_CountTables(a) != Doc_CountTables(b)) {
        DiffNote(d, "table count %d -> %d", Doc_CountTables(a), Doc_CountTables(b));
    }

    d->compared++;
    if (Doc_CountCells(a) != Doc_CountCells(b)) {
        DiffNote(d, "cell count %d -> %d", Doc_CountCells(a), Doc_CountCells(b));
    }

    // Column widths. A table that comes back without its grid is auto-sized by
    // the reader, which silently changes how the document looks, so a grid the
    // source stated has to survive.
    {
        const DocBlock* ta = a->blocks;
        const DocBlock* tb = b->blocks;
        int tableIdx = 0;

        while (ta && tb) {
            while (ta && ta->kind != BLOCK_TABLE) ta = ta->next;
            while (tb && tb->kind != BLOCK_TABLE) tb = tb->next;
            if (!ta || !tb) break;

            if (ta->table.gridCount > 0) {
                d->compared++;
                if (tb->table.gridCount != ta->table.gridCount) {
                    DiffNote(d, "table %d: %d column widths -> %d", tableIdx,
                             ta->table.gridCount, tb->table.gridCount);
                } else {
                    for (int i = 0; i < ta->table.gridCount; i++) {
                        d->compared++;
                        if (ta->table.gridEdges[i] != tb->table.gridEdges[i]) {
                            DiffNote(d, "table %d column %d: edge %d -> %d", tableIdx, i,
                                     ta->table.gridEdges[i], tb->table.gridEdges[i]);
                        }
                    }
                }
            }

            ta = ta->next;
            tb = tb->next;
            tableIdx++;
        }
    }

    static ParaList la, lb;
    la.count = lb.count = 0;
    VisitParas(a, CollectFn, &la);
    VisitParas(b, CollectFn, &lb);

    d->compared++;
    if (la.count != lb.count) {
        DiffNote(d, "paragraph count %d -> %d", la.count, lb.count);
    }

    int n = la.count < lb.count ? la.count : lb.count;
    for (int i = 0; i < n; i++) {
        CompareParas(la.paras[i], lb.paras[i], i, d);
    }
}

// ---------------------------------------------------------------------------
// Positions and editing
//
// A position in the document is a paragraph and an offset into that
// paragraph's text, where the text is its runs concatenated: a tab and a line
// break are one character each, and an empty run is not there at all. That is
// exactly what the layout engine flattens a paragraph to, which is what lets a
// click on a laid-out page name a place in the model.
// ---------------------------------------------------------------------------

// The characters one run contributes.
static unsigned RunLength(const DocRun* run) {
    if (run->tab || run->lineBreak) return 1;
    return run->text ? (unsigned)wcslen(run->text) : 0;
}

unsigned Doc_ParaLength(const DocPara* para) {
    unsigned n = 0;
    if (!para) return 0;
    for (const DocRun* r = para->runs; r; r = r->next) n += RunLength(r);
    return n;
}

WCHAR* Doc_ParaText(const DocPara* para, unsigned* lenOut) {
    unsigned len = Doc_ParaLength(para);
    WCHAR* out = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (!out) return NULL;

    unsigned at = 0;
    for (const DocRun* r = para ? para->runs : NULL; r; r = r->next) {
        unsigned n = RunLength(r);
        if (!n) continue;
        if (r->tab)            out[at] = L'\t';
        else if (r->lineBreak) out[at] = L'\n';
        else                   memcpy(out + at, r->text, n * sizeof(WCHAR));
        at += n;
    }
    out[len] = L'\0';

    if (lenOut) *lenOut = len;
    return out;
}

// ---------------------------------------------------------------------------
// Paragraphs in document order
// ---------------------------------------------------------------------------

DocPara* Doc_ParaAt(const DocModel* doc, int index) {
    if (!doc || index < 0) return NULL;

    int seen = 0;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (DocPara* p = b->para; p; p = p->next) {
                if (seen++ == index) return p;
            }
        } else {
            for (const DocRow* r = b->table.rows; r; r = r->next) {
                for (const DocCell* c = r->cells; c; c = c->next) {
                    for (DocPara* p = c->paras; p; p = p->next) {
                        if (seen++ == index) return p;
                    }
                }
            }
        }
    }
    return NULL;
}

int Doc_ParaIndexOf(const DocModel* doc, const DocPara* para) {
    if (!doc || !para) return -1;

    int seen = 0;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next, seen++) {
                if (p == para) return seen;
            }
        } else {
            for (const DocRow* r = b->table.rows; r; r = r->next) {
                for (const DocCell* c = r->cells; c; c = c->next) {
                    for (const DocPara* p = c->paras; p; p = p->next, seen++) {
                        if (p == para) return seen;
                    }
                }
            }
        }
    }
    return -1;
}

// The cell a paragraph sits in, or NULL when it is a body paragraph. Two
// positions can only be edited as one range when this agrees for both: a
// selection running from the page into a table is not a thing the model can
// splice.
const DocCell* Doc_ParaCell(const DocModel* doc, const DocPara* para) {
    if (!doc || !para) return NULL;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind != BLOCK_TABLE) continue;
        for (const DocRow* r = b->table.rows; r; r = r->next) {
            for (const DocCell* c = r->cells; c; c = c->next) {
                for (const DocPara* p = c->paras; p; p = p->next) {
                    if (p == para) return c;
                }
            }
        }
    }
    return NULL;
}

// The list a paragraph belongs to, and the block that owns that list. Editing
// needs both: the head pointer to unlink through, and the block so an emptied
// one can go with it.
static DocPara** ParaChain(DocModel* doc, const DocPara* para, DocBlock** blockOut) {
    if (blockOut) *blockOut = NULL;
    if (!doc || !para) return NULL;

    for (DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) {
                if (p != para) continue;
                if (blockOut) *blockOut = b;
                return &b->para;
            }
        } else {
            for (DocRow* r = b->table.rows; r; r = r->next) {
                for (DocCell* c = r->cells; c; c = c->next) {
                    for (const DocPara* p = c->paras; p; p = p->next) {
                        if (p != para) continue;
                        if (blockOut) *blockOut = b;
                        return &c->paras;
                    }
                }
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Editing one paragraph's runs
// ---------------------------------------------------------------------------

// Drop runs that no longer carry anything. An empty text run would be
// invisible but would still be counted and written out.
static void PruneEmptyRuns(DocPara* para) {
    DocRun* prev = NULL;
    DocRun* run = para->runs;
    while (run) {
        DocRun* next = run->next;
        if (RunLength(run) == 0) {
            if (prev) prev->next = next;
            else      para->runs = next;
            free(run->text);
            free(run);
        } else {
            prev = run;
        }
        run = next;
    }
}

// Insert text into the run that already covers `offset`, so typing inherits
// the formatting of what it is typed into. Only at a run boundary does the
// neighbour on the left decide, which is what every editor does.
static BOOL InsertIntoRuns(DocPara* para, unsigned offset, const WCHAR* text, unsigned len) {
    DocRun* target = NULL;      // the text run to grow
    unsigned into = 0;          // where in that run's text
    unsigned at = 0;

    for (DocRun* r = para->runs; r; r = r->next) {
        unsigned n = RunLength(r);
        if (!n) continue;

        unsigned start = at;
        at += n;
        if (r->tab || r->lineBreak) continue;    // nowhere to put characters

        if (offset > start && offset < at) {     // inside this run: settled
            target = r;
            into = offset - start;
            break;
        }
        if (offset == at || (offset == start && !target)) {
            // On a boundary. The run to the left owns it, which is why typing
            // at the join of plain and bold text comes out plain.
            target = r;
            into = offset - start;
        }
    }

    if (!target) {
        // Nothing in the paragraph that can hold text -- it is empty, or it is
        // all tabs and breaks.
        CharProps props = {0};
        if (para->runs) props = para->runs->props;
        return Doc_AddRun(para, text, (int)len, &props) != NULL;
    }

    unsigned old = (unsigned)wcslen(target->text);
    WCHAR* grown = (WCHAR*)realloc(target->text, ((size_t)old + len + 1) * sizeof(WCHAR));
    if (!grown) return FALSE;

    memmove(grown + into + len, grown + into, ((size_t)old - into + 1) * sizeof(WCHAR));
    memcpy(grown + into, text, (size_t)len * sizeof(WCHAR));
    target->text = grown;
    return TRUE;
}

// Cut [from, to) out of one paragraph.
static void DeleteInPara(DocPara* para, unsigned from, unsigned to) {
    if (!para || to <= from) return;

    unsigned at = 0;
    for (DocRun* r = para->runs; r; r = r->next) {
        unsigned n = RunLength(r);
        if (!n) continue;

        unsigned start = at, end = at + n;
        at = end;

        if (end <= from || start >= to) continue;

        unsigned cutFrom = (from > start ? from : start) - start;
        unsigned cutTo   = (to < end ? to : end) - start;

        if (r->tab || r->lineBreak) {
            // One character, and the range covers it: turn it into an empty
            // text run and let the prune take it.
            r->tab = FALSE;
            r->lineBreak = FALSE;
            if (!r->text) r->text = (WCHAR*)calloc(1, sizeof(WCHAR));
            else          r->text[0] = L'\0';
            continue;
        }

        unsigned len = (unsigned)wcslen(r->text);
        memmove(r->text + cutFrom, r->text + cutTo,
                ((size_t)len - cutTo + 1) * sizeof(WCHAR));
    }

    PruneEmptyRuns(para);
}

// Everything from `offset` to the end of the paragraph, moved onto `into`.
// Used by both the split (the tail becomes the new paragraph) and the merge
// that a backspace at the start of a paragraph is.
static BOOL MoveTailRuns(DocPara* from, unsigned offset, DocPara* into) {
    unsigned at = 0;
    DocRun* prev = NULL;
    DocRun* run = from->runs;

    while (run) {
        unsigned n = RunLength(run);
        unsigned start = at;
        at += n;

        if (n && start < offset && start + n > offset) {
            // The split falls inside this run: leave the head, carry the tail.
            unsigned cut = offset - start;
            CharProps props = run->props;
            if (!Doc_AddRun(into, run->text + cut, (int)(n - cut), &props)) return FALSE;
            run->text[cut] = L'\0';
            prev = run;
            run = run->next;
            continue;
        }

        if (start >= offset) {
            DocRun* next = run->next;
            if (prev) prev->next = next;
            else      from->runs = next;

            run->next = NULL;
            APPEND(into->runs, run, DocRun);
            run = next;
            continue;
        }

        prev = run;
        run = run->next;
    }

    PruneEmptyRuns(from);
    return TRUE;
}

// Unlink a paragraph and free it.
static void RemovePara(DocModel* doc, DocPara* victim) {
    DocBlock* block = NULL;
    DocPara** head = ParaChain(doc, victim, &block);
    if (!head) return;

    DocPara* prev = NULL;
    for (DocPara* p = *head; p; p = p->next) {
        if (p == victim) break;
        prev = p;
    }

    if (prev) prev->next = victim->next;
    else      *head = victim->next;

    victim->next = NULL;
    FreeParas(victim);

    // A paragraph block with nothing left in it is not a blank line, it is
    // nothing at all.
    if (block && block->kind == BLOCK_PARA && !block->para) {
        DocBlock* prevBlock = NULL;
        for (DocBlock* b = doc->blocks; b; b = b->next) {
            if (b == block) break;
            prevBlock = b;
        }
        if (prevBlock) prevBlock->next = block->next;
        else           doc->blocks = block->next;
        free(block);
    }
}

// ---------------------------------------------------------------------------
// The editing operations themselves
// ---------------------------------------------------------------------------

BOOL DocEdit_Insert(DocModel* doc, DocPos* at, const WCHAR* text, int len) {
    (void)doc;
    if (!at || !at->para || !text) return FALSE;
    if (len < 0) len = (int)wcslen(text);
    if (len == 0) return TRUE;

    unsigned paraLen = Doc_ParaLength(at->para);
    if (at->offset > paraLen) at->offset = paraLen;

    if (!InsertIntoRuns(at->para, at->offset, text, (unsigned)len)) return FALSE;

    at->offset += (unsigned)len;
    return TRUE;
}

BOOL DocEdit_SplitPara(DocModel* doc, DocPos* at) {
    if (!doc || !at || !at->para) return FALSE;

    DocBlock* block = NULL;
    DocPara** head = ParaChain(doc, at->para, &block);
    if (!head) return FALSE;

    DocPara* tail = (DocPara*)calloc(1, sizeof(DocPara));
    if (!tail) return FALSE;

    // The new paragraph keeps the old one's shape -- indent, alignment,
    // spacing -- but not its heading level: pressing Enter at the end of a
    // heading starts body text, which is what Word does and what anybody
    // typing expects.
    tail->props = at->para->props;
    tail->props.headingLevel = 0;

    unsigned paraLen = Doc_ParaLength(at->para);
    if (at->offset > paraLen) at->offset = paraLen;

    if (!MoveTailRuns(at->para, at->offset, tail)) {
        FreeParas(tail);
        return FALSE;
    }

    tail->next = at->para->next;
    at->para->next = tail;

    at->para = tail;
    at->offset = 0;
    return TRUE;
}

// Is `b` reachable from `a` by walking paragraphs forward, without crossing a
// table on the way? Fills `count` with how many paragraphs lie between them.
static BOOL ParaReaches(DocModel* doc, DocPara* a, DocPara* b) {
    DocBlock* blockA = NULL;
    DocPara** chainA = ParaChain(doc, a, &blockA);
    DocBlock* blockB = NULL;
    DocPara** chainB = ParaChain(doc, b, &blockB);
    if (!chainA || !chainB) return FALSE;

    if (chainA == chainB) {
        for (DocPara* p = a; p; p = p->next) {
            if (p == b) return TRUE;
        }
        return FALSE;
    }

    // Different lists: only body paragraphs in consecutive paragraph blocks
    // can be spliced together. Anything else means the range runs into or out
    // of a table.
    if (!blockA || !blockB) return FALSE;
    if (blockA->kind != BLOCK_PARA || blockB->kind != BLOCK_PARA) return FALSE;

    for (DocBlock* blk = blockA->next; blk; blk = blk->next) {
        if (blk->kind != BLOCK_PARA) return FALSE;   // a table in the way
        if (blk == blockB) return TRUE;
    }
    return FALSE;
}

BOOL DocEdit_DeleteRange(DocModel* doc, DocPos a, DocPos b, DocPos* out) {
    if (!doc || !a.para || !b.para) return FALSE;

    if (a.para == b.para) {
        if (b.offset <= a.offset) return FALSE;
        DeleteInPara(a.para, a.offset, b.offset);
        if (out) *out = a;
        return TRUE;
    }

    if (!ParaReaches(doc, a.para, b.para)) return FALSE;

    // Everything between the two ends goes, and the walk that finds it uses
    // the links the removal is about to change -- so it happens first, before
    // a single character is touched.
    DocBlock* blockA = NULL;
    ParaChain(doc, a.para, &blockA);

    DocPara** doomed = NULL;
    int doomedCount = 0, doomedCap = 0;

    DocPara* p = a.para->next;
    DocBlock* blk = blockA;
    for (;;) {
        if (!p) {
            blk = blk ? blk->next : NULL;
            if (!blk || blk->kind != BLOCK_PARA) break;
            p = blk->para;
            continue;
        }

        if (doomedCount == doomedCap) {
            int cap = doomedCap ? doomedCap * 2 : 32;
            DocPara** grown = (DocPara**)realloc(doomed, (size_t)cap * sizeof(DocPara*));
            if (!grown) { free(doomed); return FALSE; }
            doomed = grown;
            doomedCap = cap;
        }
        doomed[doomedCount++] = p;

        if (p == b.para) break;
        p = p->next;
    }

    if (doomedCount == 0 || doomed[doomedCount - 1] != b.para) {
        free(doomed);
        return FALSE;
    }

    // Trim both ends, move what is left of the last paragraph onto the first,
    // and drop everything in between.
    DeleteInPara(a.para, a.offset, Doc_ParaLength(a.para));
    DeleteInPara(b.para, 0, b.offset);

    BOOL moved = MoveTailRuns(b.para, 0, a.para);
    for (int i = 0; i < doomedCount; i++) RemovePara(doc, doomed[i]);
    free(doomed);

    if (out) *out = a;
    return moved;
}

WCHAR* DocEdit_RangeText(const DocModel* doc, DocPos a, DocPos b) {
    if (!doc || !a.para || !b.para) return NULL;

    int from = Doc_ParaIndexOf(doc, a.para);
    int to = Doc_ParaIndexOf(doc, b.para);
    if (from < 0 || to < 0 || to < from) return NULL;

    TextAcc acc = {0};

    for (int i = from; i <= to; i++) {
        const DocPara* para = Doc_ParaAt(doc, i);
        if (!para) break;

        unsigned len = 0;
        WCHAR* text = Doc_ParaText(para, &len);
        if (!text) break;

        unsigned start = (i == from) ? a.offset : 0;
        unsigned end   = (i == to)   ? b.offset : len;
        if (start > len) start = len;
        if (end > len)   end = len;

        if (end > start) AccAdd(&acc, text + start, end - start);
        if (i != to)     AccAdd(&acc, L"\n", 1);

        free(text);
    }

    if (!acc.buf) {
        acc.buf = (WCHAR*)calloc(1, sizeof(WCHAR));
        return acc.buf;
    }

    acc.buf[acc.len] = L'\0';
    return acc.buf;
}

// ---------------------------------------------------------------------------
// Cloning, which is how undo works
//
// A snapshot per edit rather than an inverse operation per edit: the model is
// a few hundred kilobytes for a document anybody is typing into, and a wrong
// inverse is a corruption that shows up three edits later.
//
// ponytail: the stack is bounded by its holder, not here. If documents get
// big enough for this to hurt, the fix is an operation log, not a smaller cap.
// ---------------------------------------------------------------------------

static BOOL CloneParas(const DocPara* src, DocPara** dest) {
    for (const DocPara* p = src; p; p = p->next) {
        DocPara* copy = (DocPara*)calloc(1, sizeof(DocPara));
        if (!copy) return FALSE;
        copy->props = p->props;

        for (const DocRun* r = p->runs; r; r = r->next) {
            DocRun* rc = (DocRun*)calloc(1, sizeof(DocRun));
            if (!rc) return FALSE;
            rc->props = r->props;
            rc->tab = r->tab;
            rc->lineBreak = r->lineBreak;

            size_t n = r->text ? wcslen(r->text) : 0;
            rc->text = (WCHAR*)malloc((n + 1) * sizeof(WCHAR));
            if (!rc->text) { free(rc); return FALSE; }
            if (n) memcpy(rc->text, r->text, n * sizeof(WCHAR));
            rc->text[n] = L'\0';

            APPEND(copy->runs, rc, DocRun);
        }

        APPEND(*dest, copy, DocPara);
    }
    return TRUE;
}

DocModel* Doc_Clone(const DocModel* src) {
    if (!src) return NULL;

    DocModel* copy = (DocModel*)calloc(1, sizeof(DocModel));
    if (!copy) return NULL;
    copy->section = src->section;
    copy->defaultPara = src->defaultPara;
    copy->defaultRun = src->defaultRun;

    for (const DocStyle* st = src->styles; st; st = st->next) {
        DocStyle* sc = (DocStyle*)calloc(1, sizeof(DocStyle));
        if (!sc) { Doc_Free(copy); return NULL; }
        *sc = *st;
        sc->next = NULL;
        APPEND(copy->styles, sc, DocStyle);
    }

    for (const DocBlock* b = src->blocks; b; b = b->next) {
        DocBlock* bc = (DocBlock*)calloc(1, sizeof(DocBlock));
        if (!bc) { Doc_Free(copy); return NULL; }
        bc->kind = b->kind;

        if (b->kind == BLOCK_PARA) {
            if (!CloneParas(b->para, &bc->para)) {
                free(bc);
                Doc_Free(copy);
                return NULL;
            }
        } else {
            bc->table.gridCount = b->table.gridCount;
            memcpy(bc->table.gridEdges, b->table.gridEdges, sizeof(bc->table.gridEdges));

            for (const DocRow* r = b->table.rows; r; r = r->next) {
                DocRow* rc = (DocRow*)calloc(1, sizeof(DocRow));
                if (!rc) { Doc_Free(copy); free(bc); return NULL; }

                for (const DocCell* c = r->cells; c; c = c->next) {
                    DocCell* cc = (DocCell*)calloc(1, sizeof(DocCell));
                    if (!cc) { Doc_Free(copy); free(bc); return NULL; }
                    if (!CloneParas(c->paras, &cc->paras)) {
                        Doc_Free(copy);
                        free(bc);
                        return NULL;
                    }
                    APPEND(rc->cells, cc, DocCell);
                }

                APPEND(bc->table.rows, rc, DocRow);
            }
        }

        APPEND(copy->blocks, bc, DocBlock);
    }

    return copy;
}

// ---------------------------------------------------------------------------
// Self-check for the editing operations. Run with: OpenNote.exe --selftest
//
// Every keystroke in the page view goes through these, and undo is a stack of
// the clone below, so a fault here is a document quietly losing text. The
// checks assert on the resulting text rather than on the run structure: what
// matters is that the characters are right and the formatting survived, not
// how the runs were arranged to manage it.
// ---------------------------------------------------------------------------

static BOOL ParaTextIs(const DocPara* para, const WCHAR* want) {
    unsigned len = 0;
    WCHAR* got = Doc_ParaText(para, &len);
    if (!got) return FALSE;
    BOOL ok = wcscmp(got, want) == 0;
    free(got);
    return ok;
}

BOOL DocEdit_SelfTest(char* failure, size_t failureSize) {
    DocModel* doc = NULL;

    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        Doc_Free(doc); \
        return FALSE; \
    } while (0)

    CharProps plain = {0};
    CharProps bold = {0};
    bold.bold = TRUE;

    // --- a paragraph's length and its text agree, tabs and breaks included --
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        Doc_AddRun(p, L"ab", -1, &plain);
        Doc_AddRun(p, L"", -1, &plain)->tab = TRUE;
        Doc_AddRun(p, L"cd", -1, &bold);

        if (Doc_ParaLength(p) != 5) FAIL("a tab did not count as one character");
        if (!ParaTextIs(p, L"ab\tcd")) FAIL("a paragraph's text is not its runs in order");
    }
    Doc_Free(doc); doc = NULL;

    // --- typing inside a run keeps that run's formatting --------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        Doc_AddRun(p, L"Hello world", -1, &plain);

        DocPos at = { p, 5 };
        if (!DocEdit_Insert(doc, &at, L" big", -1)) FAIL("insert failed");
        if (!ParaTextIs(p, L"Hello big world")) FAIL("insert put the text in the wrong place");
        if (at.offset != 9) FAIL("insert did not advance the position past the text");
        if (Doc_CountRuns(doc) != 1) FAIL("insert split a run it could have grown");
    }
    Doc_Free(doc); doc = NULL;

    // --- typing at the join of two runs belongs to the one on the left ------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        Doc_AddRun(p, L"plain", -1, &plain);
        Doc_AddRun(p, L"BOLD", -1, &bold);

        DocPos at = { p, 5 };
        if (!DocEdit_Insert(doc, &at, L"X", -1)) FAIL("insert at a run boundary failed");
        if (!ParaTextIs(p, L"plainXBOLD")) FAIL("insert at a boundary landed wrong");

        const DocRun* first = p->runs;
        if (!first || first->props.bold) FAIL("a character typed after plain text came out bold");
        if (wcscmp(first->text, L"plainX") != 0) FAIL("the boundary character joined the wrong run");
    }
    Doc_Free(doc); doc = NULL;

    // --- Enter splits a paragraph and keeps its shape ------------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        p->props.indentLeft = 720;
        p->props.headingLevel = 1;
        Doc_AddRun(p, L"Before", -1, &plain);
        Doc_AddRun(p, L"After", -1, &bold);

        DocPos at = { p, 6 };
        if (!DocEdit_SplitPara(doc, &at)) FAIL("split failed");
        if (!at.para || at.offset != 0) FAIL("split left the position somewhere odd");
        if (at.para == p) FAIL("split did not make a second paragraph");

        if (!ParaTextIs(p, L"Before")) FAIL("split lost the head of the paragraph");
        if (!ParaTextIs(at.para, L"After")) FAIL("split lost the tail of the paragraph");
        if (at.para->props.indentLeft != 720) FAIL("split dropped the paragraph's indent");
        if (at.para->props.headingLevel != 0) FAIL("Enter at the end of a heading made another heading");
        if (!at.para->runs || !at.para->runs->props.bold) FAIL("split lost the tail's formatting");
        if (Doc_CountParas(doc) != 2) FAIL("split did not leave two paragraphs");
    }
    Doc_Free(doc); doc = NULL;

    // --- splitting mid-run cuts the run, not the paragraph's text -----------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        Doc_AddRun(p, L"onetwo", -1, &plain);

        DocPos at = { p, 3 };
        if (!DocEdit_SplitPara(doc, &at)) FAIL("split inside a run failed");
        if (!ParaTextIs(p, L"one")) FAIL("split inside a run kept too much");
        if (!ParaTextIs(at.para, L"two")) FAIL("split inside a run lost the tail");
    }
    Doc_Free(doc); doc = NULL;

    // --- deleting inside one paragraph --------------------------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        Doc_AddRun(p, L"Hello ", -1, &plain);
        Doc_AddRun(p, L"cruel ", -1, &bold);
        Doc_AddRun(p, L"world", -1, &plain);

        DocPos a = { p, 6 }, b = { p, 12 }, out = {0};
        if (!DocEdit_DeleteRange(doc, a, b, &out)) FAIL("delete within a paragraph failed");
        if (!ParaTextIs(p, L"Hello world")) FAIL("delete removed the wrong characters");
        if (Doc_CountRuns(doc) != 2) FAIL("an emptied run was left behind");
    }
    Doc_Free(doc); doc = NULL;

    // --- deleting across paragraphs merges them -----------------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p1 = Doc_AddPara(doc);
        Doc_AddRun(p1, L"First line", -1, &plain);
        DocPara* p2 = Doc_AddPara(doc);
        Doc_AddRun(p2, L"Second line", -1, &plain);
        DocPara* p3 = Doc_AddPara(doc);
        Doc_AddRun(p3, L"Third line", -1, &plain);

        DocPos a = { p1, 6 }, b = { p3, 6 }, out = {0};
        if (!DocEdit_DeleteRange(doc, a, b, &out)) FAIL("delete across paragraphs failed");
        if (Doc_CountParas(doc) != 1) FAIL("delete across paragraphs did not merge them");
        if (!ParaTextIs(Doc_ParaAt(doc, 0), L"First line")) FAIL("the merge kept the wrong text");
        if (out.para != p1 || out.offset != 6) FAIL("delete left the caret somewhere odd");
    }
    Doc_Free(doc); doc = NULL;

    // --- backspace at the start of a paragraph joins it to the one above ----
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p1 = Doc_AddPara(doc);
        Doc_AddRun(p1, L"one", -1, &plain);
        DocPara* p2 = Doc_AddPara(doc);
        Doc_AddRun(p2, L"two", -1, &bold);

        DocPos a = { p1, 3 }, b = { p2, 0 }, out = {0};
        if (!DocEdit_DeleteRange(doc, a, b, &out)) FAIL("joining two paragraphs failed");
        if (Doc_CountParas(doc) != 1) FAIL("the paragraphs did not join");
        if (!ParaTextIs(Doc_ParaAt(doc, 0), L"onetwo")) FAIL("the join lost text");
        if (Doc_CountRuns(doc) != 2) FAIL("the join flattened the formatting");
    }
    Doc_Free(doc); doc = NULL;

    // --- a range running into a table is refused, not half-done -------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* body = Doc_AddPara(doc);
        Doc_AddRun(body, L"Body text", -1, &plain);

        DocBlock* t = Doc_AddTable(doc);
        DocRow* row = Doc_AddRow(t);
        DocPara* cellPara = Doc_AddCellPara(Doc_AddCell(row));
        Doc_AddRun(cellPara, L"In a cell", -1, &plain);

        DocPos a = { body, 2 }, b = { cellPara, 4 }, out = {0};
        if (DocEdit_DeleteRange(doc, a, b, &out)) FAIL("a delete running into a table was allowed");
        if (!ParaTextIs(body, L"Body text")) FAIL("a refused delete still changed the document");
        if (!ParaTextIs(cellPara, L"In a cell")) FAIL("a refused delete still changed the cell");

        // ...but the same range inside one cell is ordinary editing.
        DocPos ca = { cellPara, 0 }, cb = { cellPara, 3 };
        if (!DocEdit_DeleteRange(doc, ca, cb, &out)) FAIL("editing inside a cell was refused");
        if (!ParaTextIs(cellPara, L"a cell")) FAIL("editing inside a cell went wrong");

        if (Doc_ParaCell(doc, cellPara) == NULL) FAIL("a cell paragraph was not recognised as one");
        if (Doc_ParaCell(doc, body) != NULL) FAIL("a body paragraph was taken for a cell one");
    }
    Doc_Free(doc); doc = NULL;

    // --- paragraph indices address cell paragraphs too ----------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* body = Doc_AddPara(doc);
        Doc_AddRun(body, L"Body", -1, &plain);
        DocBlock* t = Doc_AddTable(doc);
        DocRow* row = Doc_AddRow(t);
        DocPara* cellPara = Doc_AddCellPara(Doc_AddCell(row));
        Doc_AddRun(cellPara, L"Cell", -1, &plain);

        int count = Doc_CountParas(doc);
        for (int i = 0; i < count; i++) {
            DocPara* p = Doc_ParaAt(doc, i);
            if (!p) FAIL("a paragraph index did not resolve");
            if (Doc_ParaIndexOf(doc, p) != i) FAIL("paragraph index and lookup disagree");
        }
        if (Doc_ParaAt(doc, count) != NULL) FAIL("an index past the end resolved to something");
    }
    Doc_Free(doc); doc = NULL;

    // --- the text of a range, which is what the clipboard gets --------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p1 = Doc_AddPara(doc);
        Doc_AddRun(p1, L"First", -1, &plain);
        DocPara* p2 = Doc_AddPara(doc);
        Doc_AddRun(p2, L"Second", -1, &plain);

        DocPos a = { p1, 2 }, b = { p2, 3 };
        WCHAR* text = DocEdit_RangeText(doc, a, b);
        if (!text) FAIL("range text produced nothing");
        BOOL ok = wcscmp(text, L"rst\nSec") == 0;
        free(text);
        if (!ok) FAIL("range text is not what the range covers");
    }
    Doc_Free(doc); doc = NULL;

    // --- a clone is equal, and separate -------------------------------------
    doc = Doc_New();
    if (!doc) FAIL("could not allocate a model");
    {
        DocPara* p = Doc_AddPara(doc);
        p->props.align = ALIGN_CENTER;
        Doc_AddRun(p, L"Cloned", -1, &bold);

        DocBlock* t = Doc_AddTable(doc);
        t->table.gridEdges[0] = 3000;
        t->table.gridCount = 1;
        DocRow* row = Doc_AddRow(t);
        Doc_AddRun(Doc_AddCellPara(Doc_AddCell(row)), L"Cell", -1, &plain);

        DocModel* copy = Doc_Clone(doc);
        if (!copy) FAIL("clone produced nothing");

        DocDiff diff = {0};
        Doc_Compare(doc, copy, &diff);
        if (diff.differences != 0) {
            Doc_Free(copy);
            FAIL("a clone did not compare equal to what it was cloned from");
        }
        if (Doc_CountCells(copy) != 1 || copy->section.pageWidth != doc->section.pageWidth) {
            Doc_Free(copy);
            FAIL("a clone lost the table or the page");
        }

        // Editing the copy must leave the original alone -- undo depends on it.
        DocPos at = { Doc_ParaAt(copy, 0), 0 };
        DocEdit_Insert(copy, &at, L"XYZ", -1);
        BOOL leaked = !ParaTextIs(Doc_ParaAt(doc, 0), L"Cloned");
        Doc_Free(copy);
        if (leaked) FAIL("editing a clone changed the original");
    }
    Doc_Free(doc); doc = NULL;

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}

// ---------------------------------------------------------------------------
// Self-check. Run with: OpenNote.exe --selftest
//
// The fidelity numbers the harness reports are only worth anything if this
// comparison actually notices a change. A Doc_Compare that always answered
// "nothing lost" would make every one of those numbers a lie, so the checks
// below deliberately break a model and require the difference to be found.
// ---------------------------------------------------------------------------

BOOL Doc_SelfTest(char* failure, size_t failureSize) {
#define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        Doc_Free(a); Doc_Free(b); \
        return FALSE; \
    } while (0)

    DocModel* a = NULL;
    DocModel* b = NULL;

    // Two identical models must compare equal.
    for (int pass = 0; pass < 2; pass++) {
        DocModel* m = Doc_New();
        if (!m) {
            strncpy_s(failure, failureSize, "could not allocate a model", _TRUNCATE);
            Doc_Free(a);
            return FALSE;
        }

        CharProps bold = {0};
        bold.bold = TRUE;
        bold.halfPoints = 24;
        wcscpy_s(bold.font, LF_FACESIZE, L"Calibri");

        CharProps plain = {0};

        DocPara* p1 = Doc_AddPara(m);
        p1->props.align = ALIGN_CENTER;
        p1->props.headingLevel = 2;
        Doc_AddRun(p1, L"Title", -1, &bold);

        DocPara* p2 = Doc_AddPara(m);
        p2->props.indentLeft = 720;
        Doc_AddRun(p2, L"Body ", -1, &plain);
        Doc_AddRun(p2, L"text", -1, &bold);

        DocBlock* t = Doc_AddTable(m);
        t->table.gridEdges[0] = 3000;
        t->table.gridEdges[1] = 6000;
        t->table.gridCount = 2;
        DocRow* row = Doc_AddRow(t);
        Doc_AddRun(Doc_AddCellPara(Doc_AddCell(row)), L"Left", -1, &plain);
        Doc_AddRun(Doc_AddCellPara(Doc_AddCell(row)), L"Right", -1, &plain);

        if (pass == 0) a = m; else b = m;
    }

    DocDiff diff;
    Doc_Compare(a, b, &diff);
    if (diff.differences != 0) FAIL("identical models compared as different");
    if (diff.compared < 10)    FAIL("comparison examined implausibly little");

    // Counting.
    if (Doc_CountParas(a) != 4)  FAIL("paragraph count wrong");   // 2 body + 2 cells
    if (Doc_CountTables(a) != 1) FAIL("table count wrong");
    if (Doc_CountCells(a) != 2)  FAIL("cell count wrong");

    // Every one of these must be noticed. A metric that cannot see a change is
    // worse than no metric, because it reports success.
    struct { const char* what; } unused;
    (void)unused;

    b->blocks->para->props.align = ALIGN_LEFT;
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("a changed alignment was not detected");
    b->blocks->para->props.align = ALIGN_CENTER;

    b->blocks->para->props.headingLevel = 0;
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("a lost heading level was not detected");
    b->blocks->para->props.headingLevel = 2;

    b->blocks->para->runs->props.bold = FALSE;
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("a lost bold was not detected");
    b->blocks->para->runs->props.bold = TRUE;

    b->blocks->para->runs->props.halfPoints = 48;
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("a changed font size was not detected");
    b->blocks->para->runs->props.halfPoints = 24;

    b->blocks->para->runs->text[0] = L'X';
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("changed text was not detected");
    b->blocks->para->runs->text[0] = L'T';

    // The table grid, which is the loss the harness currently reports for the
    // editor path -- if this stopped being noticed, that number would quietly
    // improve for the wrong reason.
    DocBlock* tb = b->blocks;
    while (tb && tb->kind != BLOCK_TABLE) tb = tb->next;
    if (!tb) FAIL("the table went missing from the model");

    tb->table.gridCount = 0;
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("a lost table grid was not detected");
    tb->table.gridCount = 2;

    tb->table.gridEdges[1] = 9999;
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("a changed column width was not detected");
    tb->table.gridEdges[1] = 6000;

    // Back to equal, or one of the restores above is wrong.
    Doc_Compare(a, b, &diff);
    if (diff.differences != 0) FAIL("models did not compare equal again after restore");

    // An unspecified property in the source is a default the reader may
    // resolve, not a loss. This is the distinction that makes the number
    // mean something, so it is checked rather than assumed.
    b->blocks->para->runs->props.italic = FALSE;
    a->blocks->para->runs->props.italic = FALSE;
    DocPara* pa = a->blocks->next->para;
    DocPara* pb = b->blocks->next->para;
    pa->runs->props.halfPoints = 0;        // source says nothing
    pb->runs->props.halfPoints = 24;       // reader resolved a default
    Doc_Compare(a, b, &diff);
    if (diff.differences != 0) {
        FAIL("a resolved default was counted as a loss");
    }

    // ...but a size the source *did* state must still be required.
    pa->runs->props.halfPoints = 20;
    Doc_Compare(a, b, &diff);
    if (diff.differences == 0) FAIL("a stated size that changed was not detected");

    // RTF emission must produce something the view could accept.
    char* rtf = DocRtf_Emit(a);
    if (!rtf) FAIL("DocRtf_Emit produced nothing");
    if (strncmp(rtf, "{\\rtf", 5) != 0) {
        free(rtf);
        FAIL("DocRtf_Emit did not produce RTF");
    }
    if (!strstr(rtf, "Title") || !strstr(rtf, "Left")) {
        free(rtf);
        FAIL("DocRtf_Emit dropped document text");
    }
    if (!strstr(rtf, "\\cellx3000")) {
        free(rtf);
        FAIL("DocRtf_Emit did not carry the table grid");
    }
    free(rtf);

    Doc_Free(a);
    Doc_Free(b);

    failure[0] = '\0';
    return TRUE;

#undef FAIL
}
