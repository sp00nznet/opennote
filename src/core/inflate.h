#ifndef INFLATE_H
#define INFLATE_H

// DEFLATE, decompressed.
//
// Written here rather than linked because the alternative is a dependency for
// one algorithm this program needs in one place: a PDF from 2005 onwards keeps
// its cross-reference table as a compressed stream, and without this, those
// files can be drawn but not read -- which is every form anybody is sent.
//
// It is RFC 1951 and nothing else: stored, fixed and dynamic blocks, a 32k
// window, and the RFC 1950 two-byte wrapper that PDF's FlateDecode uses. No
// compression -- nothing here needs to write one -- and no gzip framing.

#ifdef __cplusplus
extern "C" {
#endif

// Decompress a zlib stream (the two-byte header, then DEFLATE). Returns the
// bytes, which the caller frees, or NULL if the data is not a stream this
// reads. `outLen` is what came out.
BYTE* Inflate_Zlib(const BYTE* data, size_t len, size_t* outLen);

// The same for raw DEFLATE, with no wrapper around it.
BYTE* Inflate_Raw(const BYTE* data, size_t len, size_t* outLen);

// Self-check, run by `OpenNote.exe --selftest`.
BOOL Inflate_SelfTest(char* failure, size_t failureSize);

#ifdef __cplusplus
}
#endif

#endif // INFLATE_H
