// DEFLATE, decompressed.
//
// The shape of it, for anyone reading this who has not met the format: the
// input is a stream of bits, least significant first, holding blocks. A block
// is either stored (copy these bytes), fixed Huffman (use the code lengths the
// standard names) or dynamic Huffman (here are the code lengths, then the
// data). Inside a compressed block, a symbol is either a literal byte or a
// "go back D bytes and copy L of them", which is why the last 32k of output
// has to stay reachable -- it is where the copies come from.
//
// Everything here is bounded: a code table is a fixed array, the output grows
// by doubling, and every read checks it has bits left, because the input is a
// file somebody else wrote.

#include "supernote.h"
#include "core/inflate.h"

#include <stdlib.h>
#include <string.h>

#define MAX_BITS    15
#define MAX_SYMBOLS 288

// A dynamic block states the literal alphabet's code lengths and the distance
// alphabet's in one run: 286 of one and 30 of the other, read into one array
// before they are split. That total, not the larger alphabet, is what the
// array has to hold.
#define MAX_LENGTHS (286 + 30)

// ---------------------------------------------------------------------------
// Bits
// ---------------------------------------------------------------------------

typedef struct {
    const BYTE* data;
    size_t      len;
    size_t      at;        // next byte
    unsigned    bits;      // held bits, least significant first
    int         count;     // how many of them are real
    BOOL        overrun;
} BitReader;

static unsigned GetBits(BitReader* r, int want) {
    while (r->count < want) {
        if (r->at >= r->len) {
            r->overrun = TRUE;
            return 0;
        }
        r->bits |= (unsigned)r->data[r->at++] << r->count;
        r->count += 8;
    }

    unsigned value = r->bits & ((1u << want) - 1);
    r->bits >>= want;
    r->count -= want;
    return value;
}

static void AlignToByte(BitReader* r) {
    r->bits = 0;
    r->count = 0;
}

// ---------------------------------------------------------------------------
// Huffman
//
// A canonical code: the lengths are given, the codes follow from them. Decoding
// is the textbook walk -- take a bit, and see whether what has been taken so
// far is a code of that length.
// ---------------------------------------------------------------------------

typedef struct {
    unsigned short count[MAX_BITS + 1];     // codes of each length
    unsigned short symbol[MAX_SYMBOLS];     // symbols, ordered by code
} Huffman;

static BOOL BuildHuffman(Huffman* h, const unsigned char* lengths, int count) {
    memset(h->count, 0, sizeof(h->count));

    for (int i = 0; i < count; i++) {
        if (lengths[i] > MAX_BITS) return FALSE;
        h->count[lengths[i]]++;
    }

    // A table of nothing but zero-length codes is legal (an unused alphabet).
    h->count[0] = 0;

    unsigned short offsets[MAX_BITS + 2];
    offsets[1] = 0;
    for (int length = 1; length <= MAX_BITS; length++) {
        offsets[length + 1] = offsets[length] + h->count[length];
    }

    for (int i = 0; i < count; i++) {
        if (lengths[i]) h->symbol[offsets[lengths[i]]++] = (unsigned short)i;
    }
    return TRUE;
}

