// The rich text view -> DocModel.
//
// This is the direction that used to lose tables. Saving walked the control's
// text and emitted paragraphs, so a table that had been read in came back out
// as a run of tab-separated lines. The control does know it holds a table --
// paragraphs inside one carry PFE_TABLE, rows are delimited by U+FFF9 and
// U+FFFB, and cells within a row are separated by BEL -- so the structure is
// recoverable, and this recovers it.

#include "supernote.h"
#include "core/doctree.h"
#include "core/doctree_view.h"

#ifndef PFE_TABLE
#define PFE_TABLE 0x4000
#endif

// RichEdit's structural characters. They mark table shape and are never
// document text; writing them into a file puts unreadable characters in it.
#define CH_CELL_BREAK 0x0007   // BEL, between cells of a row
#define CH_ROW_START  0xFFF9
#define CH_ROW_END    0xFFFB
#define CH_OBJECT     0xFFFC

// The pictures a captured model can re-use, in the order they appear.
typedef struct {
    const DocImage* images[256];
    int             count;
    int             next;
} ImageSource;

static void CollectParaImages(const DocPara* para, ImageSource* out) {
    for (const DocRun* r = para->runs; r; r = r->next) {
        if (!r->image || out->count >= 256) continue;
        out->images[out->count++] = r->image;
    }
}

static void CollectImages(const DocModel* source, ImageSource* out) {
    memset(out, 0, sizeof(*out));
    if (!source) return;

    for (const DocBlock* b = source->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) CollectParaImages(p, out);
        } else {
            for (const DocRow* r = b->table.rows; r; r = r->next) {
                for (const DocCell* c = r->cells; c; c = c->next) {
                    for (const DocPara* p = c->paras; p; p = p->next) {
                        CollectParaImages(p, out);
                    }
                }
            }
        }
    }
}

static BOOL IsStructural(WCHAR c) {
    return c == CH_CELL_BREAK || c == CH_ROW_START ||
           c == CH_ROW_END || c == CH_OBJECT || c == 0xFFFA;
}

// Raw text, meaning paragraph marks stay as a single \r so offsets match the
// ones the control itself uses. GT_DEFAULT expands them to \r\n and every
// offset after the first paragraph would be wrong.
static WCHAR* GetRawText(HWND h, int* lenOut) {
    GETTEXTLENGTHEX gtl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    int len = (int)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len < 0) len = 0;

    WCHAR* buf = (WCHAR*)malloc(((size_t)len + 2) * sizeof(WCHAR));
    if (!buf) return NULL;

    GETTEXTEX gt = {
        .cb = (DWORD)(((size_t)len + 1) * sizeof(WCHAR)),
        .flags = GT_RAWTEXT,
        .codepage = 1200,
        .lpDefaultChar = NULL,
        .lpUsedDefChar = NULL
    };
    int got = (int)SendMessageW(h, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)buf);
    if (got < 0) got = 0;
    buf[got] = L'\0';

    if (lenOut) *lenOut = got;
    return buf;
}

static void GetCharFormatAt(HWND h, int pos, CHARFORMAT2W* cf) {
    CHARRANGE cr = { pos, pos + 1 };
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);

    memset(cf, 0, sizeof(*cf));
    cf->cbSize = sizeof(*cf);
    SendMessageW(h, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)cf);
}

static void GetParaFormatAt(HWND h, int pos, PARAFORMAT2* pf) {
    CHARRANGE cr = { pos, pos };
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);

    memset(pf, 0, sizeof(*pf));
    pf->cbSize = sizeof(*pf);
    SendMessageW(h, EM_GETPARAFORMAT, 0, (LPARAM)pf);
}

static BOOL SameCharFormat(const CHARFORMAT2W* a, const CHARFORMAT2W* b) {
    DWORD mask = CFE_BOLD | CFE_ITALIC | CFE_UNDERLINE | CFE_STRIKEOUT |
                 CFE_SUBSCRIPT | CFE_SUPERSCRIPT | CFE_AUTOCOLOR;
    if ((a->dwEffects & mask) != (b->dwEffects & mask)) return FALSE;
    if (a->yHeight != b->yHeight) return FALSE;
    if (!(a->dwEffects & CFE_AUTOCOLOR) && a->crTextColor != b->crTextColor) return FALSE;
    return wcscmp(a->szFaceName, b->szFaceName) == 0;
}

