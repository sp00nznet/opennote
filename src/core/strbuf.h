#ifndef STRBUF_H
#define STRBUF_H

// A growable byte string. Used to build RTF and XML, both of which are
// produced by appending small pieces a great many times.
//
// Allocation failure is recorded rather than returned from every call: the
// callers build documents thousands of fragments at a time, and checking each
// append would bury the thing being built. Check `failed` once at the end.

typedef struct {
    char*  buf;
    size_t len;
    size_t cap;
    BOOL   failed;
} StrBuf;

void  SB_Free(StrBuf* s);
void  SB_Add(StrBuf* s, const char* text);
void  SB_AddN(StrBuf* s, const char* text, size_t n);
void  SB_AddChar(StrBuf* s, char c);
void  SB_AddF(StrBuf* s, const char* fmt, ...);

// Wide text, escaped for an RTF body: braces and backslashes are control
// characters, and anything above ASCII becomes \uN with a '?' fallback.
void  SB_AddRtfText(StrBuf* s, const WCHAR* text, int len);

// Wide text as UTF-8, escaped for XML character data or an attribute value.
void  SB_AddXmlText(StrBuf* s, const WCHAR* text, int len);

#endif // STRBUF_H