static int Decode(BitReader* r, const Huffman* h) {
    int code = 0, first = 0, index = 0;

    for (int length = 1; length <= MAX_BITS; length++) {
        code |= (int)GetBits(r, 1);
        if (r->overrun) return -1;

        int count = h->count[length];
        if (code - first < count) return h->symbol[index + (code - first)];

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

typedef struct {
    BYTE*  bytes;
    size_t len, cap;
    BOOL   failed;
} Output;

static BOOL Reserve(Output* out, size_t extra) {
    if (out->failed) return FALSE;
    if (out->len + extra <= out->cap) return TRUE;

    size_t cap = out->cap ? out->cap : 4096;
    while (cap < out->len + extra) {
        if (cap > (size_t)1 << 30) { out->failed = TRUE; return FALSE; }
        cap *= 2;
    }

    BYTE* grown = (BYTE*)realloc(out->bytes, cap);
    if (!grown) { out->failed = TRUE; return FALSE; }

    out->bytes = grown;
    out->cap = cap;
    return TRUE;
}

static void PutByte(Output* out, BYTE b) {
    if (!Reserve(out, 1)) return;
    out->bytes[out->len++] = b;
}

// ---------------------------------------------------------------------------
// The blocks
// ---------------------------------------------------------------------------

static const unsigned short LENGTH_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short DISTANCE_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char DISTANCE_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static BOOL InflateBlock(BitReader* r, Output* out,
                         const Huffman* literals, const Huffman* distances) {
    for (;;) {
        int symbol = Decode(r, literals);
        if (symbol < 0) return FALSE;

        if (symbol < 256) {
            PutByte(out, (BYTE)symbol);
            if (out->failed) return FALSE;
            continue;
        }

        if (symbol == 256) return TRUE;     // end of block

        symbol -= 257;
        if (symbol >= 29) return FALSE;

        size_t length = LENGTH_BASE[symbol] + GetBits(r, LENGTH_EXTRA[symbol]);

        int distSymbol = Decode(r, distances);
        if (distSymbol < 0 || distSymbol >= 30) return FALSE;

        size_t distance = DISTANCE_BASE[distSymbol] + GetBits(r, DISTANCE_EXTRA[distSymbol]);
        if (r->overrun || distance > out->len) return FALSE;

        if (!Reserve(out, length)) return FALSE;

        // Byte at a time on purpose: the copy may overlap itself, which is how
        // a run of one byte is spelled.
        size_t from = out->len - distance;
        for (size_t i = 0; i < length; i++) {
            out->bytes[out->len++] = out->bytes[from + i];
        }
    }
}

static void FixedTables(Huffman* literals, Huffman* distances) {
    unsigned char lengths[MAX_SYMBOLS];

    for (int i = 0; i < 144; i++)   lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    BuildHuffman(literals, lengths, 288);

    for (int i = 0; i < 30; i++) lengths[i] = 5;
    BuildHuffman(distances, lengths, 30);
}

// The dynamic block's own tables, which are themselves Huffman-coded.
static BOOL DynamicTables(BitReader* r, Huffman* literals, Huffman* distances) {
    static const unsigned char ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    int literalCount = (int)GetBits(r, 5) + 257;
    int distanceCount = (int)GetBits(r, 5) + 1;
    int codeCount = (int)GetBits(r, 4) + 4;

    if (r->overrun || literalCount > 286 || distanceCount > 30) return FALSE;

    unsigned char lengths[MAX_LENGTHS];
    memset(lengths, 0, sizeof(lengths));

    for (int i = 0; i < codeCount; i++) lengths[ORDER[i]] = (unsigned char)GetBits(r, 3);
    if (r->overrun) return FALSE;

    Huffman codeTable;
    if (!BuildHuffman(&codeTable, lengths, 19)) return FALSE;

    // The lengths of the real tables, run-length coded.
    memset(lengths, 0, sizeof(lengths));
    int at = 0;
    int wanted = literalCount + distanceCount;

    while (at < wanted) {
        int symbol = Decode(r, &codeTable);
        if (symbol < 0) return FALSE;

        if (symbol < 16) {
            lengths[at++] = (unsigned char)symbol;
            continue;
        }

        int repeat = 0;
        unsigned char value = 0;

        if (symbol == 16) {
            if (at == 0) return FALSE;
            value = lengths[at - 1];
            repeat = 3 + (int)GetBits(r, 2);
        } else if (symbol == 17) {
            repeat = 3 + (int)GetBits(r, 3);
        } else {
            repeat = 11 + (int)GetBits(r, 7);
        }

        if (r->overrun || at + repeat > wanted) return FALSE;
        while (repeat-- > 0) lengths[at++] = value;
    }

    if (lengths[256] == 0) return FALSE;      // no end-of-block code

    unsigned char distanceLengths[30];
    memcpy(distanceLengths, lengths + literalCount, (size_t)distanceCount);

    return BuildHuffman(literals, lengths, literalCount) &&
           BuildHuffman(distances, distanceLengths, distanceCount);
}

BYTE* Inflate_Raw(const BYTE* data, size_t len, size_t* outLen) {
    if (outLen) *outLen = 0;
    if (!data || len == 0) return NULL;

    BitReader r = { data, len, 0, 0, 0, FALSE };
    Output out = { NULL, 0, 0, FALSE };

    for (;;) {
        unsigned last = GetBits(&r, 1);
        unsigned type = GetBits(&r, 2);
        if (r.overrun) break;

        if (type == 0) {
            // Stored: a length, its complement, then the bytes.
            AlignToByte(&r);
            if (r.at + 4 > r.len) break;

            unsigned length = (unsigned)data[r.at] | ((unsigned)data[r.at + 1] << 8);
            r.at += 4;

            if (r.at + length > r.len) break;
            if (!Reserve(&out, length)) break;

            memcpy(out.bytes + out.len, data + r.at, length);
            out.len += length;
            r.at += length;
        } else if (type == 1 || type == 2) {
            Huffman literals, distances;

            if (type == 1) {
                FixedTables(&literals, &distances);
            } else if (!DynamicTables(&r, &literals, &distances)) {
                break;
            }

            if (!InflateBlock(&r, &out, &literals, &distances)) break;
        } else {
            break;      // reserved, and so a broken stream
        }

        if (last) {
            if (out.failed) break;
            if (outLen) *outLen = out.len;
            return out.bytes;
        }
    }

    free(out.bytes);
    return NULL;
}

BYTE* Inflate_Zlib(const BYTE* data, size_t len, size_t* outLen) {
    if (!data || len < 3) return NULL;

    // RFC 1950: the low nibble of the first byte is the method, and the two
    // bytes together are a multiple of 31. A preset dictionary is refused --
    // nothing writes one here, and guessing at it would be worse.
    if ((data[0] & 0x0F) != 8) return NULL;
    if (((data[0] << 8) | data[1]) % 31 != 0) return NULL;
    if (data[1] & 0x20) return NULL;

    return Inflate_Raw(data + 2, len - 2, outLen);
}

// ---------------------------------------------------------------------------
// Self-check
//
// Three streams, compressed elsewhere and pasted in, because a decompressor
// that is only ever fed its own output proves nothing. One is stored, one is
// fixed Huffman, one is dynamic with a back-reference longer than its own
// distance -- the overlapping copy that a run of one character becomes.
// ---------------------------------------------------------------------------

BOOL Inflate_SelfTest(char* failure, size_t failureSize) {
    #define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        return FALSE; \
    } while (0)

    struct {
        const char* what;
        const BYTE* data;
        size_t      len;
        const char* expected;
    } cases[4];

    // "opennote" -- stored, no compression at all.
    static const BYTE stored[] = {
        0x78, 0x01, 0x01, 0x08, 0x00, 0xF7, 0xFF, 0x6F, 0x70, 0x65, 0x6E,
        0x6E, 0x6F, 0x74, 0x65, 0x0F, 0x66, 0x03, 0x69
    };

    // The same word, fixed Huffman.
    static const BYTE fixed[] = {
        0x78, 0xDA, 0xCB, 0x2F, 0x48, 0xCD, 0xCB, 0xCB, 0x2F, 0x49,
        0x05, 0x00, 0x0F, 0x66, 0x03, 0x69
    };

    // Sixty-four 'a's: one literal, then a copy of itself over and over.
    static const BYTE runs[] = {
        0x78, 0xDA, 0x4B, 0x4C, 0xA4, 0x0C, 0x00, 0x00, 0x14, 0x8D, 0x18, 0x41
    };

    cases[0].what = "a stored block";
    cases[0].data = stored;
    cases[0].len = sizeof(stored);
    cases[0].expected = "opennote";

    cases[1].what = "a fixed Huffman block";
    cases[1].data = fixed;
    cases[1].len = sizeof(fixed);
    cases[1].expected = "opennote";

    static char sixtyFour[65];
    memset(sixtyFour, 'a', 64);
    sixtyFour[64] = '\0';

    cases[2].what = "an overlapping copy";
    cases[2].data = runs;
    cases[2].len = sizeof(runs);
    cases[2].expected = sixtyFour;

    // A paragraph of prose, which is long and varied enough that the
    // compressor builds its own code tables: the dynamic block, and the one
    // shape of stream the other three do not produce. Every PDF worth reading
    // is made of these.
    static const BYTE dynamic[] = {
        0x78, 0xDA, 0x2D, 0x8F, 0x31, 0x76, 0xC4, 0x30, 0x08, 0x44, 0xAF, 0x32,
        0x5D, 0x9A, 0x7D, 0x7B, 0x80, 0xA4, 0xCD, 0x1D, 0x52, 0x63, 0x0B, 0x5B,
        0x64, 0x65, 0xD0, 0x13, 0x38, 0xCE, 0xDE, 0x7E, 0xB1, 0x93, 0x42, 0xE2,
        0x81, 0x66, 0xFE, 0x20, 0xEB, 0xAC, 0x6A, 0xC1, 0x68, 0xF4, 0x74, 0x10,
        0x8A, 0xCD, 0xFB, 0xC6, 0x1A, 0xB0, 0x3D, 0x8F, 0x62, 0x30, 0x35, 0x74,
        0x5A, 0xD9, 0xDF, 0xB3, 0x0C, 0x5A, 0x07, 0xF5, 0xEA, 0xD8, 0x98, 0x7C,
        0x1F, 0x5C, 0x40, 0x5A, 0x30, 0x0D, 0x7B, 0xB0, 0x82, 0x22, 0xFD, 0x4D,
        0x94, 0x71, 0xD4, 0x6C, 0xA3, 0xF2, 0x33, 0x71, 0x48, 0x3A, 0x16, 0x89,
        0x1B, 0x0E, 0x29, 0x76, 0xF8, 0xE5, 0xB0, 0xD1, 0x2B, 0xA9, 0xE3, 0xC1,
        0x3D, 0x53, 0x96, 0xE5, 0x14, 0x5F, 0x29, 0x37, 0x04, 0x4D, 0x8D, 0x1D,
        0x93, 0xFD, 0x26, 0xFD, 0x90, 0xA8, 0xD7, 0xDB, 0x6C, 0x6D, 0xDF, 0xF4,
        0x44, 0x44, 0xA6, 0x9F, 0x93, 0x45, 0x1A, 0xC3, 0x83, 0x82, 0xFD, 0x8E,
        0x4F, 0x19, 0x3C, 0xC7, 0xD7, 0x90, 0xFC, 0x48, 0x31, 0xFE, 0x53, 0x78,
        0xA5, 0x2E, 0xBA, 0x7E, 0xFC, 0xC3, 0x47, 0xAE, 0x37, 0xD9, 0x0F, 0x43,
        0x02, 0x72, 0x2A, 0xF2, 0xEA, 0xC3, 0xBE, 0xD3, 0xF8, 0xE6, 0xF7, 0x17,
        0xB5, 0x2F, 0x5F, 0x93
    };

    static const char* prose =
        "opennote lays a document out on real pages: paragraphs measured and "
        "broken at a line when they do not fit, widows and orphans kept off "
        "the page, tables boxed with the column widths the file states. "
        "DirectWrite does the shaping; the part above it is this project's.";

    cases[3].what = "a dynamic Huffman block";
    cases[3].data = dynamic;
    cases[3].len = sizeof(dynamic);
    cases[3].expected = prose;

    for (int i = 0; i < 4; i++) {
        size_t len = 0;
        BYTE* out = Inflate_Zlib(cases[i].data, cases[i].len, &len);

        if (!out) {
            char message[128];
            sprintf_s(message, sizeof(message), "%s did not decompress", cases[i].what);
            FAIL(message);
        }

        size_t expectedLen = strlen(cases[i].expected);
        BOOL same = len == expectedLen && memcmp(out, cases[i].expected, len) == 0;
        free(out);

        if (!same) {
            char message[128];
            sprintf_s(message, sizeof(message), "%s came out wrong", cases[i].what);
            FAIL(message);
        }
    }

    // Rubbish in, nothing out -- rather than something out.
    static const BYTE rubbish[] = { 0x78, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF };
    size_t len = 0;
    BYTE* out = Inflate_Zlib(rubbish, sizeof(rubbish), &len);
    if (out) {
        free(out);
        FAIL("a broken stream came back as data");
    }

    // ...and a header that is not zlib is refused before anything else.
    static const BYTE notZlib[] = { 'P', 'K', 3, 4, 0, 0 };
    if (Inflate_Zlib(notZlib, sizeof(notZlib), &len)) FAIL("a zip file was taken for a zlib stream");

    failure[0] = '\0';
    return TRUE;

    #undef FAIL
}
