// DocModel -> RTF.
//
// The rich text view speaks RTF, so this is how a model reaches the screen.
// It is the only place RTF is generated; the .docx reader builds a model and
// stops, rather than emitting markup of its own.

#include "supernote.h"
#include "core/doctree.h"
#include "core/doctree_rtf.h"
#include "core/strbuf.h"
#include "core/imagedib.h"

#define MAX_FONTS  64
#define MAX_COLORS 64

typedef struct {
    WCHAR    fonts[MAX_FONTS][LF_FACESIZE];
    int      fontCount;
    COLORREF colors[MAX_COLORS];
    int      colorCount;
} Tables;

static int FontIndex(Tables* t, const WCHAR* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < t->fontCount; i++) {
        if (_wcsicmp(t->fonts[i], name) == 0) return i;
    }
    if (t->fontCount >= MAX_FONTS) return -1;
    wcsncpy_s(t->fonts[t->fontCount], LF_FACESIZE, name, _TRUNCATE);
    return t->fontCount++;
}

// Colour 0 in an RTF colour table means "default", so real colours start at 1.
static int ColorIndex(Tables* t, COLORREF c) {
    for (int i = 0; i < t->colorCount; i++) {
        if (t->colors[i] == c) return i + 1;
    }
    if (t->colorCount >= MAX_COLORS) return 0;
    t->colors[t->colorCount] = c;
    return ++t->colorCount;
}

// Walk the model once up front so the font and colour tables are complete
// before the header that declares them is written.
static void CollectPara(const DocPara* para, Tables* t) {
    for (const DocRun* r = para->runs; r; r = r->next) {
        if (r->props.font[0]) FontIndex(t, r->props.font);
        if (r->props.hasColor) ColorIndex(t, r->props.color);
    }
}

static void CollectTables(const DocModel* doc, Tables* t) {
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) CollectPara(p, t);
        } else {
            for (const DocRow* row = b->table.rows; row; row = row->next) {
                for (const DocCell* c = row->cells; c; c = c->next) {
                    for (const DocPara* p = c->paras; p; p = p->next) CollectPara(p, t);
                }
            }
        }
    }
}

// A picture, as RTF.
//
// Decoded and then wrapped in a metafile, whatever it arrived as. RichEdit's
// RTF reader takes \wmetafile and nothing else: given \pngblip or \dibitmap it
// parses the group, discards it, and reports nothing, so every picture in a
// document was disappearing on its way to the view. The model still carries
// the original bytes, and those are what a file gets written back.
static void EmitPicture(StrBuf* sb, const DocImage* image) {
    BITMAPINFOHEADER header;
    size_t pixelsLen = 0;
    BYTE* pixels = ImageDib_Decode(image->bytes, image->len, &header, &pixelsLen);
    if (!pixels) return;

    size_t metaLen = 0;
    BYTE* meta = ImageDib_ToMetafile(pixels, &header, &metaLen);
    free(pixels);
    if (!meta) return;

    // Twips: 914400 EMU to the inch, 1440 twips to the inch.
    int widthTwips = (int)((double)image->widthEmu / 914400.0 * 1440.0);
    int heightTwips = (int)((double)image->heightEmu / 914400.0 * 1440.0);
    if (widthTwips <= 0) widthTwips = (int)header.biWidth * 15;
    if (heightTwips <= 0) heightTwips = (int)header.biHeight * 15;

    // \picw and \pich are in hundredths of a millimetre for a metafile.
    int widthMm = (int)((double)widthTwips / 1440.0 * 2540.0);
    int heightMm = (int)((double)heightTwips / 1440.0 * 2540.0);

    SB_AddF(sb, "{\\pict\\wmetafile8\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d ",
            widthMm, heightMm, widthTwips, heightTwips);

    static const char hexDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < metaLen; i++) {
        char pair[3] = { hexDigits[meta[i] >> 4], hexDigits[meta[i] & 0x0F], 0 };
        SB_Add(sb, pair);
    }

    SB_Add(sb, "}");
    free(meta);
}

static void EmitRun(StrBuf* sb, const DocRun* run, Tables* t) {
    SB_Add(sb, "\\plain");

    const CharProps* p = &run->props;
    if (p->bold)        SB_Add(sb, "\\b");
    if (p->italic)      SB_Add(sb, "\\i");
    if (p->underline)   SB_Add(sb, "\\ul");
    if (p->strike)      SB_Add(sb, "\\strike");
    if (p->superscript) SB_Add(sb, "\\super");
    if (p->subscript)   SB_Add(sb, "\\sub");

    if (p->font[0]) {
        int fi = FontIndex(t, p->font);
        if (fi >= 0) SB_AddF(sb, "\\f%d", fi);
    }
    if (p->halfPoints) SB_AddF(sb, "\\fs%d", p->halfPoints);
    if (p->hasColor)   SB_AddF(sb, "\\cf%d", ColorIndex(t, p->color));

    SB_Add(sb, " ");

    if (run->image)          EmitPicture(sb, run->image);
    else if (run->pageBreak) SB_Add(sb, "\\page ");
    else if (run->tab)       SB_Add(sb, "\\tab ");
    else if (run->lineBreak) SB_Add(sb, "\\line ");
    else                     SB_AddRtfText(sb, run->text, -1);
}