static void ToCharProps(const CHARFORMAT2W* cf, CharProps* out) {
    memset(out, 0, sizeof(*out));
    out->bold        = (cf->dwEffects & CFE_BOLD) != 0;
    out->italic      = (cf->dwEffects & CFE_ITALIC) != 0;
    out->underline   = (cf->dwEffects & CFE_UNDERLINE) != 0;
    out->strike      = (cf->dwEffects & CFE_STRIKEOUT) != 0;
    out->superscript = (cf->dwEffects & CFE_SUPERSCRIPT) != 0;
    out->subscript   = (cf->dwEffects & CFE_SUBSCRIPT) != 0;
    // yHeight is twips, halfPoints is twentieths of a point: 10 twips each.
    out->halfPoints  = cf->yHeight > 0 ? cf->yHeight / 10 : 0;
    if (!(cf->dwEffects & CFE_AUTOCOLOR)) {
        out->hasColor = TRUE;
        out->color = cf->crTextColor;
    }
    if (cf->szFaceName[0]) wcsncpy_s(out->font, LF_FACESIZE, cf->szFaceName, _TRUNCATE);
}

// Which list a paragraph belongs to.
//
// The control has no notion of one list as against another: it knows a
// paragraph is numbered and how, and nothing else. A run of list paragraphs
// counting the same way is therefore taken to be one list, and anything that
// interrupts it -- ordinary text, or a switch from numbers to bullets --
// starts the next one. That is what numbering.xml needs, because a list is
// where counting restarts.
typedef struct {
    int          nextId;
    int          currentId;
    BOOL         inList;
    DocNumFormat format;
} ListRun;

static void AssignList(ListRun* run, ParaProps* props) {
    if (props->list == LIST_NONE) {
        run->inList = FALSE;
        return;
    }

    if (!run->inList || props->numFormat != run->format) {
        run->currentId = ++run->nextId;
        run->format = props->numFormat;
        run->inList = TRUE;
    }

    props->listId = run->currentId;
}

static void ToParaProps(const PARAFORMAT2* pf, ParaProps* out) {
    memset(out, 0, sizeof(*out));

    if (pf->dwMask & PFM_ALIGNMENT) {
        switch (pf->wAlignment) {
            case PFA_CENTER:  out->align = ALIGN_CENTER;  break;
            case PFA_RIGHT:   out->align = ALIGN_RIGHT;   break;
            case PFA_JUSTIFY: out->align = ALIGN_JUSTIFY; break;
            default:          out->align = ALIGN_LEFT;    break;
        }
    }
    if (pf->dwMask & PFM_STARTINDENT) out->indentLeft = pf->dxStartIndent;
    if (pf->dwMask & PFM_OFFSET)      out->indentFirst = -pf->dxOffset;

    // The control reports where the *first line* starts and how far the rest
    // hangs from it; the model measures the indent from the margin, which is
    // the other end of the same two numbers.
    if (out->indentFirst < 0) out->indentLeft += -out->indentFirst;
    if (pf->dwMask & PFM_SPACEBEFORE) out->spaceBefore = pf->dySpaceBefore;
    if (pf->dwMask & PFM_SPACEAFTER)  out->spaceAfter = pf->dySpaceAfter;

    if (pf->dwMask & PFM_NUMBERING) {
    }
    if ((pf->dwMask & PFM_NUMBERING) && pf->wNumbering) {
        // The control keeps which way a list counts, which is most of what
        // numbering.xml said: a lettered list comes back lettered.
        switch (pf->wNumbering) {
            case PFN_ARABIC:   out->numFormat = NUMFMT_DECIMAL;      break;
            case PFN_LCLETTER: out->numFormat = NUMFMT_LOWER_LETTER; break;
            case PFN_UCLETTER: out->numFormat = NUMFMT_UPPER_LETTER; break;
            case PFN_LCROMAN:  out->numFormat = NUMFMT_LOWER_ROMAN;  break;
            case PFN_UCROMAN:  out->numFormat = NUMFMT_UPPER_ROMAN;  break;
            default:           out->numFormat = NUMFMT_BULLET;       break;
        }
        out->list = (out->numFormat == NUMFMT_BULLET) ? LIST_BULLET : LIST_NUMBER;

        // A list carries its indent through the hanging indent above; the
        // model keeps the level so the marker can be rebuilt.
        out->listLevel = out->indentLeft > 720 ? (out->indentLeft - 720) / 360 : 0;

        if (out->list == LIST_NUMBER) {
            // The punctuation lives in the high nibble: PFNS_PERIOD follows the
            // number with a stop, PFNS_PAREN with a bracket.
            const WCHAR* after = ((pf->wNumberingStyle & 0x0F00) == PFNS_PAREN)
                               ? L")" : L".";
            swprintf_s(out->listText, 24, L"%%%d%s", out->listLevel + 1, after);
        }
    }
}

