// Filling in a PDF form.
//
// A PDF is a heap of numbered objects with a table at the end saying where
// each one starts. To fill in a field you have to find the object that is the
// field, change what it says, and put a new table at the end pointing at the
// new version -- which is what an incremental update is, and why the original
// bytes never move.
//
// This reads the narrow part of the format that gets that done:
//
//   startxref  ->  the byte offset of the cross-reference table
//   xref       ->  object number -> byte offset, in subsections
//   trailer    ->  /Root, and /Prev when the file has been updated before
//   catalogue  ->  /AcroForm -> /Fields -> the fields themselves
//
// Nothing here decompresses anything, so a file whose cross-reference is a
// compressed stream is refused with a sentence saying so rather than read
// wrongly. Everything it does read, it reads as bytes: values are kept as the
// spans of file they came from, and an object is rewritten by editing that
// span rather than by re-serialising a parsed tree that might lose something.

#include "supernote.h"

#include "pdf/pdfform.h"
#include "pdf/pdfread.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS   256
#define MAX_OBJECTS  65536

// ---------------------------------------------------------------------------
// Bytes
// ---------------------------------------------------------------------------

struct Span {
    const char* at;
    size_t      len;
};

static BOOL SpanIs(Span s, const char* text) {
    size_t len = strlen(text);
    return s.len == len && memcmp(s.at, text, len) == 0;
}