static void EmitParaProps(StrBuf* sb, const ParaProps* p, BOOL inTable) {
    SB_Add(sb, "\\pard");
    if (inTable) SB_Add(sb, "\\intbl");
    if (p->pageBreakBefore) SB_Add(sb, "\\pagebb");

    switch (p->align) {
        case ALIGN_CENTER:  SB_Add(sb, "\\qc"); break;
        case ALIGN_RIGHT:   SB_Add(sb, "\\qr"); break;
        case ALIGN_JUSTIFY: SB_Add(sb, "\\qj"); break;
        default:            SB_Add(sb, "\\ql"); break;
    }

    if (p->list != LIST_NONE) {
        // The order here is the control's own: it writes the numbering group
        // before the indents, and reads back what it writes. Emitting the same
        // RTF the other way round is how a numbered list arrived in the view
        // as a bulleted one.
        int li = p->indentLeft > 0 ? p->indentLeft : 720 + p->listLevel * 360;

        if (p->list == LIST_BULLET) {
            SB_Add(sb, "{\\pntext\\f0 \\'B7\\tab}"
                       "{\\*\\pn\\pnlvlblt\\pnf0\\pnindent360{\\pntxtb\\'B7}}");
        } else {
            // ponytail: every numbered list goes to the view as decimal.
            // RichEdit's RTF reader accepts \pndec and \pnlvlblt and nothing
            // else -- given \pnlcltr it ignores the numbering entirely and
            // leaves the marker behind as literal text, which is worse than
            // counting in the wrong alphabet. The model keeps the real format
            // and the page view renders it; this lifts when editing moves
            // there for good.
            //
            // What follows the number does survive: "%1." ends in a stop,
            // "%1)" in a bracket.
            const char* after = ".";
            size_t textLen = wcslen(p->listText);
            if (textLen && p->listText[textLen - 1] == L')') after = ")";

            SB_AddF(sb, "{\\pntext\\f0 1%s\\tab}"
                        "{\\*\\pn\\pnlvlbody\\pnf0\\pnindent360\\pnstart1\\pndec{\\pntxta%s}}",
                    after, after);
        }

        SB_AddF(sb, "\\fi-360\\li%d", li);
    } else {
        if (p->indentLeft)  SB_AddF(sb, "\\li%d", p->indentLeft);
        if (p->indentFirst) SB_AddF(sb, "\\fi%d", p->indentFirst);
    }

    if (p->spaceBefore) SB_AddF(sb, "\\sb%d", p->spaceBefore);
    if (p->spaceAfter)  SB_AddF(sb, "\\sa%d", p->spaceAfter);
    if (p->lineSpacing) SB_AddF(sb, "\\sl%d\\slmult1", p->lineSpacing);
}

static void EmitPara(StrBuf* sb, const DocPara* para, Tables* t, BOOL inTable) {
    EmitParaProps(sb, &para->props, inTable);
    for (const DocRun* r = para->runs; r; r = r->next) EmitRun(sb, r, t);
}

static void EmitTable(StrBuf* sb, const DocBlock* block, Tables* t) {
    for (const DocRow* row = block->table.rows; row; row = row->next) {
        int cellCount = 0;
        for (const DocCell* c = row->cells; c; c = c->next) cellCount++;
        if (!cellCount) continue;

        // The row definition has to precede the row's content.
        SB_Add(sb, "\\trowd\\trgaph108");
        if (block->table.gridCount > 0) {
            for (int i = 0; i < block->table.gridCount; i++) {
                SB_AddF(sb, "\\cellx%d", block->table.gridEdges[i]);
            }
        } else {
            for (int i = 1; i <= cellCount; i++) {
                SB_AddF(sb, "\\cellx%d", (9000 * i) / cellCount);
            }
        }
        SB_Add(sb, "\n");

        for (const DocCell* c = row->cells; c; c = c->next) {
            // A cell's paragraphs are separated by \par; the last one is ended
            // by \cell, not by a paragraph mark, or every cell gains a blank
            // line.
            for (const DocPara* p = c->paras; p; p = p->next) {
                EmitPara(sb, p, t, TRUE);
                if (p->next) SB_Add(sb, "\\par ");
            }
            if (!c->paras) SB_Add(sb, "\\pard\\intbl\\ql");
            SB_Add(sb, "\\cell ");
        }

        SB_Add(sb, "\\row\n");
    }
    SB_Add(sb, "\\pard\n");
}

char* DocRtf_Emit(const DocModel* doc) {
    if (!doc) return NULL;

    Tables t = {0};
    // Font 0 is the document default and must exist even if nothing names it.
    FontIndex(&t, L"Calibri");
    CollectTables(doc, &t);

    StrBuf body = {0};
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) {
                EmitPara(&body, p, &t, FALSE);
                SB_Add(&body, "\\par\n");
            }
        } else {
            EmitTable(&body, b, &t);
        }
    }

    if (body.failed) {
        SB_Free(&body);
        return NULL;
    }

    StrBuf out = {0};
    SB_Add(&out, "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl");
    for (int i = 0; i < t.fontCount; i++) {
        SB_AddF(&out, "{\\f%d\\fnil ", i);
        char name[LF_FACESIZE * 3];
        WideCharToMultiByte(CP_UTF8, 0, t.fonts[i], -1, name, sizeof(name), NULL, NULL);
        SB_Add(&out, name);
        SB_Add(&out, ";}");
    }
    SB_Add(&out, "}\n{\\colortbl;");
    for (int i = 0; i < t.colorCount; i++) {
        SB_AddF(&out, "\\red%d\\green%d\\blue%d;",
                GetRValue(t.colors[i]), GetGValue(t.colors[i]), GetBValue(t.colors[i]));
    }
    SB_Add(&out, "}\n\\viewkind4\\uc1\n");

    if (body.buf) SB_Add(&out, body.buf);
    SB_Add(&out, "}\n");

    SB_Free(&body);

    if (out.failed) {
        SB_Free(&out);
        return NULL;
    }
    return out.buf;
}
