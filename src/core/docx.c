// .docx -- ECMA-376 WordprocessingML, read and written through Windows' own
// packaging and XML APIs.
//
// A .docx is an Open Packaging Conventions container. Windows ships an API for
// that exact shape (msopc.dll, IOpcFactory) and a pull XML reader (xmllite.dll,
// IXmlReader), both documented and both already on every machine this runs on.
// Neither the zip container nor the XML parser is code this project owns.
//
// Since v0.7 both directions go through the document model in doctree.h rather
// than through RTF. The reader builds a model; the writer serialises one. RTF
// is produced only when a model needs to reach the view, by doctree_rtf.c.

#define COBJMACROS

#include "supernote.h"
#include "core/docx.h"
#include "core/doctree.h"
#include "core/strbuf.h"
#include "ui/editor_rich.h"

#include <msopc.h>
#include <xmllite.h>
#include <objbase.h>

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#define REL_OFFICE_DOCUMENT \
    L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"

#define CT_MAIN_DOCUMENT \
    L"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"

#define WML_NS "http://schemas.openxmlformats.org/wordprocessingml/2006/main"

static WCHAR g_lastError[512] = {0};

static void SetError(const WCHAR* msg) {
    wcsncpy_s(g_lastError, 512, msg ? msg : L"", _TRUNCATE);
}

const WCHAR* Docx_GetLastError(void) {
    return g_lastError[0] ? g_lastError : L"No error.";
}

BOOL Docx_IsDocxPath(const WCHAR* path) {
    if (!path) return FALSE;
    const WCHAR* ext = wcsrchr(path, L'.');
    return ext && _wcsicmp(ext, L".docx") == 0;
}

// ---------------------------------------------------------------------------
// XmlLite helpers
// ---------------------------------------------------------------------------

static BOOL NameIs(const WCHAR* local, UINT len, const WCHAR* want) {
    size_t wl = wcslen(want);
    return len == wl && wcsncmp(local, want, wl) == 0;
}

static BOOL GetAttr(IXmlReader* r, const WCHAR* name, WCHAR* out, size_t outChars) {
    out[0] = L'\0';

    HRESULT hr = IXmlReader_MoveToFirstAttribute(r);
    while (hr == S_OK) {
        const WCHAR* local = NULL;
        UINT len = 0;
        if (SUCCEEDED(IXmlReader_GetLocalName(r, &local, &len)) &&
            NameIs(local, len, name)) {
            const WCHAR* val = NULL;
            UINT vlen = 0;
            if (SUCCEEDED(IXmlReader_GetValue(r, &val, &vlen))) {
                size_t copy = vlen < outChars - 1 ? vlen : outChars - 1;
                wcsncpy_s(out, outChars, val, copy);
                IXmlReader_MoveToElement(r);
                return TRUE;
            }
        }
        hr = IXmlReader_MoveToNextAttribute(r);
    }

    IXmlReader_MoveToElement(r);
    return FALSE;
}

// WordprocessingML writes booleans as w:val="0"/"false" to turn a property off;
// an absent w:val means on.
static BOOL AttrIsOn(IXmlReader* r) {
    WCHAR val[32];
    if (!GetAttr(r, L"val", val, 32)) return TRUE;
    return !(wcscmp(val, L"0") == 0 || _wcsicmp(val, L"false") == 0 ||
             _wcsicmp(val, L"off") == 0);
}

static int AttrInt(IXmlReader* r, const WCHAR* name, int fallback) {
    WCHAR val[32];
    if (!GetAttr(r, name, val, 32)) return fallback;
    return _wtoi(val);
}

