#ifndef DOCTREE_H
#define DOCTREE_H

// The document model.
//
// Until v0.7 a document *was* whatever the RichEdit control happened to be
// holding, and .docx round-tripped through RTF. That is why a table survived
// being read and flattened when it was written: there was nowhere to keep a
// table in between.
//
// This is that somewhere. Every file format becomes a serializer over one
// structure:
//
//     .docx  --Docx_Read-->  DocModel  --Docx_Write-->  .docx
//                               |  ^
//                    DocRtf_Emit|  |DocView_Capture
//                               v  |
//                        the RichEdit view
//
// It is also what the layout engine in v0.8 lays out, so it exists before the
// engine rather than arriving with it.

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

typedef struct {
    BOOL     bold;
    BOOL     italic;
    BOOL     underline;
    BOOL     strike;
    BOOL     superscript;
    BOOL     subscript;
    int      halfPoints;          // 0 = unspecified; w:sz and RTF \fs agree on this
    BOOL     hasColor;
    COLORREF color;
    WCHAR    font[LF_FACESIZE];   // empty = unspecified
} CharProps;

typedef enum {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT,
    ALIGN_JUSTIFY
} DocAlign;

typedef enum {
    LIST_NONE,
    LIST_BULLET,
    LIST_NUMBER
} DocListKind;

// How a numbered list counts, straight out of `w:numFmt`. A bullet is a list
// that does not count at all.
typedef enum {
    NUMFMT_BULLET,
    NUMFMT_DECIMAL,
    NUMFMT_LOWER_LETTER,
    NUMFMT_UPPER_LETTER,
    NUMFMT_LOWER_ROMAN,
    NUMFMT_UPPER_ROMAN
} DocNumFormat;

typedef struct {
    DocAlign    align;
    int         indentLeft;       // twips
    int         indentFirst;      // twips, negative for a hanging indent
    int         spaceBefore;      // twips
    int         spaceAfter;       // twips
    int         lineSpacing;      // twentieths of a line; 0 = single
    DocListKind list;
    int         listLevel;
    int         headingLevel;     // 0 = body text, 1..6 = Heading1..6

    // Which list this paragraph belongs to (`w:numId`), so two lists in one
    // document count separately, and how that list counts.
    int          listId;
    DocNumFormat numFormat;
    WCHAR        listText[24];    // `w:lvlText`, e.g. "%1." or "%1.%2." -- empty for a bullet

    // The named style this paragraph came from, if it had one. The properties
    // above are already resolved from it; this is kept so the style survives
    // being written back out as a style rather than as direct formatting.
    WCHAR       style[64];
} ParaProps;

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

typedef struct DocRun {
    struct DocRun* next;
    CharProps      props;
    WCHAR*         text;          // owned; never NULL, may be empty
    BOOL           lineBreak;     // a soft break rather than text
    BOOL           tab;
} DocRun;

typedef struct DocPara {
    struct DocPara* next;
    ParaProps       props;
    DocRun*         runs;
} DocPara;

typedef struct DocCell {
    struct DocCell* next;
    DocPara*        paras;
} DocCell;

typedef struct DocRow {
    struct DocRow* next;
    DocCell*       cells;
} DocRow;

// A block is either a paragraph or a table, in document order.
typedef enum { BLOCK_PARA, BLOCK_TABLE } DocBlockKind;

typedef struct DocBlock {
    struct DocBlock* next;
    DocBlockKind     kind;
    DocPara*         para;                   // BLOCK_PARA
    struct {
        DocRow* rows;
        int     gridEdges[32];               // cumulative twips
        int     gridCount;
    } table;                                 // BLOCK_TABLE
} DocBlock;

typedef struct {
    int pageWidth, pageHeight;   // twips
    int marginTop, marginRight, marginBottom, marginLeft;
} SectionProps;

// A named style out of styles.xml. Paragraphs carry their resolved properties,
// so nothing here is needed to lay a document out -- it is kept so that a
// document written back out says "Heading 1" where it said "Heading 1", rather
// than turning every style into direct formatting.
typedef struct DocStyle {
    struct DocStyle* next;
    WCHAR     id[64];
    WCHAR     name[64];
    WCHAR     basedOn[64];
    ParaProps para;
    CharProps run;
} DocStyle;

typedef struct {
    DocBlock*    blocks;
    SectionProps section;

    // styles.xml: the table, and what `w:docDefaults` states for anything that
    // names no style at all.
    DocStyle*    styles;
    ParaProps    defaultPara;
    CharProps    defaultRun;
} DocModel;

