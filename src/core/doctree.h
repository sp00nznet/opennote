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
    BOOL        pageBreakBefore;  // this paragraph starts a page

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

// A picture, carried as the bytes the file had. Nothing in the model decodes
// it: the layout engine hands the bytes to WIC, and a document written back
// out hands them back unchanged, which is the only way a picture survives a
// round trip without being re-encoded.
typedef struct {
    BYTE*  bytes;                 // owned
    size_t len;
    WCHAR  contentType[64];       // "image/png", "image/jpeg", ...
    int    widthEmu, heightEmu;   // how big it is drawn; 914400 EMU to the inch
} DocImage;

// Tracked changes, out of `w:ins` and `w:del`.
//
// A deletion is kept rather than dropped: it is content the document still
// carries, and throwing it away on read means a save cannot put it back. It is
// invisible everywhere text is measured, drawn or edited -- RunLength answers
// zero for it -- so the document reads as it would once the changes were
// accepted, which is what a reader wants to see, while the file keeps its
// history.
//
// ponytail: one display mode, the "final" one. Showing markup -- insertions
// underlined, deletions struck through -- is a layout feature, and it belongs
// with the review pane rather than in the model.
typedef enum { REV_NONE, REV_INSERTED, REV_DELETED } DocRevision;

typedef struct {
    DocRevision kind;
    WCHAR       author[64];
    WCHAR       date[32];        // ISO 8601, as the file states it
} RevisionMark;

// A field: an instruction and the result it last worked out.
//
// `{ PAGE }` in a footer, `{ DATE \\@ "d MMMM yyyy" }` on a letter, `{ REF
// name }` pointing at a bookmark. The instruction is what the document says;
// the run's text is the answer, which is cached in the file exactly as it is
// here -- a reader with no idea how to work one out still shows something
// sensible, and that is why Word stores it too.
typedef enum {
    FIELD_NONE,
    FIELD_OTHER,        // a field this does not evaluate; its result is kept
    FIELD_PAGE,
    FIELD_NUMPAGES,
    FIELD_DATE,
    FIELD_TIME,
    FIELD_REF,          // the text of a bookmark
    FIELD_PAGEREF,      // the page a bookmark is on
    FIELD_TOC
} DocFieldKind;

// A comment's markers. A comment is not in the text: it is a range of text it
// is about, a little reference where the bubble hangs, and the comment itself
// in a part of its own.
typedef enum {
    COMMENT_MARK_NONE,
    COMMENT_MARK_START,
    COMMENT_MARK_END,
    COMMENT_MARK_REF
} DocCommentMark;

typedef struct DocRun {
    struct DocRun* next;
    CharProps      props;
    WCHAR*         text;          // owned; never NULL, may be empty
    BOOL           lineBreak;     // a soft break rather than text
    BOOL           tab;
    BOOL           pageBreak;     // `w:br w:type="page"`
    DocImage*      image;         // owned; a run is a picture or it is text

    // A footnote or endnote reference. The run's text is the mark itself --
    // the number the reader worked out -- so it measures, wraps and draws like
    // any other text; this says which note it points at.
    int            noteId;        // 0 = not a reference
    BOOL           noteIsEnd;

    // Whether this run was inserted or deleted with track changes on.
    RevisionMark   rev;

    // A field instruction, when this run is a field's result. Owned.
    WCHAR*         field;

    // A bookmark's start or end. Owned, and worth no characters: a bookmark
    // is a place in the text rather than anything in it.
    WCHAR*         bookmark;
    BOOL           bookmarkEnd;

    // A comment's marker, and which comment it belongs to. Worth no
    // characters either, for the same reason.
    DocCommentMark commentMark;
    int            commentId;
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

    // Columns, from `w:cols`. One column is a page like any other; the space
    // between them is the gutter, in twips.
    int columns;
    int columnSpace;
} SectionProps;