static COLORREF ParseHexColor(const WCHAR* hex, BOOL* ok) {
    *ok = FALSE;
    if (!hex || wcslen(hex) < 6) return 0;
    if (_wcsicmp(hex, L"auto") == 0) return 0;

    unsigned int v = 0;
    for (int i = 0; i < 6; i++) {
        WCHAR c = hex[i];
        unsigned d;
        if (c >= L'0' && c <= L'9') d = (unsigned)(c - L'0');
        else if (c >= L'a' && c <= L'f') d = (unsigned)(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') d = (unsigned)(c - L'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *ok = TRUE;
    // OOXML writes RRGGBB; COLORREF is 0x00BBGGRR.
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// ---------------------------------------------------------------------------
// Opening the package and finding the main document part
// ---------------------------------------------------------------------------

static IStream* OpenMainDocumentPart(const WCHAR* path, IOpcPackage** packageOut) {
    *packageOut = NULL;

    IOpcFactory* factory = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_OpcFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IOpcFactory, (void**)&factory);
    if (FAILED(hr)) {
        SetError(L"The Windows packaging component could not be created.");
        return NULL;
    }

    IStream* fileStream = NULL;
    hr = IOpcFactory_CreateStreamOnFile(factory, path, OPC_STREAM_IO_READ,
                                        NULL, 0, &fileStream);
    if (FAILED(hr)) {
        SetError(L"The file could not be opened.");
        IOpcFactory_Release(factory);
        return NULL;
    }

    IOpcPackage* package = NULL;
    hr = IOpcFactory_ReadPackageFromStream(factory, fileStream,
                                           OPC_CACHE_ON_ACCESS, &package);
    IStream_Release(fileStream);

    if (FAILED(hr)) {
        SetError(L"This is not a valid Office package. A .doc renamed to .docx "
                 L"is a different format -- that one arrives in a later version.");
        IOpcFactory_Release(factory);
        return NULL;
    }

    IOpcPartSet* parts = NULL;
    if (FAILED(IOpcPackage_GetPartSet(package, &parts))) {
        SetError(L"The package has no parts.");
        IOpcPackage_Release(package);
        IOpcFactory_Release(factory);
        return NULL;
    }

    IOpcPartUri* docUri = NULL;

    // The main document is whatever the package-level relationship of type
    // officeDocument points at. It is /word/document.xml in practice, but
    // editing history can move it, so the relationship is the authority.
    IOpcRelationshipSet* rels = NULL;
    if (SUCCEEDED(IOpcPackage_GetRelationshipSet(package, &rels))) {
        IOpcRelationshipEnumerator* en = NULL;
        if (SUCCEEDED(IOpcRelationshipSet_GetEnumeratorForType(
                rels, REL_OFFICE_DOCUMENT, &en))) {
            BOOL has = FALSE;
            if (SUCCEEDED(IOpcRelationshipEnumerator_MoveNext(en, &has)) && has) {
                IOpcRelationship* rel = NULL;
                if (SUCCEEDED(IOpcRelationshipEnumerator_GetCurrent(en, &rel))) {
                    IUri* target = NULL;
                    if (SUCCEEDED(IOpcRelationship_GetTargetUri(rel, &target))) {
                        IOpcUri* root = NULL;
                        if (SUCCEEDED(IOpcFactory_CreatePackageRootUri(factory, &root))) {
                            IOpcUri_CombinePartUri(root, target, &docUri);
                            IOpcUri_Release(root);
                        }
                        IUri_Release(target);
                    }
                    IOpcRelationship_Release(rel);
                }
            }
            IOpcRelationshipEnumerator_Release(en);
        }
        IOpcRelationshipSet_Release(rels);
    }

    // Fall back to the conventional location when the relationship is missing
    // or unreadable, rather than refusing a file every other reader opens.
    if (!docUri) {
        IOpcFactory_CreatePartUri(factory, L"/word/document.xml", &docUri);
    }

    IStream* content = NULL;
    if (docUri) {
        IOpcPart* part = NULL;
        if (SUCCEEDED(IOpcPartSet_GetPart(parts, docUri, &part))) {
            IOpcPart_GetContentStream(part, &content);
            IOpcPart_Release(part);
        }
        IOpcPartUri_Release(docUri);
    }

    IOpcPartSet_Release(parts);
    IOpcFactory_Release(factory);

    if (!content) {
        SetError(L"The package contains no main document part.");
        IOpcPackage_Release(package);
        return NULL;
    }

    *packageOut = package;
    return content;
}

// ---------------------------------------------------------------------------
// WordprocessingML -> DocModel
// ---------------------------------------------------------------------------

typedef struct {
    DocModel* doc;

    // Where paragraphs are currently being added. Inside a table that is a
    // cell; otherwise it is the document itself.
    DocBlock* table;
    DocRow*   row;
    DocCell*  cell;

    DocPara*  para;
    CharProps run;
    CharProps paraDefaultRun;   // what the paragraph's style implies for its runs

    BOOL inParaProps;
    BOOL inRunProps;
    BOOL inTblGrid;
    BOOL inText;

    int  skipDepth;             // >0 inside content that is not document text
} Build;

static DocPara* NewParagraph(Build* b) {
    return b->cell ? Doc_AddCellPara(b->cell) : Doc_AddPara(b->doc);
}

static BOOL BuildModel(IStream* stream, DocModel* doc) {
    IXmlReader* reader = NULL;
    if (FAILED(CreateXmlReader(&IID_IXmlReader, (void**)&reader, NULL))) {
        SetError(L"The XML reader could not be created.");
        return FALSE;
    }
    if (FAILED(IXmlReader_SetInput(reader, (IUnknown*)stream))) {
        SetError(L"The main document part could not be read.");
        IXmlReader_Release(reader);
        return FALSE;
    }

    // XmlLite stops at 256 levels by default; deeply nested tables exceed it.
    IXmlReader_SetProperty(reader, XmlReaderProperty_MaxElementDepth, 0);

    Build b = {0};
    b.doc = doc;

    XmlNodeType nt;
    while (S_OK == IXmlReader_Read(reader, &nt)) {
        const WCHAR* local = NULL;
        UINT len = 0;

        if (nt == XmlNodeType_Element || nt == XmlNodeType_EndElement) {
            if (FAILED(IXmlReader_GetLocalName(reader, &local, &len))) continue;
        }

        if (nt == XmlNodeType_Element) {
            BOOL empty = IXmlReader_IsEmptyElement(reader);

            // Content inside a deletion is revision history, not the document.
            if (NameIs(local, len, L"del")) {
                if (!empty) b.skipDepth++;
                continue;
            }
            if (b.skipDepth > 0) continue;

            if (NameIs(local, len, L"p")) {
                b.para = NewParagraph(&b);
                memset(&b.run, 0, sizeof(b.run));
                memset(&b.paraDefaultRun, 0, sizeof(b.paraDefaultRun));
            } else if (NameIs(local, len, L"pPr")) {
                b.inParaProps = TRUE;
            } else if (NameIs(local, len, L"rPr")) {
                b.inRunProps = TRUE;
            } else if (NameIs(local, len, L"r")) {
                b.run = b.paraDefaultRun;
            }

            // --- paragraph properties ---
            else if (b.inParaProps && b.para && NameIs(local, len, L"jc")) {
                WCHAR val[32];
                if (GetAttr(reader, L"val", val, 32)) {
                    if (_wcsicmp(val, L"center") == 0)       b.para->props.align = ALIGN_CENTER;
                    else if (_wcsicmp(val, L"right") == 0)   b.para->props.align = ALIGN_RIGHT;
                    else if (_wcsicmp(val, L"both") == 0 ||
                             _wcsicmp(val, L"justify") == 0) b.para->props.align = ALIGN_JUSTIFY;
                    else                                     b.para->props.align = ALIGN_LEFT;
                }
            } else if (b.inParaProps && b.para && NameIs(local, len, L"ind")) {
                int left = AttrInt(reader, L"left", -1);
                if (left < 0) left = AttrInt(reader, L"start", -1);
                if (left > 0) b.para->props.indentLeft = left;
                int first = AttrInt(reader, L"firstLine", 0);
                int hang  = AttrInt(reader, L"hanging", 0);
                if (hang > 0)       b.para->props.indentFirst = -hang;
                else if (first > 0) b.para->props.indentFirst = first;
            } else if (b.inParaProps && b.para && NameIs(local, len, L"spacing")) {
                int before = AttrInt(reader, L"before", 0);
                int after  = AttrInt(reader, L"after", 0);
                if (before > 0) b.para->props.spaceBefore = before;
                if (after > 0)  b.para->props.spaceAfter = after;
            } else if (b.inParaProps && b.para && NameIs(local, len, L"numPr")) {
                // Which marker a list uses lives in numbering.xml behind two
                // levels of indirection, which v0.9 follows. Bulleted is the
                // common case and is what is assumed until then.
                b.para->props.list = LIST_BULLET;
            } else if (b.inParaProps && b.para && NameIs(local, len, L"ilvl")) {
                b.para->props.listLevel = AttrInt(reader, L"val", 0);
            } else if (b.inParaProps && b.para && NameIs(local, len, L"pStyle")) {
                WCHAR val[64];
                if (GetAttr(reader, L"val", val, 64)) {
                    // Heading styles carry their weight in styles.xml. Until
                    // that is resolved (v0.9), headings are given the shape
                    // readers expect, scaled by level.
                    if (_wcsnicmp(val, L"Heading", 7) == 0) {
                        int level = _wtoi(val + 7);
                        if (level < 1) level = 1;
                        if (level > 6) level = 6;
                        b.para->props.headingLevel = level;
                        b.paraDefaultRun.bold = TRUE;
                        b.paraDefaultRun.halfPoints = 36 - (level - 1) * 4;
                        if (b.paraDefaultRun.halfPoints < 22) b.paraDefaultRun.halfPoints = 22;
                        b.run = b.paraDefaultRun;
                        if (!b.para->props.spaceBefore) b.para->props.spaceBefore = 240;
                        if (!b.para->props.spaceAfter)  b.para->props.spaceAfter = 120;
                    }
                }
            }

            // --- run properties ---
            else if (b.inRunProps && NameIs(local, len, L"b")) {
                b.run.bold = AttrIsOn(reader);
            } else if (b.inRunProps && NameIs(local, len, L"i")) {
                b.run.italic = AttrIsOn(reader);
            } else if (b.inRunProps && NameIs(local, len, L"u")) {
                WCHAR val[32];
                b.run.underline = !GetAttr(reader, L"val", val, 32) ||
                                  _wcsicmp(val, L"none") != 0;
            } else if (b.inRunProps && NameIs(local, len, L"strike")) {
                b.run.strike = AttrIsOn(reader);
            } else if (b.inRunProps && NameIs(local, len, L"sz")) {
                int sz = AttrInt(reader, L"val", 0);
                if (sz > 0) b.run.halfPoints = sz;
            } else if (b.inRunProps && NameIs(local, len, L"color")) {
                WCHAR val[32];
                if (GetAttr(reader, L"val", val, 32)) {
                    BOOL ok = FALSE;
                    COLORREF c = ParseHexColor(val, &ok);
                    if (ok) {
                        b.run.hasColor = TRUE;
                        b.run.color = c;
                    }
                }
            } else if (b.inRunProps && NameIs(local, len, L"vertAlign")) {
                WCHAR val[32];
                if (GetAttr(reader, L"val", val, 32)) {
                    b.run.superscript = (_wcsicmp(val, L"superscript") == 0);
                    b.run.subscript   = (_wcsicmp(val, L"subscript") == 0);
                }
            } else if (b.inRunProps && NameIs(local, len, L"rFonts")) {
                WCHAR val[LF_FACESIZE];
                if (GetAttr(reader, L"ascii", val, LF_FACESIZE) && val[0]) {
                    wcsncpy_s(b.run.font, LF_FACESIZE, val, _TRUNCATE);
                }
            }

            // --- content ---
            else if (NameIs(local, len, L"t")) {
                b.inText = TRUE;
            } else if (NameIs(local, len, L"br")) {
                if (b.para) {
                    DocRun* r = Doc_AddRun(b.para, L"", 0, &b.run);
                    if (r) r->lineBreak = TRUE;
                }
            } else if (NameIs(local, len, L"tab")) {
                if (b.para) {
                    DocRun* r = Doc_AddRun(b.para, L"", 0, &b.run);
                    if (r) r->tab = TRUE;
                }
            }

            // --- tables ---
            else if (NameIs(local, len, L"tbl")) {
                // Nested tables are not modelled yet; an inner table's rows are
                // read as further rows of the outer one rather than lost.
                if (!b.table) {
                    b.table = Doc_AddTable(b.doc);
                    b.row = NULL;
                    b.cell = NULL;
                    b.para = NULL;
                }
            } else if (NameIs(local, len, L"tblGrid")) {
                b.inTblGrid = TRUE;
                if (b.table) b.table->table.gridCount = 0;
            } else if (b.inTblGrid && b.table && NameIs(local, len, L"gridCol")) {
                int cw = AttrInt(reader, L"w", 0);
                if (cw <= 0) cw = 2000;
                int n = b.table->table.gridCount;
                if (n < 32) {
                    int prev = n ? b.table->table.gridEdges[n - 1] : 0;
                    b.table->table.gridEdges[n] = prev + cw;
                    b.table->table.gridCount = n + 1;
                }
            } else if (NameIs(local, len, L"tr")) {
                if (b.table) {
                    b.row = Doc_AddRow(b.table);
                    b.cell = NULL;
                }
            } else if (NameIs(local, len, L"tc")) {
                if (b.row) {
                    b.cell = Doc_AddCell(b.row);
                    b.para = NULL;
                }
            }

        } else if (nt == XmlNodeType_Text || nt == XmlNodeType_Whitespace) {
            if (b.skipDepth > 0 || !b.inText || !b.para) continue;

            const WCHAR* val = NULL;
            UINT vlen = 0;
            if (SUCCEEDED(IXmlReader_GetValue(reader, &val, &vlen)) && vlen) {
                Doc_AddRun(b.para, val, (int)vlen, &b.run);
            }

        } else if (nt == XmlNodeType_EndElement) {
            if (NameIs(local, len, L"del")) {
                if (b.skipDepth > 0) b.skipDepth--;
                continue;
            }
            if (b.skipDepth > 0) continue;

            if (NameIs(local, len, L"t"))            b.inText = FALSE;
            else if (NameIs(local, len, L"pPr"))     b.inParaProps = FALSE;
            else if (NameIs(local, len, L"rPr"))     b.inRunProps = FALSE;
            else if (NameIs(local, len, L"tblGrid")) b.inTblGrid = FALSE;
            else if (NameIs(local, len, L"p")) {
                // An empty paragraph is a blank line and keeps its node; it
                // simply has no runs.
                b.para = NULL;
            }
            else if (NameIs(local, len, L"tc")) b.cell = NULL;
            else if (NameIs(local, len, L"tr")) b.row = NULL;
            else if (NameIs(local, len, L"tbl")) {
                b.table = NULL;
                b.row = NULL;
                b.cell = NULL;
                b.para = NULL;
            }
        }
    }

    IXmlReader_Release(reader);
    return TRUE;
}

DocModel* Docx_ReadToModel(const WCHAR* path) {
    if (!path) return NULL;
    SetError(NULL);

    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL needUninit = SUCCEEDED(init);

    IOpcPackage* package = NULL;
    IStream* docStream = OpenMainDocumentPart(path, &package);
    if (!docStream) {
        if (needUninit) CoUninitialize();
        return NULL;
    }

    DocModel* doc = Doc_New();
    BOOL ok = doc && BuildModel(docStream, doc);

    IStream_Release(docStream);
    IOpcPackage_Release(package);
    if (needUninit) CoUninitialize();

    if (!ok) {
        Doc_Free(doc);
        return NULL;
    }
    return doc;
}

char* Docx_ReadToRtf(const WCHAR* path) {
    DocModel* doc = Docx_ReadToModel(path);
    if (!doc) return NULL;

    char* rtf = DocRtf_Emit(doc);
    Doc_Free(doc);

    if (!rtf) SetError(L"The document could not be converted for display.");
    return rtf;
}

// ---------------------------------------------------------------------------
// DocModel -> WordprocessingML
// ---------------------------------------------------------------------------

static void EmitRunProps(StrBuf* x, const CharProps* p) {
    BOOL any = p->bold || p->italic || p->underline || p->strike ||
               p->superscript || p->subscript || p->halfPoints ||
               p->hasColor || p->font[0];
    if (!any) return;

    SB_Add(x, "<w:rPr>");

    if (p->font[0]) {
        SB_Add(x, "<w:rFonts w:ascii=\"");
        SB_AddXmlText(x, p->font, -1);
        SB_Add(x, "\" w:hAnsi=\"");
        SB_AddXmlText(x, p->font, -1);
        SB_Add(x, "\"/>");
    }
    if (p->bold)      SB_Add(x, "<w:b/>");
    if (p->italic)    SB_Add(x, "<w:i/>");
    if (p->strike)    SB_Add(x, "<w:strike/>");
    if (p->underline) SB_Add(x, "<w:u w:val=\"single\"/>");

    if (p->hasColor) {
        SB_AddF(x, "<w:color w:val=\"%02X%02X%02X\"/>",
                GetRValue(p->color), GetGValue(p->color), GetBValue(p->color));
    }
    if (p->halfPoints > 0) {
        SB_AddF(x, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>",
                p->halfPoints, p->halfPoints);
    }
    if (p->superscript) SB_Add(x, "<w:vertAlign w:val=\"superscript\"/>");
    if (p->subscript)   SB_Add(x, "<w:vertAlign w:val=\"subscript\"/>");

    SB_Add(x, "</w:rPr>");
}

static void EmitParaProps(StrBuf* x, const ParaProps* p) {
    StrBuf inner = {0};

    if (p->headingLevel > 0) {
        SB_AddF(&inner, "<w:pStyle w:val=\"Heading%d\"/>", p->headingLevel);
    }
    if (p->list != LIST_NONE) {
        SB_AddF(&inner, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"1\"/></w:numPr>",
                p->listLevel);
    }

    switch (p->align) {
        case ALIGN_CENTER:  SB_Add(&inner, "<w:jc w:val=\"center\"/>"); break;
        case ALIGN_RIGHT:   SB_Add(&inner, "<w:jc w:val=\"right\"/>");  break;
        case ALIGN_JUSTIFY: SB_Add(&inner, "<w:jc w:val=\"both\"/>");   break;
        default: break;
    }

    if (p->indentLeft || p->indentFirst) {
        SB_Add(&inner, "<w:ind");
        if (p->indentLeft) SB_AddF(&inner, " w:left=\"%d\"", p->indentLeft);
        if (p->indentFirst < 0) SB_AddF(&inner, " w:hanging=\"%d\"", -p->indentFirst);
        else if (p->indentFirst > 0) SB_AddF(&inner, " w:firstLine=\"%d\"", p->indentFirst);
        SB_Add(&inner, "/>");
    }

    if (p->spaceBefore || p->spaceAfter) {
        SB_Add(&inner, "<w:spacing");
        if (p->spaceBefore) SB_AddF(&inner, " w:before=\"%d\"", p->spaceBefore);
        if (p->spaceAfter)  SB_AddF(&inner, " w:after=\"%d\"", p->spaceAfter);
        SB_Add(&inner, "/>");
    }

    if (inner.len) {
        SB_Add(x, "<w:pPr>");
        SB_Add(x, inner.buf);
        SB_Add(x, "</w:pPr>");
    }
    SB_Free(&inner);
}

static void EmitPara(StrBuf* x, const DocPara* para) {
    SB_Add(x, "<w:p>");
    EmitParaProps(x, &para->props);

    for (const DocRun* r = para->runs; r; r = r->next) {
        SB_Add(x, "<w:r>");
        EmitRunProps(x, &r->props);

        if (r->tab) {
            SB_Add(x, "<w:tab/>");
        } else if (r->lineBreak) {
            SB_Add(x, "<w:br/>");
        } else {
            SB_Add(x, "<w:t xml:space=\"preserve\">");
            SB_AddXmlText(x, r->text, -1);
            SB_Add(x, "</w:t>");
        }
        SB_Add(x, "</w:r>");
    }

    SB_Add(x, "</w:p>");
}

static void EmitTable(StrBuf* x, const DocBlock* block) {
    SB_Add(x, "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr>");

    if (block->table.gridCount > 0) {
        SB_Add(x, "<w:tblGrid>");
        int prev = 0;
        for (int i = 0; i < block->table.gridCount; i++) {
            SB_AddF(x, "<w:gridCol w:w=\"%d\"/>", block->table.gridEdges[i] - prev);
            prev = block->table.gridEdges[i];
        }
        SB_Add(x, "</w:tblGrid>");
    }

    for (const DocRow* row = block->table.rows; row; row = row->next) {
        SB_Add(x, "<w:tr>");
        for (const DocCell* c = row->cells; c; c = c->next) {
            SB_Add(x, "<w:tc>");
            // A cell must contain at least one paragraph to be valid.
            if (!c->paras) SB_Add(x, "<w:p/>");
            for (const DocPara* p = c->paras; p; p = p->next) EmitPara(x, p);
            SB_Add(x, "</w:tc>");
        }
        SB_Add(x, "</w:tr>");
    }

    SB_Add(x, "</w:tbl>");
}

static BOOL BuildDocumentXml(const DocModel* doc, StrBuf* x) {
    SB_Add(x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
              "<w:document xmlns:w=\"" WML_NS "\"><w:body>");

    BOOL any = FALSE;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) {
                EmitPara(x, p);
                any = TRUE;
            }
        } else {
            EmitTable(x, b);
            any = TRUE;

            // Word requires a paragraph after a table; without one the table
            // and whatever follows it run together.
            if (!b->next || b->next->kind == BLOCK_TABLE) SB_Add(x, "<w:p/>");
        }
    }

    // A body with no paragraph at all is not a document Word will open.
    if (!any) SB_Add(x, "<w:p/>");

    SB_AddF(x, "<w:sectPr><w:pgSz w:w=\"%d\" w:h=\"%d\"/>"
               "<w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" "
               "w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>",
            doc->section.pageWidth, doc->section.pageHeight,
            doc->section.marginTop, doc->section.marginRight,
            doc->section.marginBottom, doc->section.marginLeft);

    SB_Add(x, "</w:body></w:document>");

    if (x->failed) {
        SetError(L"Ran out of memory building the document.");
        return FALSE;
    }
    return TRUE;
}