// Add the text between [from, to) to a paragraph, split into runs wherever the
// character formatting changes, with tabs and structural characters handled.
static void AddRuns(HWND h, DocPara* para, const WCHAR* text, int from, int to,
                    ImageSource* pictures) {
    if (!para || from >= to) return;

    int i = from;
    while (i < to) {
        if (text[i] == L'\t') {
            CHARFORMAT2W cf;
            GetCharFormatAt(h, i, &cf);
            CharProps props;
            ToCharProps(&cf, &props);
            DocRun* r = Doc_AddRun(para, L"", 0, &props);
            if (r) r->tab = TRUE;
            i++;
            continue;
        }
        if (text[i] == CH_OBJECT) {
            // An object in the text is a picture the control is holding. It
            // will not hand the bytes back, so they come from the model the
            // view was loaded from, matched in order. A picture the control
            // gained some other way -- pasted in, inserted from a file -- has
            // nothing to match and is lost here, which is the ceiling on
            // editing pictures in this view at all.
            if (pictures && pictures->next < pictures->count) {
                const DocImage* image = pictures->images[pictures->next++];
                Doc_AddImageRun(para, image->bytes, image->len, image->contentType,
                                image->widthEmu, image->heightEmu);
            }
            i++;
            continue;
        }
        if (IsStructural(text[i])) {
            i++;
            continue;
        }

        CHARFORMAT2W base;
        GetCharFormatAt(h, i, &base);

        int j = i + 1;
        while (j < to && text[j] != L'\t' && text[j] != CH_OBJECT &&
               !IsStructural(text[j])) {
            CHARFORMAT2W next;
            GetCharFormatAt(h, j, &next);
            if (!SameCharFormat(&base, &next)) break;
            j++;
        }

        CharProps props;
        ToCharProps(&base, &props);
        Doc_AddRun(para, text + i, j - i, &props);
        i = j;
    }
}

// Column widths, straight from the control. RichEdit 4.1 answers
// EM_GETTABLEPARMS for a selection sitting inside a table row, which is a far
// better source than guessing equal columns -- and without a grid Word
// auto-sizes the table, which silently changes a document's appearance.
static void CaptureGrid(HWND h, int pos, const PARAFORMAT2* pf, DocBlock* table) {
    if (!table || table->table.gridCount > 0) return;   // first row decides

    CHARRANGE cr = { pos, pos };
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);

    TABLEROWPARMS trp = {0};
    trp.cbRow = sizeof(TABLEROWPARMS);
    trp.cbCell = sizeof(TABLECELLPARMS);
    trp.cCell = 32;

    TABLECELLPARMS cells[32] = {0};

    if (SendMessageW(h, EM_GETTABLEPARMS, (WPARAM)&trp, (LPARAM)cells) == S_OK &&
        trp.cCell > 0 && trp.cCell <= 32) {
        int accum = 0;
        int i = 0;
        for (; i < trp.cCell; i++) {
            if (cells[i].dxWidth <= 0) break;
            accum += cells[i].dxWidth;
            table->table.gridEdges[i] = accum;
        }
        if (i == trp.cCell) {
            table->table.gridCount = i;
            return;
        }
    }

    // EM_GETTABLEPARMS does not answer for every table, notably ones that
    // arrived by streaming RTF in. A table paragraph keeps its cell edges in
    // the paragraph's tab stops, which is where RichEdit put the \cellx values,
    // so that is the second source rather than guessing equal columns.
    if (!(pf->dwMask & PFM_TABSTOPS)) return;

    int n = pf->cTabCount;
    if (n <= 0 || n > 32) return;

    for (int i = 0; i < n; i++) {
        // The high byte carries alignment and leader flags.
        int edge = pf->rgxTabs[i] & 0x00FFFFFF;
        if (edge <= 0) return;
        if (i > 0 && edge <= table->table.gridEdges[i - 1]) return;
        table->table.gridEdges[i] = edge;
    }
    table->table.gridCount = n;
}

