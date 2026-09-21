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
#include "pdf/pdfsign.h"

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

#define MAX_OPTIONS 32

struct FormField {
    int   object;            // the object number that is this field
    WCHAR name[128];
    WCHAR value[1024];
    PdfFieldKind kind;
    BOOL  isText;
    BOOL  changed;

    // A tick box: the name of its "on" state, and whether it is on.
    char  onState[64];
    BOOL  checked;

    // A radio group: the widgets underneath it, and what each one calls the
    // state that means "this one".
    int   kids[MAX_OPTIONS];
    char  kidStates[MAX_OPTIONS][64];
    int   kidCount;

    // A choice list: what it offers.
    WCHAR options[MAX_OPTIONS][128];
    int   optionCount;

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

// Something waiting to be put on a page: where it goes, which page it goes
// on, and either the pixels of a picture or a line of text.
//
// Text is here rather than in its own list because everything after the
// appearance stream -- the annotation, the page's /Annots, the save -- is the
// same for both, and a second list would mean a second copy of all of it.
struct Stamp {
    int    page;             // the page's object number
    float  rect[4];          // in points, from the bottom left

    BYTE*  rgb;              // owned; three bytes a pixel, top row first
    BYTE*  alpha;            // owned; one byte a pixel, or NULL when opaque
    int    width, height;

    WCHAR  text[512];        // a text stamp when this is set, a picture when not
    float  size;             // points
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