BOOL Docx_WriteModel(const DocModel* doc, const WCHAR* path) {
    if (!doc || !path) return FALSE;
    SetError(NULL);

    StrBuf xml = {0};
    if (!BuildDocumentXml(doc, &xml)) {
        SB_Free(&xml);
        return FALSE;
    }

    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL needUninit = SUCCEEDED(init);

    IOpcFactory* factory = NULL;
    IOpcPackage* package = NULL;
    IOpcPartSet* parts = NULL;
    IOpcPartUri* docUri = NULL;
    IOpcPart* part = NULL;
    IStream* content = NULL;
    IStream* file = NULL;
    IOpcRelationshipSet* rels = NULL;
    IOpcRelationship* rel = NULL;
    BOOL ok = FALSE;

    if (FAILED(CoCreateInstance(&CLSID_OpcFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IOpcFactory, (void**)&factory))) {
        SetError(L"The Windows packaging component could not be created.");
        goto done;
    }

    if (FAILED(IOpcFactory_CreatePackage(factory, &package)) ||
        FAILED(IOpcPackage_GetPartSet(package, &parts)) ||
        FAILED(IOpcFactory_CreatePartUri(factory, L"/word/document.xml", &docUri))) {
        SetError(L"The package could not be created.");
        goto done;
    }

    if (FAILED(IOpcPartSet_CreatePart(parts, docUri, CT_MAIN_DOCUMENT,
                                      OPC_COMPRESSION_NORMAL, &part)) ||
        FAILED(IOpcPart_GetContentStream(part, &content))) {
        SetError(L"The main document part could not be created.");
        goto done;
    }

    // Through the vtable rather than the COBJMACROS name: shlwapi.h declares an
    // IStream_Write helper of its own with a different signature, and it wins.
    ULONG written = 0;
    if (FAILED(content->lpVtbl->Write(content, xml.buf, (ULONG)xml.len, &written)) ||
        written != (ULONG)xml.len) {
        SetError(L"The document content could not be written.");
        goto done;
    }

    // Without this relationship the package is a zip of XML that no reader
    // knows how to start from.
    if (FAILED(IOpcPackage_GetRelationshipSet(package, &rels)) ||
        FAILED(IOpcRelationshipSet_CreateRelationship(
            rels, NULL, REL_OFFICE_DOCUMENT, (IUri*)docUri,
            OPC_URI_TARGET_MODE_INTERNAL, &rel))) {
        SetError(L"The package relationship could not be created.");
        goto done;
    }

    if (FAILED(IOpcFactory_CreateStreamOnFile(factory, path, OPC_STREAM_IO_WRITE,
                                              NULL, 0, &file))) {
        SetError(L"The file could not be created for writing.");
        goto done;
    }

    if (FAILED(IOpcFactory_WritePackageToStream(factory, package,
                                                OPC_WRITE_DEFAULT, file))) {
        SetError(L"The package could not be written.");
        goto done;
    }

    ok = TRUE;

done:
    if (rel)     IOpcRelationship_Release(rel);
    if (rels)    IOpcRelationshipSet_Release(rels);
    if (file)    IStream_Release(file);
    if (content) IStream_Release(content);
    if (part)    IOpcPart_Release(part);
    if (docUri)  IOpcPartUri_Release(docUri);
    if (parts)   IOpcPartSet_Release(parts);
    if (package) IOpcPackage_Release(package);
    if (factory) IOpcFactory_Release(factory);
    if (needUninit) CoUninitialize();

    SB_Free(&xml);
    return ok;
}

