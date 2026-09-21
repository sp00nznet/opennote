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
#include "core/inflate.h"
#include "core/imagedib.h"

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

// An object that lives inside an object stream rather than in the file's own
// body: which stream, and where in it.
struct Compressed {
    int stream;
    int index;
};

// An object stream, decompressed once and kept: a form's fields are usually
// all in the same one, so decoding it per field would be the whole file's
// worth of work per answer.
struct DecodedStream {
    int    object;
    BYTE*  bytes;
    size_t len;
};

#define MAX_DECODED 8

// A picture waiting to be put on a page: its pixels, where it goes, and which
// page it goes on.
struct Stamp {
    int    page;             // the page's object number
    float  rect[4];          // in points, from the bottom left
    BYTE*  rgb;              // owned; three bytes a pixel, top row first
    BYTE*  alpha;            // owned; one byte a pixel, or NULL when opaque
    int    width, height;
};

#define MAX_STAMPS 16

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

    // Where the compressed objects are, when the file keeps any.
    Compressed*   compressed;
    DecodedStream decoded[MAX_DECODED];
    int           decodedCount;

    // A file whose cross-reference is a stream has to be updated with one
    // too: a reader that understands only streams would not see a classic
    // table appended after them.
    BOOL xrefIsStream;

    FormField fields[MAX_FIELDS];
    int       fieldCount;

    // The pages, in order, and anything waiting to be stamped on one.
    int   pages[1024];
    int   pageCount;
    BOOL  pagesRead;

    Stamp stamps[MAX_STAMPS];
    int   stampCount;
};

static BOOL ObjectBody(const PdfForm* form, int number, Span* out);
static BYTE* DecodeStream(PdfForm* form, Span body, size_t* lenOut);
static BOOL ReadTrailer(PdfForm* form, Span trailer, int depth);
static BOOL Resolve(const PdfForm* form, Span value, Span* out);
static BOOL DictValue(Span dict, const char* key, Span* out);
static int  AsReference(Span value);
static double AsNumber(Span value);

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

