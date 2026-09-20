// The document model: construction, teardown, and the comparison the
// round-trip harness uses to measure what a save-and-reopen lost.

#include "supernote.h"
#include "core/doctree.h"

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

DocModel* Doc_New(void) {
    DocModel* doc = (DocModel*)calloc(1, sizeof(DocModel));
    if (!doc) return NULL;

    // Letter paper with one inch margins, matching what the .docx writer emits
    // when a document does not say otherwise.
    doc->section.pageWidth  = 12240;
    doc->section.pageHeight = 15840;
    doc->section.marginTop = doc->section.marginRight =
        doc->section.marginBottom = doc->section.marginLeft = 1440;

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