// Put the pictures back.
//
// The control displays a picture perfectly well and then refuses to say where
// it is: the text it hands back has no object character to match, whichever
// way it is asked. So each picture goes back on the paragraph it came from,
// and only when that paragraph still reads exactly as it did -- a paragraph
// whose text was edited has no way to say where within it the picture was.
//
// ponytail: a picture in an edited paragraph is lost on the way back out.
// The page view holds the model itself and has no such problem; this exists
// because the RichEdit view is still where typing happens.
static BOOL SameTextIgnoringPictures(const DocPara* a, const DocPara* b) {
    unsigned alen = 0, blen = 0;
    WCHAR* at = Doc_ParaText(a, &alen);
    WCHAR* bt = Doc_ParaText(b, &blen);
    if (!at || !bt) {
        free(at);
        free(bt);
        return FALSE;
    }

    unsigned i = 0, j = 0;
    BOOL same = TRUE;
    for (;;) {
        while (i < alen && at[i] == (WCHAR)DOC_IMAGE_CHAR) i++;
        while (j < blen && bt[j] == (WCHAR)DOC_IMAGE_CHAR) j++;
        if (i >= alen || j >= blen) break;
        if (at[i] != bt[j]) { same = FALSE; break; }
        i++;
        j++;
    }

    if (same) {
        while (i < alen && at[i] == (WCHAR)DOC_IMAGE_CHAR) i++;
        while (j < blen && bt[j] == (WCHAR)DOC_IMAGE_CHAR) j++;
        same = (i >= alen && j >= blen);
    }

    free(at);
    free(bt);
    return same;
}

static BOOL HasImages(const DocPara* para) {
    for (const DocRun* r = para->runs; r; r = r->next) {
        if (r->image) return TRUE;
    }
    return FALSE;
}

static BOOL TextIsOnlyPictures(const DocPara* para) {
    for (const DocRun* r = para->runs; r; r = r->next) {
        if (r->image) continue;
        if (r->tab || r->lineBreak) return FALSE;
        if (r->text && r->text[0]) return FALSE;
    }
    return TRUE;
}

static void CopyImages(const DocPara* from, DocPara* into) {
    for (const DocRun* r = from->runs; r; r = r->next) {
        if (!r->image) continue;
        Doc_AddImageRun(into, r->image->bytes, r->image->len, r->image->contentType,
                        r->image->widthEmu, r->image->heightEmu);
    }
}

// Walk the two models together, putting each picture back on the paragraph it
// came from. A paragraph that held nothing but a picture is not in the
// captured model at all -- the control's text has nothing where a picture is,
// not even a space -- so it is put back as well.
static void ReattachImages(DocModel* captured, const DocModel* source) {
    int sourceCount = Doc_CountParas(source);
    int si = 0, ci = 0;

    while (si < sourceCount) {
        DocPara* from = Doc_ParaAt((DocModel*)source, si);
        if (!from) break;

        if (!HasImages(from)) {
            si++;
            ci++;
            continue;
        }

        DocPara* into = Doc_ParaAt(captured, ci);

        if (TextIsOnlyPictures(from)) {
            // A picture on a line of its own: the paragraph itself has to come
            // back before the picture has anywhere to go.
            DocPara* fresh = Doc_InsertParaBefore(captured, into);
            if (fresh) {
                fresh->props = from->props;
                CopyImages(from, fresh);
            }
        } else if (into && SameTextIgnoringPictures(from, into)) {
            CopyImages(from, into);
        }
        // A paragraph whose text was edited keeps its words and loses its
        // picture: there is no way to say where in the new text it belonged.

        si++;
        ci++;
    }
}

