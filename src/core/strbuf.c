#include "supernote.h"
#include "core/strbuf.h"

void SB_Free(StrBuf* s) {
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

static BOOL Reserve(StrBuf* s, size_t extra) {
    if (s->failed) return FALSE;
    if (s->len + extra + 1 <= s->cap) return TRUE;

    size_t want = s->cap ? s->cap * 2 : 8192;
    while (want < s->len + extra + 1) want *= 2;

    char* grown = (char*)realloc(s->buf, want);
    if (!grown) {
        s->failed = TRUE;
        return FALSE;
    }
    s->buf = grown;
    s->cap = want;
    return TRUE;
}

void SB_AddN(StrBuf* s, const char* text, size_t n) {
    if (!text || !n) return;
    if (!Reserve(s, n)) return;
    memcpy(s->buf + s->len, text, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

void SB_Add(StrBuf* s, const char* text) {
    if (text) SB_AddN(s, text, strlen(text));
}

void SB_AddChar(StrBuf* s, char c) {
    if (!Reserve(s, 1)) return;
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

void SB_AddF(StrBuf* s, const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) SB_AddN(s, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

void SB_AddRtfText(StrBuf* s, const WCHAR* text, int len) {
    if (!text) return;
    if (len < 0) len = (int)wcslen(text);

    for (int i = 0; i < len; i++) {
        WCHAR c = text[i];
        switch (c) {
            case L'\\': SB_Add(s, "\\\\");   break;
            case L'{':  SB_Add(s, "\\{");    break;
            case L'}':  SB_Add(s, "\\}");    break;
            case L'\t': SB_Add(s, "\\tab "); break;
            case L'\r': case L'\n': break;   // paragraph breaks are structural
            default:
                if (c >= 0x20 && c < 0x80) {
                    SB_AddChar(s, (char)c);
                } else if (c >= 0x80) {
                    // RTF wants a signed 16-bit value here.
                    SB_AddF(s, "\\u%d?", (int)(short)c);
                }
                break;
        }
    }
}

void SB_AddXmlText(StrBuf* s, const WCHAR* text, int len) {
    if (!text) return;
    if (len < 0) len = (int)wcslen(text);
    if (!len) return;

    int u8len = WideCharToMultiByte(CP_UTF8, 0, text, len, NULL, 0, NULL, NULL);
    if (u8len <= 0) return;

    char* u8 = (char*)malloc((size_t)u8len + 1);
    if (!u8) {
        s->failed = TRUE;
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, len, u8, u8len, NULL, NULL);
    u8[u8len] = '\0';

    for (int i = 0; i < u8len; i++) {
        char c = u8[i];
        switch (c) {
            case '&':  SB_Add(s, "&amp;");  break;
            case '<':  SB_Add(s, "&lt;");   break;
            case '>':  SB_Add(s, "&gt;");   break;
            case '"':  SB_Add(s, "&quot;"); break;
            case '\'': SB_Add(s, "&apos;"); break;
            default:
                // Control characters other than tab are not legal in XML 1.0.
                if ((unsigned char)c < 0x20 && c != '\t') break;
                SB_AddChar(s, c);
                break;
        }
    }
    free(u8);
}
