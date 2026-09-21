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

#define REL_STYLES     L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"
#define REL_NUMBERING     L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering"
#define REL_IMAGE         L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image"
#define REL_FOOTNOTES     L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes"
#define REL_ENDNOTES      L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/endnotes"

#define CT_FOOTNOTES \
    L"application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml"
#define CT_ENDNOTES \
    L"application/vnd.openxmlformats-officedocument.wordprocessingml.endnotes+xml"

#define REL_HEADER        L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/header"
#define REL_FOOTER        L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer"

#define CT_HEADER \
    L"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"
#define CT_FOOTER \
    L"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"

#define CT_STYLES     L"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"
#define CT_NUMBERING     L"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"

#define CT_MAIN_DOCUMENT \
    L"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"

#define WML_NS "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
#define REL_NS "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define WP_NS  "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
#define DML_NS "http://schemas.openxmlformats.org/drawingml/2006/main"
#define PIC_NS "http://schemas.openxmlformats.org/drawingml/2006/picture"

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

// ---------------------------------------------------------------------------
// The package, and the parts hanging off the main document
//
// A .docx keeps its styles, its numbering, its images and its headers in
// separate parts, each reached by a relationship from the document part. The
// relationship is the authority for where a part lives: /word/styles.xml is
// only a convention, and editing history moves things.
// ---------------------------------------------------------------------------

typedef struct {
    IOpcFactory* factory;
    IOpcPackage* package;
    IOpcPartSet* parts;
    IOpcPart*    docPart;
    IOpcPartUri* docUri;
} DocxPkg;

static void ClosePackage(DocxPkg* pkg) {
    if (pkg->docUri)  IOpcPartUri_Release(pkg->docUri);
    if (pkg->docPart) IOpcPart_Release(pkg->docPart);
    if (pkg->parts)   IOpcPartSet_Release(pkg->parts);
    if (pkg->package) IOpcPackage_Release(pkg->package);
    if (pkg->factory) IOpcFactory_Release(pkg->factory);
    memset(pkg, 0, sizeof(*pkg));
}