// A footnote or an endnote: paragraphs, and the id the references use.
typedef struct DocNote {
    struct DocNote* next;
    int             id;
    BOOL            endnote;
    DocPara*        paras;
} DocNote;

// A comment: who wrote it, when, and what it says. The text it is about is
// marked in the document by a pair of markers carrying this id.
typedef struct DocComment {
    struct DocComment* next;
    int       id;
    WCHAR     author[64];
    WCHAR     initials[16];
    WCHAR     date[32];        // ISO 8601
    DocPara*  paras;
} DocComment;

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

typedef struct DocModel {
    DocBlock*    blocks;
    SectionProps section;

    // styles.xml: the table, and what `w:docDefaults` states for anything that
    // names no style at all.
    DocStyle*    styles;
    ParaProps    defaultPara;
    CharProps    defaultRun;

    // What goes in the margins, on every page. Paragraphs like any others,
    // laid out into the space above the top margin and below the bottom one.
    DocPara*     header;
    DocPara*     footer;
    int          headerFromTop;     // twips from the paper edge
    int          footerFromBottom;

    // Comments, by id, each marked in the text by a pair of markers.
    DocComment*  comments;

    // Footnotes and endnotes, by id. A footnote is laid out at the bottom of
    // whichever page its reference landed on; an endnote at the end.
    DocNote*     notes;
} DocModel;

// ---------------------------------------------------------------------------
// Building and freeing
// ---------------------------------------------------------------------------

DocModel* Doc_New(void);

// A model from plain text: one paragraph per line, one run each, with `props`
// on every run. This is how a text file reaches the layout engine -- the
// engine has no idea it came from anywhere else, which is the point.
DocModel* Doc_FromText(const WCHAR* text, const CharProps* props);
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

// A picture, which counts as one character in the paragraph's text -- the
// object replacement character, the same one the editor control uses -- so a
// caret can sit either side of it and an offset means the same thing
// everywhere.
DocRun*   Doc_AddImageRun(DocPara* para, const BYTE* bytes, size_t len,
                          const WCHAR* contentType, int widthEmu, int heightEmu);

#define DOC_IMAGE_CHAR 0xFFFC

// styles.xml, when the document had one.
DocNote*  Doc_AddNote(DocModel* doc, int id, BOOL endnote);
DocNote*  Doc_FindNote(const DocModel* doc, int id, BOOL endnote);

DocStyle* Doc_AddStyle(DocModel* doc, const WCHAR* id);
DocStyle* Doc_FindStyle(const DocModel* doc, const WCHAR* id);

// Resolve a style into properties, following `basedOn` to the root and
// applying each style over the one it is based on. Starts from the document
// defaults, so a paragraph that states nothing still comes out right.
void Doc_ResolveStyle(const DocModel* doc, const WCHAR* id,
                      ParaProps* paraOut, CharProps* runOut);

// ---------------------------------------------------------------------------
// Fields and bookmarks
// ---------------------------------------------------------------------------

// A field whose result is `text`. The instruction is kept as the document
// states it, including its switches.
DocRun* Doc_AddFieldRun(DocPara* para, const WCHAR* instr, const WCHAR* text,
                        const CharProps* props);

// A bookmark's start or end: a marker in the text that takes up no room.
DocRun* Doc_AddBookmark(DocPara* para, const WCHAR* name, BOOL isEnd);

DocFieldKind Doc_FieldKind(const WCHAR* instr);

// The first argument after the keyword -- a bookmark name, usually. Empty
// when there is none.
void Doc_FieldArgument(const WCHAR* instr, WCHAR* out, size_t outChars);

// The `\@ "..."` picture, which says how a date is to be written. Empty when
// the field does not state one.
void Doc_FieldPicture(const WCHAR* instr, WCHAR* out, size_t outChars);

// The text a field resolves to when the answer does not depend on where it
// lands: the date, the time, the text of a bookmark. Returns how many results
// changed, so a caller knows whether anything has to be laid out again.
int Doc_UpdateFields(DocModel* doc);