// ---------------------------------------------------------------------------
// Building and freeing
// ---------------------------------------------------------------------------

DocModel* Doc_New(void);
void      Doc_Free(DocModel* doc);

// The page a new model starts on. A document read from a file states its own
// and overwrites this; a document captured from the editor states nothing,
// because the control has no notion of a page -- so this is how Page Setup
// reaches the layout engine, the printer and the .docx writer at once.
void Doc_SetPageDefaults(const SectionProps* page);
void Doc_GetPageDefaults(SectionProps* out);

DocPara*  Doc_AddPara(DocModel* doc);                 // appends a paragraph block
DocBlock* Doc_AddTable(DocModel* doc);                // appends an empty table
DocRow*   Doc_AddRow(DocBlock* table);
DocCell*  Doc_AddCell(DocRow* row);
DocPara*  Doc_AddCellPara(DocCell* cell);
DocRun*   Doc_AddRun(DocPara* para, const WCHAR* text, int len, const CharProps* props);

// styles.xml, when the document had one.
DocStyle* Doc_AddStyle(DocModel* doc, const WCHAR* id);
DocStyle* Doc_FindStyle(const DocModel* doc, const WCHAR* id);

// Resolve a style into properties, following `basedOn` to the root and
// applying each style over the one it is based on. Starts from the document
// defaults, so a paragraph that states nothing still comes out right.
void Doc_ResolveStyle(const DocModel* doc, const WCHAR* id,
                      ParaProps* paraOut, CharProps* runOut);

// Counts, for the round-trip report.
int Doc_CountParas(const DocModel* doc);
int Doc_CountRuns(const DocModel* doc);
int Doc_CountTables(const DocModel* doc);
int Doc_CountCells(const DocModel* doc);

// Concatenated text of the whole document, paragraphs separated by \n.
// Caller frees.
WCHAR* Doc_GetText(const DocModel* doc);

// ---------------------------------------------------------------------------
// Positions and editing
//
// A position is a paragraph and an offset into that paragraph's text, where
// the text is its runs concatenated with a tab or a line break counting one
// character each. That is the same string the layout engine flattens a
// paragraph to, so a click on a laid-out page names a place in the model and
// the two cannot disagree about where anything is.
// ---------------------------------------------------------------------------

typedef struct {
    DocPara* para;
    unsigned offset;
} DocPos;

unsigned Doc_ParaLength(const DocPara* para);
WCHAR*   Doc_ParaText(const DocPara* para, unsigned* lenOut);   // caller frees

// Paragraphs in document order, table cells included. The editor holds a
// caret as an index rather than a pointer, because undo replaces the model.
DocPara*        Doc_ParaAt(const DocModel* doc, int index);
int             Doc_ParaIndexOf(const DocModel* doc, const DocPara* para);
const DocCell*  Doc_ParaCell(const DocModel* doc, const DocPara* para);

// Insert text at `at`, which is advanced past it.
BOOL DocEdit_Insert(DocModel* doc, DocPos* at, const WCHAR* text, int len);

// Break a paragraph in two at `at`, which lands at the start of the new one.
BOOL DocEdit_SplitPara(DocModel* doc, DocPos* at);

// Delete [a, b). Fails rather than half-doing it when the range runs into or
// out of a table, which is not a splice the model can make.
BOOL DocEdit_DeleteRange(DocModel* doc, DocPos a, DocPos b, DocPos* out);

// The text of [a, b), one paragraph per line. Caller frees.
WCHAR* DocEdit_RangeText(const DocModel* doc, DocPos a, DocPos b);

// A deep copy. Undo is a stack of these.
DocModel* Doc_Clone(const DocModel* src);

// Self-check for the editing operations, run by `OpenNote.exe --selftest`.
BOOL DocEdit_SelfTest(char* failure, size_t failureSize);

// ---------------------------------------------------------------------------
// Comparison, for measuring what a round trip lost
// ---------------------------------------------------------------------------

#define DOC_DIFF_MAX 32

typedef struct {
    int   differences;
    int   compared;
    char  first[DOC_DIFF_MAX][160];   // the first few, described
    int   firstCount;
} DocDiff;

// Compare two models property by property. `diff` is filled with a count of
// everything that did not survive, and a description of the first few.
void Doc_Compare(const DocModel* a, const DocModel* b, DocDiff* diff);

// Self-check, run by `OpenNote.exe --selftest`. Verifies that the comparison
// above actually notices a change -- the fidelity numbers mean nothing if it
// does not.
BOOL Doc_SelfTest(char* failure, size_t failureSize);

#endif // DOCTREE_H
