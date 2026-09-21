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

typedef struct {
    DocBlock*    blocks;
    SectionProps section;
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

// Counts, for the round-trip report.
int Doc_CountParas(const DocModel* doc);
int Doc_CountRuns(const DocModel* doc);
int Doc_CountTables(const DocModel* doc);
int Doc_CountCells(const DocModel* doc);

// Concatenated text of the whole document, paragraphs separated by \n.
// Caller frees.
WCHAR* Doc_GetText(const DocModel* doc);

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