BOOL Docx_WriteFromEditor(HWND hRichEdit, const WCHAR* path) {
    if (!hRichEdit || !path) return FALSE;

    DocModel* doc = DocView_Capture(hRichEdit);
    if (!doc) {
        SetError(L"The document could not be read out of the editor.");
        return FALSE;
    }

    BOOL ok = Docx_WriteModel(doc, path);
    Doc_Free(doc);

    if (ok) SendMessageW(hRichEdit, EM_SETMODIFY, FALSE, 0);
    return ok;
}


// ---------------------------------------------------------------------------
// Self-check. Run with: OpenNote.exe --selftest
//
// The reader is covered against externally produced documents by
// --docx-check; what is checked here is the writer, and the only honest way to
// check a writer is to read back what it wrote.
// ---------------------------------------------------------------------------

BOOL Docx_SelfTest(char* failure, size_t failureSize) {
#define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        if (h) DestroyWindow(h); \
        if (tmpFile[0]) DeleteFileW(tmpFile); \
        free(rtf); \
        return FALSE; \
    } while (0)

    HWND   h = NULL;
    char*  rtf = NULL;
    WCHAR  tmpFile[MAX_PATH] = {0};
    WCHAR  tmpDir[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmpDir) == 0 ||
        GetTempFileNameW(tmpDir, L"onx", 0, tmpFile) == 0) {
        strncpy_s(failure, failureSize, "could not make a temporary file", _TRUNCATE);
        return FALSE;
    }

    // GetTempFileName makes a .tmp; the writer does not care, but keeping the
    // extension honest keeps the file recognisable if a run leaves one behind.
    WCHAR docxPath[MAX_PATH];
    swprintf_s(docxPath, MAX_PATH, L"%s.docx", tmpFile);
    DeleteFileW(tmpFile);
    wcscpy_s(tmpFile, MAX_PATH, docxPath);

    // Not inherited from whichever check ran before this one.
    Rich_EnsureLoaded();

    h = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
                        WS_POPUP | ES_MULTILINE | ES_NOHIDESEL,
                        0, 0, 100, 100, HWND_MESSAGE, NULL,
                        GetModuleHandleW(NULL), NULL);
    if (!h) {
        strncpy_s(failure, failureSize, "could not create a RichEdit control", _TRUNCATE);
        DeleteFileW(tmpFile);
        return FALSE;
    }
    SendMessageW(h, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO, 0);
    SendMessageW(h, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);

    // Three paragraphs: a bold+coloured run beside a plain one, a centred
    // paragraph, and text with characters that have to survive XML escaping.
    Rich_SetText(h, L"Alpha bravo\rCentred line\rAmpersand & angle < and \x2014 dash");

    Rich_SetSelection(h, 0, 5);
    Rich_ToggleEffect(h, CFE_BOLD);
    Rich_SetTextColor(h, RGB(200, 30, 30));
    Rich_SetFontSize(h, 18);

    Rich_SetSelection(h, 12, 24);
    Rich_SetAlignment(h, PFA_CENTER);

    if (!Docx_WriteFromEditor(h, tmpFile)) FAIL("Docx_WriteFromEditor failed");

    DWORD attrs = GetFileAttributesW(tmpFile);
    if (attrs == INVALID_FILE_ATTRIBUTES) FAIL("the written .docx does not exist");

    // Read it back through the same path a user opening the file would take.
    rtf = Docx_ReadToRtf(tmpFile);
    if (!rtf) FAIL("the written .docx could not be read back");

    if (strncmp(rtf, "{\\rtf", 5) != 0) FAIL("reading back did not produce RTF");

    if (!strstr(rtf, "Alpha")) FAIL("text did not survive the .docx round trip");
    if (!strstr(rtf, "bravo")) FAIL("later text in a paragraph was lost");
    if (!strstr(rtf, "Centred line")) FAIL("a later paragraph was lost");

    // Formatting must come back, not just the characters.
    if (!strstr(rtf, "\\b"))  FAIL("bold did not survive the .docx round trip");
    if (!strstr(rtf, "\\cf")) FAIL("colour did not survive the .docx round trip");
    if (!strstr(rtf, "\\qc")) FAIL("centred alignment did not survive the round trip");

    // XML-significant characters must have been escaped on the way out and
    // decoded on the way back, arriving as themselves.
    if (!strstr(rtf, "Ampersand & angle < and")) {
        FAIL("XML escaping did not round trip");
    }
    // The em dash is non-ASCII and must come back as an RTF unicode escape.
    if (!strstr(rtf, "\\u8212?")) FAIL("a non-ASCII character was lost");

    // Writing must leave the document unmodified, or saving would immediately
    // mark it dirty again.
    if (Rich_GetModified(h)) FAIL("writing a .docx left the document modified");

    free(rtf);
    rtf = NULL;

    // An empty document must still produce a package a reader accepts, rather
    // than a zero-paragraph body that fails to open.
    Rich_SetText(h, L"");
    if (!Docx_WriteFromEditor(h, tmpFile)) FAIL("writing an empty document failed");
    rtf = Docx_ReadToRtf(tmpFile);
    if (!rtf) FAIL("an empty .docx could not be read back");

    free(rtf);
    rtf = NULL;

    DeleteFileW(tmpFile);
    DestroyWindow(h);

    failure[0] = '\0';
    return TRUE;

#undef FAIL
}