// An object at a byte offset, whether or not the table has been read yet.
static BOOL ObjectBodyAt(const PdfForm* form, size_t offset, Span* out) {
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

// An object stream, decompressed and kept. Its header is pairs of "number
// offset", and `/First` says where the objects themselves begin.
static const DecodedStream* DecodedObjectStream(PdfForm* form, int number) {
    for (int i = 0; i < form->decodedCount; i++) {
        if (form->decoded[i].object == number) return &form->decoded[i];
    }
    if (form->decodedCount >= MAX_DECODED) return NULL;

    Span body;
    if (!ObjectBodyAt(form, number < form->objectCount ? form->offsets[number] : 0, &body)) {
        return NULL;
    }

    size_t len = 0;
    BYTE* bytes = DecodeStream(form, body, &len);
    if (!bytes) return NULL;

    DecodedStream* slot = &form->decoded[form->decodedCount++];
    slot->object = number;
    slot->bytes = bytes;
    slot->len = len;
    return slot;
}

// The body of object `number`: everything between "N G obj" and "endobj", or,
// for an object kept inside an object stream, its part of that stream.
static BOOL ObjectBody(const PdfForm* form, int number, Span* out) {
    if (number <= 0 || number >= form->objectCount) return FALSE;

    if (form->compressed && form->compressed[number].stream > 0) {
        PdfForm* mutableForm = (PdfForm*)form;    // the cache is the only change

        const Compressed* where = &form->compressed[number];
        const DecodedStream* stream = DecodedObjectStream(mutableForm, where->stream);
        if (!stream) return FALSE;

        Span container;
        if (!ObjectBodyAt(form, form->offsets[where->stream], &container)) return FALSE;

        Span field;
        int count = DictValue(container, "N", &field) ? (int)AsNumber(field) : 0;
        int first = DictValue(container, "First", &field) ? (int)AsNumber(field) : 0;
        if (where->index >= count || first <= 0) return FALSE;

        // The header is `count` pairs of "number offset", and an object runs
        // to where the next one starts. Reading to the end of the stream
        // instead -- which is what this did at first -- hands back this object
        // and every one after it, and a dictionary copied from that span
        // carries its neighbours with it.
        const char* at = (const char*)stream->bytes;
        const char* end = at + ((size_t)first < stream->len ? (size_t)first : stream->len);

        size_t offsets[2] = { 0, 0 };
        BOOL haveThis = FALSE, haveNext = FALSE;

        for (int i = 0; i < count && at < end; i++) {
            size_t pair[2] = { 0, 0 };

            for (int part = 0; part < 2; part++) {
                while (at < end && IsWhite(*at)) at++;

                size_t value = 0;
                while (at < end && *at >= 48 && *at <= 57) {
                    value = value * 10 + (size_t)(*at - 48);
                    at++;
                }
                pair[part] = value;
            }

            if (i == where->index)     { offsets[0] = pair[1]; haveThis = TRUE; }
            if (i == where->index + 1) { offsets[1] = pair[1]; haveNext = TRUE; break; }
        }

        if (!haveThis) return FALSE;

        size_t begin = (size_t)first + offsets[0];
        size_t stop = haveNext ? (size_t)first + offsets[1] : stream->len;

        if (begin >= stream->len || stop > stream->len || stop <= begin) return FALSE;

        out->at = (const char*)stream->bytes + begin;
        out->len = stop - begin;
        return TRUE;
    }

    return ObjectBodyAt(form, form->offsets[number], out);
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
// Streams
//
// A stream is a dictionary, the word "stream", the bytes, and "endstream".
// The dictionary says how many bytes and what was done to them; the only
// filter here is FlateDecode, because that is the one a cross-reference or an
// object stream uses, and the only predictor is PNG's, which is what every
// writer that bothers with one picks.
// ---------------------------------------------------------------------------

// Undo a PNG predictor: each row starts with a tag saying how it was coded
// against the row above and the bytes to its left.
static BYTE* Unpredict(BYTE* data, size_t len, int colors, int bpc, int columns,
                       size_t* outLen) {
    int sample = (colors * bpc + 7) / 8;
    if (sample < 1) sample = 1;

    size_t row = (size_t)((size_t)columns * colors * bpc + 7) / 8;
    if (row == 0) return NULL;

    size_t rows = len / (row + 1);
    BYTE* out = (BYTE*)calloc(rows ? rows * row : 1, 1);
    if (!out) return NULL;

    const BYTE* at = data;
    for (size_t r = 0; r < rows; r++) {
        int tag = *at++;
        BYTE* line = out + r * row;
        const BYTE* above = r ? line - row : NULL;

        for (size_t i = 0; i < row; i++) {
            int raw = at[i];
            int left = (i >= (size_t)sample) ? line[i - sample] : 0;
            int up = above ? above[i] : 0;
            int upLeft = (above && i >= (size_t)sample) ? above[i - sample] : 0;

            int value = raw;
            switch (tag) {
                case 0: break;                                  // none
                case 1: value = raw + left; break;              // sub
                case 2: value = raw + up; break;                // up
                case 3: value = raw + ((left + up) / 2); break; // average
                case 4: {                                       // Paeth
                    int p = left + up - upLeft;
                    int pa = abs(p - left), pb = abs(p - up), pc = abs(p - upLeft);
                    int best = (pa <= pb && pa <= pc) ? left : (pb <= pc) ? up : upLeft;
                    value = raw + best;
                    break;
                }
                default: break;
            }
            line[i] = (BYTE)(value & 0xFF);
        }
        at += row;
    }

    *outLen = rows * row;
    return out;
}

// An object's stream bytes, filters undone. NULL when there is no stream, or
// when it is behind a filter this does not do.
static BYTE* DecodeStream(PdfForm* form, Span body, size_t* lenOut) {
    if (lenOut) *lenOut = 0;

    const char* keyword = FindIn(body.at, body.len, "stream");
    if (!keyword) return NULL;

    const char* at = keyword + 6;
    if (at < body.at + body.len && *at == 13) at++;    // CR
    if (at < body.at + body.len && *at == 10) at++;    // LF

    // How long it is. The length may be an object of its own, which is what a
    // writer does when it does not know the length until it has finished.
    size_t length = 0;
    Span value;
    if (DictValue(body, "Length", &value)) {
        int reference = AsReference(value);
        if (reference > 0) {
            Span target;
            if (ObjectBody(form, reference, &target)) length = (size_t)AsNumber(target);
        } else {
            length = (size_t)AsNumber(value);
        }
    }

    size_t available = (size_t)(body.at + body.len - at);
    if (length == 0 || length > available) {
        // A length that cannot be right: take everything up to "endstream".
        const char* end = FindIn(at, available, "endstream");
        if (!end) return NULL;
        length = (size_t)(end - at);
    }

    BOOL flate = FALSE;
    if (DictValue(body, "Filter", &value)) {
        flate = FindIn(value.at, value.len, "FlateDecode") != NULL;

        // A filter this does not do -- anything but Flate -- is not guessed at.
        if (!flate) return NULL;
    }

    BYTE* out = NULL;
    size_t outLen = 0;

    if (flate) {
        out = Inflate_Zlib((const BYTE*)at, length, &outLen);
        if (!out) return NULL;
    } else {
        out = (BYTE*)malloc(length ? length : 1);
        if (!out) return NULL;
        memcpy(out, at, length);
        outLen = length;
    }

    // ...and the predictor, when one was used.
    Span parms;
    if (DictValue(body, "DecodeParms", &parms) && Resolve(form, parms, &parms)) {
        Span field;
        int predictor = DictValue(parms, "Predictor", &field) ? (int)AsNumber(field) : 1;

        if (predictor >= 10) {
            int colors = DictValue(parms, "Colors", &field) ? (int)AsNumber(field) : 1;
            int bpc = DictValue(parms, "BitsPerComponent", &field) ? (int)AsNumber(field) : 8;
            int columns = DictValue(parms, "Columns", &field) ? (int)AsNumber(field) : 1;

            size_t plainLen = 0;
            BYTE* plain = Unpredict(out, outLen, colors, bpc, columns, &plainLen);
            free(out);

            if (!plain) return NULL;
            out = plain;
            outLen = plainLen;
        }
    }

    if (lenOut) *lenOut = outLen;
    return out;
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
static BOOL ReadXrefStream(PdfForm* form, size_t offset, int depth);

// The table as a stream: `/W` says how wide each of the three fields is, and
// the rows say, for each object, what kind it is and where it lives. Type 1 is
// a byte offset like the classic table's; type 2 is "inside object stream N,
// at index M", which is where a 1.5 file keeps most of its dictionaries.
static BOOL ReadXrefStream(PdfForm* form, size_t offset, int depth) {
    Span body;
    if (!ObjectBodyAt(form, offset, &body)) {
        Why(L"This PDF's cross-reference table could not be found.");
        return FALSE;
    }

    Span type;
    if (!DictValue(body, "Type", &type) || !SpanIs(type, "/XRef")) {
        Why(L"This PDF's cross-reference table is in a shape opennote does not read.");
        return FALSE;
    }

    Span field;
    int widths[3] = { 1, 1, 1 };
    if (DictValue(body, "W", &field)) {
        const char* at = field.at;
        const char* end = field.at + field.len;

        for (int i = 0; i < 3 && at < end; i++) {
            while (at < end && (IsWhite(*at) || *at == '[')) at++;

            Span number = { at, (size_t)(end - at) };
            widths[i] = (int)AsNumber(number);

            while (at < end && !IsWhite(*at) && *at != ']') at++;
        }
    }

    int size = DictValue(body, "Size", &field) ? (int)AsNumber(field) : 0;

    size_t dataLen = 0;
    BYTE* data = DecodeStream(form, body, &dataLen);
    if (!data) {
        Why(L"This PDF's cross-reference table could not be decompressed.");
        return FALSE;
    }

    if (!form->compressed) {
        form->compressed = (Compressed*)calloc(MAX_OBJECTS, sizeof(Compressed));
        if (!form->compressed) {
            free(data);
            return FALSE;
        }
    }

    form->xrefIsStream = TRUE;

    int rowLen = widths[0] + widths[1] + widths[2];
    if (rowLen <= 0) {
        free(data);
        return FALSE;
    }

    // `/Index` gives the object numbers the rows are about, in pairs; with no
    // index, they start at zero.
    int index[64];
    int indexCount = 0;

    if (DictValue(body, "Index", &field)) {
        const char* at = field.at;
        const char* end = field.at + field.len;

        while (at < end && indexCount < 64) {
            while (at < end && (IsWhite(*at) || *at == '[')) at++;
            if (at >= end || *at == ']') break;

            Span number = { at, (size_t)(end - at) };
            index[indexCount++] = (int)AsNumber(number);

            while (at < end && !IsWhite(*at) && *at != ']') at++;
        }
    }

    if (indexCount < 2) {
        index[0] = 0;
        index[1] = size;
        indexCount = 2;
    }

    size_t row = 0;
    for (int section = 0; section + 1 < indexCount; section += 2) {
        int first = index[section];
        int count = index[section + 1];

        for (int i = 0; i < count; i++, row++) {
            if ((row + 1) * (size_t)rowLen > dataLen) break;

            const BYTE* at = data + row * rowLen;

            // A missing first field means type 1, which is the default.
            long long fields[3] = { 1, 0, 0 };
            int taken = 0;

            for (int f = 0; f < 3; f++) {
                if (widths[f] == 0) continue;

                long long value = 0;
                for (int b = 0; b < widths[f]; b++) value = (value << 8) | at[taken + b];

                fields[f] = value;
                taken += widths[f];
            }

            int number = first + i;
            if (number <= 0 || number >= MAX_OBJECTS) continue;
            if (number >= form->objectCount) form->objectCount = number + 1;

            // The newest table read wins, as with the classic one.
            if (form->offsets[number] != 0 || form->compressed[number].stream > 0) continue;

            if (fields[0] == 1) {
                form->offsets[number] = (size_t)fields[1];
            } else if (fields[0] == 2) {
                form->compressed[number].stream = (int)fields[1];
                form->compressed[number].index = (int)fields[2];
            }
        }
    }

    free(data);

    // The stream's own dictionary is the trailer.
    return ReadTrailer(form, body, depth);
}



static BOOL ReadTrailerImpl(PdfForm* form, Span trailer, int depth) {
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

static BOOL ReadTrailer(PdfForm* form, Span trailer, int depth) {
    return ReadTrailerImpl(form, trailer, depth);
}

static BOOL ReadXrefAt(PdfForm* form, size_t offset, int depth) {
    if (depth > 32 || offset >= form->len) return FALSE;

    const char* at = form->bytes + offset;
    size_t left = form->len - offset;

    while (left > 0 && IsWhite(*at)) { at++; left--; }

    if (left < 4 || memcmp(at, "xref", 4) != 0) {
        // A cross-reference stream: PDF 1.5's spelling, where the table is an
        // object like any other and usually compressed.
        return ReadXrefStream(form, offset, depth);
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

    for (int i = 0; i < form->decodedCount; i++) free(form->decoded[i].bytes);
    for (int i = 0; i < form->stampCount; i++) free(form->stamps[i].rgb);

    free(form->compressed);
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
// Pages
//
// The page tree: the catalogue names a /Pages node, which has /Kids, which are
// either pages or more nodes. Flattened in order, because "page 3" has to mean
// the third one somebody sees.
// ---------------------------------------------------------------------------

static void CollectPages(PdfForm* form, int node, int depth) {
    if (depth > 32 || form->pageCount >= 1024) return;

    Span body;
    if (!ObjectBody(form, node, &body)) return;

    Span type;
    if (DictValue(body, "Type", &type) && SpanIs(type, "/Page")) {
        form->pages[form->pageCount++] = node;
        return;
    }

    Span kids;
    if (!DictValue(body, "Kids", &kids)) return;

    // Every reference in the array, in the order they are written.
    const char* at = kids.at;
    const char* end = kids.at + kids.len;

    while (at < end) {
        while (at < end && !(*at >= 48 && *at <= 57)) at++;
        if (at >= end) break;

        const char* start = at;
        int tokens = 0;

        while (at < end && tokens < 3) {
            while (at < end && IsWhite(*at)) at++;
            const char* tokenStart = at;
            while (at < end && !IsWhite(*at) && *at != ']') at++;
            if (at == tokenStart) break;
            tokens++;
        }

        if (tokens < 3) break;

        Span reference = { start, (size_t)(at - start) };
        int number = AsReference(reference);
        if (number > 0) CollectPages(form, number, depth + 1);
    }
}

static void ReadPages(PdfForm* form) {
    if (form->pagesRead) return;
    form->pagesRead = TRUE;

    Span catalog, pages;
    if (!ObjectBody(form, form->rootObject, &catalog)) return;
    if (!DictValue(catalog, "Pages", &pages)) return;

    int root = AsReference(pages);
    if (root > 0) CollectPages(form, root, 0);
}

extern "C" int PdfForm_PageCount(PdfForm* form) {
    if (!form) return 0;
    ReadPages(form);
    return form->pageCount;
}

extern "C" BOOL PdfForm_StampImage(PdfForm* form, int pageIndex, const WCHAR* imagePath,
                                   float x, float y, float width, float height) {
    if (!form || !imagePath || width <= 0.0f || height <= 0.0f) return FALSE;
    if (form->stampCount >= MAX_STAMPS) return FALSE;

    ReadPages(form);
    if (pageIndex < 0 || pageIndex >= form->pageCount) return FALSE;

    // The picture, as pixels. WIC reads whatever the file is; what a PDF wants
    // is three bytes a pixel with the top row first, which a DIB is upside
    // down from.
    HANDLE file = CreateFileW(imagePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return FALSE;
    }

    BYTE* encoded = (BYTE*)malloc((size_t)size.QuadPart);
    DWORD read = 0;
    if (!encoded || !ReadFile(file, encoded, (DWORD)size.QuadPart, &read, NULL) ||
        read != (DWORD)size.QuadPart) {
        free(encoded);
        CloseHandle(file);
        return FALSE;
    }
    CloseHandle(file);

    int imageWidth = 0, imageHeight = 0;
    BYTE* bgra = ImageDib_DecodeAlpha(encoded, (size_t)read, &imageWidth, &imageHeight);
    free(encoded);

    if (!bgra || imageWidth <= 0 || imageHeight <= 0) {
        free(bgra);
        return FALSE;
    }

    size_t pixelCount = (size_t)imageWidth * imageHeight;
    BYTE* rgb = (BYTE*)malloc(pixelCount * 3);
    BYTE* alpha = (BYTE*)malloc(pixelCount);

    if (!rgb || !alpha) {
        free(rgb);
        free(alpha);
        free(bgra);
        return FALSE;
    }

    BOOL transparent = FALSE;

    for (size_t i = 0; i < pixelCount; i++) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];

        alpha[i] = bgra[i * 4 + 3];
        if (alpha[i] != 255) transparent = TRUE;
    }
    free(bgra);

    // A picture with nothing transparent in it needs no mask, and a mask
    // costs a byte a pixel.
    if (!transparent) {
        free(alpha);
        alpha = NULL;
    }

    Stamp* stamp = &form->stamps[form->stampCount++];
    stamp->page = form->pages[pageIndex];
    stamp->rect[0] = x;
    stamp->rect[1] = y;
    stamp->rect[2] = x + width;
    stamp->rect[3] = y + height;
    stamp->rgb = rgb;
    stamp->alpha = alpha;
    stamp->width = imageWidth;
    stamp->height = imageHeight;
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

// Copy a dictionary, leaving out the keys about to be rewritten, and leave it
// open for the caller to add them and close it.
//
// Rewriting an object means keeping everything it said except the one thing
// being changed -- a page keeps its size, its contents and its resources; a
// field keeps its name, its box and its font. Re-serialising a parsed tree
// would lose whatever this does not model, so the bytes are copied and only
// the named keys are stepped over.
static void CopyDictExcept(Out* out, Span body, const char* const* skip) {
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
            if (depth == 0) break;          // the caller closes it
            OutAdd(out, at, 2);
            at += 2;
            continue;
        }

        if (depth == 1 && *at == '/') {
            BOOL skipped = FALSE;

            for (int i = 0; skip[i] && !skipped; i++) {
                size_t len = strlen(skip[i]);
                if ((size_t)(end - at) < len + 1) continue;
                if (memcmp(at + 1, skip[i], len) != 0) continue;

                char after = at[1 + len];
                if (!IsWhite(after) && !IsDelimiter(after)) continue;

                // Where the value ends is the same question DictValue answers,
                // so it is asked -- with a dictionary wrapped round the rest so
                // that the key is at the top level of something.
                char shim[4096];
                size_t take = (size_t)(end - at);
                if (take > sizeof(shim) - 8) take = sizeof(shim) - 8;

                shim[0] = '<';
                shim[1] = '<';
                memcpy(shim + 2, at, take);
                shim[2 + take] = '>';
                shim[3 + take] = '>';

                Span shimSpan = { shim, take + 4 };
                Span found;
                if (DictValue(shimSpan, skip[i], &found)) {
                    at += (size_t)(found.at - shim) + found.len - 2;
                    skipped = TRUE;
                }
            }

            if (skipped) continue;
        }

        OutAdd(out, at, 1);
        at++;
    }
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

    static const char* const replaced[] = { "V", "AP", "AS", NULL };
    CopyDictExcept(out, body, replaced);

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

    int changed = form->stampCount;
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
    Written written[MAX_FIELDS * 3 + MAX_STAMPS * 4 + 4];
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

    // The stamps: a picture, an appearance that draws it, an annotation that
    // carries the appearance, and the page told about the annotation.
    for (int i = 0; i < form->stampCount; i++) {
        Stamp* stamp = &form->stamps[i];

        // The transparency first, when there is any: a soft mask is an image
        // of its own that the picture points at.
        int mask = 0;
        if (stamp->alpha) {
            mask = nextObject++;
            written[writtenCount].number = mask;
            written[writtenCount].offset = out.len;
            writtenCount++;

            size_t alphaLen = (size_t)stamp->width * stamp->height;

            OutFormat(&out,
                "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                "/ColorSpace /DeviceGray /BitsPerComponent 8 /Length %zu >>\nstream\n",
                mask, stamp->width, stamp->height, alphaLen);
            OutAdd(&out, (const char*)stamp->alpha, alphaLen);
            OutText(&out, "\nendstream\nendobj\n");
        }

        int image = nextObject++;
        written[writtenCount].number = image;
        written[writtenCount].offset = out.len;
        writtenCount++;

        size_t rgbLen = (size_t)stamp->width * stamp->height * 3;

        OutFormat(&out,
            "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
            "/ColorSpace /DeviceRGB /BitsPerComponent 8",
            image, stamp->width, stamp->height);

        if (mask > 0) OutFormat(&out, " /SMask %d 0 R", mask);

        OutFormat(&out, " /Length %zu >>\nstream\n", rgbLen);
        OutAdd(&out, (const char*)stamp->rgb, rgbLen);
        OutText(&out, "\nendstream\nendobj\n");

        float width = stamp->rect[2] - stamp->rect[0];
        float height = stamp->rect[3] - stamp->rect[1];

        int appearance = nextObject++;
        written[writtenCount].number = appearance;
        written[writtenCount].offset = out.len;
        writtenCount++;

        char content[256];
        int contentLen = snprintf(content, sizeof(content),
            "q\n%.2f 0 0 %.2f 0 0 cm\n/Im0 Do\nQ\n", width, height);
        if (contentLen < 0) contentLen = 0;

        OutFormat(&out,
            "%d 0 obj\n<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 %.2f %.2f] "
            "/Resources << /ProcSet [/PDF /ImageC] /XObject << /Im0 %d 0 R >> >> "
            "/Length %d >>\nstream\n",
            appearance, width, height, image, contentLen);
        OutAdd(&out, content, (size_t)contentLen);
        OutText(&out, "\nendstream\nendobj\n");

        int annotation = nextObject++;
        written[writtenCount].number = annotation;
        written[writtenCount].offset = out.len;
        writtenCount++;

        OutFormat(&out,
            "%d 0 obj\n<< /Type /Annot /Subtype /Stamp /Name /Signature "
            "/Rect [%.2f %.2f %.2f %.2f] /F 4 /AP << /N %d 0 R >> >>\nendobj\n",
            annotation, stamp->rect[0], stamp->rect[1], stamp->rect[2], stamp->rect[3],
            appearance);

        // The page, with the annotation added to whatever it already had.
        Span page;
        if (!ObjectBody(form, stamp->page, &page)) continue;

        written[writtenCount].number = stamp->page;
        written[writtenCount].offset = out.len;
        writtenCount++;

        OutFormat(&out, "%d 0 obj\n", stamp->page);

        // The page as it was, with the annotation added to whatever it had.
        Span annots;
        BOOL hasAnnots = DictValue(page, "Annots", &annots);

        static const char* const pageKeys[] = { "Annots", NULL };
        CopyDictExcept(&out, page, pageKeys);

        OutText(&out, " /Annots [");
        if (hasAnnots && annots.len > 2) OutAdd(&out, annots.at + 1, annots.len - 2);
        OutFormat(&out, " %d 0 R] >>", annotation);

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

            static const char* const formKeys[] = { "NeedAppearances", NULL };
            CopyDictExcept(&out, acro, formKeys);
            OutText(&out, " /NeedAppearances true >>");

            OutText(&out, "\nendobj\n");
        }
    }

    // The new cross-reference, in whichever shape the file already uses. A
    // reader that understands only streams would not see a classic table
    // appended after one, so a 1.5 file gets a stream and a 1.4 file gets a
    // table -- and neither is told about the other.
    size_t xrefOffset = out.len;

    if (form->xrefIsStream) {
        int xrefObject = nextObject++;

        // One row per object written, plus the table itself. /W [1 4 2]: a
        // type byte, a four-byte offset, a two-byte generation.
        int rows = writtenCount + 1;
        BYTE* data = (BYTE*)malloc((size_t)rows * 7);
        if (!data) {
            free(out.bytes);
            return FALSE;
        }

        for (int i = 0; i < writtenCount; i++) {
            BYTE* row = data + (size_t)i * 7;
            size_t offset = written[i].offset;

            row[0] = 1;
            row[1] = (BYTE)((offset >> 24) & 0xFF);
            row[2] = (BYTE)((offset >> 16) & 0xFF);
            row[3] = (BYTE)((offset >> 8) & 0xFF);
            row[4] = (BYTE)(offset & 0xFF);
            row[5] = 0;
            row[6] = 0;
        }

        BYTE* last = data + (size_t)writtenCount * 7;
        last[0] = 1;
        last[1] = (BYTE)((xrefOffset >> 24) & 0xFF);
        last[2] = (BYTE)((xrefOffset >> 16) & 0xFF);
        last[3] = (BYTE)((xrefOffset >> 8) & 0xFF);
        last[4] = (BYTE)(xrefOffset & 0xFF);
        last[5] = 0;
        last[6] = 0;

        // The index names which object each row is about, in pairs. They are
        // written in the order the objects were, which is why every pair is a
        // run of one.
        OutFormat(&out, "%d 0 obj\n<< /Type /XRef /Size %d /W [1 4 2] /Index [",
                  xrefObject, nextObject);

        for (int i = 0; i < writtenCount; i++) OutFormat(&out, "%d 1 ", written[i].number);
        OutFormat(&out, "%d 1]", xrefObject);

        OutFormat(&out, " /Root %d 0 R /Prev %zu /Length %d >>\nstream\n",
                  form->rootObject, form->prevXref, rows * 7);

        OutAdd(&out, (const char*)data, (size_t)rows * 7);
        free(data);

        OutText(&out, "\nendstream\nendobj\n");
        OutFormat(&out, "startxref\n%zu\n%%%%EOF\n", xrefOffset);
    } else {
        // One subsection per object, which is legal and saves sorting them.
        OutText(&out, "xref\n");

        for (int i = 0; i < writtenCount; i++) {
            OutFormat(&out, "%d 1\n%010zu 00000 n \n", written[i].number, written[i].offset);
        }

        OutFormat(&out,
            "trailer\n<< /Size %d /Root %d 0 R /Prev %zu >>\nstartxref\n%zu\n%%%%EOF\n",
            nextObject, form->rootObject, form->prevXref, xrefOffset);
    }

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
        for (int i = 0; i < form->stampCount; i++) {
            free(form->stamps[i].rgb);
            free(form->stamps[i].alpha);
        }
        form->stampCount = 0;
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


// A PDF 1.5 form, byte for byte: its catalogue and its field live inside a
// compressed object stream, and its cross-reference is a compressed stream
// with a PNG predictor on it. That is what every writer has produced for
// twenty years, and none of it can be read without an inflate -- so this is
// the case that proves the inflate, the predictor, the object stream and the
// cross-reference stream all work together.
static const BYTE MODERN_FORM[] = {
    0x25, 0x50, 0x44, 0x46, 0x2D, 0x31, 0x2E, 0x35, 0x0A, 0x35, 0x20, 0x30,
    0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x4C, 0x65, 0x6E,
    0x67, 0x74, 0x68, 0x20, 0x34, 0x32, 0x20, 0x3E, 0x3E, 0x0A, 0x73, 0x74,
    0x72, 0x65, 0x61, 0x6D, 0x0A, 0x42, 0x54, 0x20, 0x2F, 0x48, 0x65, 0x6C,
    0x76, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66, 0x20, 0x37, 0x32, 0x20, 0x37,
    0x34, 0x30, 0x20, 0x54, 0x64, 0x20, 0x28, 0x53, 0x75, 0x72, 0x6E, 0x61,
    0x6D, 0x65, 0x3A, 0x29, 0x20, 0x54, 0x6A, 0x20, 0x45, 0x54, 0x0A, 0x65,
    0x6E, 0x64, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E, 0x64,
    0x6F, 0x62, 0x6A, 0x0A, 0x36, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A,
    0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x46, 0x6F,
    0x6E, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20,
    0x2F, 0x54, 0x79, 0x70, 0x65, 0x31, 0x20, 0x2F, 0x42, 0x61, 0x73, 0x65,
    0x46, 0x6F, 0x6E, 0x74, 0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x65, 0x74,
    0x69, 0x63, 0x61, 0x20, 0x2F, 0x45, 0x6E, 0x63, 0x6F, 0x64, 0x69, 0x6E,
    0x67, 0x20, 0x2F, 0x57, 0x69, 0x6E, 0x41, 0x6E, 0x73, 0x69, 0x45, 0x6E,
    0x63, 0x6F, 0x64, 0x69, 0x6E, 0x67, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E,
    0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x37, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A,
    0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x4F,
    0x62, 0x6A, 0x53, 0x74, 0x6D, 0x20, 0x2F, 0x4E, 0x20, 0x34, 0x20, 0x2F,
    0x46, 0x69, 0x72, 0x73, 0x74, 0x20, 0x32, 0x32, 0x20, 0x2F, 0x46, 0x69,
    0x6C, 0x74, 0x65, 0x72, 0x20, 0x2F, 0x46, 0x6C, 0x61, 0x74, 0x65, 0x44,
    0x65, 0x63, 0x6F, 0x64, 0x65, 0x20, 0x2F, 0x4C, 0x65, 0x6E, 0x67, 0x74,
    0x68, 0x20, 0x32, 0x35, 0x31, 0x20, 0x3E, 0x3E, 0x0A, 0x73, 0x74, 0x72,
    0x65, 0x61, 0x6D, 0x0A, 0x78, 0xDA, 0x7D, 0x51, 0x5D, 0x6B, 0x84, 0x30,
    0x10, 0xFC, 0x2B, 0xF3, 0xE8, 0x3D, 0xE5, 0x43, 0xAB, 0x1C, 0x1C, 0x07,
    0xF6, 0x8A, 0x14, 0x4A, 0xE1, 0xB0, 0xD2, 0x3E, 0x48, 0x1F, 0x52, 0x4D,
    0x45, 0xF0, 0x4C, 0x49, 0x62, 0xB9, 0xFE, 0xFB, 0xAE, 0xF1, 0x5A, 0x39,
    0x0A, 0x85, 0x6C, 0xC8, 0xEE, 0x4E, 0x66, 0x26, 0x1B, 0x01, 0x0E, 0x09,
    0x21, 0x05, 0x62, 0x88, 0x34, 0x46, 0x02, 0xB9, 0x95, 0xD8, 0xED, 0xC0,
    0xAA, 0xAF, 0x0F, 0x0D, 0x76, 0x50, 0x5E, 0x0D, 0xA6, 0x03, 0x3B, 0xAA,
    0x4E, 0x3B, 0x82, 0x72, 0x94, 0x60, 0x79, 0x63, 0x4D, 0x61, 0xEC, 0x29,
    0x00, 0x8B, 0x5E, 0x0F, 0xAD, 0x43, 0x9D, 0xCC, 0xBD, 0x57, 0xB0, 0xBB,
    0x1C, 0x11, 0xBB, 0xD7, 0xC3, 0x27, 0xE5, 0xD5, 0x3B, 0x6D, 0xDD, 0x86,
    0x8A, 0xE5, 0x82, 0x35, 0xA3, 0x0F, 0x87, 0xD0, 0x4F, 0x03, 0xDB, 0x7E,
    0xBF, 0xAE, 0x55, 0x78, 0xD1, 0x63, 0x0F, 0xFD, 0x4C, 0x1D, 0x5F, 0xA8,
    0x0F, 0x66, 0xA2, 0xFB, 0xE2, 0x2F, 0x72, 0xDE, 0xAD, 0xA6, 0xDE, 0xC5,
    0xE0, 0xA3, 0x6E, 0x7B, 0x75, 0x6B, 0xCE, 0xA8, 0x39, 0x15, 0x52, 0x21,
    0x91, 0x6D, 0x65, 0x20, 0x18, 0x3D, 0xC1, 0x1C, 0x6E, 0x16, 0x5C, 0xA9,
    0x9D, 0x99, 0x6C, 0x43, 0x4A, 0xFF, 0xBB, 0x63, 0xF9, 0x38, 0x1A, 0xBF,
    0x3E, 0xF2, 0xCA, 0x40, 0xE8, 0x81, 0x3D, 0x4D, 0x6F, 0x3E, 0xE4, 0x2F,
    0x7D, 0xDB, 0x69, 0x2A, 0x14, 0x15, 0x21, 0xCE, 0x14, 0x88, 0xDC, 0x64,
    0x47, 0x75, 0xD2, 0x34, 0x88, 0x67, 0x44, 0x9B, 0x59, 0xB7, 0xF1, 0xA8,
    0x33, 0xB2, 0xC5, 0x39, 0x12, 0x8A, 0x4C, 0x26, 0x57, 0xA3, 0x23, 0xC7,
    0xBF, 0xB3, 0x2B, 0xE8, 0x57, 0xD8, 0x11, 0xF1, 0x8F, 0x9F, 0x6F, 0x9D,
    0x8D, 0x72, 0x59, 0x0A, 0x65, 0x6E, 0x64, 0x73, 0x74, 0x72, 0x65, 0x61,
    0x6D, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x38, 0x20, 0x30,
    0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70,
    0x65, 0x20, 0x2F, 0x58, 0x52, 0x65, 0x66, 0x20, 0x2F, 0x53, 0x69, 0x7A,
    0x65, 0x20, 0x39, 0x20, 0x2F, 0x57, 0x20, 0x5B, 0x31, 0x20, 0x34, 0x20,
    0x32, 0x5D, 0x20, 0x2F, 0x52, 0x6F, 0x6F, 0x74, 0x20, 0x31, 0x20, 0x30,
    0x20, 0x52, 0x20, 0x2F, 0x46, 0x69, 0x6C, 0x74, 0x65, 0x72, 0x20, 0x2F,
    0x46, 0x6C, 0x61, 0x74, 0x65, 0x44, 0x65, 0x63, 0x6F, 0x64, 0x65, 0x20,
    0x2F, 0x44, 0x65, 0x63, 0x6F, 0x64, 0x65, 0x50, 0x61, 0x72, 0x6D, 0x73,
    0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x50, 0x72, 0x65, 0x64, 0x69, 0x63, 0x74,
    0x6F, 0x72, 0x20, 0x31, 0x32, 0x20, 0x2F, 0x43, 0x6F, 0x6C, 0x75, 0x6D,
    0x6E, 0x73, 0x20, 0x37, 0x20, 0x3E, 0x3E, 0x20, 0x2F, 0x4C, 0x65, 0x6E,
    0x67, 0x74, 0x68, 0x20, 0x34, 0x32, 0x20, 0x3E, 0x3E, 0x0A, 0x73, 0x74,
    0x72, 0x65, 0x61, 0x6D, 0x0A, 0x78, 0xDA, 0x63, 0x62, 0x00, 0x83, 0xFF,
    0x4C, 0x4C, 0x40, 0x92, 0x9D, 0x81, 0x91, 0x09, 0xC2, 0xC7, 0xA4, 0xFF,
    0x03, 0x49, 0x26, 0x86, 0xBF, 0x60, 0x7E, 0x34, 0x88, 0x09, 0x04, 0x89,
    0x10, 0x9A, 0x29, 0x81, 0x81, 0x01, 0x00, 0x8A, 0x86, 0x04, 0x3B, 0x0A,
    0x65, 0x6E, 0x64, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E,
    0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x73, 0x74, 0x61, 0x72, 0x74, 0x78, 0x72,
    0x65, 0x66, 0x0A, 0x35, 0x34, 0x39, 0x0A, 0x25, 0x25, 0x45, 0x4F, 0x46,
    0x0A
};

static BOOL WriteBytes(const WCHAR* path, const BYTE* bytes, size_t len) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    DWORD written = 0;
    BOOL ok = WriteFile(file, bytes, (DWORD)len, &written, NULL) && written == (DWORD)len;
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

    // --- a stamp on a page ---
    //
    // Put down, written, and the file still one Windows opens: a picture on a
    // page is the whole of what a visible signature is.
    form = PdfForm_Open(path, &why);
    if (!form) FAIL("the form could not be opened for stamping");

    if (PdfForm_PageCount(form) != 1) FAIL("the test form did not report one page");

    WCHAR picture[MAX_PATH];
    swprintf_s(picture, MAX_PATH, L"%sopennote-selftest-stamp.bmp", temp);

    {
        const int w = 2, h = 2;
        const int rowBytes = ((w * 3 + 3) & ~3);
        const int pixelBytes = rowBytes * h;

        BYTE bmp[14 + 40 + 32] = {0};
        bmp[0] = 'B';
        bmp[1] = 'M';
        *(DWORD*)(bmp + 2) = 14 + 40 + pixelBytes;
        *(DWORD*)(bmp + 10) = 14 + 40;

        BITMAPINFOHEADER* bi = (BITMAPINFOHEADER*)(bmp + 14);
        bi->biSize = sizeof(BITMAPINFOHEADER);
        bi->biWidth = w;
        bi->biHeight = h;
        bi->biPlanes = 1;
        bi->biBitCount = 24;
        bi->biCompression = BI_RGB;
        bi->biSizeImage = pixelBytes;

        for (int i = 0; i < pixelBytes; i++) bmp[14 + 40 + i] = (BYTE)(i * 9);

        if (!WriteBytes(picture, bmp, 14 + 40 + pixelBytes)) {
            FAIL("could not write a test picture to stamp");
        }
    }

    if (!PdfForm_StampImage(form, 0, picture, 72.0f, 72.0f, 144.0f, 54.0f)) {
        DeleteFileW(picture);
        FAIL("the picture would not go on the page");
    }

    if (!PdfForm_Save(form, filled)) {
        DeleteFileW(picture);
        FAIL("the stamped file could not be written");
    }

    PdfForm_Close(form);
    form = NULL;
    DeleteFileW(picture);

    rendered = Pdf_Open(filled);
    if (!rendered) FAIL("Windows would not open the stamped file");

    int stampedPages = Pdf_PageCount(rendered);
    Pdf_Close(rendered);
    if (stampedPages != 1) FAIL("the stamped file did not come back as one page");

    // --- and the same, for a file in the shape everything writes today ---
    if (!WriteBytes(path, MODERN_FORM, sizeof(MODERN_FORM))) {
        FAIL("could not write the 1.5 test form");
    }

    form = PdfForm_Open(path, &why);
    if (!form) FAIL("a PDF 1.5 form could not be opened");

    if (PdfForm_FieldCount(form) != 1) FAIL("the 1.5 form's field was not found");
    if (wcscmp(PdfForm_FieldName(form, 0), L"surname") != 0) {
        FAIL("the 1.5 form's field came back with the wrong name");
    }

    if (!PdfForm_SetFieldValue(form, 0, L"Lovelace")) FAIL("the 1.5 field would not take a value");
    if (!PdfForm_Save(form, filled)) FAIL("the filled 1.5 form could not be written");

    PdfForm_Close(form);
    form = NULL;

    form = PdfForm_Open(filled, &why);
    if (!form) FAIL("the filled 1.5 form could not be opened again");
    if (wcscmp(PdfForm_FieldValue(form, 0), L"Lovelace") != 0) {
        FAIL("the 1.5 form did not keep what was written into it");
    }

    PdfForm_Close(form);
    form = NULL;

    rendered = Pdf_Open(filled);
    if (!rendered) FAIL("Windows would not open the filled 1.5 form");
    Pdf_Close(rendered);

    DeleteFileW(path);
    DeleteFileW(filled);

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