DocModel* DocView_Capture(HWND h) {
    return DocView_CaptureWith(h, NULL);
}

DocModel* DocView_CaptureWith(HWND h, const DocModel* source) {
    if (!h) return NULL;

    int textLen = 0;
    WCHAR* text = GetRawText(h, &textLen);
    if (!text) return NULL;

    DocModel* doc = Doc_New();
    if (!doc) {
        free(text);
        return NULL;
    }

    // Walking moves the selection around and would repaint on every step.
    CHARRANGE saved = {0};
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&saved);
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);

    DocBlock* table = NULL;
    DocRow*   row = NULL;
    ListRun   lists = {0};

    ImageSource pictures;
    CollectImages(source, &pictures);

    int pos = 0;
    while (pos <= textLen) {
        int paraEnd = pos;
        while (paraEnd < textLen && text[paraEnd] != L'\r' && text[paraEnd] != L'\n') {
            paraEnd++;
        }

        PARAFORMAT2 pf;
        GetParaFormatAt(h, pos, &pf);
        BOOL inTable = (pf.dwMask & PFM_TABLE) && (pf.wEffects & PFE_TABLE);

        // A paragraph holding nothing but the row-start marker is structure.
        BOOL onlyMarkers = TRUE;
        for (int i = pos; i < paraEnd; i++) {
            if (!IsStructural(text[i])) { onlyMarkers = FALSE; break; }
        }

        if (inTable) {
            if (!table) {
                table = Doc_AddTable(doc);
                row = NULL;
            }
            if (!row) row = Doc_AddRow(table);

            CaptureGrid(h, pos, &pf, table);

            if (!(onlyMarkers && paraEnd > pos)) {
                // A row reads as cell<SEP>cell<SEP>cell<SEP> and may then carry
                // a row-end marker. Everything after the last separator is
                // structure, not a cell -- counting it gave every row one
                // phantom column.
                int contentEnd = paraEnd;
                while (contentEnd > pos &&
                       IsStructural(text[contentEnd - 1]) &&
                       text[contentEnd - 1] != CH_CELL_BREAK) {
                    contentEnd--;
                }

                int cellStart = pos;
                int emitted = 0;

                for (int i = pos; i < contentEnd; i++) {
                    if (text[i] != CH_CELL_BREAK) continue;

                    DocCell* cell = Doc_AddCell(row);
                    DocPara* cp = Doc_AddCellPara(cell);
                    if (cp) {
                        ToParaProps(&pf, &cp->props);
                        AssignList(&lists, &cp->props);
                        AddRuns(h, cp, text, cellStart, i, &pictures);
                    }
                    emitted++;
                    cellStart = i + 1;
                }

                // Trailing content with no separator after it is a final cell;
                // a row whose separators accounted for everything is complete.
                if (cellStart < contentEnd || emitted == 0) {
                    DocCell* cell = Doc_AddCell(row);
                    DocPara* cp = Doc_AddCellPara(cell);
                    if (cp) {
                        ToParaProps(&pf, &cp->props);
                        AssignList(&lists, &cp->props);
                        AddRuns(h, cp, text, cellStart, contentEnd, &pictures);
                    }
                }
            }

            // The row ends where the control says it does.
            for (int i = pos; i < paraEnd; i++) {
                if (text[i] == CH_ROW_END) { row = NULL; break; }
            }
        } else {
            table = NULL;
            row = NULL;

            if (!onlyMarkers || paraEnd == pos) {
                DocPara* para = Doc_AddPara(doc);
                if (para) {
                    ToParaProps(&pf, &para->props);
                    AssignList(&lists, &para->props);
                    AddRuns(h, para, text, pos, paraEnd, &pictures);
                }
            }
        }

        if (paraEnd >= textLen) break;
        pos = paraEnd + 1;
        if (pos < textLen && text[paraEnd] == L'\r' && text[pos] == L'\n') pos++;
    }

    if (pictures.count > 0 && pictures.next == 0) ReattachImages(doc, source);

    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&saved);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);

    free(text);
    return doc;
}