static BOOL IsWhite(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

static BOOL IsDelimiter(char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}

// The last occurrence of `needle` in `hay`, which is how a PDF is read: the
// table that counts is the one at the end.
static const char* FindLast(const char* hay, size_t hayLen, const char* needle) {
    size_t len = strlen(needle);
    if (len > hayLen) return NULL;

    for (size_t i = hayLen - len + 1; i-- > 0; ) {
        if (memcmp(hay + i, needle, len) == 0) return hay + i;
    }
    return NULL;
}

static const char* FindIn(const char* hay, size_t hayLen, const char* needle) {
    size_t len = strlen(needle);
    if (len > hayLen) return NULL;

    for (size_t i = 0; i + len <= hayLen; i++) {
        if (memcmp(hay + i, needle, len) == 0) return hay + i;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

struct FormField {
    int   object;            // the object number that is this field
    WCHAR name[128];
    WCHAR value[1024];
    BOOL  isText;
    BOOL  changed;

    float rect[4];           // the widget's box, for the appearance stream
    int   fontObject;        // /DR font to draw with, 0 when there is none
    char  fontName[32];      // ...and what the field's /DA calls it
    float fontSize;
};

struct PdfForm {
    char*  bytes;
    size_t len;
    WCHAR  path[MAX_PATH];

    // object number -> byte offset. Zero means "not in the table", which for
    // object 0 is true anyway.
    size_t* offsets;
    int     objectCount;

    int acroFormObject;      // the /AcroForm dictionary, when it is an object
    int rootObject;
    size_t prevXref;         // what the new table has to point back to

    FormField fields[MAX_FIELDS];
    int       fieldCount;
};

static const WCHAR* g_why = NULL;

static void Why(const WCHAR* text) {
    g_why = text;
}

// ---------------------------------------------------------------------------
// Tokens, dictionaries and references
// ---------------------------------------------------------------------------

// The value of `key` in the dictionary `dict`, as the bytes it occupies.
// Only the dictionary's own keys are looked at: a `/V` inside a nested
// dictionary is not this dictionary's `/V`.
static BOOL DictValue(Span dict, const char* key, Span* out) {
    size_t keyLen = strlen(key);
    int depth = 0;

    for (size_t i = 0; i + 1 < dict.len; i++) {
        char c = dict.at[i];

        if (c == '<' && dict.at[i + 1] == '<') { depth++; i++; continue; }
        if (c == '>' && dict.at[i + 1] == '>') { depth--; i++; continue; }
        if (c == '(') {
            // A string: skip it whole, so a "/V" inside one is not a key.
            int nested = 1;
            i++;
            while (i < dict.len && nested > 0) {
                if (dict.at[i] == '\\') i++;
                else if (dict.at[i] == '(') nested++;
                else if (dict.at[i] == ')') nested--;
                i++;
            }
            i--;
            continue;
        }

        if (depth != 1 || c != '/') continue;
        if (i + keyLen >= dict.len) break;
        if (memcmp(dict.at + i + 1, key, keyLen) != 0) continue;

        // The name has to end here, or /V would match /Vx.
        char after = dict.at[i + 1 + keyLen];
        if (!IsWhite(after) && !IsDelimiter(after)) continue;

        size_t at = i + 1 + keyLen;
        while (at < dict.len && IsWhite(dict.at[at])) at++;

        size_t start = at;

        if (dict.at[at] == '(') {
            int nested = 1;
            at++;
            while (at < dict.len && nested > 0) {
                if (dict.at[at] == '\\') at++;
                else if (dict.at[at] == '(') nested++;
                else if (dict.at[at] == ')') nested--;
                at++;
            }
        } else if (dict.at[at] == '[') {
            int nested = 1;
            at++;
            while (at < dict.len && nested > 0) {
                if (dict.at[at] == '[') nested++;
                else if (dict.at[at] == ']') nested--;
                at++;
            }
        } else if (dict.at[at] == '<' && at + 1 < dict.len && dict.at[at + 1] != '<') {
            // A hex string -- `<FEFF...>` -- which is how a form holds a value
            // with anything but ASCII in it.
            at++;
            while (at < dict.len && dict.at[at] != '>') at++;
            if (at < dict.len) at++;
        } else if (dict.at[at] == '<' && at + 1 < dict.len && dict.at[at + 1] == '<') {
            int nested = 1;
            at += 2;
            while (at + 1 < dict.len && nested > 0) {
                if (dict.at[at] == '<' && dict.at[at + 1] == '<') { nested++; at += 2; continue; }
                if (dict.at[at] == '>' && dict.at[at + 1] == '>') { nested--; at += 2; continue; }
                at++;
            }
        } else {
            // A name, a number, or "N G R" -- which is three tokens.
            int tokens = 0;
            while (at < dict.len && tokens < 3) {
                size_t tokenStart = at;
                if (dict.at[at] == '/') at++;
                while (at < dict.len && !IsWhite(dict.at[at]) && !IsDelimiter(dict.at[at])) at++;
                if (at == tokenStart) break;

                tokens++;

                size_t peek = at;
                while (peek < dict.len && IsWhite(dict.at[peek])) peek++;

                // "N G R" is a reference and is one value; anything else ends
                // at the first token.
                if (tokens == 2 && peek < dict.len && dict.at[peek] == 'R') {
                    at = peek + 1;
                    break;
                }
                if (tokens >= 2) break;
                if (dict.at[tokenStart] == '/') break;
                if (peek >= dict.len || dict.at[peek] < '0' || dict.at[peek] > '9') break;
                at = peek;
            }
        }

        out->at = dict.at + start;
        out->len = at - start;
        return TRUE;
    }

    return FALSE;
}

// "12 0 R" -> 12. Zero when the value is not a reference.
static int AsReference(Span value) {
    if (value.len < 3) return 0;
    if (value.at[value.len - 1] != 'R') return 0;

    int number = 0;
    size_t i = 0;
    while (i < value.len && value.at[i] >= '0' && value.at[i] <= '9') {
        number = number * 10 + (value.at[i] - '0');
        i++;
    }
    return (i > 0) ? number : 0;
}

static double AsNumber(Span value) {
    char text[64];
    size_t len = value.len < 63 ? value.len : 63;
    memcpy(text, value.at, len);
    text[len] = '\0';
    return atof(text);
}

// The body of object `number`: everything between "N G obj" and "endobj".
static BOOL ObjectBody(const PdfForm* form, int number, Span* out) {
    if (number <= 0 || number >= form->objectCount) return FALSE;

    size_t offset = form->offsets[number];
    if (offset == 0 || offset >= form->len) return FALSE;

    const char* at = form->bytes + offset;
    size_t left = form->len - offset;

    const char* obj = FindIn(at, left < 64 ? left : 64, "obj");
    if (!obj) return FALSE;

    const char* start = obj + 3;
    size_t remaining = form->len - (start - form->bytes);

    const char* end = FindIn(start, remaining, "endobj");
    if (!end) return FALSE;

    out->at = start;
    out->len = (size_t)(end - start);
    return TRUE;
}

// A value that may be given directly or as a reference to an object.
static BOOL Resolve(const PdfForm* form, Span value, Span* out) {
    int reference = AsReference(value);
    if (reference == 0) {
        *out = value;
        return TRUE;
    }
    return ObjectBody(form, reference, out);
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// A PDF string -- `(text)` or `<hex>` -- as characters. PDFTextString may be
// UTF-16 with a byte order mark, which is how a form holds anything but ASCII.
static void ReadPdfString(Span value, WCHAR* out, size_t outChars) {
    out[0] = L'\0';
    if (value.len < 2 || outChars < 2) return;

    char* raw = (char*)malloc(value.len + 1);
    if (!raw) return;

    size_t n = 0;

    if (value.at[0] == '(') {
        for (size_t i = 1; i + 1 < value.len; i++) {
            char c = value.at[i];
            if (c == '\\' && i + 2 < value.len) {
                i++;
                char e = value.at[i];
                switch (e) {
                    case 'n': raw[n++] = '\n'; break;
                    case 'r': raw[n++] = '\r'; break;
                    case 't': raw[n++] = '\t'; break;
                    case 'b': raw[n++] = '\b'; break;
                    case 'f': raw[n++] = '\f'; break;
                    default:  raw[n++] = e; break;
                }
                continue;
            }
            raw[n++] = c;
        }
    } else if (value.at[0] == '<') {
        int high = -1;
        for (size_t i = 1; i + 1 < value.len; i++) {
            char c = value.at[i];
            int digit = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                      : -1;
            if (digit < 0) continue;

            if (high < 0) high = digit;
            else {
                raw[n++] = (char)((high << 4) | digit);
                high = -1;
            }
        }
    }

    raw[n] = '\0';

    // UTF-16 big-endian with a mark, or bytes.
    if (n >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        size_t chars = 0;
        for (size_t i = 2; i + 1 < n && chars + 1 < outChars; i += 2) {
            out[chars++] = (WCHAR)(((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1]);
        }
        out[chars] = L'\0';
    } else {
        int chars = MultiByteToWideChar(CP_ACP, 0, raw, (int)n, out, (int)outChars - 1);
        if (chars < 0) chars = 0;
        out[chars] = L'\0';
    }

    free(raw);
}

// ...and the other way, for writing one back. Anything but ASCII goes out as
// UTF-16 in hex, which is the spelling every reader takes.
static void WritePdfString(const WCHAR* text, char* out, size_t outBytes) {
    out[0] = '\0';
    if (!text) return;

    BOOL ascii = TRUE;
    for (const WCHAR* c = text; *c; c++) {
        if (*c > 126 || *c < 32) { ascii = FALSE; break; }
    }

    size_t n = 0;

    if (ascii) {
        if (n + 1 < outBytes) out[n++] = '(';
        for (const WCHAR* c = text; *c && n + 3 < outBytes; c++) {
            if (*c == '(' || *c == ')' || *c == '\\') out[n++] = '\\';
            out[n++] = (char)*c;
        }
        if (n + 1 < outBytes) out[n++] = ')';
    } else {
        static const char* hex = "0123456789ABCDEF";
        if (n + 5 < outBytes) {
            out[n++] = '<';
            out[n++] = 'F'; out[n++] = 'E';
            out[n++] = 'F'; out[n++] = 'F';
        }
        for (const WCHAR* c = text; *c && n + 5 < outBytes; c++) {
            out[n++] = hex[(*c >> 12) & 0xF];
            out[n++] = hex[(*c >> 8) & 0xF];
            out[n++] = hex[(*c >> 4) & 0xF];
            out[n++] = hex[*c & 0xF];
        }
        if (n + 1 < outBytes) out[n++] = '>';
    }

    out[n] = '\0';
}

// ---------------------------------------------------------------------------
// The cross-reference table
// ---------------------------------------------------------------------------

static BOOL ReadXrefAt(PdfForm* form, size_t offset, int depth);

static BOOL ReadTrailer(PdfForm* form, Span trailer, int depth) {
    Span value;

    if (form->rootObject == 0 && DictValue(trailer, "Root", &value)) {
        form->rootObject = AsReference(value);
    }

    // An updated file has its older tables chained behind it.
    if (DictValue(trailer, "Prev", &value)) {
        size_t previous = (size_t)AsNumber(value);
        if (previous > 0 && previous < form->len) ReadXrefAt(form, previous, depth + 1);
    }

    return TRUE;
}

static BOOL ReadXrefAt(PdfForm* form, size_t offset, int depth) {
    if (depth > 32 || offset >= form->len) return FALSE;

    const char* at = form->bytes + offset;
    size_t left = form->len - offset;

    while (left > 0 && IsWhite(*at)) { at++; left--; }

    if (left < 4 || memcmp(at, "xref", 4) != 0) {
        // A cross-reference stream: PDF 1.5's compressed spelling, which needs
        // inflate to read. Refused rather than half-read.
        Why(L"This PDF keeps its cross-reference table as a compressed stream, "
            L"which opennote cannot read yet.");
        return FALSE;
    }

    at += 4;
    left -= 4;

    // Subsections: "first count", then one 20-byte entry each.
    for (;;) {
        while (left > 0 && IsWhite(*at)) { at++; left--; }
        if (left == 0) return FALSE;

        if (left >= 7 && memcmp(at, "trailer", 7) == 0) {
            at += 7;
            left -= 7;

            while (left > 1 && !(at[0] == '<' && at[1] == '<')) { at++; left--; }
            if (left < 2) return FALSE;

            Span trailer = { at, left };
            return ReadTrailer(form, trailer, depth);
        }

        if (*at < '0' || *at > '9') return FALSE;

        int first = 0, count = 0;
        while (left > 0 && *at >= '0' && *at <= '9') { first = first * 10 + (*at - '0'); at++; left--; }
        while (left > 0 && IsWhite(*at)) { at++; left--; }
        while (left > 0 && *at >= '0' && *at <= '9') { count = count * 10 + (*at - '0'); at++; left--; }

        for (int i = 0; i < count; i++) {
            while (left > 0 && IsWhite(*at)) { at++; left--; }
            if (left < 18) return FALSE;

            size_t entryOffset = 0;
            for (int d = 0; d < 10; d++) entryOffset = entryOffset * 10 + (at[d] - '0');

            char kind = at[17];

            int number = first + i;
            // An older table must not overwrite a newer one: the first table
            // read is the newest, and it wins.
            if (kind == 'n' && number > 0 && number < MAX_OBJECTS) {
                if (number >= form->objectCount) form->objectCount = number + 1;
                if (form->offsets[number] == 0) form->offsets[number] = entryOffset;
            }

            at += 18;
            left -= 18;
        }
    }
}

// ---------------------------------------------------------------------------
// The fields
// ---------------------------------------------------------------------------

static void ReadFieldFont(PdfForm* form, Span field, FormField* out) {
    strcpy_s(out->fontName, 32, "Helv");
    out->fontSize = 0.0f;

    // "/Helv 12 Tf 0 g" -- the name and the size the field draws with. A size
    // of zero means "fit the box", which is answered when it is drawn.
    Span da;
    if (DictValue(field, "DA", &da)) {
        WCHAR text[128];
        ReadPdfString(da, text, 128);

        char plain[128];
        WideCharToMultiByte(CP_ACP, 0, text, -1, plain, 128, NULL, NULL);

        const char* slash = strchr(plain, '/');
        if (slash) {
            size_t n = 0;
            slash++;
            while (slash[n] && !IsWhite(slash[n]) && !IsDelimiter(slash[n]) && n < 31) n++;
            memcpy(out->fontName, slash, n);
            out->fontName[n] = '\0';

            out->fontSize = (float)atof(slash + n);
        }
    }

    // Which object that name points at, in the form's resources.
    if (form->acroFormObject > 0) {
        Span acro, dr, font, entry;
        if (ObjectBody(form, form->acroFormObject, &acro) &&
            DictValue(acro, "DR", &dr) && Resolve(form, dr, &dr) &&
            DictValue(dr, "Font", &font) && Resolve(form, font, &font) &&
            DictValue(font, out->fontName, &entry)) {
            out->fontObject = AsReference(entry);
        }
    }
}

static void ReadField(PdfForm* form, int object, const WCHAR* inheritedName);

static void ReadFieldKids(PdfForm* form, Span kids, const WCHAR* name) {
    // "[ 12 0 R 13 0 R ]" -- every reference in the array.
    for (size_t i = 0; i + 1 < kids.len; i++) {
        if (kids.at[i] < '0' || kids.at[i] > '9') continue;
        if (i > 0 && !IsWhite(kids.at[i - 1]) && kids.at[i - 1] != '[') continue;

        Span rest = { kids.at + i, kids.len - i };
        int number = AsReference(rest);

        // AsReference wants the span to end at the R, so find it.
        size_t j = i;
        int tokens = 0;
        while (j < kids.len && tokens < 3) {
            while (j < kids.len && IsWhite(kids.at[j])) j++;
            size_t start = j;
            while (j < kids.len && !IsWhite(kids.at[j]) && kids.at[j] != ']') j++;
            if (j == start) break;
            tokens++;
            if (tokens == 3) {
                Span reference = { kids.at + i, j - i };
                number = AsReference(reference);
                break;
            }
        }

        if (number > 0) ReadField(form, number, name);
        i = j;
    }
}

static void ReadField(PdfForm* form, int object, const WCHAR* inheritedName) {
    if (form->fieldCount >= MAX_FIELDS) return;

    Span field;
    if (!ObjectBody(form, object, &field)) return;

    WCHAR name[128];
    wcscpy_s(name, 128, inheritedName ? inheritedName : L"");

    Span title;
    if (DictValue(field, "T", &title)) {
        WCHAR own[128];
        ReadPdfString(title, own, 128);

        if (name[0] && own[0]) {
            wcscat_s(name, 128, L".");
            wcscat_s(name, 128, own);
        } else if (own[0]) {
            wcscpy_s(name, 128, own);
        }
    }

    // A field with children is a group: the leaves are the fields.
    Span kids;
    if (DictValue(field, "Kids", &kids)) {
        ReadFieldKids(form, kids, name);
        return;
    }

    Span type;
    BOOL isText = DictValue(field, "FT", &type) && SpanIs(type, "/Tx");

    FormField* out = &form->fields[form->fieldCount++];
    memset(out, 0, sizeof(*out));

    out->object = object;
    out->isText = isText;
    wcscpy_s(out->name, 128, name);

    Span value;
    if (DictValue(field, "V", &value)) {
        Span resolved;
        if (Resolve(form, value, &resolved)) ReadPdfString(resolved, out->value, 1024);
    }

    Span rect;
    if (DictValue(field, "Rect", &rect)) {
        const char* at = rect.at;
        const char* end = rect.at + rect.len;
        int n = 0;

        while (at < end && n < 4) {
            while (at < end && (IsWhite(*at) || *at == '[')) at++;
            if (at >= end) break;

            Span number = { at, (size_t)(end - at) };
            out->rect[n++] = (float)AsNumber(number);

            while (at < end && !IsWhite(*at) && *at != ']') at++;
        }
    }

    if (isText) ReadFieldFont(form, field, out);
}

static BOOL ReadFields(PdfForm* form) {
    if (form->rootObject <= 0) {
        Why(L"This PDF does not say where its catalogue is.");
        return FALSE;
    }

    Span catalog;
    if (!ObjectBody(form, form->rootObject, &catalog)) {
        Why(L"This PDF's catalogue could not be found.");
        return FALSE;
    }

    Span acroForm;
    if (!DictValue(catalog, "AcroForm", &acroForm)) {
        Why(L"This PDF has no form in it.");
        return FALSE;
    }

    form->acroFormObject = AsReference(acroForm);

    Span acro;
    if (!Resolve(form, acroForm, &acro)) {
        Why(L"This PDF's form could not be read.");
        return FALSE;
    }

    Span fields;
    if (!DictValue(acro, "Fields", &fields)) {
        Why(L"This PDF's form has no fields.");
        return FALSE;
    }

    ReadFieldKids(form, fields, NULL);

    if (form->fieldCount == 0) {
        Why(L"This PDF's form has no fields in it.");
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

extern "C" PdfForm* PdfForm_Open(const WCHAR* path, const WCHAR** whyOut) {
    Why(L"This file could not be read.");
    if (whyOut) *whyOut = NULL;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (whyOut) *whyOut = g_why;
        return NULL;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 32 ||
        size.QuadPart > 256 * 1024 * 1024) {
        CloseHandle(file);
        if (whyOut) *whyOut = g_why;
        return NULL;
    }

    PdfForm* form = (PdfForm*)calloc(1, sizeof(PdfForm));
    if (!form) {
        CloseHandle(file);
        return NULL;
    }

    form->len = (size_t)size.QuadPart;
    form->bytes = (char*)malloc(form->len);
    form->offsets = (size_t*)calloc(MAX_OBJECTS, sizeof(size_t));

    DWORD read = 0;
    if (!form->bytes || !form->offsets ||
        !ReadFile(file, form->bytes, (DWORD)form->len, &read, NULL) ||
        read != form->len) {
        CloseHandle(file);
        PdfForm_Close(form);
        if (whyOut) *whyOut = g_why;
        return NULL;
    }
    CloseHandle(file);

    wcsncpy_s(form->path, MAX_PATH, path, _TRUNCATE);

    if (memcmp(form->bytes, "%PDF-", 5) != 0) {
        Why(L"This file is not a PDF.");
        PdfForm_Close(form);
        if (whyOut) *whyOut = g_why;
        return NULL;
    }

    const char* startxref = FindLast(form->bytes, form->len, "startxref");
    if (!startxref) {
        Why(L"This PDF has no cross-reference table.");
        PdfForm_Close(form);
        if (whyOut) *whyOut = g_why;
        return NULL;
    }

    const char* at = startxref + 9;
    while (at < form->bytes + form->len && IsWhite(*at)) at++;

    size_t xref = 0;
    while (at < form->bytes + form->len && *at >= '0' && *at <= '9') {
        xref = xref * 10 + (size_t)(*at - '0');
        at++;
    }

    form->prevXref = xref;

    if (!ReadXrefAt(form, xref, 0) || !ReadFields(form)) {
        PdfForm_Close(form);
        if (whyOut) *whyOut = g_why;
        return NULL;
    }

    return form;
}

extern "C" void PdfForm_Close(PdfForm* form) {
    if (!form) return;
    free(form->bytes);
    free(form->offsets);
    free(form);
}

extern "C" int PdfForm_FieldCount(const PdfForm* form) {
    return form ? form->fieldCount : 0;
}

extern "C" const WCHAR* PdfForm_FieldName(const PdfForm* form, int index) {
    if (!form || index < 0 || index >= form->fieldCount) return L"";
    return form->fields[index].name;
}

extern "C" const WCHAR* PdfForm_FieldValue(const PdfForm* form, int index) {
    if (!form || index < 0 || index >= form->fieldCount) return L"";
    return form->fields[index].value;
}

extern "C" BOOL PdfForm_FieldIsText(const PdfForm* form, int index) {
    if (!form || index < 0 || index >= form->fieldCount) return FALSE;
    return form->fields[index].isText;
}

extern "C" BOOL PdfForm_SetFieldValue(PdfForm* form, int index, const WCHAR* text) {
    if (!form || index < 0 || index >= form->fieldCount) return FALSE;
    if (!form->fields[index].isText) return FALSE;

    wcsncpy_s(form->fields[index].value, 1024, text ? text : L"", _TRUNCATE);
    form->fields[index].changed = TRUE;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Writing the update
// ---------------------------------------------------------------------------

struct Out {
    char*  bytes;
    size_t len, cap;
    BOOL   failed;
};

static void OutAdd(Out* out, const char* data, size_t len) {
    if (out->failed) return;

    while (out->len + len + 1 > out->cap) {
        size_t cap = out->cap ? out->cap * 2 : 8192;
        char* grown = (char*)realloc(out->bytes, cap);
        if (!grown) { out->failed = TRUE; return; }
        out->bytes = grown;
        out->cap = cap;
    }

    memcpy(out->bytes + out->len, data, len);
    out->len += len;
    out->bytes[out->len] = '\0';
}

static void OutText(Out* out, const char* text) {
    OutAdd(out, text, strlen(text));
}

static void OutFormat(Out* out, const char* format, ...) {
    char buffer[4096];

    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len > 0) OutAdd(out, buffer, (size_t)len);
}

// The field's dictionary with its /V replaced, and an /AP pointing at the
// appearance this writes for it. Everything else in the dictionary is carried
// through as the bytes it already was.
static void WriteFieldObject(Out* out, const PdfForm* form, const FormField* field,
                             int appearanceObject) {
    Span body;
    if (!ObjectBody(form, field->object, &body)) return;

    char value[4096];
    WritePdfString(field->value, value, sizeof(value));

    // Copy the dictionary, dropping the keys being replaced.
    const char* at = body.at;
    const char* end = body.at + body.len;

    while (at < end && IsWhite(*at)) at++;
    if (at + 1 >= end || at[0] != '<' || at[1] != '<') return;

    OutText(out, "<<");
    at += 2;

    int depth = 1;
    while (at < end && depth > 0) {
        if (at + 1 < end && at[0] == '<' && at[1] == '<') { depth++; OutAdd(out, at, 2); at += 2; continue; }
        if (at + 1 < end && at[0] == '>' && at[1] == '>') {
            depth--;
            if (depth == 0) break;
            OutAdd(out, at, 2);
            at += 2;
            continue;
        }

        // At the dictionary's own level, the keys being rewritten are skipped
        // along with their values.
        if (depth == 1 && *at == '/') {
            Span rest = { at, (size_t)(end - at) };
            BOOL skip = FALSE;

            static const char* replaced[] = { "V", "AP", "AS", NULL };
            for (int i = 0; replaced[i] && !skip; i++) {
                size_t len = strlen(replaced[i]);
                if (rest.len < len + 1) continue;
                if (memcmp(at + 1, replaced[i], len) != 0) continue;

                char after = at[1 + len];
                if (!IsWhite(after) && !IsDelimiter(after)) continue;

                // Find where this key's value ends by asking the same code
                // that reads one.
                Span wrapper = { at - 2, (size_t)(end - at) + 2 };
                // ...which needs a dictionary around it: the two characters
                // before are "<<" only for the first key, so a small shim is
                // used instead.
                char shim[4096];
                size_t take = rest.len < sizeof(shim) - 8 ? rest.len : sizeof(shim) - 8;
                shim[0] = '<'; shim[1] = '<';
                memcpy(shim + 2, at, take);
                shim[2 + take] = '>';
                shim[3 + take] = '>';

                Span shimSpan = { shim, take + 4 };
                Span found;
                if (DictValue(shimSpan, replaced[i], &found)) {
                    size_t consumed = (size_t)(found.at - shim) + found.len - 2;
                    at += consumed;
                    skip = TRUE;
                }
                (void)wrapper;
            }

            if (skip) continue;
        }

        OutAdd(out, at, 1);
        at++;
    }

    OutFormat(out, " /V %s", value);
    if (appearanceObject > 0) OutFormat(out, " /AP << /N %d 0 R >>", appearanceObject);
    OutText(out, " >>");
}

// A string for the *appearance* stream, which is a different question from a
// string for the field's value. The value is data and goes out as UTF-16 when
// it has to; this is text being drawn with a font, so it has to be in that
// font's encoding -- WinAnsi, for the Helvetica this writes. A character the
// encoding has no room for is drawn as a question mark, which is better than
// the two glyphs per character that writing UTF-16 here produced.
static void WriteDrawnString(const WCHAR* text, char* out, size_t outBytes) {
    out[0] = 0;
    if (!text) return;

    char ansi[2048];
    int len = WideCharToMultiByte(1252, 0, text, -1, ansi, sizeof(ansi) - 1, "?", NULL);
    if (len <= 0) {
        strcpy_s(out, outBytes, "()");
        return;
    }
    ansi[len - 1] = 0;      // the count includes the terminator

    size_t n = 0;
    if (n + 1 < outBytes) out[n++] = '(';

    for (const char* c = ansi; *c && n + 3 < outBytes; c++) {
        // The three characters a literal string has to escape, by code so
        // that nothing between here and the compiler can mangle them.
        if (*c == 40 || *c == 41 || *c == 92) out[n++] = 92;
        out[n++] = *c;
    }

    if (n + 1 < outBytes) out[n++] = ')';
    out[n] = 0;
}

// What the filled field looks like. A form XObject with the text in it, which
// is what a reader draws when it does not regenerate appearances itself.
static void WriteAppearance(Out* out, const FormField* field, int fontObject) {
    float width = field->rect[2] - field->rect[0];
    float height = field->rect[3] - field->rect[1];
    if (width < 0) width = -width;
    if (height < 0) height = -height;
    if (width < 1.0f) width = 200.0f;
    if (height < 1.0f) height = 20.0f;

    float size = field->fontSize;
    if (size <= 0.0f) size = height * 0.62f;      // "auto" means fit the box
    if (size > 24.0f) size = 24.0f;
    if (size < 4.0f) size = 4.0f;

    char value[4096];
    WriteDrawnString(field->value, value, sizeof(value));

    char content[4200];
    int contentLen = snprintf(content, sizeof(content),
        "/Tx BMC\nq\nBT\n/%s %.2f Tf\n0 g\n2 %.2f Td\n%s Tj\nET\nQ\nEMC\n",
        field->fontName[0] ? field->fontName : "Helv", size,
        (height - size) / 2.0f + size * 0.2f, value);
    if (contentLen < 0) contentLen = 0;

    OutFormat(out,
        "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 %.2f %.2f] "
        "/Resources << /ProcSet [/PDF /Text] /Font << /%s %d 0 R >> >> /Length %d >>\n"
        "stream\n",
        width, height,
        field->fontName[0] ? field->fontName : "Helv", fontObject, contentLen);

    OutAdd(out, content, (size_t)contentLen);
    OutText(out, "endstream");
}

extern "C" BOOL PdfForm_Save(PdfForm* form, const WCHAR* path) {
    if (!form || !path || !path[0]) return FALSE;

    int changed = 0;
    for (int i = 0; i < form->fieldCount; i++) {
        if (form->fields[i].changed) changed++;
    }
    if (changed == 0) return TRUE;      // nothing to append

    Out out = {};

    // Everything the file already was, byte for byte.
    OutAdd(&out, form->bytes, form->len);
    if (out.failed) { free(out.bytes); return FALSE; }

    if (out.len > 0 && out.bytes[out.len - 1] != '\n') OutText(&out, "\n");

    // New object numbers start after the highest the file used.
    int nextObject = form->objectCount > 0 ? form->objectCount : 1;

    struct Written { int number; size_t offset; };
    Written written[MAX_FIELDS * 3 + 4];
    int writtenCount = 0;

    // A font to draw the filled text with, when the form does not name one
    // this file already has.
    int helvetica = 0;
    for (int i = 0; i < form->fieldCount; i++) {
        if (form->fields[i].changed && form->fields[i].fontObject == 0) {
            helvetica = nextObject++;
            break;
        }
    }

    if (helvetica > 0) {
        written[writtenCount].number = helvetica;
        written[writtenCount].offset = out.len;
        writtenCount++;

        OutFormat(&out, "%d 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                        "/Encoding /WinAnsiEncoding >>\nendobj\n", helvetica);
    }

    for (int i = 0; i < form->fieldCount; i++) {
        FormField* field = &form->fields[i];
        if (!field->changed) continue;

        int fontObject = field->fontObject > 0 ? field->fontObject : helvetica;

        int appearance = nextObject++;
        written[writtenCount].number = appearance;
        written[writtenCount].offset = out.len;
        writtenCount++;

        OutFormat(&out, "%d 0 obj\n", appearance);
        WriteAppearance(&out, field, fontObject);
        OutText(&out, "\nendobj\n");

        written[writtenCount].number = field->object;
        written[writtenCount].offset = out.len;
        writtenCount++;

        OutFormat(&out, "%d 0 obj\n", field->object);
        WriteFieldObject(&out, form, field, appearance);
        OutText(&out, "\nendobj\n");
    }

    // The form itself, told to regenerate appearances -- belt as well as the
    // braces of writing them.
    if (form->acroFormObject > 0) {
        Span acro;
        if (ObjectBody(form, form->acroFormObject, &acro)) {
            written[writtenCount].number = form->acroFormObject;
            written[writtenCount].offset = out.len;
            writtenCount++;

            OutFormat(&out, "%d 0 obj\n", form->acroFormObject);

            // The dictionary as it was, with /NeedAppearances added or made
            // true. Dropping the old key and adding it back is the same trick
            // the field uses.
            const char* at = acro.at;
            const char* end = acro.at + acro.len;
            while (at < end && IsWhite(*at)) at++;

            if (at + 1 < end && at[0] == '<' && at[1] == '<') {
                Span inner = { at, (size_t)(end - at) };
                Span need;

                OutText(&out, "<< /NeedAppearances true");

                if (DictValue(inner, "NeedAppearances", &need)) {
                    // Copy everything except that key and its value.
                    size_t before = (size_t)(need.at - at) - 17;   // "/NeedAppearances "
                    OutAdd(&out, at + 2, before - 2);
                    OutAdd(&out, need.at + need.len, (size_t)(end - (need.at + need.len)));
                } else {
                    OutAdd(&out, at + 2, (size_t)(end - at) - 2);
                }
            }

            OutText(&out, "\nendobj\n");
        }
    }

    // The new cross-reference table: one subsection per object, which is
    // legal and saves sorting them.
    size_t xrefOffset = out.len;
    OutText(&out, "xref\n");

    for (int i = 0; i < writtenCount; i++) {
        OutFormat(&out, "%d 1\n%010zu 00000 n \n", written[i].number, written[i].offset);
    }

    OutFormat(&out,
        "trailer\n<< /Size %d /Root %d 0 R /Prev %zu >>\nstartxref\n%zu\n%%%%EOF\n",
        nextObject, form->rootObject, form->prevXref, xrefOffset);

    if (out.failed) {
        free(out.bytes);
        return FALSE;
    }

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        free(out.bytes);
        return FALSE;
    }

    DWORD wrote = 0;
    BOOL ok = WriteFile(file, out.bytes, (DWORD)out.len, &wrote, NULL) && wrote == out.len;
    CloseHandle(file);
    free(out.bytes);

    if (ok) {
        for (int i = 0; i < form->fieldCount; i++) form->fields[i].changed = FALSE;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Self-check
//
// A form written by hand -- catalogue, page, one text field, and the
// cross-reference table that says where each object starts -- then filled,
// saved, and opened again. The point being proved is the round trip: what
// goes in comes back, and the original bytes are still there in front of it.
// ---------------------------------------------------------------------------

static BOOL WriteFormPdf(const WCHAR* path) {
    // Built in two passes: the objects, then the table of where they landed.
    const char* objects[] = {
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] "
            "/DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv 6 0 R >> >> >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 5 0 R "
            "/Resources << /Font << /Helv 6 0 R >> >> /Annots [4 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (full name) /V () "
            "/Rect [72 700 400 724] /DA (/Helv 12 Tf 0 g) /F 4 /P 3 0 R >>",
        NULL,   // the page's content stream
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
            "/Encoding /WinAnsiEncoding >>",
    };

    const char* content = "BT /Helv 12 Tf 72 740 Td (Name:) Tj ET\n";

    char body[8192];
    int offsets[8] = {0};
    int len = 0;

    len += sprintf_s(body + len, sizeof(body) - len, "%%PDF-1.4\n");

    int count = (int)(sizeof(objects) / sizeof(objects[0]));
    for (int i = 0; i < count; i++) {
        offsets[i + 1] = len;

        if (objects[i]) {
            len += sprintf_s(body + len, sizeof(body) - len, "%d 0 obj\n%s\nendobj\n",
                             i + 1, objects[i]);
        } else {
            len += sprintf_s(body + len, sizeof(body) - len,
                             "%d 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n",
                             i + 1, (int)strlen(content), content);
        }
    }

    int xref = len;
    len += sprintf_s(body + len, sizeof(body) - len, "xref\n0 %d\n0000000000 65535 f \n",
                     count + 1);
    for (int i = 1; i <= count; i++) {
        len += sprintf_s(body + len, sizeof(body) - len, "%010d 00000 n \n", offsets[i]);
    }
    len += sprintf_s(body + len, sizeof(body) - len,
                     "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n",
                     count + 1, xref);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    DWORD written = 0;
    BOOL ok = WriteFile(file, body, (DWORD)len, &written, NULL) && written == (DWORD)len;
    CloseHandle(file);
    return ok;
}

extern "C" BOOL PdfForm_SelfTest(char* failure, size_t failureSize) {
    WCHAR temp[MAX_PATH];
    WCHAR path[MAX_PATH];
    WCHAR filled[MAX_PATH];

    GetTempPathW(MAX_PATH, temp);
    swprintf_s(path, MAX_PATH, L"%sopennote-selftest-form.pdf", temp);
    swprintf_s(filled, MAX_PATH, L"%sopennote-selftest-filled.pdf", temp);

    PdfForm* form = NULL;

    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        if (form) PdfForm_Close(form); \
        DeleteFileW(path); \
        DeleteFileW(filled); \
        return FALSE; \
    } while (0)

    if (!WriteFormPdf(path)) FAIL("could not write a test form");

    const WCHAR* why = NULL;
    form = PdfForm_Open(path, &why);
    if (!form) FAIL("a form this program wrote could not be opened");

    if (PdfForm_FieldCount(form) != 1) FAIL("the form's one field was not found");
    if (wcscmp(PdfForm_FieldName(form, 0), L"full name") != 0) {
        FAIL("the field's name did not come back");
    }
    if (!PdfForm_FieldIsText(form, 0)) FAIL("a text field was not recognised as one");
    if (PdfForm_FieldValue(form, 0)[0] != L'\0') FAIL("an empty field came back full");

    // Something with a bracket and a non-ASCII character in it, because both
    // are what a name actually contains and both have to survive the writing.
    const WCHAR* typed = L"Ada Lovelace (née Byron)";
    if (!PdfForm_SetFieldValue(form, 0, typed)) FAIL("the field would not take a value");
    if (!PdfForm_Save(form, filled)) FAIL("the filled form could not be written");

    PdfForm_Close(form);
    form = NULL;

    // The original is still in front of the update, untouched: that is what
    // makes this an incremental save rather than a rewrite.
    HANDLE check = CreateFileW(filled, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (check == INVALID_HANDLE_VALUE) FAIL("the filled form was not written");

    char head[16] = {0};
    DWORD read = 0;
    ReadFile(check, head, 8, &read, NULL);

    LARGE_INTEGER size = {};
    GetFileSizeEx(check, &size);
    CloseHandle(check);

    if (memcmp(head, "%PDF-1.4", 8) != 0) FAIL("the filled form lost its header");

    form = PdfForm_Open(filled, &why);
    if (!form) FAIL("the filled form could not be opened again");

    if (PdfForm_FieldCount(form) != 1) FAIL("the filled form lost its field");
    if (wcscmp(PdfForm_FieldValue(form, 0), typed) != 0) {
        FAIL("the value written is not the value that came back");
    }

    PdfForm_Close(form);
    form = NULL;

    // ...and it is still a PDF as far as Windows is concerned, which is the
    // only opinion that matters for showing it.
    PdfFile* rendered = Pdf_Open(filled);
    if (!rendered) FAIL("Windows would not open the filled form");

    int pages = Pdf_PageCount(rendered);
    Pdf_Close(rendered);
    if (pages != 1) FAIL("the filled form did not come back as one page");

    DeleteFileW(path);
    DeleteFileW(filled);

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