// Does this document hold a field whose answer depends on the page it is on?
// Those are the ones that need the document laid out before they can be told
// what they say.
BOOL Doc_HasPageFields(const DocModel* doc);

// The text of a bookmark: everything between its start and its end marker.
BOOL Doc_BookmarkText(const DocModel* doc, const WCHAR* name,
                      WCHAR* out, size_t outChars);

// A footer of "Page N of M", centred, replacing whatever footer was there.
// The numbers are fields, so they are answered when the document is laid out
// rather than written in and left to go stale.
void Doc_InsertPageNumbers(DocModel* doc);

// A table of contents at the top of the document, built from its headings.
// Each entry points at a bookmark on its heading with a PAGEREF field, so the
// page numbers follow the document instead of describing where it used to be.
// Returns how many entries there were; zero means the document has no
// headings to build one from, and nothing was inserted.
int Doc_InsertTableOfContents(DocModel* doc);

// ---------------------------------------------------------------------------
// Comments
// ---------------------------------------------------------------------------

// A comment with `text` as its body. The id is the next one free, and it is
// the caller's job to mark the text the comment is about.
DocComment* Doc_AddComment(DocModel* doc, const WCHAR* author, const WCHAR* initials,
                           const WCHAR* text);

DocComment* Doc_FindComment(const DocModel* doc, int id);
int         Doc_CountComments(const DocModel* doc);

// Mark a paragraph as the text a comment is about: a start marker at the
// front, an end and a reference at the back.
void Doc_MarkComment(DocPara* para, int id);

// Remove a comment and every marker pointing at it.
void Doc_DeleteComment(DocModel* doc, int id);

// The comment's text, paragraphs joined by spaces, for showing in a list.
WCHAR* Doc_CommentText(const DocComment* comment);   // caller frees

// ---------------------------------------------------------------------------
// Tracked changes
// ---------------------------------------------------------------------------

int  Doc_CountRevisions(const DocModel* doc);

// Accept: the deletions go and the insertions become ordinary text -- which
// changes nothing on screen, because that is already what is shown.
// Reject: the insertions go and the deletions come back, which does.
void Doc_AcceptRevisions(DocModel* doc);
void Doc_RejectRevisions(DocModel* doc);

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

// What one run is worth in the text: the characters it contributes, and
// whether it contributes nothing because the document carries it without
// showing it (a tracked deletion). Everything that walks runs by offset --
// editing, the layout engine -- asks these rather than working it out again,
// because two answers to "how long is this run" is how a caret ends up in the
// wrong place.
BOOL     Doc_RunIsHidden(const DocRun* run);
unsigned Doc_RunLength(const DocRun* run);

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

// A new, empty paragraph in front of `before`, or at the end of the document
// when `before` is NULL.
DocPara* Doc_InsertParaBefore(DocModel* doc, DocPara* before);

// Delete [a, b). Fails rather than half-doing it when the range runs into or
// out of a table, which is not a splice the model can make.
BOOL DocEdit_DeleteRange(DocModel* doc, DocPos a, DocPos b, DocPos* out);

// The text of [a, b), one paragraph per line. Caller frees.
WCHAR* DocEdit_RangeText(const DocModel* doc, DocPos a, DocPos b);

// A deep copy. Undo is a stack of these.
DocModel* Doc_Clone(const DocModel* src);

// A deep copy of a chain of paragraphs, for the parts of a document that are
// not blocks: a header, a footer.
DocPara* Doc_CloneParas(const DocPara* src);

// Replace a paragraph's runs, freeing the ones that were there, and free a
// chain of paragraphs. Both are for code that builds runs elsewhere and hands
// them over -- putting back what a view could not hold.
void Doc_SetRuns(DocPara* para, DocRun* runs);
void Doc_FreeParas(DocPara* paras);

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