    // A signature waiting to be made. The certificate is the user's; this
    // holds it only until the file is written.
    PdfCertificate signingCert;
    WCHAR          signName[256];
    WCHAR          signReason[256];
    WCHAR          timestampUrl[256];
    BOOL           timestamped;
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

// A tick box's "on" state, out of its appearance dictionary. The states are
// the keys of `/AP /N`: one of them is /Off, and the other is what ticking it
// means in this particular form -- /Yes, /On, /1, /Ja, whatever the writer
// chose.
static void ReadCheckboxState(PdfForm* form, Span field, FormField* out) {
    strcpy_s(out->onState, 64, "Yes");

    Span ap, normal;
    if (DictValue(field, "AP", &ap) && Resolve(form, ap, &ap) &&
        DictValue(ap, "N", &normal) && Resolve(form, normal, &normal)) {

        // The first key that is not /Off, at the top level of that dictionary.
        int depth = 0;
        for (size_t i = 0; i + 1 < normal.len; i++) {
            char c = normal.at[i];

            if (c == '<' && normal.at[i + 1] == '<') { depth++; i++; continue; }
            if (c == '>' && normal.at[i + 1] == '>') { depth--; i++; continue; }
            if (depth != 1 || c != '/') continue;

            size_t n = 0;
            char name[64];
            size_t at = i + 1;

            while (at < normal.len && n + 1 < sizeof(name) &&
                   !IsWhite(normal.at[at]) && !IsDelimiter(normal.at[at])) {
                name[n++] = normal.at[at++];
            }
            name[n] = 0;

            if (n > 0 && _stricmp(name, "Off") != 0) {
                strcpy_s(out->onState, 64, name);
                break;
            }
            i = at - 1;
        }
    }

    // What it is now: /V when the field says, /AS when only the widget does.
    Span state;
    if (DictValue(field, "V", &state) || DictValue(field, "AS", &state)) {
        if (state.len > 1 && state.at[0] == '/') {
            char name[64];
            size_t n = 0;
            for (size_t i = 1; i < state.len && n + 1 < sizeof(name); i++) {
                if (IsWhite(state.at[i]) || IsDelimiter(state.at[i])) break;
                name[n++] = state.at[i];
            }
            name[n] = 0;

            out->checked = (n > 0 && _stricmp(name, "Off") != 0);
        }
    }
}

// A choice list's options: `/Opt` is an array of strings, or of two-string
// arrays where the first is what goes in the file and the second is what the
// reader shows.
static void ReadChoiceOptions(PdfForm* form, Span field, FormField* out) {
    Span opt;
    if (!DictValue(field, "Opt", &opt) || !Resolve(form, opt, &opt)) return;

    const char* at = opt.at;
    const char* end = opt.at + opt.len;

    while (at < end && out->optionCount < MAX_OPTIONS) {
        while (at < end && *at != '(' && *at != '<') at++;
        if (at >= end) break;

        Span value = { at, (size_t)(end - at) };

        // The span has to stop at the end of this string, which is what
        // reading one does anyway.
        if (*at == '(') {
            size_t depth = 1;
            size_t i = 1;
            while (i < value.len && depth > 0) {
                if (value.at[i] == 92) i++;
                else if (value.at[i] == '(') depth++;
                else if (value.at[i] == ')') depth--;
                i++;
            }
            value.len = i;
        } else {
            size_t i = 1;
            while (i < value.len && value.at[i] != '>') i++;
            value.len = i + 1;
        }

        ReadPdfString(value, out->options[out->optionCount], 128);
        if (out->options[out->optionCount][0]) out->optionCount++;

        at += value.len;
    }
}

// The buttons under a radio group: which objects they are, and what each one
// calls the state that means "this one is chosen". Those names are the
// group's options -- they are what goes in the field's value.
static void ReadRadioKids(PdfForm* form, Span kids, Span field, FormField* out) {
    const char* at = kids.at;
    const char* end = kids.at + kids.len;

    while (at < end && out->kidCount < MAX_OPTIONS) {
        while (at < end && (*at < 48 || *at > 57)) at++;
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
        int object = AsReference(reference);
        if (object <= 0) continue;

        Span widget;
        if (!ObjectBody(form, object, &widget)) continue;

        // Its own state name, the same way a tick box's is found.
        FormField probe = {0};
        ReadCheckboxState(form, widget, &probe);

        out->kids[out->kidCount] = object;
        strcpy_s(out->kidStates[out->kidCount], 64, probe.onState);

        wcsncpy_s(out->options[out->kidCount], 128, L"", _TRUNCATE);
        MultiByteToWideChar(CP_ACP, 0, probe.onState, -1,
                            out->options[out->kidCount], 128);

        out->kidCount++;
        out->optionCount = out->kidCount;
    }

    // What is chosen now, which the group states rather than the buttons.
    Span value;
    if (DictValue(field, "V", &value) && value.len > 1 && value.at[0] == '/') {
        char name[64];
        size_t n = 0;

        for (size_t i = 1; i < value.len && n + 1 < sizeof(name); i++) {
            if (IsWhite(value.at[i]) || IsDelimiter(value.at[i])) break;
            name[n++] = value.at[i];
        }
        name[n] = 0;

        if (n > 0 && _stricmp(name, "Off") != 0) {
            MultiByteToWideChar(CP_ACP, 0, name, -1, out->value, 1024);
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

    Span kids;
    BOOL hasKids = DictValue(field, "Kids", &kids);

    // A radio group has children too, but it is one question rather than
    // several: the answer is on the field and only the state is on each
    // button. Reading its buttons as fields of their own gives a form full of
    // nameless boxes that cannot be set.
    Span kindType, kindFlags;
    BOOL isRadio = hasKids &&
                   DictValue(field, "FT", &kindType) && SpanIs(kindType, "/Btn") &&
                   DictValue(field, "Ff", &kindFlags) &&
                   ((int)AsNumber(kindFlags) & 0x8000);

    if (hasKids && !isRadio) {
        ReadFieldKids(form, kids, name);
        return;
    }

    Span type;
    BOOL hasType = DictValue(field, "FT", &type);

    PdfFieldKind kind = isRadio ? PDF_FIELD_RADIO : PDF_FIELD_OTHER;
    if (isRadio) hasType = FALSE;      // decided already
    if (hasType) {
        if (SpanIs(type, "/Tx"))      kind = PDF_FIELD_TEXT;
        else if (SpanIs(type, "/Btn")) kind = PDF_FIELD_CHECKBOX;
        else if (SpanIs(type, "/Ch"))  kind = PDF_FIELD_CHOICE;
    }

    // A push button is a /Btn with nothing to tick: it runs something when it
    // is clicked, and has no value worth writing.
    if (kind == PDF_FIELD_CHECKBOX) {
        Span flags;
        if (DictValue(field, "Ff", &flags) && ((int)AsNumber(flags) & 0x10000)) {
            kind = PDF_FIELD_OTHER;
        }
    }

    FormField* out = &form->fields[form->fieldCount++];
    memset(out, 0, sizeof(*out));

    out->object = object;
    out->kind = kind;
    out->isText = (kind == PDF_FIELD_TEXT);
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

    if (kind == PDF_FIELD_TEXT || kind == PDF_FIELD_CHOICE) ReadFieldFont(form, field, out);
    if (kind == PDF_FIELD_CHECKBOX) ReadCheckboxState(form, field, out);
    if (kind == PDF_FIELD_CHOICE)   ReadChoiceOptions(form, field, out);
    if (kind == PDF_FIELD_RADIO)    ReadRadioKids(form, kids, field, out);
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
        // No form at all. That is not a failure: a PDF with nothing to fill in
        // is still one that can be signed or stamped, and the caller finds out
        // there are no fields by asking how many there are.
        return TRUE;
    }

    form->acroFormObject = AsReference(acroForm);

    Span acro;
    if (!Resolve(form, acroForm, &acro)) return TRUE;

    Span fields;
    if (DictValue(acro, "Fields", &fields)) ReadFieldKids(form, fields, NULL);
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

    // A timestamp is asked for by default, from the built-in authority or
    // from whichever one this machine names.
    if (GetEnvironmentVariableW(L"OPENNOTE_TSA", form->timestampUrl, 256) == 0) {
        wcsncpy_s(form->timestampUrl, 256, PDFSIGN_DEFAULT_TIMESTAMP, _TRUNCATE);
    }

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

extern "C" PdfFieldKind PdfForm_FieldKind(const PdfForm* form, int index) {
    if (!form || index < 0 || index >= form->fieldCount) return PDF_FIELD_OTHER;
    return form->fields[index].kind;
}

extern "C" BOOL PdfForm_FieldChecked(const PdfForm* form, int index) {
    if (!form || index < 0 || index >= form->fieldCount) return FALSE;
    return form->fields[index].checked;
}

extern "C" BOOL PdfForm_SetFieldChecked(PdfForm* form, int index, BOOL checked) {
    if (!form || index < 0 || index >= form->fieldCount) return FALSE;
    if (form->fields[index].kind != PDF_FIELD_CHECKBOX) return FALSE;

    form->fields[index].checked = checked;
    form->fields[index].changed = TRUE;

    // What it reads as, for anything listing the form.
    MultiByteToWideChar(CP_ACP, 0,
                        checked ? form->fields[index].onState : "Off", -1,
                        form->fields[index].value, 1024);
    return TRUE;
}

extern "C" int PdfForm_FieldOptionCount(const PdfForm* form, int index) {
    if (!form || index < 0 || index >= form->fieldCount) return 0;
    return form->fields[index].optionCount;
}

extern "C" const WCHAR* PdfForm_FieldOption(const PdfForm* form, int index, int option) {
    if (!form || index < 0 || index >= form->fieldCount) return L"";
    if (option < 0 || option >= form->fields[index].optionCount) return L"";
    return form->fields[index].options[option];
}

extern "C" BOOL PdfForm_SetFieldValue(PdfForm* form, int index, const WCHAR* text) {
    if (!form || index < 0 || index >= form->fieldCount) return FALSE;

    PdfFieldKind kind = form->fields[index].kind;

    // A radio group takes one of its own options and nothing else: a value it
    // has no button for would leave the question answered in the file and
    // blank on the page.
    if (kind == PDF_FIELD_RADIO) {
        FormField* field = &form->fields[index];

        for (int i = 0; i < field->kidCount; i++) {
            if (_wcsicmp(field->options[i], text ? text : L"") != 0) continue;

            wcsncpy_s(field->value, 1024, field->options[i], _TRUNCATE);
            field->changed = TRUE;
            return TRUE;
        }
        return FALSE;
    }

    if (kind != PDF_FIELD_TEXT && kind != PDF_FIELD_CHOICE) return FALSE;

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

extern "C" void PdfForm_SetTimestampUrl(PdfForm* form, const WCHAR* url) {
    if (!form) return;
    wcsncpy_s(form->timestampUrl, 256, url ? url : L"", _TRUNCATE);
}

extern "C" BOOL PdfForm_WasTimestamped(const PdfForm* form) {
    return form ? form->timestamped : FALSE;
}

extern "C" BOOL PdfForm_SignWithCertificate(PdfForm* form, PdfCertificate certificate,
                                            const WCHAR* name, const WCHAR* reason) {
    if (!form || !certificate) return FALSE;

    form->signingCert = certificate;
    wcsncpy_s(form->signName, 256, name ? name : L"", _TRUNCATE);
    wcsncpy_s(form->signReason, 256, reason ? reason : L"", _TRUNCATE);
    return TRUE;
}

extern "C" BOOL PdfForm_StampImageBytes(PdfForm* form, int pageIndex,
                                        const BYTE* encoded, size_t len,
                                        float x, float y, float width, float height) {
    if (!form || !encoded || len == 0 || width <= 0.0f || height <= 0.0f) return FALSE;
    if (form->stampCount >= MAX_STAMPS) return FALSE;

    ReadPages(form);
    if (pageIndex < 0 || pageIndex >= form->pageCount) return FALSE;

    // WIC reads it or it is not a picture. The transparency is kept: a
    // signature is mostly transparent, and dropping that paints a box over
    // whatever it was signed on top of.
    int imageWidth = 0, imageHeight = 0;
    BYTE* bgra = ImageDib_DecodeAlpha(encoded, len, &imageWidth, &imageHeight);

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
    stamp->text[0] = L'\0';
    stamp->size = 0.0f;
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

extern "C" BOOL PdfForm_StampText(PdfForm* form, int pageIndex, const WCHAR* text,
                                  float x, float y, float size) {
    if (!form || !text || !text[0]) return FALSE;
    if (form->stampCount >= MAX_STAMPS) return FALSE;

    ReadPages(form);
    if (pageIndex < 0 || pageIndex >= form->pageCount) return FALSE;

    if (size <= 0.0f) size = 11.0f;
    if (size < 4.0f) size = 4.0f;
    if (size > 144.0f) size = 144.0f;

    Stamp* stamp = &form->stamps[form->stampCount++];
    stamp->page = form->pages[pageIndex];
    stamp->rgb = NULL;
    stamp->alpha = NULL;
    stamp->width = 0;
    stamp->height = 0;
    stamp->size = size;
    wcsncpy_s(stamp->text, 512, text, _TRUNCATE);

    // Helvetica's characters average a little over half their height. The box
    // only has to contain the text -- a reader that finds the appearance too
    // big for it clips, and one that is generous wastes nothing.
    float width = (float)wcslen(stamp->text) * size * 0.60f + 4.0f;

    stamp->rect[0] = x;
    stamp->rect[1] = y - size * 0.25f;          // room for descenders
    stamp->rect[2] = x + width;
    stamp->rect[3] = y + size;
    return TRUE;
}

// The same thing, for a picture that is a file. Reading it is the only
// difference, so that is all this does.
extern "C" BOOL PdfForm_StampImage(PdfForm* form, int pageIndex, const WCHAR* imagePath,
                                   float x, float y, float width, float height) {
    if (!imagePath || !imagePath[0]) return FALSE;

    HANDLE file = CreateFileW(imagePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return FALSE;
    }

    BYTE* bytes = (BYTE*)malloc((size_t)size.QuadPart);
    DWORD read = 0;

    BOOL ok = bytes && ReadFile(file, bytes, (DWORD)size.QuadPart, &read, NULL) &&
              read == (DWORD)size.QuadPart;
    CloseHandle(file);

    if (!ok) {
        free(bytes);
        return FALSE;
    }

    BOOL stamped = PdfForm_StampImageBytes(form, pageIndex, bytes, (size_t)read,
                                           x, y, width, height);
    free(bytes);
    return stamped;
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

    // A tick box is a different animal from a box to type in. Its appearance
    // is already in the file -- one drawing per state -- so what changes is
    // which state is showing, and replacing its /AP with a drawing of our own
    // would throw away the tick the form was made with.
    if (field->kind == PDF_FIELD_CHECKBOX) {
        static const char* const ticked[] = { "V", "AS", NULL };
        CopyDictExcept(out, body, ticked);

        const char* state = field->checked ? field->onState : "Off";
        OutFormat(out, " /V /%s /AS /%s >>", state, state);
        return;
    }

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

    int changed = form->stampCount + (form->signingCert ? 1 : 0);
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
    // A picture stamp writes five: mask, image, appearance, annotation, and
    // the page it goes on.
    Written written[MAX_FIELDS * 3 + MAX_STAMPS * 5 + 4];
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

    // A text stamp always needs one: there is no field to have named a font.
    for (int i = 0; i < form->stampCount && helvetica == 0; i++) {
        if (form->stamps[i].text[0]) helvetica = nextObject++;
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

        // A radio group: the answer on the field, the state on every button,
        // including the ones being turned off.
        if (field->kind == PDF_FIELD_RADIO) {
            char chosen[64] = "Off";
            WideCharToMultiByte(CP_ACP, 0, field->value, -1, chosen, 64, NULL, NULL);

            written[writtenCount].number = field->object;
            written[writtenCount].offset = out.len;
            writtenCount++;

            static const char* const answered[] = { "V", NULL };

            Span group;
            if (ObjectBody(form, field->object, &group)) {
                OutFormat(&out, "%d 0 obj\n", field->object);
                CopyDictExcept(&out, group, answered);
                OutFormat(&out, " /V /%s >>\nendobj\n", chosen);
            }

            for (int k = 0; k < field->kidCount; k++) {
                Span widget;
                if (!ObjectBody(form, field->kids[k], &widget)) continue;

                written[writtenCount].number = field->kids[k];
                written[writtenCount].offset = out.len;
                writtenCount++;

                const char* state = _stricmp(field->kidStates[k], chosen) == 0
                                  ? field->kidStates[k] : "Off";

                static const char* const shown[] = { "AS", NULL };

                OutFormat(&out, "%d 0 obj\n", field->kids[k]);
                CopyDictExcept(&out, widget, shown);
                OutFormat(&out, " /AS /%s >>\nendobj\n", state);
            }
            continue;
        }

        // A tick box keeps the appearance the form drew for it.
        if (field->kind == PDF_FIELD_CHECKBOX) {
            written[writtenCount].number = field->object;
            written[writtenCount].offset = out.len;
            writtenCount++;

            OutFormat(&out, "%d 0 obj\n", field->object);
            WriteFieldObject(&out, form, field, 0);
            OutText(&out, "\nendobj\n");
            continue;
        }

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

        // Text takes a shorter road: no picture, no mask, an appearance that
        // draws characters, and an annotation that carries the characters as
        // well so a reader can still find them.
        if (stamp->text[0]) {
            float boxWidth = stamp->rect[2] - stamp->rect[0];
            float boxHeight = stamp->rect[3] - stamp->rect[1];

            char drawn[2048];
            WriteDrawnString(stamp->text, drawn, sizeof(drawn));

            int appearance = nextObject++;
            written[writtenCount].number = appearance;
            written[writtenCount].offset = out.len;
            writtenCount++;

            // Drawn from the bottom of the box plus the room left for
            // descenders, which is where the caller asked for the baseline.
            char content[2200];
            int contentLen = snprintf(content, sizeof(content),
                "q\nBT\n/Helv %.2f Tf\n0 g\n2 %.2f Td\n%s Tj\nET\nQ\n",
                stamp->size, stamp->size * 0.25f, drawn);
            if (contentLen < 0) contentLen = 0;

            OutFormat(&out,
                "%d 0 obj\n<< /Type /XObject /Subtype /Form /FormType 1 "
                "/BBox [0 0 %.2f %.2f] "
                "/Resources << /ProcSet [/PDF /Text] /Font << /Helv %d 0 R >> >> "
                "/Length %d >>\nstream\n",
                appearance, boxWidth, boxHeight, helvetica, contentLen);
            OutAdd(&out, content, (size_t)contentLen);
            OutText(&out, "\nendstream\nendobj\n");

            int annotation = nextObject++;
            written[writtenCount].number = annotation;
            written[writtenCount].offset = out.len;
            writtenCount++;

            // /BS with no width: a FreeText annotation draws a border by
            // default, and a box around every answer is not what typing on a
            // form looks like.
            OutFormat(&out,
                "%d 0 obj\n<< /Type /Annot /Subtype /FreeText "
                "/Rect [%.2f %.2f %.2f %.2f] /F 4 /Contents %s "
                "/DA (/Helv %.2f Tf 0 g) /BS << /W 0 >> /AP << /N %d 0 R >> >>\nendobj\n",
                annotation, stamp->rect[0], stamp->rect[1], stamp->rect[2], stamp->rect[3],
                drawn, stamp->size, appearance);

            Span textPage;
            if (!ObjectBody(form, stamp->page, &textPage)) continue;

            written[writtenCount].number = stamp->page;
            written[writtenCount].offset = out.len;
            writtenCount++;

            OutFormat(&out, "%d 0 obj\n", stamp->page);

            Span textAnnots;
            BOOL hadAnnots = DictValue(textPage, "Annots", &textAnnots);

            static const char* const textPageKeys[] = { "Annots", NULL };
            CopyDictExcept(&out, textPage, textPageKeys);

            OutText(&out, " /Annots [");
            if (hadAnnots && textAnnots.len > 2) {
                OutAdd(&out, textAnnots.at + 1, textAnnots.len - 2);
            }
            OutFormat(&out, " %d 0 R] >>", annotation);

            OutText(&out, "\nendobj\n");
            continue;
        }

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

    // The signature: a dictionary with a hole in it, a field that points at
    // the dictionary, and the form told that it holds one.
    //
    // The hole is the point. `/Contents` is written as a run of zeros, and
    // `/ByteRange` says "everything except that", so the signature can cover a
    // file it is itself part of. Both are written at a fixed width and patched
    // once the file is finished, because changing their length afterwards
    // would move everything after them.
    size_t byteRangeAt = 0;
    size_t contentsAt = 0;
    size_t contentsLen = 0;
    int signatureField = 0;

    if (form->signingCert) {
        int signatureObject = nextObject++;
        written[writtenCount].number = signatureObject;
        written[writtenCount].offset = out.len;
        writtenCount++;

        SYSTEMTIME now;
        GetLocalTime(&now);

        char name[512] = "";
        char reason[512] = "";
        WriteDrawnString(form->signName, name, sizeof(name));
        WriteDrawnString(form->signReason, reason, sizeof(reason));

        OutFormat(&out,
            "%d 0 obj\n<< /Type /Sig /Filter /Adobe.PPKLite "
            "/SubFilter /adbe.pkcs7.detached /Name %s /Reason %s "
            "/M (D:%04d%02d%02d%02d%02d%02d) /ByteRange [0 ",
            signatureObject, name, reason,
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

        // Three ten-digit numbers, filled in later.
        byteRangeAt = out.len;
        OutText(&out, "0000000000 0000000000 0000000000] /Contents ");

        // 16KB of room. A plain signature is about 1.2KB and a timestamped
        // one about 7KB, so this is deliberately more than double what is
        // needed: an authority with a longer certificate chain that did not
        // fit would fail the save outright, and 16KB of zeros in the file is
        // the cheaper end of that trade.
        contentsAt = out.len;
        OutText(&out, "<");
        for (int i = 0; i < 32768; i++) OutText(&out, "0");
        OutText(&out, ">");
        contentsLen = out.len - contentsAt;

        OutText(&out, " >>\nendobj\n");

        // The field that carries it. Invisible: a certificate signature says
        // something about the bytes, and putting a picture on the page is the
        // other command's job.
        signatureField = nextObject++;
        written[writtenCount].number = signatureField;
        written[writtenCount].offset = out.len;
        writtenCount++;

        int page = form->pageCount > 0 ? form->pages[0] : 0;

        OutFormat(&out,
            "%d 0 obj\n<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature) "
            "/Rect [0 0 0 0] /F 132 /V %d 0 R",
            signatureField, signatureObject);
        if (page > 0) OutFormat(&out, " /P %d 0 R", page);
        OutText(&out, " >>\nendobj\n");
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

            static const char* const formKeys[] = { "NeedAppearances", "Fields", "SigFlags", NULL };

            Span existingFields;
            BOOL hasFields = DictValue(acro, "Fields", &existingFields);

            CopyDictExcept(&out, acro, formKeys);
            OutText(&out, " /NeedAppearances true");

            OutText(&out, " /Fields [");
            if (hasFields && existingFields.len > 2) {
                OutAdd(&out, existingFields.at + 1, existingFields.len - 2);
            }
            if (signatureField > 0) OutFormat(&out, " %d 0 R", signatureField);
            OutText(&out, "]");

            // 3 is "this document holds a signature, and appending to it would
            // invalidate one".
            if (signatureField > 0) OutText(&out, " /SigFlags 3");

            OutText(&out, " >>");

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

    // --- the signature, now that there are bytes to sign --------------------
    //
    // The file is finished except for two holes in it. The byte range says
    // where the second hole is, and the signature covers everything either
    // side of it -- so the numbers go in first, and then the signature is made
    // over the file as it will be on disk.
    if (form->signingCert && contentsAt > 0) {
        size_t contentsEnd = contentsAt + contentsLen;

        char range[64];
        int rangeLen = snprintf(range, sizeof(range), "%010zu %010zu %010zu",
                                contentsAt, contentsEnd, out.len - contentsEnd);

        // The placeholder was written at exactly this width; if that ever
        // stopped being true, every offset in the file would shift.
        if (rangeLen != 32 || byteRangeAt + 32 > out.len) {
            free(out.bytes);
            return FALSE;
        }
        memcpy(out.bytes + byteRangeAt, range, 32);

        size_t signatureLen = 0;
        BYTE* signature = PdfSign_DetachedTimestamped(
            form->signingCert,
            (const BYTE*)out.bytes, contentsAt,
            (const BYTE*)out.bytes + contentsEnd, out.len - contentsEnd,
            form->timestampUrl, &form->timestamped,
            &signatureLen);

        if (!signature || signatureLen == 0) {
            free(signature);
            free(out.bytes);
            return FALSE;
        }

        // Hex, into the hole, zero-padded to the end of it. The brackets stay
        // where they are: they are part of the hole the range describes.
        size_t room = (contentsLen - 2) / 2;
        if (signatureLen > room) {
            free(signature);
            free(out.bytes);
            return FALSE;
        }

        static const char* const hex = "0123456789ABCDEF";
        char* at = out.bytes + contentsAt + 1;

        for (size_t i = 0; i < signatureLen; i++) {
            at[i * 2] = hex[(signature[i] >> 4) & 0xF];
            at[i * 2 + 1] = hex[signature[i] & 0xF];
        }
        for (size_t i = signatureLen * 2; i < contentsLen - 2; i++) at[i] = '0';

        // Checked here rather than believed: the bytes that went into the
        // file have to verify against the file they went into, read back out
        // of the hole the way a reader would read them.
        BYTE* readBack = (BYTE*)malloc(room ? room : 1);
        if (readBack) {
            size_t readLen = 0;

            for (size_t i = 0; i + 1 < contentsLen - 2; i += 2) {
                int high = at[i], low = at[i + 1];

                int hi = (high >= '0' && high <= '9') ? high - '0'
                       : (high >= 'A' && high <= 'F') ? high - 'A' + 10 : -1;
                int lo = (low >= '0' && low <= '9') ? low - '0'
                       : (low >= 'A' && low <= 'F') ? low - 'A' + 10 : -1;
                if (hi < 0 || lo < 0) break;

                readBack[readLen++] = (BYTE)((hi << 4) | lo);
                if (readLen >= signatureLen) break;
            }

            BOOL verified = PdfSign_VerifyDetached(
                readBack, readLen,
                (const BYTE*)out.bytes, contentsAt,
                (const BYTE*)out.bytes + contentsEnd, out.len - contentsEnd);

            free(readBack);

            if (!verified) {
                free(signature);
                free(out.bytes);
                return FALSE;
            }
        }

        free(signature);
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

        // A signature is made once: saving again would sign a file that
        // already holds one, which is a different operation.
        form->signingCert = NULL;
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


// A form with the three kinds of field in it: a box to type in, a box to tick
// and a list to choose from. The tick box carries the two drawings a tick box
// has -- one for off, one for on -- because ticking it is choosing between
// them, and writing a drawing of our own over them is the mistake this
// checks against.
static const BYTE MIXED_FORM[] = {
    0x25, 0x50, 0x44, 0x46, 0x2D, 0x31, 0x2E, 0x34, 0x0A, 0x31, 0x20, 0x30,
    0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70,
    0x65, 0x20, 0x2F, 0x43, 0x61, 0x74, 0x61, 0x6C, 0x6F, 0x67, 0x20, 0x2F,
    0x50, 0x61, 0x67, 0x65, 0x73, 0x20, 0x32, 0x20, 0x30, 0x20, 0x52, 0x20,
    0x2F, 0x41, 0x63, 0x72, 0x6F, 0x46, 0x6F, 0x72, 0x6D, 0x20, 0x3C, 0x3C,
    0x20, 0x2F, 0x46, 0x69, 0x65, 0x6C, 0x64, 0x73, 0x20, 0x5B, 0x34, 0x20,
    0x30, 0x20, 0x52, 0x20, 0x37, 0x20, 0x30, 0x20, 0x52, 0x20, 0x31, 0x30,
    0x20, 0x30, 0x20, 0x52, 0x5D, 0x20, 0x2F, 0x44, 0x41, 0x20, 0x28, 0x2F,
    0x48, 0x65, 0x6C, 0x76, 0x20, 0x30, 0x20, 0x54, 0x66, 0x20, 0x30, 0x20,
    0x67, 0x29, 0x20, 0x2F, 0x44, 0x52, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x46,
    0x6F, 0x6E, 0x74, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76,
    0x20, 0x36, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62,
    0x20, 0x31, 0x31, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E,
    0x3E, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F,
    0x62, 0x6A, 0x0A, 0x32, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C,
    0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x50, 0x61, 0x67,
    0x65, 0x73, 0x20, 0x2F, 0x4B, 0x69, 0x64, 0x73, 0x20, 0x5B, 0x33, 0x20,
    0x30, 0x20, 0x52, 0x5D, 0x20, 0x2F, 0x43, 0x6F, 0x75, 0x6E, 0x74, 0x20,
    0x31, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A,
    0x33, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F,
    0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x50, 0x61, 0x67, 0x65, 0x20, 0x2F,
    0x50, 0x61, 0x72, 0x65, 0x6E, 0x74, 0x20, 0x32, 0x20, 0x30, 0x20, 0x52,
    0x20, 0x2F, 0x4D, 0x65, 0x64, 0x69, 0x61, 0x42, 0x6F, 0x78, 0x20, 0x5B,
    0x30, 0x20, 0x30, 0x20, 0x36, 0x31, 0x32, 0x20, 0x37, 0x39, 0x32, 0x5D,
    0x20, 0x2F, 0x43, 0x6F, 0x6E, 0x74, 0x65, 0x6E, 0x74, 0x73, 0x20, 0x35,
    0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x52, 0x65, 0x73, 0x6F, 0x75, 0x72,
    0x63, 0x65, 0x73, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x46, 0x6F, 0x6E, 0x74,
    0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x36, 0x20,
    0x30, 0x20, 0x52, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20, 0x31, 0x31,
    0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20, 0x2F,
    0x41, 0x6E, 0x6E, 0x6F, 0x74, 0x73, 0x20, 0x5B, 0x34, 0x20, 0x30, 0x20,
    0x52, 0x20, 0x37, 0x20, 0x30, 0x20, 0x52, 0x20, 0x31, 0x30, 0x20, 0x30,
    0x20, 0x52, 0x5D, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62,
    0x6A, 0x0A, 0x34, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C,
    0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x41, 0x6E, 0x6E, 0x6F,
    0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F,
    0x57, 0x69, 0x64, 0x67, 0x65, 0x74, 0x20, 0x2F, 0x46, 0x54, 0x20, 0x2F,
    0x54, 0x78, 0x20, 0x2F, 0x54, 0x20, 0x28, 0x66, 0x75, 0x6C, 0x6C, 0x20,
    0x6E, 0x61, 0x6D, 0x65, 0x29, 0x20, 0x2F, 0x56, 0x20, 0x28, 0x29, 0x20,
    0x2F, 0x52, 0x65, 0x63, 0x74, 0x20, 0x5B, 0x31, 0x35, 0x30, 0x20, 0x37,
    0x30, 0x30, 0x20, 0x34, 0x30, 0x30, 0x20, 0x37, 0x32, 0x34, 0x5D, 0x20,
    0x2F, 0x44, 0x41, 0x20, 0x28, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x31,
    0x32, 0x20, 0x54, 0x66, 0x20, 0x30, 0x20, 0x67, 0x29, 0x20, 0x2F, 0x46,
    0x20, 0x34, 0x20, 0x2F, 0x50, 0x20, 0x33, 0x20, 0x30, 0x20, 0x52, 0x20,
    0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x35, 0x20,
    0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x4C, 0x65,
    0x6E, 0x67, 0x74, 0x68, 0x20, 0x31, 0x31, 0x38, 0x20, 0x3E, 0x3E, 0x0A,
    0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x42, 0x54, 0x20, 0x2F, 0x48,
    0x65, 0x6C, 0x76, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66, 0x20, 0x37, 0x32,
    0x20, 0x37, 0x30, 0x36, 0x20, 0x54, 0x64, 0x20, 0x28, 0x4E, 0x61, 0x6D,
    0x65, 0x3A, 0x29, 0x20, 0x54, 0x6A, 0x20, 0x45, 0x54, 0x0A, 0x42, 0x54,
    0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66,
    0x20, 0x37, 0x32, 0x20, 0x36, 0x35, 0x36, 0x20, 0x54, 0x64, 0x20, 0x28,
    0x41, 0x67, 0x72, 0x65, 0x65, 0x3A, 0x29, 0x20, 0x54, 0x6A, 0x20, 0x45,
    0x54, 0x0A, 0x42, 0x54, 0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x31,
    0x32, 0x20, 0x54, 0x66, 0x20, 0x37, 0x32, 0x20, 0x36, 0x30, 0x36, 0x20,
    0x54, 0x64, 0x20, 0x28, 0x54, 0x65, 0x61, 0x6D, 0x3A, 0x29, 0x20, 0x54,
    0x6A, 0x20, 0x45, 0x54, 0x0A, 0x65, 0x6E, 0x64, 0x73, 0x74, 0x72, 0x65,
    0x61, 0x6D, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x36, 0x20,
    0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79,
    0x70, 0x65, 0x20, 0x2F, 0x46, 0x6F, 0x6E, 0x74, 0x20, 0x2F, 0x53, 0x75,
    0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x31,
    0x20, 0x2F, 0x42, 0x61, 0x73, 0x65, 0x46, 0x6F, 0x6E, 0x74, 0x20, 0x2F,
    0x48, 0x65, 0x6C, 0x76, 0x65, 0x74, 0x69, 0x63, 0x61, 0x20, 0x2F, 0x45,
    0x6E, 0x63, 0x6F, 0x64, 0x69, 0x6E, 0x67, 0x20, 0x2F, 0x57, 0x69, 0x6E,
    0x41, 0x6E, 0x73, 0x69, 0x45, 0x6E, 0x63, 0x6F, 0x64, 0x69, 0x6E, 0x67,
    0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x37,
    0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54,
    0x79, 0x70, 0x65, 0x20, 0x2F, 0x41, 0x6E, 0x6E, 0x6F, 0x74, 0x20, 0x2F,
    0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x57, 0x69, 0x64,
    0x67, 0x65, 0x74, 0x20, 0x2F, 0x46, 0x54, 0x20, 0x2F, 0x42, 0x74, 0x6E,
    0x20, 0x2F, 0x54, 0x20, 0x28, 0x61, 0x67, 0x72, 0x65, 0x65, 0x29, 0x20,
    0x2F, 0x56, 0x20, 0x2F, 0x4F, 0x66, 0x66, 0x20, 0x2F, 0x41, 0x53, 0x20,
    0x2F, 0x4F, 0x66, 0x66, 0x20, 0x2F, 0x52, 0x65, 0x63, 0x74, 0x20, 0x5B,
    0x31, 0x35, 0x30, 0x20, 0x36, 0x35, 0x30, 0x20, 0x31, 0x36, 0x38, 0x20,
    0x36, 0x36, 0x38, 0x5D, 0x20, 0x2F, 0x46, 0x20, 0x34, 0x20, 0x2F, 0x50,
    0x20, 0x33, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x4D, 0x4B, 0x20, 0x3C,
    0x3C, 0x20, 0x2F, 0x42, 0x43, 0x20, 0x5B, 0x30, 0x20, 0x30, 0x20, 0x30,
    0x5D, 0x20, 0x3E, 0x3E, 0x20, 0x2F, 0x41, 0x50, 0x20, 0x3C, 0x3C, 0x20,
    0x2F, 0x4E, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x4F, 0x6E, 0x20, 0x38, 0x20,
    0x30, 0x20, 0x52, 0x20, 0x2F, 0x4F, 0x66, 0x66, 0x20, 0x39, 0x20, 0x30,
    0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x0A,
    0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x38, 0x20, 0x30, 0x20, 0x6F,
    0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20,
    0x2F, 0x58, 0x4F, 0x62, 0x6A, 0x65, 0x63, 0x74, 0x20, 0x2F, 0x53, 0x75,
    0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x46, 0x6F, 0x72, 0x6D, 0x20,
    0x2F, 0x42, 0x42, 0x6F, 0x78, 0x20, 0x5B, 0x30, 0x20, 0x30, 0x20, 0x31,
    0x38, 0x20, 0x31, 0x38, 0x5D, 0x20, 0x2F, 0x52, 0x65, 0x73, 0x6F, 0x75,
    0x72, 0x63, 0x65, 0x73, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x46, 0x6F, 0x6E,
    0x74, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20, 0x31,
    0x31, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20,
    0x2F, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x20, 0x34, 0x35, 0x20, 0x3E,
    0x3E, 0x0A, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x71, 0x20, 0x30,
    0x20, 0x30, 0x20, 0x30, 0x20, 0x72, 0x67, 0x20, 0x42, 0x54, 0x20, 0x2F,
    0x5A, 0x61, 0x44, 0x62, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66, 0x20, 0x32,
    0x20, 0x32, 0x20, 0x54, 0x64, 0x20, 0x28, 0x34, 0x29, 0x20, 0x54, 0x6A,
    0x20, 0x45, 0x54, 0x20, 0x51, 0x0A, 0x65, 0x6E, 0x64, 0x73, 0x74, 0x72,
    0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x39,
    0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54,
    0x79, 0x70, 0x65, 0x20, 0x2F, 0x58, 0x4F, 0x62, 0x6A, 0x65, 0x63, 0x74,
    0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x46,
    0x6F, 0x72, 0x6D, 0x20, 0x2F, 0x42, 0x42, 0x6F, 0x78, 0x20, 0x5B, 0x30,
    0x20, 0x30, 0x20, 0x31, 0x38, 0x20, 0x31, 0x38, 0x5D, 0x20, 0x2F, 0x4C,
    0x65, 0x6E, 0x67, 0x74, 0x68, 0x20, 0x34, 0x20, 0x3E, 0x3E, 0x0A, 0x73,
    0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x71, 0x20, 0x51, 0x0A, 0x65, 0x6E,
    0x64, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E, 0x64, 0x6F,
    0x62, 0x6A, 0x0A, 0x31, 0x30, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A,
    0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x41, 0x6E,
    0x6E, 0x6F, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65,
    0x20, 0x2F, 0x57, 0x69, 0x64, 0x67, 0x65, 0x74, 0x20, 0x2F, 0x46, 0x54,
    0x20, 0x2F, 0x43, 0x68, 0x20, 0x2F, 0x54, 0x20, 0x28, 0x64, 0x65, 0x70,
    0x61, 0x72, 0x74, 0x6D, 0x65, 0x6E, 0x74, 0x29, 0x20, 0x2F, 0x56, 0x20,
    0x28, 0x29, 0x20, 0x2F, 0x4F, 0x70, 0x74, 0x20, 0x5B, 0x28, 0x45, 0x6E,
    0x67, 0x69, 0x6E, 0x65, 0x65, 0x72, 0x69, 0x6E, 0x67, 0x29, 0x20, 0x28,
    0x4C, 0x65, 0x67, 0x61, 0x6C, 0x29, 0x20, 0x28, 0x41, 0x63, 0x63, 0x6F,
    0x75, 0x6E, 0x74, 0x73, 0x29, 0x5D, 0x20, 0x2F, 0x52, 0x65, 0x63, 0x74,
    0x20, 0x5B, 0x31, 0x35, 0x30, 0x20, 0x36, 0x30, 0x30, 0x20, 0x34, 0x30,
    0x30, 0x20, 0x36, 0x32, 0x34, 0x5D, 0x20, 0x2F, 0x44, 0x41, 0x20, 0x28,
    0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66, 0x20,
    0x30, 0x20, 0x67, 0x29, 0x20, 0x2F, 0x46, 0x20, 0x34, 0x20, 0x2F, 0x50,
    0x20, 0x33, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x46, 0x66, 0x20, 0x31,
    0x33, 0x31, 0x30, 0x37, 0x32, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64,
    0x6F, 0x62, 0x6A, 0x0A, 0x31, 0x31, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A,
    0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x46,
    0x6F, 0x6E, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65,
    0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x31, 0x20, 0x2F, 0x42, 0x61, 0x73,
    0x65, 0x46, 0x6F, 0x6E, 0x74, 0x20, 0x2F, 0x5A, 0x61, 0x70, 0x66, 0x44,
    0x69, 0x6E, 0x67, 0x62, 0x61, 0x74, 0x73, 0x20, 0x3E, 0x3E, 0x0A, 0x65,
    0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x78, 0x72, 0x65, 0x66, 0x0A, 0x30,
    0x20, 0x31, 0x32, 0x0A, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x20, 0x36, 0x35, 0x35, 0x33, 0x35, 0x20, 0x66, 0x20, 0x0A,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x39, 0x20, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x31, 0x37, 0x31, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x32,
    0x32, 0x38, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x33, 0x39, 0x38, 0x20, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x35, 0x33, 0x38, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x37,
    0x30, 0x36, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x38, 0x30, 0x33, 0x20, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x39, 0x39, 0x31, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x31,
    0x37, 0x35, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x32, 0x37, 0x35, 0x20, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x31, 0x34, 0x36, 0x38, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x74, 0x72, 0x61, 0x69, 0x6C, 0x65, 0x72, 0x0A,
    0x3C, 0x3C, 0x20, 0x2F, 0x53, 0x69, 0x7A, 0x65, 0x20, 0x31, 0x32, 0x20,
    0x2F, 0x52, 0x6F, 0x6F, 0x74, 0x20, 0x31, 0x20, 0x30, 0x20, 0x52, 0x20,
    0x3E, 0x3E, 0x0A, 0x73, 0x74, 0x61, 0x72, 0x74, 0x78, 0x72, 0x65, 0x66,
    0x0A, 0x31, 0x35, 0x34, 0x32, 0x0A, 0x25, 0x25, 0x45, 0x4F, 0x46, 0x0A
};


// A radio group: one question, three buttons, and the answer on the field
// rather than on any of them. Read as three nameless boxes -- which is what
// reading the buttons as fields gives you -- it cannot be filled in at all.
static const BYTE RADIO_FORM[] = {
    0x25, 0x50, 0x44, 0x46, 0x2D, 0x31, 0x2E, 0x34, 0x0A, 0x31, 0x20, 0x30,
    0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70,
    0x65, 0x20, 0x2F, 0x43, 0x61, 0x74, 0x61, 0x6C, 0x6F, 0x67, 0x20, 0x2F,
    0x50, 0x61, 0x67, 0x65, 0x73, 0x20, 0x32, 0x20, 0x30, 0x20, 0x52, 0x20,
    0x2F, 0x41, 0x63, 0x72, 0x6F, 0x46, 0x6F, 0x72, 0x6D, 0x20, 0x3C, 0x3C,
    0x20, 0x2F, 0x46, 0x69, 0x65, 0x6C, 0x64, 0x73, 0x20, 0x5B, 0x34, 0x20,
    0x30, 0x20, 0x52, 0x5D, 0x20, 0x2F, 0x44, 0x41, 0x20, 0x28, 0x2F, 0x48,
    0x65, 0x6C, 0x76, 0x20, 0x30, 0x20, 0x54, 0x66, 0x20, 0x30, 0x20, 0x67,
    0x29, 0x20, 0x2F, 0x44, 0x52, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x46, 0x6F,
    0x6E, 0x74, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20,
    0x36, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20,
    0x31, 0x31, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E,
    0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62,
    0x6A, 0x0A, 0x32, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C,
    0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x50, 0x61, 0x67, 0x65,
    0x73, 0x20, 0x2F, 0x4B, 0x69, 0x64, 0x73, 0x20, 0x5B, 0x33, 0x20, 0x30,
    0x20, 0x52, 0x5D, 0x20, 0x2F, 0x43, 0x6F, 0x75, 0x6E, 0x74, 0x20, 0x31,
    0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x33,
    0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54,
    0x79, 0x70, 0x65, 0x20, 0x2F, 0x50, 0x61, 0x67, 0x65, 0x20, 0x2F, 0x50,
    0x61, 0x72, 0x65, 0x6E, 0x74, 0x20, 0x32, 0x20, 0x30, 0x20, 0x52, 0x20,
    0x2F, 0x4D, 0x65, 0x64, 0x69, 0x61, 0x42, 0x6F, 0x78, 0x20, 0x5B, 0x30,
    0x20, 0x30, 0x20, 0x36, 0x31, 0x32, 0x20, 0x37, 0x39, 0x32, 0x5D, 0x20,
    0x2F, 0x43, 0x6F, 0x6E, 0x74, 0x65, 0x6E, 0x74, 0x73, 0x20, 0x35, 0x20,
    0x30, 0x20, 0x52, 0x20, 0x2F, 0x52, 0x65, 0x73, 0x6F, 0x75, 0x72, 0x63,
    0x65, 0x73, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x46, 0x6F, 0x6E, 0x74, 0x20,
    0x3C, 0x3C, 0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x36, 0x20, 0x30,
    0x20, 0x52, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20, 0x31, 0x31, 0x20,
    0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20, 0x2F, 0x41,
    0x6E, 0x6E, 0x6F, 0x74, 0x73, 0x20, 0x5B, 0x31, 0x32, 0x20, 0x30, 0x20,
    0x52, 0x20, 0x31, 0x33, 0x20, 0x30, 0x20, 0x52, 0x20, 0x31, 0x34, 0x20,
    0x30, 0x20, 0x52, 0x5D, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F,
    0x62, 0x6A, 0x0A, 0x34, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C,
    0x3C, 0x20, 0x2F, 0x46, 0x54, 0x20, 0x2F, 0x42, 0x74, 0x6E, 0x20, 0x2F,
    0x46, 0x66, 0x20, 0x33, 0x32, 0x37, 0x36, 0x38, 0x20, 0x2F, 0x54, 0x20,
    0x28, 0x70, 0x6F, 0x73, 0x74, 0x69, 0x6E, 0x67, 0x29, 0x20, 0x2F, 0x56,
    0x20, 0x2F, 0x4F, 0x66, 0x66, 0x20, 0x2F, 0x4B, 0x69, 0x64, 0x73, 0x20,
    0x5B, 0x31, 0x32, 0x20, 0x30, 0x20, 0x52, 0x20, 0x31, 0x33, 0x20, 0x30,
    0x20, 0x52, 0x20, 0x31, 0x34, 0x20, 0x30, 0x20, 0x52, 0x5D, 0x20, 0x3E,
    0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x35, 0x20, 0x30,
    0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x4C, 0x65, 0x6E,
    0x67, 0x74, 0x68, 0x20, 0x31, 0x36, 0x39, 0x20, 0x3E, 0x3E, 0x0A, 0x73,
    0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x42, 0x54, 0x20, 0x2F, 0x48, 0x65,
    0x6C, 0x76, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66, 0x20, 0x37, 0x32, 0x20,
    0x36, 0x35, 0x36, 0x20, 0x54, 0x64, 0x20, 0x28, 0x43, 0x6F, 0x6E, 0x74,
    0x61, 0x63, 0x74, 0x20, 0x6D, 0x65, 0x20, 0x62, 0x79, 0x3A, 0x29, 0x20,
    0x54, 0x6A, 0x20, 0x45, 0x54, 0x0A, 0x42, 0x54, 0x20, 0x2F, 0x48, 0x65,
    0x6C, 0x76, 0x20, 0x31, 0x30, 0x20, 0x54, 0x66, 0x20, 0x31, 0x37, 0x32,
    0x20, 0x36, 0x35, 0x36, 0x20, 0x54, 0x64, 0x20, 0x28, 0x65, 0x6D, 0x61,
    0x69, 0x6C, 0x29, 0x20, 0x54, 0x6A, 0x20, 0x45, 0x54, 0x0A, 0x42, 0x54,
    0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x31, 0x30, 0x20, 0x54, 0x66,
    0x20, 0x32, 0x37, 0x32, 0x20, 0x36, 0x35, 0x36, 0x20, 0x54, 0x64, 0x20,
    0x28, 0x70, 0x6F, 0x73, 0x74, 0x29, 0x20, 0x54, 0x6A, 0x20, 0x45, 0x54,
    0x0A, 0x42, 0x54, 0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x20, 0x31, 0x30,
    0x20, 0x54, 0x66, 0x20, 0x33, 0x37, 0x32, 0x20, 0x36, 0x35, 0x36, 0x20,
    0x54, 0x64, 0x20, 0x28, 0x6E, 0x65, 0x69, 0x74, 0x68, 0x65, 0x72, 0x29,
    0x20, 0x54, 0x6A, 0x20, 0x45, 0x54, 0x0A, 0x65, 0x6E, 0x64, 0x73, 0x74,
    0x72, 0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A,
    0x36, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F,
    0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x46, 0x6F, 0x6E, 0x74, 0x20, 0x2F,
    0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x54, 0x79, 0x70,
    0x65, 0x31, 0x20, 0x2F, 0x42, 0x61, 0x73, 0x65, 0x46, 0x6F, 0x6E, 0x74,
    0x20, 0x2F, 0x48, 0x65, 0x6C, 0x76, 0x65, 0x74, 0x69, 0x63, 0x61, 0x20,
    0x2F, 0x45, 0x6E, 0x63, 0x6F, 0x64, 0x69, 0x6E, 0x67, 0x20, 0x2F, 0x57,
    0x69, 0x6E, 0x41, 0x6E, 0x73, 0x69, 0x45, 0x6E, 0x63, 0x6F, 0x64, 0x69,
    0x6E, 0x67, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A,
    0x0A, 0x39, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20,
    0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x58, 0x4F, 0x62, 0x6A, 0x65,
    0x63, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20,
    0x2F, 0x46, 0x6F, 0x72, 0x6D, 0x20, 0x2F, 0x42, 0x42, 0x6F, 0x78, 0x20,
    0x5B, 0x30, 0x20, 0x30, 0x20, 0x31, 0x38, 0x20, 0x31, 0x38, 0x5D, 0x20,
    0x2F, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x20, 0x34, 0x20, 0x3E, 0x3E,
    0x0A, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x71, 0x20, 0x51, 0x0A,
    0x65, 0x6E, 0x64, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E,
    0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x31, 0x31, 0x20, 0x30, 0x20, 0x6F, 0x62,
    0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F,
    0x46, 0x6F, 0x6E, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70,
    0x65, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x31, 0x20, 0x2F, 0x42, 0x61,
    0x73, 0x65, 0x46, 0x6F, 0x6E, 0x74, 0x20, 0x2F, 0x5A, 0x61, 0x70, 0x66,
    0x44, 0x69, 0x6E, 0x67, 0x62, 0x61, 0x74, 0x73, 0x20, 0x3E, 0x3E, 0x0A,
    0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x31, 0x32, 0x20, 0x30, 0x20,
    0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65,
    0x20, 0x2F, 0x41, 0x6E, 0x6E, 0x6F, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62,
    0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x57, 0x69, 0x64, 0x67, 0x65, 0x74,
    0x20, 0x2F, 0x50, 0x61, 0x72, 0x65, 0x6E, 0x74, 0x20, 0x34, 0x20, 0x30,
    0x20, 0x52, 0x20, 0x2F, 0x46, 0x54, 0x20, 0x2F, 0x42, 0x74, 0x6E, 0x20,
    0x2F, 0x46, 0x66, 0x20, 0x33, 0x32, 0x37, 0x36, 0x38, 0x20, 0x2F, 0x52,
    0x65, 0x63, 0x74, 0x20, 0x5B, 0x31, 0x35, 0x30, 0x20, 0x36, 0x35, 0x30,
    0x20, 0x31, 0x36, 0x38, 0x20, 0x36, 0x36, 0x38, 0x5D, 0x20, 0x2F, 0x46,
    0x20, 0x34, 0x20, 0x2F, 0x50, 0x20, 0x33, 0x20, 0x30, 0x20, 0x52, 0x20,
    0x2F, 0x41, 0x53, 0x20, 0x2F, 0x4F, 0x66, 0x66, 0x20, 0x2F, 0x41, 0x50,
    0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x4E, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x45,
    0x6D, 0x61, 0x69, 0x6C, 0x20, 0x32, 0x32, 0x20, 0x30, 0x20, 0x52, 0x20,
    0x2F, 0x4F, 0x66, 0x66, 0x20, 0x39, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E,
    0x3E, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F,
    0x62, 0x6A, 0x0A, 0x31, 0x33, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A,
    0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x41, 0x6E,
    0x6E, 0x6F, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65,
    0x20, 0x2F, 0x57, 0x69, 0x64, 0x67, 0x65, 0x74, 0x20, 0x2F, 0x50, 0x61,
    0x72, 0x65, 0x6E, 0x74, 0x20, 0x34, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F,
    0x46, 0x54, 0x20, 0x2F, 0x42, 0x74, 0x6E, 0x20, 0x2F, 0x46, 0x66, 0x20,
    0x33, 0x32, 0x37, 0x36, 0x38, 0x20, 0x2F, 0x52, 0x65, 0x63, 0x74, 0x20,
    0x5B, 0x32, 0x35, 0x30, 0x20, 0x36, 0x35, 0x30, 0x20, 0x32, 0x36, 0x38,
    0x20, 0x36, 0x36, 0x38, 0x5D, 0x20, 0x2F, 0x46, 0x20, 0x34, 0x20, 0x2F,
    0x50, 0x20, 0x33, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x41, 0x53, 0x20,
    0x2F, 0x4F, 0x66, 0x66, 0x20, 0x2F, 0x41, 0x50, 0x20, 0x3C, 0x3C, 0x20,
    0x2F, 0x4E, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x50, 0x6F, 0x73, 0x74, 0x20,
    0x32, 0x33, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x4F, 0x66, 0x66, 0x20,
    0x39, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20,
    0x3E, 0x3E, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x31, 0x34,
    0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54,
    0x79, 0x70, 0x65, 0x20, 0x2F, 0x41, 0x6E, 0x6E, 0x6F, 0x74, 0x20, 0x2F,
    0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x57, 0x69, 0x64,
    0x67, 0x65, 0x74, 0x20, 0x2F, 0x50, 0x61, 0x72, 0x65, 0x6E, 0x74, 0x20,
    0x34, 0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x46, 0x54, 0x20, 0x2F, 0x42,
    0x74, 0x6E, 0x20, 0x2F, 0x46, 0x66, 0x20, 0x33, 0x32, 0x37, 0x36, 0x38,
    0x20, 0x2F, 0x52, 0x65, 0x63, 0x74, 0x20, 0x5B, 0x33, 0x35, 0x30, 0x20,
    0x36, 0x35, 0x30, 0x20, 0x33, 0x36, 0x38, 0x20, 0x36, 0x36, 0x38, 0x5D,
    0x20, 0x2F, 0x46, 0x20, 0x34, 0x20, 0x2F, 0x50, 0x20, 0x33, 0x20, 0x30,
    0x20, 0x52, 0x20, 0x2F, 0x41, 0x53, 0x20, 0x2F, 0x4F, 0x66, 0x66, 0x20,
    0x2F, 0x41, 0x50, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x4E, 0x20, 0x3C, 0x3C,
    0x20, 0x2F, 0x4E, 0x65, 0x69, 0x74, 0x68, 0x65, 0x72, 0x20, 0x32, 0x34,
    0x20, 0x30, 0x20, 0x52, 0x20, 0x2F, 0x4F, 0x66, 0x66, 0x20, 0x39, 0x20,
    0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E,
    0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x32, 0x32, 0x20, 0x30,
    0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70,
    0x65, 0x20, 0x2F, 0x58, 0x4F, 0x62, 0x6A, 0x65, 0x63, 0x74, 0x20, 0x2F,
    0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x46, 0x6F, 0x72,
    0x6D, 0x20, 0x2F, 0x42, 0x42, 0x6F, 0x78, 0x20, 0x5B, 0x30, 0x20, 0x30,
    0x20, 0x31, 0x38, 0x20, 0x31, 0x38, 0x5D, 0x20, 0x2F, 0x52, 0x65, 0x73,
    0x6F, 0x75, 0x72, 0x63, 0x65, 0x73, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x46,
    0x6F, 0x6E, 0x74, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62,
    0x20, 0x31, 0x31, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E,
    0x3E, 0x20, 0x2F, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x20, 0x34, 0x35,
    0x20, 0x3E, 0x3E, 0x0A, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x71,
    0x20, 0x30, 0x20, 0x30, 0x20, 0x30, 0x20, 0x72, 0x67, 0x20, 0x42, 0x54,
    0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66,
    0x20, 0x33, 0x20, 0x33, 0x20, 0x54, 0x64, 0x20, 0x28, 0x6C, 0x29, 0x20,
    0x54, 0x6A, 0x20, 0x45, 0x54, 0x20, 0x51, 0x0A, 0x65, 0x6E, 0x64, 0x73,
    0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A,
    0x0A, 0x32, 0x33, 0x20, 0x30, 0x20, 0x6F, 0x62, 0x6A, 0x0A, 0x3C, 0x3C,
    0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x58, 0x4F, 0x62, 0x6A,
    0x65, 0x63, 0x74, 0x20, 0x2F, 0x53, 0x75, 0x62, 0x74, 0x79, 0x70, 0x65,
    0x20, 0x2F, 0x46, 0x6F, 0x72, 0x6D, 0x20, 0x2F, 0x42, 0x42, 0x6F, 0x78,
    0x20, 0x5B, 0x30, 0x20, 0x30, 0x20, 0x31, 0x38, 0x20, 0x31, 0x38, 0x5D,
    0x20, 0x2F, 0x52, 0x65, 0x73, 0x6F, 0x75, 0x72, 0x63, 0x65, 0x73, 0x20,
    0x3C, 0x3C, 0x20, 0x2F, 0x46, 0x6F, 0x6E, 0x74, 0x20, 0x3C, 0x3C, 0x20,
    0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20, 0x31, 0x31, 0x20, 0x30, 0x20, 0x52,
    0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20, 0x2F, 0x4C, 0x65, 0x6E, 0x67,
    0x74, 0x68, 0x20, 0x34, 0x35, 0x20, 0x3E, 0x3E, 0x0A, 0x73, 0x74, 0x72,
    0x65, 0x61, 0x6D, 0x0A, 0x71, 0x20, 0x30, 0x20, 0x30, 0x20, 0x30, 0x20,
    0x72, 0x67, 0x20, 0x42, 0x54, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20,
    0x31, 0x32, 0x20, 0x54, 0x66, 0x20, 0x33, 0x20, 0x33, 0x20, 0x54, 0x64,
    0x20, 0x28, 0x6C, 0x29, 0x20, 0x54, 0x6A, 0x20, 0x45, 0x54, 0x20, 0x51,
    0x0A, 0x65, 0x6E, 0x64, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x65,
    0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x32, 0x34, 0x20, 0x30, 0x20, 0x6F,
    0x62, 0x6A, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x54, 0x79, 0x70, 0x65, 0x20,
    0x2F, 0x58, 0x4F, 0x62, 0x6A, 0x65, 0x63, 0x74, 0x20, 0x2F, 0x53, 0x75,
    0x62, 0x74, 0x79, 0x70, 0x65, 0x20, 0x2F, 0x46, 0x6F, 0x72, 0x6D, 0x20,
    0x2F, 0x42, 0x42, 0x6F, 0x78, 0x20, 0x5B, 0x30, 0x20, 0x30, 0x20, 0x31,
    0x38, 0x20, 0x31, 0x38, 0x5D, 0x20, 0x2F, 0x52, 0x65, 0x73, 0x6F, 0x75,
    0x72, 0x63, 0x65, 0x73, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x46, 0x6F, 0x6E,
    0x74, 0x20, 0x3C, 0x3C, 0x20, 0x2F, 0x5A, 0x61, 0x44, 0x62, 0x20, 0x31,
    0x31, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x20, 0x3E, 0x3E, 0x20,
    0x2F, 0x4C, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x20, 0x34, 0x35, 0x20, 0x3E,
    0x3E, 0x0A, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6D, 0x0A, 0x71, 0x20, 0x30,
    0x20, 0x30, 0x20, 0x30, 0x20, 0x72, 0x67, 0x20, 0x42, 0x54, 0x20, 0x2F,
    0x5A, 0x61, 0x44, 0x62, 0x20, 0x31, 0x32, 0x20, 0x54, 0x66, 0x20, 0x33,
    0x20, 0x33, 0x20, 0x54, 0x64, 0x20, 0x28, 0x6C, 0x29, 0x20, 0x54, 0x6A,
    0x20, 0x45, 0x54, 0x20, 0x51, 0x0A, 0x65, 0x6E, 0x64, 0x73, 0x74, 0x72,
    0x65, 0x61, 0x6D, 0x0A, 0x65, 0x6E, 0x64, 0x6F, 0x62, 0x6A, 0x0A, 0x78,
    0x72, 0x65, 0x66, 0x0A, 0x31, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x39, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x32, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x31, 0x35, 0x38, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x33, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x32, 0x31, 0x35, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x34, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x33, 0x38, 0x37, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x35, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x34, 0x37, 0x37, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x36, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x36, 0x39, 0x36, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x39, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x37, 0x39, 0x33, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x20, 0x6E, 0x20, 0x0A, 0x31, 0x31, 0x20, 0x31, 0x0A, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x38, 0x39, 0x33, 0x20, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x20, 0x6E, 0x20, 0x0A, 0x31, 0x32, 0x20, 0x31, 0x0A, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x39, 0x36, 0x37, 0x20, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x31, 0x33, 0x20, 0x31, 0x0A, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x31, 0x34, 0x33, 0x20, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x31, 0x34, 0x20, 0x31, 0x0A,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x33, 0x31, 0x38, 0x20, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x32, 0x32, 0x20, 0x31,
    0x0A, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x34, 0x39, 0x36, 0x20,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x32, 0x33, 0x20,
    0x31, 0x0A, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x36, 0x38, 0x31,
    0x20, 0x30, 0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x32, 0x34,
    0x20, 0x31, 0x0A, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x38, 0x36,
    0x36, 0x20, 0x30, 0x30, 0x30, 0x30, 0x30, 0x20, 0x6E, 0x20, 0x0A, 0x74,
    0x72, 0x61, 0x69, 0x6C, 0x65, 0x72, 0x0A, 0x3C, 0x3C, 0x20, 0x2F, 0x53,
    0x69, 0x7A, 0x65, 0x20, 0x32, 0x35, 0x20, 0x2F, 0x52, 0x6F, 0x6F, 0x74,
    0x20, 0x31, 0x20, 0x30, 0x20, 0x52, 0x20, 0x3E, 0x3E, 0x0A, 0x73, 0x74,
    0x61, 0x72, 0x74, 0x78, 0x72, 0x65, 0x66, 0x0A, 0x32, 0x30, 0x35, 0x31,
    0x0A, 0x25, 0x25, 0x45, 0x4F, 0x46, 0x0A
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

// ---------------------------------------------------------------------------
// Checking a signature that is already in a file
//
// The reverse of writing one: find `/ByteRange` and `/Contents`, take the
// signature out of the hole, and hand the two covered ranges to Windows. What
// comes back is "these bytes have not changed", which is a smaller claim than
// most readers' green ticks imply -- see the report's comment.
// ---------------------------------------------------------------------------

extern "C" BOOL PdfForm_CheckSignature(const WCHAR* path, PdfSignatureReport* out) {
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) return FALSE;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 32 ||
        size.QuadPart > 256 * 1024 * 1024) {
        CloseHandle(file);
        return FALSE;
    }

    size_t len = (size_t)size.QuadPart;
    BYTE* bytes = (BYTE*)malloc(len);
    DWORD read = 0;

    BOOL ok = bytes && ReadFile(file, bytes, (DWORD)len, &read, NULL) && read == (DWORD)len;
    CloseHandle(file);

    if (!ok) {
        free(bytes);
        return FALSE;
    }

    const char* range = FindLast((const char*)bytes, len, "/ByteRange");
    if (!range) {
        free(bytes);
        return TRUE;        // no signature is not a failure to look
    }

    out->present = TRUE;

    size_t numbers[4] = { 0, 0, 0, 0 };
    const char* at = range + 10;
    const char* end = (const char*)bytes + len;

    for (int i = 0; i < 4; i++) {
        while (at < end && (*at < 48 || *at > 57)) at++;
        while (at < end && *at >= 48 && *at <= 57) {
            numbers[i] = numbers[i] * 10 + (size_t)(*at - 48);
            at++;
        }
    }

    if (numbers[1] >= len || numbers[2] > len || numbers[2] <= numbers[1]) {
        free(bytes);
        return TRUE;
    }

    // Anything after the range's end is content the signature never saw --
    // which is legal, and worth saying out loud.
    out->coversWholeFile = (numbers[2] + numbers[3] == len);

    const char* hole = (const char*)bytes + numbers[1];
    if (*hole != '<') {
        free(bytes);
        return TRUE;
    }

    size_t holeLen = numbers[2] - numbers[1];
    BYTE* signature = (BYTE*)malloc(holeLen / 2 + 1);
    size_t signatureLen = 0;

    for (size_t i = 1; signature && i + 1 < holeLen - 1; i += 2) {
        int high = hole[i], low = hole[i + 1];

        int hi = (high >= 48 && high <= 57) ? high - 48
               : (high >= 65 && high <= 70) ? high - 65 + 10
               : (high >= 97 && high <= 102) ? high - 97 + 10 : -1;
        int lo = (low >= 48 && low <= 57) ? low - 48
               : (low >= 65 && low <= 70) ? low - 65 + 10
               : (low >= 97 && low <= 102) ? low - 97 + 10 : -1;
        if (hi < 0 || lo < 0) break;

        signature[signatureLen++] = (BYTE)((hi << 4) | lo);
    }

    // The hole is padded with zeros to a fixed size; the signature itself ends
    // where its own encoding says, and the padding is not part of it.
    while (signatureLen > 0 && signature[signatureLen - 1] == 0) signatureLen--;

    if (signature && signatureLen > 0) {
        out->intact = PdfSign_VerifyDetachedNamed(
            signature, signatureLen,
            bytes, numbers[1],
            bytes + numbers[2], numbers[3],
            out->signer, 256, &out->trust);

        out->timestamped = PdfSign_ReadTimestamp(signature, signatureLen,
                                                 &out->signedAt,
                                                 out->timestampAuthority, 256);
    }

    free(signature);
    free(bytes);
    return TRUE;
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

    // --- a line typed onto a page with nowhere to type ---
    //
    // The scanned-form case: no field, so the text goes on the page. What is
    // checked is that it is still a PDF afterwards and that the characters are
    // in the file -- an annotation that draws the text but does not carry it
    // is a picture of an answer nobody can read back.
    form = PdfForm_Open(path, &why);
    if (!form) FAIL("the form could not be opened for typing on");

    if (!PdfForm_StampText(form, 0, L"Typed on the page", 72.0f, 300.0f, 11.0f)) {
        FAIL("nothing could be typed onto the page");
    }

    // A page that does not exist is not a place to type.
    if (PdfForm_StampText(form, 4, L"nowhere", 10.0f, 10.0f, 11.0f)) {
        FAIL("text went onto a page the file does not have");
    }

    if (!PdfForm_Save(form, filled)) FAIL("the typed-on file could not be written");
    PdfForm_Close(form);
    form = NULL;

    rendered = Pdf_Open(filled);
    if (!rendered) FAIL("Windows would not open the typed-on file");

    int typedPages = Pdf_PageCount(rendered);
    Pdf_Close(rendered);
    if (typedPages != 1) FAIL("the typed-on file did not come back as one page");

    {
        HANDLE check = CreateFileW(filled, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (check == INVALID_HANDLE_VALUE) FAIL("the typed-on file vanished");

        LARGE_INTEGER size = {};
        GetFileSizeEx(check, &size);

        char* bytes = (char*)malloc((size_t)size.QuadPart + 1);
        DWORD read = 0;
        BOOL ok = bytes && ReadFile(check, bytes, (DWORD)size.QuadPart, &read, NULL);
        CloseHandle(check);

        if (!ok) {
            free(bytes);
            FAIL("the typed-on file could not be read back");
        }
        bytes[read] = 0;

        BOOL carried = FindLast(bytes, read, "(Typed on the page)") != NULL;
        BOOL drawn = FindLast(bytes, read, "/FreeText") != NULL;
        free(bytes);

        if (!drawn)   FAIL("the typed text was not put on the page");
        if (!carried) FAIL("the annotation draws the text but does not carry it");
    }

    // --- a signature over the file it is part of ---
    //
    // The one that matters: the byte range has to cover the document, and the
    // signature has to verify against the bytes on disk. Both are checked the
    // way a reader checks them -- by reading the file back and doing the sums
    // again -- rather than by trusting the call that wrote it.
    {
        PdfCertificate certificate = PdfSign_TemporaryCertificate();
        if (!certificate) FAIL("no certificate to sign the test file with");

        form = PdfForm_Open(path, &why);
        if (!form) {
            PdfSign_DiscardTemporary(certificate);
            FAIL("the form could not be opened for signing");
        }

        // No timestamp: a self-check that needs somebody else's web server is
        // a self-check that fails on a train.
        PdfForm_SetTimestampUrl(form, L"");

        PdfForm_SignWithCertificate(form, certificate, L"opennote self-check",
                                    L"checking that a signature covers the file");

        BOOL saved = PdfForm_Save(form, filled);
        PdfForm_Close(form);
        form = NULL;

        if (!saved) {
            PdfSign_DiscardTemporary(certificate);
            FAIL("the signed file could not be written");
        }

        // Read it back as bytes, the way anybody verifying it would.
        HANDLE signedFile = CreateFileW(filled, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (signedFile == INVALID_HANDLE_VALUE) {
            PdfSign_DiscardTemporary(certificate);
            FAIL("the signed file could not be read back");
        }

        LARGE_INTEGER signedSize = {};
        GetFileSizeEx(signedFile, &signedSize);

        BYTE* signedBytes = (BYTE*)malloc((size_t)signedSize.QuadPart);
        DWORD got = 0;

        BOOL readOk = signedBytes &&
                      ReadFile(signedFile, signedBytes, (DWORD)signedSize.QuadPart, &got, NULL) &&
                      got == (DWORD)signedSize.QuadPart;
        CloseHandle(signedFile);

        if (!readOk) {
            free(signedBytes);
            PdfSign_DiscardTemporary(certificate);
            FAIL("the signed file came back short");
        }

        size_t signedLen = (size_t)signedSize.QuadPart;

        // /ByteRange [0 a b c] -- four numbers saying what is covered.
        const char* range = FindLast((const char*)signedBytes, signedLen, "/ByteRange");
        if (!range) {
            free(signedBytes);
            PdfSign_DiscardTemporary(certificate);
            FAIL("the signed file has no byte range in it");
        }

        size_t numbers[4] = { 0, 0, 0, 0 };
        const char* at = range + 10;
        for (int i = 0; i < 4; i++) {
            while (at < (const char*)signedBytes + signedLen &&
                   (*at < 48 || *at > 57)) at++;
            while (at < (const char*)signedBytes + signedLen && *at >= 48 && *at <= 57) {
                numbers[i] = numbers[i] * 10 + (size_t)(*at - 48);
                at++;
            }
        }

        // The two covered halves have to be the whole file bar the hole.
        if (numbers[0] != 0 ||
            numbers[1] + numbers[3] + (numbers[2] - numbers[1]) != signedLen ||
            numbers[2] != numbers[1] + (numbers[2] - numbers[1])) {
            free(signedBytes);
            PdfSign_DiscardTemporary(certificate);
            FAIL("the byte range does not cover the file");
        }

        // The signature lives in the hole, as hex between angle brackets.
        const char* hole = (const char*)signedBytes + numbers[1];
        if (*hole != '<') {
            free(signedBytes);
            PdfSign_DiscardTemporary(certificate);
            FAIL("the byte range does not start at the signature");
        }

        size_t holeLen = numbers[2] - numbers[1];
        BYTE* signature = (BYTE*)malloc(holeLen / 2);
        size_t signatureLen = 0;

        for (size_t i = 1; i + 1 < holeLen - 1 && signature; i += 2) {
            int high = hole[i], low = hole[i + 1];

            int hi = (high >= '0' && high <= '9') ? high - '0'
                   : (high >= 'A' && high <= 'F') ? high - 'A' + 10 : -1;
            int lo = (low >= '0' && low <= '9') ? low - '0'
                   : (low >= 'A' && low <= 'F') ? low - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) break;

            signature[signatureLen++] = (BYTE)((hi << 4) | lo);
        }

        // The trailing zeros are padding rather than signature; DER says how
        // long the real thing is, and a verify over the lot would fail.
        while (signatureLen > 0 && signature[signatureLen - 1] == 0) signatureLen--;

        BOOL verified = signature && signatureLen > 0 && PdfSign_VerifyDetached(
            signature, signatureLen,
            signedBytes, numbers[1],
            signedBytes + numbers[2], numbers[3]);

        if (!verified) {
            free(signature);
            free(signedBytes);
            PdfSign_DiscardTemporary(certificate);
            FAIL("the signature does not verify against the file it is in");
        }

        // A signed file is still a file anybody can open: a signature that
        // costs you the document is not worth making.
        PdfFile* opened = Pdf_Open(filled);
        if (!opened) {
            free(signature);
            free(signedBytes);
            PdfSign_DiscardTemporary(certificate);
            FAIL("Windows would not open the signed file");
        }
        Pdf_Close(opened);

        // ...and it is a signature over the document: change a byte of the
        // document and it has to stop verifying.
        signedBytes[10] = (BYTE)(signedBytes[10] ^ 0xFF);

        BOOL stillVerifies = PdfSign_VerifyDetached(
            signature, signatureLen,
            signedBytes, numbers[1],
            signedBytes + numbers[2], numbers[3]);

        free(signature);
        PdfSign_DiscardTemporary(certificate);

        if (stillVerifies) {
            free(signedBytes);
            FAIL("a changed document still verified");
        }

        // The same questions, asked the way the program asks them of a file
        // somebody hands it.
        PdfSignatureReport report = {0};
        if (!PdfForm_CheckSignature(filled, &report)) {
            free(signedBytes);
            FAIL("the signed file could not be checked");
        }

        if (!report.present)  { free(signedBytes); FAIL("the signature was not found"); }
        if (!report.intact)   { free(signedBytes); FAIL("the signature did not check out"); }
        if (!report.coversWholeFile) {
            free(signedBytes);
            FAIL("the signature was reported as covering less than the file");
        }
        if (!wcsstr(report.signer, L"opennote self-check")) {
            free(signedBytes);
            FAIL("the signer was not reported");
        }

        // Intact and untrusted at the same time, which is exactly what a
        // self-signed certificate should produce.
        // The self-check signs without asking an authority, so there should
        // be no date on it. A date here would mean one was read out of a
        // signature that never had one.
        if (report.timestamped) {
            free(signedBytes);
            FAIL("a signature made without a timestamp came back with a date");
        }

        if (report.trust == PDFTRUST_TRUSTED) {
            free(signedBytes);
            FAIL("a certificate made for the test was reported as trusted");
        }

        // ...and on a file somebody has since edited. The tampered bytes are
        // already in hand; written out, they have to fail the same check.
        WCHAR tamperedPath[MAX_PATH];
        swprintf_s(tamperedPath, MAX_PATH, L"%sopennote-selftest-tampered.pdf", temp);

        BOOL wroteTampered = WriteBytes(tamperedPath, signedBytes, signedLen);
        free(signedBytes);

        if (wroteTampered) {
            PdfSignatureReport after = {0};
            BOOL checked = PdfForm_CheckSignature(tamperedPath, &after);
            DeleteFileW(tamperedPath);

            if (!checked) FAIL("the tampered file could not be checked");
            if (!after.present) FAIL("the tampered file lost its signature");
            if (after.intact) FAIL("a tampered file was reported as unchanged");
        }
    }

    // --- the other two kinds of field ---
    if (!WriteBytes(path, MIXED_FORM, sizeof(MIXED_FORM))) {
        FAIL("could not write the mixed test form");
    }

    form = PdfForm_Open(path, &why);
    if (!form) FAIL("the mixed form could not be opened");

    if (PdfForm_FieldCount(form) != 3) FAIL("the mixed form's three fields were not found");

    int textField = -1, tickField = -1, listField = -1;
    for (int i = 0; i < PdfForm_FieldCount(form); i++) {
        switch (PdfForm_FieldKind(form, i)) {
            case PDF_FIELD_TEXT:     textField = i; break;
            case PDF_FIELD_CHECKBOX: tickField = i; break;
            case PDF_FIELD_CHOICE:   listField = i; break;
            default: break;
        }
    }

    if (textField < 0) FAIL("the text field was not recognised");
    if (tickField < 0) FAIL("the tick box was not recognised");
    if (listField < 0) FAIL("the choice list was not recognised");

    if (PdfForm_FieldChecked(form, tickField)) FAIL("an unticked box came back ticked");

    if (PdfForm_FieldOptionCount(form, listField) != 3) {
        FAIL("the choice list did not offer its three options");
    }
    if (wcscmp(PdfForm_FieldOption(form, listField, 1), L"Legal") != 0) {
        FAIL("the choice list's options came back wrong");
    }

    // A tick box takes no typed value, and a text box takes no tick.
    if (PdfForm_SetFieldValue(form, tickField, L"Yes")) {
        FAIL("a tick box accepted a typed value");
    }
    if (PdfForm_SetFieldChecked(form, textField, TRUE)) {
        FAIL("a text box accepted a tick");
    }

    if (!PdfForm_SetFieldChecked(form, tickField, TRUE)) FAIL("the box would not tick");
    if (!PdfForm_SetFieldValue(form, listField, L"Legal")) FAIL("the list would not take a value");
    if (!PdfForm_SetFieldValue(form, textField, L"Grace Hopper")) FAIL("the text would not take");

    if (!PdfForm_Save(form, filled)) FAIL("the filled mixed form could not be written");
    PdfForm_Close(form);
    form = NULL;

    form = PdfForm_Open(filled, &why);
    if (!form) FAIL("the filled mixed form could not be opened again");

    for (int i = 0; i < PdfForm_FieldCount(form); i++) {
        switch (PdfForm_FieldKind(form, i)) {
            case PDF_FIELD_CHECKBOX:
                if (!PdfForm_FieldChecked(form, i)) FAIL("the tick did not survive the save");
                break;
            case PDF_FIELD_CHOICE:
                if (wcscmp(PdfForm_FieldValue(form, i), L"Legal") != 0) {
                    FAIL("the chosen option did not survive the save");
                }
                break;
            case PDF_FIELD_TEXT:
                if (wcscmp(PdfForm_FieldValue(form, i), L"Grace Hopper") != 0) {
                    FAIL("the typed value did not survive the save");
                }
                break;
            default:
                break;
        }
    }

    PdfForm_Close(form);
    form = NULL;

    // The tick box has to keep the drawings the form gave it: a saved form
    // whose tick was replaced with something of ours would still say it was
    // ticked and show nothing.
    {
        HANDLE check = CreateFileW(filled, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (check == INVALID_HANDLE_VALUE) FAIL("the filled mixed form vanished");

        LARGE_INTEGER size = {};
        GetFileSizeEx(check, &size);

        char* bytes = (char*)malloc((size_t)size.QuadPart + 1);
        DWORD read = 0;
        BOOL ok = bytes && ReadFile(check, bytes, (DWORD)size.QuadPart, &read, NULL);
        CloseHandle(check);

        if (!ok) {
            free(bytes);
            FAIL("the filled mixed form could not be read back");
        }
        bytes[read] = 0;

        const char* ticked = FindLast(bytes, read, "/AS /On");
        BOOL keptStates = FindLast(bytes, read, "/Off 9 0 R") != NULL ||
                          FindLast(bytes, read, "/On 8 0 R") != NULL;
        free(bytes);

        if (!ticked) FAIL("the tick box does not say which state it is showing");
        if (!keptStates) FAIL("the tick box lost the drawings it came with");
    }

    // --- a radio group is one question ---
    if (!WriteBytes(path, RADIO_FORM, sizeof(RADIO_FORM))) {
        FAIL("could not write the radio test form");
    }

    form = PdfForm_Open(path, &why);
    if (!form) FAIL("the radio form could not be opened");

    if (PdfForm_FieldCount(form) != 1) FAIL("the radio group did not come back as one field");
    if (PdfForm_FieldKind(form, 0) != PDF_FIELD_RADIO) FAIL("the radio group was not recognised");
    if (wcscmp(PdfForm_FieldName(form, 0), L"posting") != 0) FAIL("the group lost its name");

    if (PdfForm_FieldOptionCount(form, 0) != 3) FAIL("the group did not offer three buttons");
    if (wcscmp(PdfForm_FieldOption(form, 0, 1), L"Post") != 0) {
        FAIL("the group's buttons came back wrong");
    }

    // Only what the buttons are called: anything else would answer the
    // question in the file and leave the page blank.
    if (PdfForm_SetFieldValue(form, 0, L"Carrier pigeon")) {
        FAIL("the group took an answer it has no button for");
    }
    if (!PdfForm_SetFieldValue(form, 0, L"Post")) FAIL("the group would not take one of its own");

    if (!PdfForm_Save(form, filled)) FAIL("the answered radio form could not be written");
    PdfForm_Close(form);
    form = NULL;

    form = PdfForm_Open(filled, &why);
    if (!form) FAIL("the answered radio form could not be opened again");
    if (wcscmp(PdfForm_FieldValue(form, 0), L"Post") != 0) {
        FAIL("the answer did not survive the save");
    }
    PdfForm_Close(form);
    form = NULL;

    // The buttons have to agree with the field: the chosen one showing, the
    // others off. A file where the field says Post and every button says Off
    // is answered on paper and blank on the page.
    {
        HANDLE check = CreateFileW(filled, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (check == INVALID_HANDLE_VALUE) FAIL("the answered radio form vanished");

        LARGE_INTEGER size = {};
        GetFileSizeEx(check, &size);

        char* bytes = (char*)malloc((size_t)size.QuadPart + 1);
        DWORD read = 0;
        BOOL ok = bytes && ReadFile(check, bytes, (DWORD)size.QuadPart, &read, NULL);
        CloseHandle(check);

        if (!ok) {
            free(bytes);
            FAIL("the answered radio form could not be read back");
        }
        bytes[read] = 0;

        BOOL chosen = FindLast(bytes, read, "/AS /Post") != NULL;
        BOOL others = FindLast(bytes, read, "/AS /Off") != NULL;
        BOOL onField = FindLast(bytes, read, "/V /Post") != NULL;
        free(bytes);

        if (!onField) FAIL("the group does not carry the answer");
        if (!chosen)  FAIL("the chosen button is not showing");
        if (!others)  FAIL("the other buttons were not turned off");
    }

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