static BOOL OpenPackage(const WCHAR* path, DocxPkg* pkg) {
    memset(pkg, 0, sizeof(*pkg));

    HRESULT hr = CoCreateInstance(&CLSID_OpcFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IOpcFactory, (void**)&pkg->factory);
    if (FAILED(hr)) {
        SetError(L"The Windows packaging component could not be created.");
        return FALSE;
    }

    IStream* fileStream = NULL;
    hr = IOpcFactory_CreateStreamOnFile(pkg->factory, path, OPC_STREAM_IO_READ,
                                        NULL, 0, &fileStream);
    if (FAILED(hr)) {
        SetError(L"The file could not be opened.");
        ClosePackage(pkg);
        return FALSE;
    }

    hr = IOpcFactory_ReadPackageFromStream(pkg->factory, fileStream,
                                           OPC_CACHE_ON_ACCESS, &pkg->package);
    IStream_Release(fileStream);

    if (FAILED(hr)) {
        SetError(L"This is not a valid Office package. A .doc renamed to .docx "
                 L"is a different format -- that one arrives in a later version.");
        ClosePackage(pkg);
        return FALSE;
    }

    if (FAILED(IOpcPackage_GetPartSet(pkg->package, &pkg->parts))) {
        SetError(L"The package has no parts.");
        ClosePackage(pkg);
        return FALSE;
    }

    // The main document is whatever the package-level relationship of type
    // officeDocument points at.
    IOpcRelationshipSet* rels = NULL;
    if (SUCCEEDED(IOpcPackage_GetRelationshipSet(pkg->package, &rels))) {
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
                        if (SUCCEEDED(IOpcFactory_CreatePackageRootUri(pkg->factory, &root))) {
                            IOpcUri_CombinePartUri(root, target, &pkg->docUri);
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
    if (!pkg->docUri) {
        IOpcFactory_CreatePartUri(pkg->factory, L"/word/document.xml", &pkg->docUri);
    }

    if (!pkg->docUri ||
        FAILED(IOpcPartSet_GetPart(pkg->parts, pkg->docUri, &pkg->docPart))) {
        SetError(L"The package contains no main document part.");
        ClosePackage(pkg);
        return FALSE;
    }

    return TRUE;
}

// The part a relationship of `relType` from the document points at. NULL when
// the document has no such relationship, which is the ordinary case -- a
// document with no lists has no numbering part.
static IStream* RelatedStream(DocxPkg* pkg, const WCHAR* relType) {
    IOpcRelationshipSet* rels = NULL;
    if (FAILED(IOpcPart_GetRelationshipSet(pkg->docPart, &rels))) return NULL;

    IStream* stream = NULL;
    IOpcRelationshipEnumerator* en = NULL;

    if (SUCCEEDED(IOpcRelationshipSet_GetEnumeratorForType(rels, relType, &en))) {
        BOOL has = FALSE;
        if (SUCCEEDED(IOpcRelationshipEnumerator_MoveNext(en, &has)) && has) {
            IOpcRelationship* rel = NULL;
            if (SUCCEEDED(IOpcRelationshipEnumerator_GetCurrent(en, &rel))) {
                IUri* target = NULL;
                if (SUCCEEDED(IOpcRelationship_GetTargetUri(rel, &target))) {
                    IOpcPartUri* uri = NULL;
                    // Relative to the document part, which is what puts
                    // styles.xml beside document.xml wherever that is.
                    if (SUCCEEDED(IOpcUri_CombinePartUri((IOpcUri*)pkg->docUri, target, &uri))) {
                        IOpcPart* part = NULL;
                        if (SUCCEEDED(IOpcPartSet_GetPart(pkg->parts, uri, &part))) {
                            IOpcPart_GetContentStream(part, &stream);
                            IOpcPart_Release(part);
                        }
                        IOpcPartUri_Release(uri);
                    }
                    IUri_Release(target);
                }
                IOpcRelationship_Release(rel);
            }
        }
        IOpcRelationshipEnumerator_Release(en);
    }

    IOpcRelationshipSet_Release(rels);
    return stream;
}


// The part a particular relationship id points at, with its content type --
// which is how a picture is found: the drawing says r:embed="rId7", and the
// relationship says which part that is and what kind of image it holds.
static BYTE* ReadRelatedBytes(DocxPkg* pkg, const WCHAR* relId, size_t* lenOut,
                              WCHAR* contentTypeOut, size_t contentTypeChars) {
    *lenOut = 0;
    if (contentTypeOut && contentTypeChars) contentTypeOut[0] = L'\0';
    if (!relId || !relId[0]) return NULL;

    IOpcRelationshipSet* rels = NULL;
    if (FAILED(IOpcPart_GetRelationshipSet(pkg->docPart, &rels))) return NULL;

    IOpcRelationship* rel = NULL;
    BYTE* bytes = NULL;

    if (SUCCEEDED(IOpcRelationshipSet_GetRelationship(rels, relId, &rel))) {
        IUri* target = NULL;
        if (SUCCEEDED(IOpcRelationship_GetTargetUri(rel, &target))) {
            IOpcPartUri* uri = NULL;
            if (SUCCEEDED(IOpcUri_CombinePartUri((IOpcUri*)pkg->docUri, target, &uri))) {
                IOpcPart* part = NULL;
                if (SUCCEEDED(IOpcPartSet_GetPart(pkg->parts, uri, &part))) {
                    LPWSTR type = NULL;
                    if (contentTypeOut && SUCCEEDED(IOpcPart_GetContentType(part, &type)) && type) {
                        wcsncpy_s(contentTypeOut, contentTypeChars, type, _TRUNCATE);
                        CoTaskMemFree(type);
                    }

                    IStream* stream = NULL;
                    if (SUCCEEDED(IOpcPart_GetContentStream(part, &stream)) && stream) {
                        // Read it whole: a picture is small enough to hold and
                        // the model keeps the bytes anyway.
                        STATSTG stat = {0};
                        if (SUCCEEDED(stream->lpVtbl->Stat(stream, &stat, STATFLAG_NONAME)) &&
                            stat.cbSize.QuadPart > 0 &&
                            stat.cbSize.QuadPart < 64 * 1024 * 1024) {

                            size_t size = (size_t)stat.cbSize.QuadPart;
                            bytes = (BYTE*)malloc(size);
                            if (bytes) {
                                ULONG got = 0;
                                if (SUCCEEDED(stream->lpVtbl->Read(stream, bytes, (ULONG)size, &got))) {
                                    *lenOut = got;
                                } else {
                                    free(bytes);
                                    bytes = NULL;
                                }
                            }
                        }
                        IStream_Release(stream);
                    }
                    IOpcPart_Release(part);
                }
                IOpcPartUri_Release(uri);
            }
            IUri_Release(target);
        }
        IOpcRelationship_Release(rel);
    }

    IOpcRelationshipSet_Release(rels);
    return bytes;
}

// The part a relationship id names, as a stream -- the same lookup the
// pictures use, for the parts that hold markup rather than bytes.
static IStream* RelatedStreamById(DocxPkg* pkg, const WCHAR* relId) {
    if (!relId || !relId[0]) return NULL;

    IOpcRelationshipSet* rels = NULL;
    if (FAILED(IOpcPart_GetRelationshipSet(pkg->docPart, &rels))) return NULL;

    IStream* stream = NULL;
    IOpcRelationship* rel = NULL;

    if (SUCCEEDED(IOpcRelationshipSet_GetRelationship(rels, relId, &rel))) {
        IUri* target = NULL;
        if (SUCCEEDED(IOpcRelationship_GetTargetUri(rel, &target))) {
            IOpcPartUri* uri = NULL;
            if (SUCCEEDED(IOpcUri_CombinePartUri((IOpcUri*)pkg->docUri, target, &uri))) {
                IOpcPart* part = NULL;
                if (SUCCEEDED(IOpcPartSet_GetPart(pkg->parts, uri, &part))) {
                    IOpcPart_GetContentStream(part, &stream);
                    IOpcPart_Release(part);
                }
                IOpcPartUri_Release(uri);
            }
            IUri_Release(target);
        }
        IOpcRelationship_Release(rel);
    }

    IOpcRelationshipSet_Release(rels);
    return stream;
}

// "width:120pt;height:90pt" out of a VML shape's style attribute, in EMU.
// A point is 12700 EMU; the other units a style can use are rarer than the
// documents that never state a size at all.
static void ParseVmlSize(const WCHAR* style, int* widthEmu, int* heightEmu) {
    const WCHAR* at = style;
    while (at && *at) {
        while (*at == L' ' || *at == L';') at++;

        BOOL isWidth = _wcsnicmp(at, L"width:", 6) == 0;
        BOOL isHeight = _wcsnicmp(at, L"height:", 7) == 0;
        if (isWidth || isHeight) {
            const WCHAR* value = at + (isWidth ? 6 : 7);
            double number = _wtof(value);

            double emu = number * 12700.0;          // points
            if (wcsstr(value, L"in"))      emu = number * 914400.0;
            else if (wcsstr(value, L"cm")) emu = number * 360000.0;
            else if (wcsstr(value, L"mm")) emu = number * 36000.0;
            else if (wcsstr(value, L"px")) emu = number * 9525.0;

            if (isWidth) *widthEmu = (int)emu;
            else         *heightEmu = (int)emu;
        }

        const WCHAR* semi = wcschr(at, L';');
        if (!semi) break;
        at = semi + 1;
    }
}

// ---------------------------------------------------------------------------
// Properties, shared between the document and styles.xml
//
// `w:rPr` and `w:pPr` mean the same thing wherever they appear, which is why
// reading them lives here rather than inside the document parser: a style
// states its properties in exactly the elements a paragraph does.
// ---------------------------------------------------------------------------

static BOOL ReadRunProp(IXmlReader* r, const WCHAR* local, UINT len, CharProps* run) {
    if (NameIs(local, len, L"b")) {
        run->bold = AttrIsOn(r);
    } else if (NameIs(local, len, L"i")) {
        run->italic = AttrIsOn(r);
    } else if (NameIs(local, len, L"u")) {
        WCHAR val[32];
        run->underline = !GetAttr(r, L"val", val, 32) || _wcsicmp(val, L"none") != 0;
    } else if (NameIs(local, len, L"strike")) {
        run->strike = AttrIsOn(r);
    } else if (NameIs(local, len, L"sz")) {
        int sz = AttrInt(r, L"val", 0);
        if (sz > 0) run->halfPoints = sz;
    } else if (NameIs(local, len, L"color")) {
        WCHAR val[32];
        if (GetAttr(r, L"val", val, 32)) {
            BOOL ok = FALSE;
            COLORREF c = ParseHexColor(val, &ok);
            if (ok) {
                run->hasColor = TRUE;
                run->color = c;
            }
        }
    } else if (NameIs(local, len, L"vertAlign")) {
        WCHAR val[32];
        if (GetAttr(r, L"val", val, 32)) {
            run->superscript = (_wcsicmp(val, L"superscript") == 0);
            run->subscript   = (_wcsicmp(val, L"subscript") == 0);
        }
    } else if (NameIs(local, len, L"rFonts")) {
        WCHAR val[LF_FACESIZE];
        if (GetAttr(r, L"ascii", val, LF_FACESIZE) && val[0]) {
            wcsncpy_s(run->font, LF_FACESIZE, val, _TRUNCATE);
        }
    } else {
        return FALSE;
    }
    return TRUE;
}

static BOOL ReadParaProp(IXmlReader* r, const WCHAR* local, UINT len, ParaProps* pp) {
    if (NameIs(local, len, L"jc")) {
        WCHAR val[32];
        if (GetAttr(r, L"val", val, 32)) {
            if (_wcsicmp(val, L"center") == 0)       pp->align = ALIGN_CENTER;
            else if (_wcsicmp(val, L"right") == 0)   pp->align = ALIGN_RIGHT;
            else if (_wcsicmp(val, L"both") == 0 ||
                     _wcsicmp(val, L"justify") == 0) pp->align = ALIGN_JUSTIFY;
            else                                     pp->align = ALIGN_LEFT;
        }
    } else if (NameIs(local, len, L"ind")) {
        int left = AttrInt(r, L"left", -1);
        if (left < 0) left = AttrInt(r, L"start", -1);
        if (left > 0) pp->indentLeft = left;

        int first = AttrInt(r, L"firstLine", 0);
        int hang  = AttrInt(r, L"hanging", 0);
        if (hang > 0)       pp->indentFirst = -hang;
        else if (first > 0) pp->indentFirst = first;
    } else if (NameIs(local, len, L"spacing")) {
        int before = AttrInt(r, L"before", 0);
        int after  = AttrInt(r, L"after", 0);
        if (before > 0) pp->spaceBefore = before;
        if (after > 0)  pp->spaceAfter = after;
    } else {
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// numbering.xml
//
// Two levels of indirection: a paragraph names a `w:numId`, which names an
// abstract numbering, which holds nine levels -- and a level says whether it
// counts, how, and what the marker looks like. Reading it is what turns "this
// paragraph is in a list" into "this paragraph is item (b)".
// ---------------------------------------------------------------------------

typedef struct {
    BOOL         stated;
    DocNumFormat fmt;
    WCHAR        text[24];
    int          indentLeft;
    int          indentHanging;
} NumLevel;

typedef struct {
    int      id;
    NumLevel levels[9];
} AbstractNum;

typedef struct {
    AbstractNum* abstracts;
    int          abstractCount, abstractCap;

    struct { int numId; int abstractId; }* map;
    int mapCount, mapCap;
} NumTable;

static void FreeNumTable(NumTable* t) {
    free(t->abstracts);
    free(t->map);
    memset(t, 0, sizeof(*t));
}

static DocNumFormat ParseNumFmt(const WCHAR* val) {
    if (_wcsicmp(val, L"bullet") == 0)      return NUMFMT_BULLET;
    if (_wcsicmp(val, L"lowerLetter") == 0) return NUMFMT_LOWER_LETTER;
    if (_wcsicmp(val, L"upperLetter") == 0) return NUMFMT_UPPER_LETTER;
    if (_wcsicmp(val, L"lowerRoman") == 0)  return NUMFMT_LOWER_ROMAN;
    if (_wcsicmp(val, L"upperRoman") == 0)  return NUMFMT_UPPER_ROMAN;
    return NUMFMT_DECIMAL;
}

static AbstractNum* AddAbstract(NumTable* t, int id) {
    for (int i = 0; i < t->abstractCount; i++) {
        if (t->abstracts[i].id == id) return &t->abstracts[i];
    }
    if (t->abstractCount == t->abstractCap) {
        int cap = t->abstractCap ? t->abstractCap * 2 : 8;
        AbstractNum* grown = (AbstractNum*)realloc(t->abstracts, (size_t)cap * sizeof(AbstractNum));
        if (!grown) return NULL;
        t->abstracts = grown;
        t->abstractCap = cap;
    }
    AbstractNum* a = &t->abstracts[t->abstractCount++];
    memset(a, 0, sizeof(*a));
    a->id = id;
    return a;
}

static void ReadNumbering(IStream* stream, NumTable* table) {
    IXmlReader* reader = NULL;
    if (FAILED(CreateXmlReader(&IID_IXmlReader, (void**)&reader, NULL))) return;
    if (FAILED(IXmlReader_SetInput(reader, (IUnknown*)stream))) {
        IXmlReader_Release(reader);
        return;
    }

    AbstractNum* current = NULL;
    NumLevel* level = NULL;
    int pendingNumId = -1;

    XmlNodeType nt;
    while (S_OK == IXmlReader_Read(reader, &nt)) {
        const WCHAR* local = NULL;
        UINT len = 0;
        if (nt != XmlNodeType_Element && nt != XmlNodeType_EndElement) continue;
        if (FAILED(IXmlReader_GetLocalName(reader, &local, &len))) continue;

        if (nt == XmlNodeType_Element) {
            if (NameIs(local, len, L"abstractNum")) {
                current = AddAbstract(table, AttrInt(reader, L"abstractNumId", -1));
                level = NULL;
            } else if (NameIs(local, len, L"lvl") && current) {
                int ilvl = AttrInt(reader, L"ilvl", 0);
                level = (ilvl >= 0 && ilvl < 9) ? &current->levels[ilvl] : NULL;
                if (level) {
                    memset(level, 0, sizeof(*level));
                    level->stated = TRUE;
                    level->fmt = NUMFMT_DECIMAL;
                }
            } else if (level && NameIs(local, len, L"numFmt")) {
                WCHAR val[32];
                if (GetAttr(reader, L"val", val, 32)) level->fmt = ParseNumFmt(val);
            } else if (level && NameIs(local, len, L"lvlText")) {
                GetAttr(reader, L"val", level->text, 24);
            } else if (level && NameIs(local, len, L"ind")) {
                int left = AttrInt(reader, L"left", -1);
                if (left < 0) left = AttrInt(reader, L"start", -1);
                if (left > 0) level->indentLeft = left;
                int hang = AttrInt(reader, L"hanging", 0);
                if (hang > 0) level->indentHanging = hang;
            } else if (NameIs(local, len, L"num")) {
                pendingNumId = AttrInt(reader, L"numId", -1);
            } else if (NameIs(local, len, L"abstractNumId") && pendingNumId >= 0) {
                int absId = AttrInt(reader, L"val", -1);
                if (absId >= 0) {
                    if (table->mapCount == table->mapCap) {
                        int cap = table->mapCap ? table->mapCap * 2 : 8;
                        void* grown = realloc(table->map, (size_t)cap * sizeof(*table->map));
                        if (grown) {
                            table->map = grown;
                            table->mapCap = cap;
                        }
                    }
                    if (table->mapCount < table->mapCap) {
                        table->map[table->mapCount].numId = pendingNumId;
                        table->map[table->mapCount].abstractId = absId;
                        table->mapCount++;
                    }
                }
                pendingNumId = -1;
            }
        } else {
            if (NameIs(local, len, L"lvl")) level = NULL;
            else if (NameIs(local, len, L"abstractNum")) current = NULL;
        }
    }

    IXmlReader_Release(reader);
}

// Turn a `w:numId` and a level into what the paragraph actually looks like.
static void ApplyNumbering(const NumTable* table, int numId, int ilvl, ParaProps* pp) {
    pp->listId = numId;
    pp->listLevel = (ilvl >= 0 && ilvl < 9) ? ilvl : 0;

    const NumLevel* level = NULL;
    for (int i = 0; i < table->mapCount && !level; i++) {
        if (table->map[i].numId != numId) continue;
        for (int a = 0; a < table->abstractCount; a++) {
            if (table->abstracts[a].id != table->map[i].abstractId) continue;
            const NumLevel* candidate = &table->abstracts[a].levels[pp->listLevel];
            if (candidate->stated) level = candidate;
            break;
        }
    }

    if (!level) {
        // A list whose numbering part says nothing about it. Bulleted is what
        // the reader assumed before any of this existed, and it is still the
        // safer guess than counting something Word does not count.
        pp->list = LIST_BULLET;
        pp->numFormat = NUMFMT_BULLET;
        return;
    }

    pp->numFormat = level->fmt;
    pp->list = (level->fmt == NUMFMT_BULLET) ? LIST_BULLET : LIST_NUMBER;
    if (level->fmt != NUMFMT_BULLET && level->text[0]) {
        wcsncpy_s(pp->listText, 24, level->text, _TRUNCATE);
    }

    // The level's indents, unless the paragraph stated its own.
    if (level->indentLeft && !pp->indentLeft) pp->indentLeft = level->indentLeft;
    if (level->indentHanging && !pp->indentFirst) pp->indentFirst = -level->indentHanging;
}

// ---------------------------------------------------------------------------
// styles.xml
// ---------------------------------------------------------------------------

// "heading 1" as a style name, or "Heading1" as an id, is how a document says
// this paragraph is a heading. Both spellings appear in the wild.
static int HeadingLevelOf(const WCHAR* text) {
    if (!text || !text[0]) return 0;

    const WCHAR* rest = NULL;
    if (_wcsnicmp(text, L"heading", 7) == 0) rest = text + 7;
    else return 0;

    while (*rest == L' ') rest++;
    int level = _wtoi(rest);
    if (level < 1 || level > 6) return 0;
    return level;
}

static void ReadStyles(IStream* stream, DocModel* doc) {
    IXmlReader* reader = NULL;
    if (FAILED(CreateXmlReader(&IID_IXmlReader, (void**)&reader, NULL))) return;
    if (FAILED(IXmlReader_SetInput(reader, (IUnknown*)stream))) {
        IXmlReader_Release(reader);
        return;
    }

    DocStyle* style = NULL;
    BOOL inDefaults = FALSE;
    BOOL inRunProps = FALSE;
    BOOL inParaProps = FALSE;

    XmlNodeType nt;
    while (S_OK == IXmlReader_Read(reader, &nt)) {
        const WCHAR* local = NULL;
        UINT len = 0;
        if (nt != XmlNodeType_Element && nt != XmlNodeType_EndElement) continue;
        if (FAILED(IXmlReader_GetLocalName(reader, &local, &len))) continue;

        if (nt == XmlNodeType_Element) {
            if (NameIs(local, len, L"docDefaults")) {
                inDefaults = TRUE;
            } else if (NameIs(local, len, L"style")) {
                WCHAR id[64];
                if (GetAttr(reader, L"styleId", id, 64)) {
                    style = Doc_AddStyle(doc, id);
                    if (style) style->para.headingLevel = HeadingLevelOf(id);
                }
            } else if (NameIs(local, len, L"name") && style) {
                WCHAR val[64];
                if (GetAttr(reader, L"val", val, 64)) {
                    wcsncpy_s(style->name, 64, val, _TRUNCATE);
                    int level = HeadingLevelOf(val);
                    if (level) style->para.headingLevel = level;
                }
            } else if (NameIs(local, len, L"basedOn") && style) {
                GetAttr(reader, L"val", style->basedOn, 64);
            } else if (NameIs(local, len, L"rPr")) {
                inRunProps = TRUE;
            } else if (NameIs(local, len, L"pPr")) {
                inParaProps = TRUE;
            } else if (inRunProps) {
                CharProps* target = style ? &style->run : (inDefaults ? &doc->defaultRun : NULL);
                if (target) ReadRunProp(reader, local, len, target);
            } else if (inParaProps) {
                ParaProps* target = style ? &style->para : (inDefaults ? &doc->defaultPara : NULL);
                if (target) ReadParaProp(reader, local, len, target);
            }
        } else {
            if (NameIs(local, len, L"style"))            style = NULL;
            else if (NameIs(local, len, L"docDefaults")) inDefaults = FALSE;
            else if (NameIs(local, len, L"rPr"))         inRunProps = FALSE;
            else if (NameIs(local, len, L"pPr"))         inParaProps = FALSE;
        }
    }

    IXmlReader_Release(reader);
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

    // A `w:numPr` names a list and a level in two child elements, so both are
    // collected before either can be resolved.
    BOOL inNumPr;
    int  numId;
    int  ilvl;

    // The page setup, which arrives at the end of the body -- and again at the
    // end of any paragraph that closes a section.
    BOOL  inSection;
    WCHAR headerRel[64];
    WCHAR footerRel[64];

    // The marks are numbered by the order the references appear in.
    int   footnoteMarks;
    int   endnoteMarks;

    // A picture, which arrives in pieces: an extent, then a relationship id
    // several elements later.
    BOOL  inDrawing;
    BOOL  inPicture;          // the VML spelling
    int   imageWidthEmu;
    int   imageHeightEmu;

    int  skipDepth;             // >0 inside content that is not document text

    // Track changes. `w:ins` and `w:del` wrap whole runs, so the mark is held
    // here while their children go past and stamped onto every run inside.
    RevisionMark rev;
    int          revDepth;      // nesting, because an insertion can be deleted
} Build;

static DocPara* NewParagraph(Build* b) {
    return b->cell ? Doc_AddCellPara(b->cell) : Doc_AddPara(b->doc);
}

static void ReadNotes(IStream* stream, DocModel* doc, BOOL endnote,
                      const NumTable* numbering, DocxPkg* pkg);

static BOOL BuildModel(IStream* stream, DocModel* doc, const NumTable* numbering,
                       DocxPkg* pkg) {
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

            // A tracked change wraps the runs it applies to. It used to be
            // skipped, which read correctly and threw the history away: a
            // document saved after being opened had lost its deletions.
            if (NameIs(local, len, L"ins") || NameIs(local, len, L"del")) {
                // `w:ins` also appears inside `w:rPr` as a *paragraph mark*
                // revision, which is a different thing and applies to nothing
                // this model holds.
                if (b.inRunProps || b.inParaProps) continue;

                b.rev.kind = NameIs(local, len, L"del") ? REV_DELETED : REV_INSERTED;
                GetAttr(reader, L"author", b.rev.author, 64);
                GetAttr(reader, L"date", b.rev.date, 32);
                if (!empty) b.revDepth++;
                continue;
            }
            if (b.skipDepth > 0) continue;

            if (NameIs(local, len, L"p")) {
                b.para = NewParagraph(&b);
                // What `w:docDefaults` states is where every paragraph starts.
                if (b.para) b.para->props = doc->defaultPara;
                b.paraDefaultRun = doc->defaultRun;
                b.run = b.paraDefaultRun;
            } else if (NameIs(local, len, L"pPr")) {
                b.inParaProps = TRUE;
            } else if (NameIs(local, len, L"rPr")) {
                b.inRunProps = TRUE;
            } else if (NameIs(local, len, L"r")) {
                b.run = b.paraDefaultRun;
            }

            // --- paragraph properties ---
            else if (b.inParaProps && b.para && NameIs(local, len, L"numPr")) {
                b.inNumPr = TRUE;
                b.numId = -1;
                b.ilvl = 0;
                if (empty) {
                    // `<w:numPr/>` with nothing in it: a list with no number.
                    b.para->props.list = LIST_BULLET;
                    b.para->props.numFormat = NUMFMT_BULLET;
                    b.inNumPr = FALSE;
                }
            } else if (b.inNumPr && NameIs(local, len, L"numId")) {
                b.numId = AttrInt(reader, L"val", -1);
            } else if (b.inNumPr && NameIs(local, len, L"ilvl")) {
                b.ilvl = AttrInt(reader, L"val", 0);
            } else if (b.inParaProps && b.para && NameIs(local, len, L"pStyle")) {
                WCHAR val[64];
                if (GetAttr(reader, L"val", val, 64)) {
                    if (Doc_FindStyle(doc, val)) {
                        // The style table says what this paragraph looks like.
                        // Resolving it here rather than at layout time keeps
                        // the model a description of the document rather than
                        // a set of references that have to be chased again.
                        ParaProps resolved = b.para->props;
                        ParaProps fromStyle;
                        CharProps runFromStyle;
                        Doc_ResolveStyle(doc, val, &fromStyle, &runFromStyle);

                        fromStyle.list = resolved.list;
                        fromStyle.listLevel = resolved.listLevel;
                        fromStyle.numFormat = resolved.numFormat;
                        b.para->props = fromStyle;

                        b.paraDefaultRun = runFromStyle;
                        b.run = b.paraDefaultRun;
                    } else {
                        // No styles.xml, or a style it does not define. A
                        // heading still has to look like one, so the old
                        // approximation stays as the fallback it always was.
                        wcsncpy_s(b.para->props.style, 64, val, _TRUNCATE);
                        int level = HeadingLevelOf(val);
                        if (level) {
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
            } else if (b.inParaProps && b.para && NameIs(local, len, L"pageBreakBefore")) {
                b.para->props.pageBreakBefore = AttrIsOn(reader);
            } else if (b.inParaProps && b.para &&
                       ReadParaProp(reader, local, len, &b.para->props)) {
                // jc, ind and spacing, read the same way styles.xml reads them.
            }

            // --- run properties ---
            else if (b.inRunProps && NameIs(local, len, L"rStyle")) {
                WCHAR val[64];
                if (GetAttr(reader, L"val", val, 64) && Doc_FindStyle(doc, val)) {
                    CharProps fromStyle;
                    Doc_ResolveStyle(doc, val, NULL, &fromStyle);
                    b.run = fromStyle;
                }
            } else if (b.inRunProps && ReadRunProp(reader, local, len, &b.run)) {
                // b, i, u, strike, sz, color, vertAlign, rFonts.
            }

            // --- footnotes and endnotes ---
            //
            // A reference is an empty run in the document pointing at a note
            // that lives in a part of its own. The mark itself -- the little
            // number -- is not in the file at all: Word works it out from the
            // order the references appear in, and so does this.
            else if (b.para && (NameIs(local, len, L"footnoteReference") ||
                                NameIs(local, len, L"endnoteReference"))) {
                BOOL endnote = NameIs(local, len, L"endnoteReference");
                int id = AttrInt(reader, L"id", -1);

                // Word's own separator notes are ids 0 and -1 and are not
                // references to anything anybody wrote.
                if (id > 0) {
                    WCHAR mark[16];
                    int number = endnote ? ++b.endnoteMarks : ++b.footnoteMarks;
                    swprintf_s(mark, 16, L"%d", number);

                    CharProps props = b.run;
                    props.superscript = TRUE;

                    DocRun* r = Doc_AddRun(b.para, mark, -1, &props);
                    if (r) {
                        r->noteId = id;
                        r->noteIsEnd = endnote;
                        r->rev = b.rev;
                    }
                }
            }

            // --- the page ---
            //
            // `w:sectPr` states the paper, the margins and the columns. A
            // document can hold several of them, one per section; the last is
            // the document's own and is the one kept -- see the roadmap for
            // why several page setups in one document is a later problem.
            else if (NameIs(local, len, L"sectPr")) {
                b.inSection = TRUE;

                // A section break in the middle of a document starts a new
                // page, which is the part of it that can be honoured now.
                if (b.para) b.para->props.pageBreakBefore = FALSE;
            } else if (b.inSection && NameIs(local, len, L"pgSz")) {
                int w = AttrInt(reader, L"w", 0);
                int h = AttrInt(reader, L"h", 0);
                if (w > 0 && h > 0) {
                    doc->section.pageWidth = w;
                    doc->section.pageHeight = h;
                }

                WCHAR orient[16];
                if (GetAttr(reader, L"orient", orient, 16) &&
                    _wcsicmp(orient, L"landscape") == 0 &&
                    doc->section.pageWidth < doc->section.pageHeight) {
                    // Word states the turned-round size as well, but not
                    // always; swapping when it did not is how a landscape
                    // document comes out portrait.
                    int swap = doc->section.pageWidth;
                    doc->section.pageWidth = doc->section.pageHeight;
                    doc->section.pageHeight = swap;
                }
            } else if (b.inSection && NameIs(local, len, L"pgMar")) {
                int top = AttrInt(reader, L"top", -1);
                int right = AttrInt(reader, L"right", -1);
                int bottom = AttrInt(reader, L"bottom", -1);
                int left = AttrInt(reader, L"left", -1);
                if (top >= 0)    doc->section.marginTop = top;
                if (right >= 0)  doc->section.marginRight = right;
                if (bottom >= 0) doc->section.marginBottom = bottom;
                if (left >= 0)   doc->section.marginLeft = left;

                int header = AttrInt(reader, L"header", -1);
                int footer = AttrInt(reader, L"footer", -1);
                if (header >= 0) doc->headerFromTop = header;
                if (footer >= 0) doc->footerFromBottom = footer;
            } else if (b.inSection && (NameIs(local, len, L"headerReference") ||
                                       NameIs(local, len, L"footerReference"))) {
                // Which header this is -- default, first page, even pages.
                // Only the default one is kept: a document with a different
                // first page is a section problem, and sections are one per
                // document here.
                WCHAR type[24];
                BOOL isDefault = !GetAttr(reader, L"type", type, 24) ||
                                 _wcsicmp(type, L"default") == 0;

                WCHAR relId[64];
                if (isDefault && GetAttr(reader, L"id", relId, 64)) {
                    BOOL header = NameIs(local, len, L"headerReference");
                    if (header) wcsncpy_s(b.headerRel, 64, relId, _TRUNCATE);
                    else        wcsncpy_s(b.footerRel, 64, relId, _TRUNCATE);
                }
            } else if (b.inSection && NameIs(local, len, L"pgMar2_unused")) {
                // (kept out of the way; the real pgMar is read above)
            } else if (b.inSection && NameIs(local, len, L"cols")) {
                int num = AttrInt(reader, L"num", 1);
                int space = AttrInt(reader, L"space", 720);
                if (num >= 1 && num <= 8) doc->section.columns = num;
                if (space >= 0) doc->section.columnSpace = space;
            }

            // --- pictures ---
            //
            // Two spellings of the same thing. DrawingML is what Word has
            // written since 2007: an extent in EMU, then a blip naming the
            // relationship the image part hangs off. VML is what it wrote
            // before that, and what it still writes for some shapes.
            else if (NameIs(local, len, L"drawing")) {
                b.inDrawing = TRUE;
                b.imageWidthEmu = 0;
                b.imageHeightEmu = 0;
            } else if (NameIs(local, len, L"pict")) {
                b.inPicture = TRUE;
                b.imageWidthEmu = 0;
                b.imageHeightEmu = 0;
            } else if (b.inDrawing && NameIs(local, len, L"extent")) {
                b.imageWidthEmu = AttrInt(reader, L"cx", 0);
                b.imageHeightEmu = AttrInt(reader, L"cy", 0);
            } else if (b.inPicture && NameIs(local, len, L"shape")) {
                WCHAR style[256];
                if (GetAttr(reader, L"style", style, 256)) {
                    ParseVmlSize(style, &b.imageWidthEmu, &b.imageHeightEmu);
                }
            } else if ((b.inDrawing || b.inPicture) && b.para &&
                       (NameIs(local, len, L"blip") || NameIs(local, len, L"imagedata"))) {
                // r:embed on a blip, r:id on VML image data. Both name a
                // relationship rather than a file.
                WCHAR relId[64];
                if (GetAttr(reader, L"embed", relId, 64) ||
                    GetAttr(reader, L"id", relId, 64)) {

                    size_t bytes = 0;
                    WCHAR contentType[64];
                    BYTE* data = ReadRelatedBytes(pkg, relId, &bytes, contentType, 64);
                    if (data) {
                        Doc_AddImageRun(b.para, data, bytes, contentType,
                                        b.imageWidthEmu, b.imageHeightEmu);
                        free(data);
                    }
                }
            }

            // --- content ---
            else if (NameIs(local, len, L"t") || NameIs(local, len, L"delText")) {
                b.inText = TRUE;
            } else if (NameIs(local, len, L"br")) {
                if (b.para) {
                    WCHAR type[16];
                    BOOL page = GetAttr(reader, L"type", type, 16) &&
                                _wcsicmp(type, L"page") == 0;

                    DocRun* r = Doc_AddRun(b.para, L"", 0, &b.run);
                    if (r) {
                        if (page) r->pageBreak = TRUE;
                        else      r->lineBreak = TRUE;
                        r->rev = b.rev;
                    }
                }
            } else if (NameIs(local, len, L"tab")) {
                if (b.para) {
                    DocRun* r = Doc_AddRun(b.para, L"", 0, &b.run);
                    if (r) {
                        r->tab = TRUE;
                        r->rev = b.rev;
                    }
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
                DocRun* r = Doc_AddRun(b.para, val, (int)vlen, &b.run);
                if (r) r->rev = b.rev;
            }

        } else if (nt == XmlNodeType_EndElement) {
            if (NameIs(local, len, L"ins") || NameIs(local, len, L"del")) {
                if (b.inRunProps || b.inParaProps) continue;
                if (b.revDepth > 0 && --b.revDepth == 0) {
                    RevisionMark none = {0};
                    b.rev = none;
                }
                continue;
            }
            if (b.skipDepth > 0) continue;

            if (NameIs(local, len, L"sectPr")) b.inSection = FALSE;
            else if (NameIs(local, len, L"drawing")) b.inDrawing = FALSE;
            else if (NameIs(local, len, L"pict")) b.inPicture = FALSE;
            else if (NameIs(local, len, L"numPr")) {
                if (b.inNumPr && b.para) {
                    if (b.numId > 0) ApplyNumbering(numbering, b.numId, b.ilvl, &b.para->props);
                    else {
                        b.para->props.list = LIST_BULLET;
                        b.para->props.numFormat = NUMFMT_BULLET;
                    }
                }
                b.inNumPr = FALSE;
            }
            else if (NameIs(local, len, L"t") ||
                     NameIs(local, len, L"delText"))     b.inText = FALSE;
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

    // The notes, which are one part holding all of them: a `w:footnote` per
    // note, each full of ordinary paragraphs.
    {
        struct { const WCHAR* rel; BOOL endnote; } noteParts[] = {
            { REL_FOOTNOTES, FALSE },
            { REL_ENDNOTES,  TRUE  },
        };

        for (int i = 0; i < 2; i++) {
            IStream* part = RelatedStream(pkg, noteParts[i].rel);
            if (!part) continue;

            ReadNotes(part, doc, noteParts[i].endnote, numbering, pkg);
            IStream_Release(part);
        }
    }

    // The header and the footer are parts of their own, holding paragraphs
    // like any others -- so they are read the same way, into a model of their
    // own, and what comes out is taken over.
    if (b.headerRel[0] || b.footerRel[0]) {
        struct { const WCHAR* rel; DocPara** into; } wanted[] = {
            { b.headerRel, &doc->header },
            { b.footerRel, &doc->footer },
        };

        for (int i = 0; i < 2; i++) {
            if (!wanted[i].rel[0]) continue;

            IStream* part = RelatedStreamById(pkg, wanted[i].rel);
            if (!part) continue;

            DocModel* piece = Doc_New();
            if (piece) {
                piece->defaultRun = doc->defaultRun;
                piece->defaultPara = doc->defaultPara;

                if (BuildModel(part, piece, numbering, pkg)) {
                    // Take the paragraphs, leave the rest.
                    DocPara* tail = NULL;
                    for (DocBlock* blk = piece->blocks; blk; blk = blk->next) {
                        if (blk->kind != BLOCK_PARA || !blk->para) continue;

                        DocPara* copied = Doc_CloneParas(blk->para);
                        if (!copied) continue;

                        if (tail) tail->next = copied;
                        else      *wanted[i].into = copied;

                        tail = copied;
                        while (tail->next) tail = tail->next;
                    }
                }
                Doc_Free(piece);
            }
            IStream_Release(part);
        }
    }

    return TRUE;
}

// footnotes.xml holds every note in one part, each wrapped in `w:footnote`.
// The paragraphs inside are ordinary ones, so the document parser reads them:
// the part is split into per-note pieces and each is handed to it.
//
// ponytail: the split is textual rather than a second parser. The alternative
// is teaching BuildModel about note boundaries, which would put a document
// structure nothing else uses into the middle of it.
static void ReadNotes(IStream* stream, DocModel* doc, BOOL endnote,
                      const NumTable* numbering, DocxPkg* pkg) {
    IXmlReader* reader = NULL;
    if (FAILED(CreateXmlReader(&IID_IXmlReader, (void**)&reader, NULL))) return;
    if (FAILED(IXmlReader_SetInput(reader, (IUnknown*)stream))) {
        IXmlReader_Release(reader);
        return;
    }
    IXmlReader_SetProperty(reader, XmlReaderProperty_MaxElementDepth, 0);

    const WCHAR* wrapper = endnote ? L"endnote" : L"footnote";

    DocNote* note = NULL;
    DocPara* para = NULL;
    CharProps run = {0};
    BOOL inRunProps = FALSE;
    BOOL inParaProps = FALSE;
    BOOL inText = FALSE;
    int  skipDepth = 0;

    XmlNodeType nt;
    while (S_OK == IXmlReader_Read(reader, &nt)) {
        const WCHAR* local = NULL;
        UINT len = 0;

        if (nt == XmlNodeType_Element || nt == XmlNodeType_EndElement) {
            if (FAILED(IXmlReader_GetLocalName(reader, &local, &len))) continue;
        }

        if (nt == XmlNodeType_Element) {
            BOOL empty = IXmlReader_IsEmptyElement(reader);

            if (NameIs(local, len, L"del")) {
                if (!empty) skipDepth++;
                continue;
            }
            if (skipDepth > 0) continue;

            if (NameIs(local, len, wrapper)) {
                // Ids 0 and -1 are the separators Word keeps in every
                // document, not notes anybody wrote.
                int id = AttrInt(reader, L"id", -1);
                note = (id > 0) ? Doc_AddNote(doc, id, endnote) : NULL;
                para = NULL;
            } else if (!note) {
                continue;
            } else if (NameIs(local, len, L"p")) {
                para = (DocPara*)calloc(1, sizeof(DocPara));
                if (para) {
                    para->props = doc->defaultPara;

                    if (!note->paras) {
                        note->paras = para;
                    } else {
                        DocPara* tail = note->paras;
                        while (tail->next) tail = tail->next;
                        tail->next = para;
                    }
                }
                run = doc->defaultRun;
            } else if (NameIs(local, len, L"pPr")) {
                inParaProps = TRUE;
            } else if (NameIs(local, len, L"rPr")) {
                inRunProps = TRUE;
            } else if (NameIs(local, len, L"r")) {
                run = doc->defaultRun;
            } else if (inParaProps && para) {
                ReadParaProp(reader, local, len, &para->props);
            } else if (inRunProps) {
                ReadRunProp(reader, local, len, &run);
            } else if (NameIs(local, len, L"t")) {
                inText = TRUE;
            } else if (NameIs(local, len, L"tab") && para) {
                DocRun* r = Doc_AddRun(para, L"", 0, &run);
                if (r) r->tab = TRUE;
            }

            (void)numbering;
            (void)pkg;

        } else if (nt == XmlNodeType_Text || nt == XmlNodeType_Whitespace) {
            if (skipDepth > 0 || !inText || !para) continue;

            const WCHAR* val = NULL;
            UINT vlen = 0;
            if (SUCCEEDED(IXmlReader_GetValue(reader, &val, &vlen)) && vlen) {
                Doc_AddRun(para, val, (int)vlen, &run);
            }

        } else if (nt == XmlNodeType_EndElement) {
            if (NameIs(local, len, L"del")) {
                if (skipDepth > 0) skipDepth--;
                continue;
            }
            if (skipDepth > 0) continue;

            if (NameIs(local, len, L"t"))            inText = FALSE;
            else if (NameIs(local, len, L"pPr"))     inParaProps = FALSE;
            else if (NameIs(local, len, L"rPr"))     inRunProps = FALSE;
            else if (NameIs(local, len, L"p"))       para = NULL;
            else if (NameIs(local, len, wrapper))    note = NULL;
        }
    }

    IXmlReader_Release(reader);
}

DocModel* Docx_ReadToModel(const WCHAR* path) {
    if (!path) return NULL;
    SetError(NULL);

    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL needUninit = SUCCEEDED(init);

    DocxPkg pkg;
    if (!OpenPackage(path, &pkg)) {
        if (needUninit) CoUninitialize();
        return NULL;
    }

    DocModel* doc = Doc_New();
    NumTable numbering = {0};
    BOOL ok = FALSE;

    if (doc) {
        // Styles and numbering first: the document part refers to both, and a
        // paragraph cannot be resolved against a style table that has not been
        // read yet.
        IStream* styles = RelatedStream(&pkg, REL_STYLES);
        if (styles) {
            ReadStyles(styles, doc);
            IStream_Release(styles);
        }

        IStream* nums = RelatedStream(&pkg, REL_NUMBERING);
        if (nums) {
            ReadNumbering(nums, &numbering);
            IStream_Release(nums);
        }

        IStream* docStream = NULL;
        if (SUCCEEDED(IOpcPart_GetContentStream(pkg.docPart, &docStream)) && docStream) {
            ok = BuildModel(docStream, doc, &numbering, &pkg);
            IStream_Release(docStream);
        } else {
            SetError(L"The main document part could not be read.");
        }
    }

    FreeNumTable(&numbering);
    ClosePackage(&pkg);
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

// The properties themselves. `styleRefs` is off when writing a style, which
// cannot refer to itself and cannot be in a list.
static void EmitParaPropsInner(StrBuf* inner, const ParaProps* p, BOOL styleRefs) {
    if (styleRefs && p->pageBreakBefore) {
        SB_Add(inner, "<w:pageBreakBefore/>");
    }

    if (styleRefs) {
        if (p->style[0]) {
            SB_Add(inner, "<w:pStyle w:val=\"");
            SB_AddXmlText(inner, p->style, -1);
            SB_Add(inner, "\"/>");
        } else if (p->headingLevel > 0) {
            SB_AddF(inner, "<w:pStyle w:val=\"Heading%d\"/>", p->headingLevel);
        }

        if (p->list != LIST_NONE) {
            SB_AddF(inner, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>",
                    p->listLevel, p->listId > 0 ? p->listId : 1);
        }
    }

    switch (p->align) {
        case ALIGN_CENTER:  SB_Add(inner, "<w:jc w:val=\"center\"/>"); break;
        case ALIGN_RIGHT:   SB_Add(inner, "<w:jc w:val=\"right\"/>");  break;
        case ALIGN_JUSTIFY: SB_Add(inner, "<w:jc w:val=\"both\"/>");   break;
        default: break;
    }

    if (p->indentLeft || p->indentFirst) {
        SB_Add(inner, "<w:ind");
        if (p->indentLeft) SB_AddF(inner, " w:left=\"%d\"", p->indentLeft);
        if (p->indentFirst < 0) SB_AddF(inner, " w:hanging=\"%d\"", -p->indentFirst);
        else if (p->indentFirst > 0) SB_AddF(inner, " w:firstLine=\"%d\"", p->indentFirst);
        SB_Add(inner, "/>");
    }

    if (p->spaceBefore || p->spaceAfter) {
        SB_Add(inner, "<w:spacing");
        if (p->spaceBefore) SB_AddF(inner, " w:before=\"%d\"", p->spaceBefore);
        if (p->spaceAfter)  SB_AddF(inner, " w:after=\"%d\"", p->spaceAfter);
        SB_Add(inner, "/>");
    }
}

static void EmitParaProps(StrBuf* x, const ParaProps* p) {
    StrBuf inner = {0};
    EmitParaPropsInner(&inner, p, TRUE);

    if (inner.len) {
        SB_Add(x, "<w:pPr>");
        SB_Add(x, inner.buf);
        SB_Add(x, "</w:pPr>");
    }
    SB_Free(&inner);
}

// Pictures are numbered as they are written, and each one becomes a part and
// a relationship. The number is the same one the drawing refers to.
typedef struct {
    const DocImage* images[64];
    int             count;
} ImagePlan;

static int PlanImage(ImagePlan* plan, const DocImage* image) {
    if (plan->count >= 64) return -1;
    plan->images[plan->count] = image;
    return plan->count++;
}

// An inline picture: the extent in EMU, and a blip naming the relationship the
// bytes hang off. The ids and names are cosmetic; the relationship is not.
static void EmitDrawing(StrBuf* x, const DocImage* image, int index) {
    SB_AddF(x,
        "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
        "<wp:extent cx=\"%d\" cy=\"%d\"/>"
        "<wp:docPr id=\"%d\" name=\"Picture %d\"/>"
        "<a:graphic xmlns:a=\"" DML_NS "\">"
        "<a:graphicData uri=\"" PIC_NS "\">"
        "<pic:pic xmlns:pic=\"" PIC_NS "\">"
        "<pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"Picture %d\"/><pic:cNvPicPr/></pic:nvPicPr>"
        "<pic:blipFill><a:blip r:embed=\"rIdImg%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
        "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%d\" cy=\"%d\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
        "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing>",
        image->widthEmu, image->heightEmu,
        index + 1, index + 1, index + 1, index + 1, index + 1,
        image->widthEmu, image->heightEmu);
}

// Track changes, on the way out. `w:ins` and `w:del` are elements around the
// runs they apply to, so consecutive runs sharing a mark share one wrapper --
// which is both what Word writes and what makes a round trip come back with
// the run boundaries it went out with.
static BOOL SameMark(const RevisionMark* a, const RevisionMark* b) {
    return a->kind == b->kind &&
           wcscmp(a->author, b->author) == 0 &&
           wcscmp(a->date, b->date) == 0;
}

static void OpenMark(StrBuf* x, const RevisionMark* m, int* idOut) {
    if (m->kind == REV_NONE) return;

    SB_AddF(x, "<w:%s w:id=\"%d\"", m->kind == REV_DELETED ? "del" : "ins", ++(*idOut));
    if (m->author[0]) {
        SB_Add(x, " w:author=\"");
        SB_AddXmlText(x, m->author, -1);
        SB_Add(x, "\"");
    }
    if (m->date[0]) {
        SB_Add(x, " w:date=\"");
        SB_AddXmlText(x, m->date, -1);
        SB_Add(x, "\"");
    }
    SB_Add(x, ">");
}

static void CloseMark(StrBuf* x, const RevisionMark* m) {
    if (m->kind == REV_NONE) return;
    SB_AddF(x, "</w:%s>", m->kind == REV_DELETED ? "del" : "ins");
}

static int g_revId = 0;    // ids only have to be unique within the document

static void EmitPara(StrBuf* x, const DocPara* para, ImagePlan* plan) {
    SB_Add(x, "<w:p>");
    EmitParaProps(x, &para->props);

    RevisionMark open = {0};

    for (const DocRun* r = para->runs; r; r = r->next) {
        if (!SameMark(&open, &r->rev)) {
            CloseMark(x, &open);
            open = r->rev;
            OpenMark(x, &open, &g_revId);
        }

        SB_Add(x, "<w:r>");
        EmitRunProps(x, &r->props);

        if (r->noteId > 0) {
            // The mark itself is not written: Word numbers the notes from the
            // order the references appear in, which is where this one's text
            // came from in the first place.
            SB_AddF(x, "<w:%sReference w:id=\"%d\"/>",
                    r->noteIsEnd ? "endnote" : "footnote", r->noteId);
        } else if (r->image) {
            int index = PlanImage(plan, r->image);
            if (index >= 0) EmitDrawing(x, r->image, index);
        } else if (r->pageBreak) {
            SB_Add(x, "<w:br w:type=\"page\"/>");
        } else if (r->tab) {
            SB_Add(x, "<w:tab/>");
        } else if (r->lineBreak) {
            SB_Add(x, "<w:br/>");
        } else {
            // Deleted text is `w:delText`; the same characters under another
            // name, which is how a reader knows not to show them.
            const char* tag = r->rev.kind == REV_DELETED ? "delText" : "t";
            SB_AddF(x, "<w:%s xml:space=\"preserve\">", tag);
            SB_AddXmlText(x, r->text, -1);
            SB_AddF(x, "</w:%s>", tag);
        }
        SB_Add(x, "</w:r>");
    }

    CloseMark(x, &open);
    SB_Add(x, "</w:p>");
}

static void EmitTable(StrBuf* x, const DocBlock* block, ImagePlan* plan) {
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
            for (const DocPara* p = c->paras; p; p = p->next) EmitPara(x, p, plan);
            SB_Add(x, "</w:tc>");
        }
        SB_Add(x, "</w:tr>");
    }

    SB_Add(x, "</w:tbl>");
}

// ---------------------------------------------------------------------------
// styles.xml and numbering.xml, written back out
//
// A document that came in with styles goes out with them: the paragraphs carry
// resolved properties for laying out, and the style table carries the names.
// A document that never had a style table still needs one for its headings,
// which is what the synthesised styles below are for.
// ---------------------------------------------------------------------------

static void EmitStyleBody(StrBuf* x, const ParaProps* pp, const CharProps* cp) {
    StrBuf inner = {0};
    EmitParaPropsInner(&inner, pp, FALSE);
    if (inner.len) {
        SB_Add(x, "<w:pPr>");
        SB_Add(x, inner.buf);
        SB_Add(x, "</w:pPr>");
    }
    SB_Free(&inner);

    EmitRunProps(x, cp);
}

// Which heading levels the document uses but the style table does not define.
static void HeadingsInUse(const DocModel* doc, BOOL used[7]) {
    for (int i = 0; i < 7; i++) used[i] = FALSE;

    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        const DocPara* paras = (b->kind == BLOCK_PARA) ? b->para : NULL;
        for (const DocPara* p = paras; p; p = p->next) {
            int level = p->props.headingLevel;
            if (level >= 1 && level <= 6) used[level] = TRUE;
        }
        if (b->kind != BLOCK_TABLE) continue;
        for (const DocRow* r = b->table.rows; r; r = r->next) {
            for (const DocCell* c = r->cells; c; c = c->next) {
                for (const DocPara* p = c->paras; p; p = p->next) {
                    int level = p->props.headingLevel;
                    if (level >= 1 && level <= 6) used[level] = TRUE;
                }
            }
        }
    }
}

static BOOL BuildStylesXml(const DocModel* doc, StrBuf* x) {
    SB_Add(x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
              "<w:styles xmlns:w=\"" WML_NS "\">");

    SB_Add(x, "<w:docDefaults><w:rPrDefault>");
    EmitRunProps(x, &doc->defaultRun);
    SB_Add(x, "</w:rPrDefault><w:pPrDefault>");
    {
        StrBuf inner = {0};
        EmitParaPropsInner(&inner, &doc->defaultPara, FALSE);
        if (inner.len) {
            SB_Add(x, "<w:pPr>");
            SB_Add(x, inner.buf);
            SB_Add(x, "</w:pPr>");
        }
        SB_Free(&inner);
    }
    SB_Add(x, "</w:pPrDefault></w:docDefaults>");

    BOOL haveNormal = Doc_FindStyle(doc, L"Normal") != NULL;
    if (!haveNormal) {
        SB_Add(x, "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
                  "<w:name w:val=\"Normal\"/></w:style>");
    }

    for (const DocStyle* st = doc->styles; st; st = st->next) {
        SB_Add(x, "<w:style w:type=\"paragraph\" w:styleId=\"");
        SB_AddXmlText(x, st->id, -1);
        SB_Add(x, "\">");

        SB_Add(x, "<w:name w:val=\"");
        SB_AddXmlText(x, st->name[0] ? st->name : st->id, -1);
        SB_Add(x, "\"/>");

        if (st->basedOn[0]) {
            SB_Add(x, "<w:basedOn w:val=\"");
            SB_AddXmlText(x, st->basedOn, -1);
            SB_Add(x, "\"/>");
        }

        EmitStyleBody(x, &st->para, &st->run);
        SB_Add(x, "</w:style>");
    }

    // Headings the document uses that the table says nothing about -- a
    // document captured from the editor, or one that never had a styles part.
    BOOL used[7];
    HeadingsInUse(doc, used);
    for (int level = 1; level <= 6; level++) {
        WCHAR id[16];
        swprintf_s(id, 16, L"Heading%d", level);
        if (!used[level] || Doc_FindStyle(doc, id)) continue;

        SB_AddF(x, "<w:style w:type=\"paragraph\" w:styleId=\"Heading%d\">"
                   "<w:name w:val=\"heading %d\"/><w:basedOn w:val=\"Normal\"/>"
                   "<w:pPr><w:outlineLvl w:val=\"%d\"/></w:pPr>"
                   "<w:rPr><w:b/><w:sz w:val=\"%d\"/></w:rPr></w:style>",
                level, level, level - 1,
                36 - (level - 1) * 4 < 22 ? 22 : 36 - (level - 1) * 4);
    }

    SB_Add(x, "</w:styles>");
    return !x->failed;
}

// Every list in the document, with what each of its levels looked like. A
// document can hold several lists that count separately, which is what the
// `w:numId` on each paragraph says.
#define MAX_LISTS 32

typedef struct {
    int          id;
    BOOL         levelUsed[9];
    DocNumFormat levelFmt[9];
    WCHAR        levelText[9][24];
} ListOut;

static void CollectList(const DocPara* p, ListOut* lists, int* count) {
    if (p->props.list == LIST_NONE) return;

    int id = p->props.listId > 0 ? p->props.listId : 1;
    int level = p->props.listLevel;
    if (level < 0 || level > 8) level = 0;

    ListOut* entry = NULL;
    for (int i = 0; i < *count; i++) {
        if (lists[i].id == id) { entry = &lists[i]; break; }
    }
    if (!entry) {
        if (*count >= MAX_LISTS) return;
        entry = &lists[(*count)++];
        memset(entry, 0, sizeof(*entry));
        entry->id = id;
    }

    entry->levelUsed[level] = TRUE;
    entry->levelFmt[level] = p->props.numFormat;
    if (p->props.listText[0]) {
        wcsncpy_s(entry->levelText[level], 24, p->props.listText, _TRUNCATE);
    }
}

static int CollectLists(const DocModel* doc, ListOut* lists) {
    int count = 0;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) CollectList(p, lists, &count);
        } else {
            for (const DocRow* r = b->table.rows; r; r = r->next) {
                for (const DocCell* c = r->cells; c; c = c->next) {
                    for (const DocPara* p = c->paras; p; p = p->next) CollectList(p, lists, &count);
                }
            }
        }
    }
    return count;
}

static const char* NumFmtName(DocNumFormat fmt) {
    switch (fmt) {
        case NUMFMT_BULLET:       return "bullet";
        case NUMFMT_LOWER_LETTER: return "lowerLetter";
        case NUMFMT_UPPER_LETTER: return "upperLetter";
        case NUMFMT_LOWER_ROMAN:  return "lowerRoman";
        case NUMFMT_UPPER_ROMAN:  return "upperRoman";
        default:                  return "decimal";
    }
}

static BOOL BuildNumberingXml(const DocModel* doc, StrBuf* x, ListOut* lists, int count) {
    SB_Add(x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
              "<w:numbering xmlns:w=\"" WML_NS "\">");

    for (int i = 0; i < count; i++) {
        SB_AddF(x, "<w:abstractNum w:abstractNumId=\"%d\">", lists[i].id);

        for (int level = 0; level < 9; level++) {
            DocNumFormat fmt = lists[i].levelUsed[level] ? lists[i].levelFmt[level]
                                                         : NUMFMT_DECIMAL;
            SB_AddF(x, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/>"
                       "<w:numFmt w:val=\"%s\"/><w:lvlText w:val=\"",
                    level, NumFmtName(fmt));

            if (fmt == NUMFMT_BULLET) {
                // U+F0B7 is the bullet Word writes, in the Symbol font.
                SB_Add(x, "\xEF\x82\xB7");
            } else if (lists[i].levelUsed[level] && lists[i].levelText[level][0]) {
                SB_AddXmlText(x, lists[i].levelText[level], -1);
            } else {
                SB_AddF(x, "%%%d.", level + 1);
            }

            SB_AddF(x, "\"/><w:lvlJc w:val=\"left\"/>"
                       "<w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr>",
                    720 + level * 360);

            if (fmt == NUMFMT_BULLET) {
                SB_Add(x, "<w:rPr><w:rFonts w:ascii=\"Symbol\" w:hAnsi=\"Symbol\"/></w:rPr>");
            }
            SB_Add(x, "</w:lvl>");
        }

        SB_Add(x, "</w:abstractNum>");
    }

    for (int i = 0; i < count; i++) {
        SB_AddF(x, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/></w:num>",
                lists[i].id, lists[i].id);
    }

    SB_Add(x, "</w:numbering>");
    return !x->failed;
}

// A header or a footer part: the same paragraphs, in a root of their own.
static BOOL BuildMarginXml(const DocPara* paras, BOOL header, StrBuf* x, ImagePlan* plan) {
    SB_AddF(x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
               "<w:%s xmlns:w=\"" WML_NS "\" xmlns:r=\"" REL_NS "\""
               " xmlns:wp=\"" WP_NS "\">",
            header ? "hdr" : "ftr");

    BOOL any = FALSE;
    for (const DocPara* p = paras; p; p = p->next) {
        EmitPara(x, p, plan);
        any = TRUE;
    }
    if (!any) SB_Add(x, "<w:p/>");

    SB_AddF(x, "</w:%s>", header ? "hdr" : "ftr");
    return !x->failed;
}

// footnotes.xml or endnotes.xml: every note of that kind in one part, each in
// its own wrapper. The separator notes Word expects at ids 0 and -1 are
// written too, because a document without them opens with a complaint.
static BOOL BuildNotesXml(const DocModel* doc, BOOL endnote, StrBuf* x, ImagePlan* plan) {
    const char* root = endnote ? "endnotes" : "footnotes";
    const char* item = endnote ? "endnote" : "footnote";

    SB_AddF(x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
               "<w:%s xmlns:w=\"" WML_NS "\" xmlns:r=\"" REL_NS "\""
               " xmlns:wp=\"" WP_NS "\">",
            root);

    SB_AddF(x, "<w:%s w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r>"
               "</w:p></w:%s>"
               "<w:%s w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r>"
               "<w:continuationSeparator/></w:r></w:p></w:%s>",
            item, item, item, item);

    for (const DocNote* n = doc->notes; n; n = n->next) {
        if (n->endnote != endnote) continue;

        SB_AddF(x, "<w:%s w:id=\"%d\">", item, n->id);
        if (!n->paras) SB_Add(x, "<w:p/>");
        for (const DocPara* p = n->paras; p; p = p->next) EmitPara(x, p, plan);
        SB_AddF(x, "</w:%s>", item);
    }

    SB_AddF(x, "</w:%s>", root);
    return !x->failed;
}

static BOOL BuildDocumentXml(const DocModel* doc, StrBuf* x, ImagePlan* plan) {
    // The drawing namespaces are declared on the root whether or not the
    // document holds a picture; a namespace nothing uses costs a line.
    SB_Add(x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
              "<w:document xmlns:w=\"" WML_NS "\""
              " xmlns:r=\"" REL_NS "\""
              " xmlns:wp=\"" WP_NS "\"><w:body>");

    BOOL any = FALSE;
    for (const DocBlock* b = doc->blocks; b; b = b->next) {
        if (b->kind == BLOCK_PARA) {
            for (const DocPara* p = b->para; p; p = p->next) {
                EmitPara(x, p, plan);
                any = TRUE;
            }
        } else {
            EmitTable(x, b, plan);
            any = TRUE;

            // Word requires a paragraph after a table; without one the table
            // and whatever follows it run together.
            if (!b->next || b->next->kind == BLOCK_TABLE) SB_Add(x, "<w:p/>");
        }
    }

    // A body with no paragraph at all is not a document Word will open.
    if (!any) SB_Add(x, "<w:p/>");

    SB_Add(x, "<w:sectPr>");

    if (doc->header) SB_Add(x, "<w:headerReference w:type=\"default\" r:id=\"rIdHdr\"/>");
    if (doc->footer) SB_Add(x, "<w:footerReference w:type=\"default\" r:id=\"rIdFtr\"/>");

    SB_AddF(x, "<w:pgSz w:w=\"%d\" w:h=\"%d\"%s/>"
               "<w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" "
               "w:header=\"%d\" w:footer=\"%d\" w:gutter=\"0\"/>",
            doc->section.pageWidth, doc->section.pageHeight,
            doc->section.pageWidth > doc->section.pageHeight
                ? " w:orient=\"landscape\"" : "",
            doc->section.marginTop, doc->section.marginRight,
            doc->section.marginBottom, doc->section.marginLeft,
            doc->headerFromTop > 0 ? doc->headerFromTop : 720,
            doc->footerFromBottom > 0 ? doc->footerFromBottom : 720);

    if (doc->section.columns > 1) {
        SB_AddF(x, "<w:cols w:num=\"%d\" w:space=\"%d\"/>",
                doc->section.columns, doc->section.columnSpace);
    }

    SB_Add(x, "</w:sectPr>");

    SB_Add(x, "</w:body></w:document>");

    if (x->failed) {
        SetError(L"Ran out of memory building the document.");
        return FALSE;
    }
    return TRUE;
}

// Add a part to the package and relate the document to it. The relationship is
// what makes a part findable: a part nothing points at is dead weight that
// Word ignores.
static BOOL AddRelatedPartWithId(IOpcFactory* factory, IOpcPartSet* parts,
                                 IOpcRelationshipSet* docRels, const WCHAR* uriText,
                                 const WCHAR* relId, const WCHAR* contentType,
                                 const WCHAR* relType, const char* bytes, size_t len) {
    IOpcPartUri* uri = NULL;
    if (FAILED(IOpcFactory_CreatePartUri(factory, uriText, &uri))) return FALSE;

    IOpcPart* part = NULL;
    IStream* content = NULL;
    IOpcRelationship* rel = NULL;
    BOOL ok = FALSE;

    if (SUCCEEDED(IOpcPartSet_CreatePart(parts, uri, contentType,
                                         OPC_COMPRESSION_NORMAL, &part)) &&
        SUCCEEDED(IOpcPart_GetContentStream(part, &content))) {

        ULONG written = 0;
        if (SUCCEEDED(content->lpVtbl->Write(content, bytes, (ULONG)len, &written)) &&
            written == (ULONG)len) {
            ok = SUCCEEDED(IOpcRelationshipSet_CreateRelationship(
                docRels, relId, relType, (IUri*)uri,
                OPC_URI_TARGET_MODE_INTERNAL, &rel));
        }
    }

    if (rel)     IOpcRelationship_Release(rel);
    if (content) IStream_Release(content);
    if (part)    IOpcPart_Release(part);
    IOpcPartUri_Release(uri);
    return ok;
}

static BOOL AddRelatedPart(IOpcFactory* factory, IOpcPartSet* parts,
                           IOpcRelationshipSet* docRels, const WCHAR* uriText,
                           const WCHAR* contentType, const WCHAR* relType,
                           const char* bytes, size_t len) {
    return AddRelatedPartWithId(factory, parts, docRels, uriText, NULL,
                                contentType, relType, bytes, len);
}

BOOL Docx_WriteModel(const DocModel* doc, const WCHAR* path) {
    if (!doc || !path) return FALSE;
    SetError(NULL);

    ImagePlan plan = {0};

    StrBuf xml = {0};
    if (!BuildDocumentXml(doc, &xml, &plan)) {
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

    // styles.xml, and numbering.xml when the document holds a list. Both hang
    // off the document part, which is what makes them its styles rather than
    // loose XML in the container.
    {
        IOpcRelationshipSet* docRels = NULL;
        if (FAILED(IOpcPart_GetRelationshipSet(part, &docRels))) {
            SetError(L"The document part could not be given relationships.");
            goto done;
        }

        StrBuf styles = {0};
        BOOL stylesOk = BuildStylesXml(doc, &styles) &&
                        AddRelatedPart(factory, parts, docRels, L"/word/styles.xml",
                                       CT_STYLES, REL_STYLES, styles.buf, styles.len);
        SB_Free(&styles);

        if (!stylesOk) {
            IOpcRelationshipSet_Release(docRels);
            SetError(L"The styles part could not be written.");
            goto done;
        }

        // A part per picture, named by the relationship the drawing refers to.
        for (int i = 0; i < plan.count; i++) {
            const DocImage* image = plan.images[i];

            const WCHAR* extension = L"png";
            if (wcsstr(image->contentType, L"jpeg") || wcsstr(image->contentType, L"jpg")) {
                extension = L"jpeg";
            } else if (wcsstr(image->contentType, L"gif")) {
                extension = L"gif";
            } else if (wcsstr(image->contentType, L"bmp")) {
                extension = L"bmp";
            } else if (wcsstr(image->contentType, L"tiff")) {
                extension = L"tiff";
            }

            WCHAR uri[128], relId[32];
            swprintf_s(uri, 128, L"/word/media/image%d.%s", i + 1, extension);
            swprintf_s(relId, 32, L"rIdImg%d", i + 1);

            if (!AddRelatedPartWithId(factory, parts, docRels, uri, relId,
                                      image->contentType, REL_IMAGE,
                                      (const char*)image->bytes, image->len)) {
                IOpcRelationshipSet_Release(docRels);
                SetError(L"A picture could not be written.");
                goto done;
            }
        }

        // The header and the footer, each a part with a relationship id the
        // section refers to by name.
        if (doc->header || doc->footer) {
            struct {
                const DocPara* paras;
                BOOL           header;
                const WCHAR*   uri;
                const WCHAR*   relId;
                const WCHAR*   type;
                const WCHAR*   relType;
            } margins[] = {
                { doc->header, TRUE,  L"/word/header1.xml", L"rIdHdr", CT_HEADER, REL_HEADER },
                { doc->footer, FALSE, L"/word/footer1.xml", L"rIdFtr", CT_FOOTER, REL_FOOTER },
            };

            for (int i = 0; i < 2; i++) {
                if (!margins[i].paras) continue;

                StrBuf part = {0};
                BOOL partOk = BuildMarginXml(margins[i].paras, margins[i].header,
                                             &part, &plan) &&
                              AddRelatedPartWithId(factory, parts, docRels,
                                                   margins[i].uri, margins[i].relId,
                                                   margins[i].type, margins[i].relType,
                                                   part.buf, part.len);
                SB_Free(&part);

                if (!partOk) {
                    IOpcRelationshipSet_Release(docRels);
                    SetError(L"The header or footer part could not be written.");
                    goto done;
                }
            }
        }

        // The notes, one part per kind, and only when the document has any.
        {
            BOOL haveFootnotes = FALSE, haveEndnotes = FALSE;
            for (const DocNote* n = doc->notes; n; n = n->next) {
                if (n->endnote) haveEndnotes = TRUE;
                else            haveFootnotes = TRUE;
            }

            struct {
                BOOL         wanted;
                BOOL         endnote;
                const WCHAR* uri;
                const WCHAR* type;
                const WCHAR* relType;
            } noteParts[] = {
                { haveFootnotes, FALSE, L"/word/footnotes.xml", CT_FOOTNOTES, REL_FOOTNOTES },
                { haveEndnotes,  TRUE,  L"/word/endnotes.xml",  CT_ENDNOTES,  REL_ENDNOTES  },
            };

            for (int i = 0; i < 2; i++) {
                if (!noteParts[i].wanted) continue;

                StrBuf part = {0};
                BOOL partOk = BuildNotesXml(doc, noteParts[i].endnote, &part, &plan) &&
                              AddRelatedPart(factory, parts, docRels, noteParts[i].uri,
                                             noteParts[i].type, noteParts[i].relType,
                                             part.buf, part.len);
                SB_Free(&part);

                if (!partOk) {
                    IOpcRelationshipSet_Release(docRels);
                    SetError(L"The notes part could not be written.");
                    goto done;
                }
            }
        }

        ListOut lists[MAX_LISTS];
        int listCount = CollectLists(doc, lists);
        if (listCount > 0) {
            StrBuf nums = {0};
            BOOL numsOk = BuildNumberingXml(doc, &nums, lists, listCount) &&
                          AddRelatedPart(factory, parts, docRels, L"/word/numbering.xml",
                                         CT_NUMBERING, REL_NUMBERING, nums.buf, nums.len);
            SB_Free(&nums);

            if (!numsOk) {
                IOpcRelationshipSet_Release(docRels);
                SetError(L"The numbering part could not be written.");
                goto done;
            }
        }

        IOpcRelationshipSet_Release(docRels);
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
    return Docx_WriteFromEditorWith(hRichEdit, path, NULL);
}

BOOL Docx_WriteFromEditorWith(HWND hRichEdit, const WCHAR* path, const DocModel* source) {
    if (!hRichEdit || !path) return FALSE;

    DocModel* doc = DocView_CaptureWith(hRichEdit, source);
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
