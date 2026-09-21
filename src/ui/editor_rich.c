// The rich text view.
//
// This is a RichEdit 4.1 control (Msftedit.dll), which is what WordPad was
// built on: RTF is its native format, so loading and saving are EM_STREAMIN
// and EM_STREAMOUT rather than a parser we have to write.
//
// ponytail: RichEdit, not a DirectWrite layout engine. It buys a working
// WordPad replacement in weeks instead of quarters, and the ceiling is known
// and documented -- tables are weak and there is no true pagination, so the
// view flows rather than showing page boundaries. When .docx forces real
// layout (ROADMAP v0.6) that engine replaces this file, and the Editor_*
// dispatch is already the seam it slots into.

#include "supernote.h"
#include "ui/editor_rich.h"

#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

// Msftedit.dll is loaded once, on first use, and deliberately never freed --
// the control class it registers must outlive every editor window.
static HMODULE g_richEditLib = NULL;

BOOL Rich_EnsureLoaded(void) {
    if (g_richEditLib) return TRUE;
    g_richEditLib = LoadLibraryW(L"Msftedit.dll");
    return g_richEditLib != NULL;
}

#define EnsureRichEditLoaded Rich_EnsureLoaded

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

HWND Rich_Create(HWND hParent) {
    if (!EnsureRichEditLoaded()) return NULL;

    HWND h = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        MSFTEDIT_CLASS,
        NULL,
        WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_NOHIDESEL | ES_SAVESEL | ES_WANTRETURN,
        0, 0, 100, 100,
        hParent,
        NULL,
        g_app->hInstance,
        NULL
    );
    if (!h) return NULL;

    // Somewhere to keep an embedded picture. Without it the control drops
    // every one it reads, silently -- see richole.c.
    RichOle_Attach(h);

    // TM_RICHTEXT keeps formatting on paste and on undo; TM_MULTILEVELUNDO is
    // what makes Ctrl+Z more than a single step.
    SendMessageW(h, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);

    // Without this the control tells the parent nothing and the title bar never
    // learns the document was modified.
    SendMessageW(h, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);

    // RichEdit caps at 64KB unless told otherwise, which is far too small for
    // a document editor.
    SendMessageW(h, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);

    SendMessageW(h, EM_SETUNDOLIMIT, 100, 0);

    // Wrap to the window, matching what WordPad did by default. The control is
    // created without WS_HSCROLL on purpose: a document view that does not wrap
    // and has no horizontal scrollbar puts text where nobody can reach it.
    SendMessageW(h, EM_SETTARGETDEVICE, 0, 0);

    // A document font, not the code font the plain text view uses. WordPad
    // opened on Calibri 11; matching that beats inheriting Consolas.
    CHARFORMAT2W cf = { .cbSize = sizeof(cf) };
    cf.dwMask = CFM_FACE | CFM_SIZE;
    wcscpy_s(cf.szFaceName, LF_FACESIZE, L"Calibri");
    cf.yHeight = 11 * 20;  // twips
    SendMessageW(h, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

    return h;
}

// ---------------------------------------------------------------------------
// Stream helpers for RTF in and out
// ---------------------------------------------------------------------------

typedef struct {
    HANDLE hFile;
    BOOL   failed;
} FileStream;

static DWORD CALLBACK ReadFromFile(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
    FileStream* fs = (FileStream*)cookie;
    DWORD read = 0;
    if (!ReadFile(fs->hFile, buf, (DWORD)cb, &read, NULL)) {
        fs->failed = TRUE;
        return 1;
    }
    *pcb = (LONG)read;
    return 0;
}

static DWORD CALLBACK WriteToFile(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
    FileStream* fs = (FileStream*)cookie;
    DWORD written = 0;
    if (!WriteFile(fs->hFile, buf, (DWORD)cb, &written, NULL)) {
        fs->failed = TRUE;
        return 1;
    }
    *pcb = (LONG)written;
    return 0;
}

typedef struct {
    char*  data;
    size_t size;
    size_t pos;
    BOOL   failed;
} MemStream;

static DWORD CALLBACK ReadFromMem(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
    MemStream* ms = (MemStream*)cookie;
    size_t remaining = ms->size - ms->pos;
    size_t take = (size_t)cb < remaining ? (size_t)cb : remaining;
    if (take) memcpy(buf, ms->data + ms->pos, take);
    ms->pos += take;
    *pcb = (LONG)take;
    return 0;
}

static DWORD CALLBACK WriteToMem(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
    MemStream* ms = (MemStream*)cookie;
    char* grown = (char*)realloc(ms->data, ms->size + (size_t)cb + 1);
    if (!grown) {
        ms->failed = TRUE;
        return 1;
    }
    ms->data = grown;
    memcpy(ms->data + ms->size, buf, (size_t)cb);
    ms->size += (size_t)cb;
    ms->data[ms->size] = '\0';
    *pcb = cb;
    return 0;
}

BOOL Rich_LoadRtfFile(HWND h, const WCHAR* path) {
    if (!h || !path) return FALSE;

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    FileStream fs = { hFile, FALSE };
    EDITSTREAM es = { (DWORD_PTR)&fs, 0, ReadFromFile };

    SendMessageW(h, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    CloseHandle(hFile);

    // es.dwError is the callback's return; a malformed RTF shows up here rather
    // than as a half-loaded document the user then saves over the original.
    if (fs.failed || es.dwError != 0) return FALSE;

    SendMessageW(h, EM_SETMODIFY, FALSE, 0);
    SendMessageW(h, EM_EMPTYUNDOBUFFER, 0, 0);
    return TRUE;
}

BOOL Rich_SaveRtfFile(HWND h, const WCHAR* path) {
    if (!h || !path) return FALSE;

    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    FileStream fs = { hFile, FALSE };
    EDITSTREAM es = { (DWORD_PTR)&fs, 0, WriteToFile };

    SendMessageW(h, EM_STREAMOUT, SF_RTF, (LPARAM)&es);
    CloseHandle(hFile);

    if (fs.failed || es.dwError != 0) return FALSE;

    SendMessageW(h, EM_SETMODIFY, FALSE, 0);
    return TRUE;
}

BOOL Rich_SetRtf(HWND h, const char* rtf) {
    if (!h || !rtf) return FALSE;

    MemStream ms = { (char*)rtf, strlen(rtf), 0, FALSE };
    EDITSTREAM es = { (DWORD_PTR)&ms, 0, ReadFromMem };

    SendMessageW(h, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    if (es.dwError != 0) return FALSE;

    SendMessageW(h, EM_SETMODIFY, FALSE, 0);
    SendMessageW(h, EM_EMPTYUNDOBUFFER, 0, 0);
    return TRUE;
}

char* Rich_GetRtf(HWND h) {
    if (!h) return NULL;

    MemStream ms = { NULL, 0, 0, FALSE };
    EDITSTREAM es = { (DWORD_PTR)&ms, 0, WriteToMem };

    SendMessageW(h, EM_STREAMOUT, SF_RTF, (LPARAM)&es);

    if (ms.failed || es.dwError != 0) {
        free(ms.data);
        return NULL;
    }
    return ms.data;  // NUL-terminated by WriteToMem
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void Rich_SetText(HWND h, const WCHAR* text) {
    if (!h) return;

    SETTEXTEX st = { ST_DEFAULT, 1200 };  // 1200 = Unicode
    SendMessageW(h, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)(text ? text : L""));

    SendMessageW(h, EM_SETMODIFY, FALSE, 0);
    SendMessageW(h, EM_EMPTYUNDOBUFFER, 0, 0);
}

int Rich_GetTextLength(HWND h) {
    if (!h) return 0;
    GETTEXTLENGTHEX gtl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    return (int)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
}

WCHAR* Rich_GetText(HWND h) {
    if (!h) return NULL;

    int len = Rich_GetTextLength(h);
    WCHAR* buf = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (!buf) return NULL;

    GETTEXTEX gt = {
        .cb = (DWORD)(((size_t)len + 1) * sizeof(WCHAR)),
        .flags = GT_DEFAULT,
        .codepage = 1200,
        .lpDefaultChar = NULL,
        .lpUsedDefChar = NULL
    };
    SendMessageW(h, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)buf);
    buf[len] = L'\0';
    return buf;
}

// ---------------------------------------------------------------------------
// Selection and position
// ---------------------------------------------------------------------------

void Rich_GetSelection(HWND h, int* start, int* end) {
    if (!h) return;
    CHARRANGE cr = {0};
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (start) *start = cr.cpMin;
    if (end)   *end   = cr.cpMax;
}

void Rich_SetSelection(HWND h, int start, int end) {
    if (!h) return;
    CHARRANGE cr = { start, end };
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
}

void Rich_ReplaceSelection(HWND h, const WCHAR* text) {
    if (!h) return;
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)(text ? text : L""));
}

WCHAR* Rich_GetSelectedText(HWND h) {
    if (!h) return NULL;

    int start, end;
    Rich_GetSelection(h, &start, &end);
    if (start >= end) return NULL;

    WCHAR* buf = (WCHAR*)malloc(((size_t)(end - start) + 1) * sizeof(WCHAR));
    if (!buf) return NULL;

    SendMessageW(h, EM_GETSELTEXT, 0, (LPARAM)buf);
    return buf;
}

void Rich_ReplaceRange(HWND h, int start, int end, const WCHAR* text) {
    if (!h || !text || start > end) return;

    int selStart, selEnd;
    Rich_GetSelection(h, &selStart, &selEnd);

    Rich_SetSelection(h, start, end);
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)text);

    // Put the caret back roughly where it was rather than leaving it wherever
    // the replacement ended.
    int delta = (int)wcslen(text) - (end - start);
    Rich_SetSelection(h, selStart + (selStart > start ? delta : 0),
                         selEnd + (selEnd > start ? delta : 0));
}

int Rich_GetPositionFromPoint(HWND h, int x, int y) {
    if (!h) return 0;

    POINT pt = { x, y };
    ScreenToClient(h, &pt);

    POINTL ptl = { pt.x, pt.y };
    return (int)SendMessageW(h, EM_CHARFROMPOS, 0, (LPARAM)&ptl);
}

WCHAR* Rich_GetWordAt(HWND h, int pos, int* wordStart, int* wordEnd) {
    if (!h || !wordStart || !wordEnd) return NULL;

    int line = (int)SendMessageW(h, EM_EXLINEFROMCHAR, 0, pos);
    int lineStart = (int)SendMessageW(h, EM_LINEINDEX, line, 0);
    int lineLen = (int)SendMessageW(h, EM_LINELENGTH, lineStart, 0);
    if (lineLen <= 0) return NULL;

    // EM_GETLINE wants the buffer's capacity in its first word, and does not
    // NUL-terminate.
    WCHAR* lineBuf = (WCHAR*)calloc((size_t)lineLen + 2, sizeof(WCHAR));
    if (!lineBuf) return NULL;
    *(WORD*)lineBuf = (WORD)(lineLen + 1);

    int got = (int)SendMessageW(h, EM_GETLINE, line, (LPARAM)lineBuf);
    if (got <= 0) {
        free(lineBuf);
        return NULL;
    }
    lineBuf[got] = L'\0';

    int rel = pos - lineStart;
    if (rel < 0) rel = 0;
    if (rel > got) rel = got;

    int s = rel, e = rel;
    while (s > 0 && (iswalnum(lineBuf[s - 1]) || lineBuf[s - 1] == L'\'')) s--;
    while (e < got && (iswalnum(lineBuf[e]) || lineBuf[e] == L'\'')) e++;

    if (s >= e) {
        free(lineBuf);
        return NULL;
    }

    size_t wordLen = (size_t)(e - s);
    WCHAR* word = (WCHAR*)malloc((wordLen + 1) * sizeof(WCHAR));
    if (word) {
        memcpy(word, lineBuf + s, wordLen * sizeof(WCHAR));
        word[wordLen] = L'\0';
    }

    free(lineBuf);

    *wordStart = lineStart + s;
    *wordEnd = lineStart + e;
    return word;
}

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

void Rich_GetCursorPos(HWND h, int* line, int* column) {
    if (!h) return;

    int start, end;
    Rich_GetSelection(h, &start, &end);

    int ln = (int)SendMessageW(h, EM_EXLINEFROMCHAR, 0, start);
    int lineStart = (int)SendMessageW(h, EM_LINEINDEX, ln, 0);

    if (line)   *line = ln + 1;
    if (column) *column = start - lineStart + 1;
}

void Rich_GotoLine(HWND h, int line) {
    if (!h) return;
    if (line < 1) line = 1;

    int pos = (int)SendMessageW(h, EM_LINEINDEX, line - 1, 0);
    if (pos < 0) return;

    Rich_SetSelection(h, pos, pos);
    SendMessageW(h, EM_SCROLLCARET, 0, 0);
}

int Rich_GetLineCount(HWND h) {
    return h ? (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0) : 0;
}

int Rich_GetCurrentLine(HWND h) {
    if (!h) return 0;
    int start, end;
    Rich_GetSelection(h, &start, &end);
    return (int)SendMessageW(h, EM_EXLINEFROMCHAR, 0, start) + 1;
}

// ---------------------------------------------------------------------------
// History and clipboard
// ---------------------------------------------------------------------------

BOOL Rich_CanUndo(HWND h) { return h && SendMessageW(h, EM_CANUNDO, 0, 0); }
BOOL Rich_CanRedo(HWND h) { return h && SendMessageW(h, EM_CANREDO, 0, 0); }
void Rich_Undo(HWND h)    { if (h) SendMessageW(h, EM_UNDO, 0, 0); }
void Rich_Redo(HWND h)    { if (h) SendMessageW(h, EM_REDO, 0, 0); }
void Rich_Cut(HWND h)     { if (h) SendMessageW(h, WM_CUT, 0, 0); }
void Rich_Copy(HWND h)    { if (h) SendMessageW(h, WM_COPY, 0, 0); }
void Rich_Paste(HWND h)   { if (h) SendMessageW(h, WM_PASTE, 0, 0); }

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

void Rich_SetWordWrap(HWND h, BOOL wrap) {
    (void)wrap;
    if (!h) return;

    // ponytail: the rich view always wraps to the window, and the plain text
    // view's Word Wrap setting does not reach it. Turning wrapping off here
    // needs a horizontal scrollbar and a page-width notion to wrap to instead;
    // that arrives with real pagination in v0.6. Until then, honouring the
    // setting would only ever hide text.
    SendMessageW(h, EM_SETTARGETDEVICE, 0, 0);
}

void Rich_SetFont(HWND h, HFONT hFont) {
    if (!h || !hFont) return;

    LOGFONTW lf;
    if (!GetObjectW(hFont, sizeof(lf), &lf)) return;

    HDC hdc = GetDC(NULL);
    int logY = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);

    CHARFORMAT2W cf = { .cbSize = sizeof(cf) };
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC;
    wcscpy_s(cf.szFaceName, LF_FACESIZE, lf.lfFaceName);
    cf.yHeight = MulDiv(abs(lf.lfHeight), 1440, logY);  // twips
    cf.dwEffects = 0;
    if (lf.lfWeight >= FW_BOLD) cf.dwEffects |= CFE_BOLD;
    if (lf.lfItalic)            cf.dwEffects |= CFE_ITALIC;

    // SCF_ALL: this is the document default changing, not a formatting edit.
    SendMessageW(h, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

void Rich_SetZoom(HWND h, int zoomPercent) {
    if (!h) return;
    if (zoomPercent < 10)  zoomPercent = 10;
    if (zoomPercent > 500) zoomPercent = 500;
    SendMessageW(h, EM_SETZOOM, zoomPercent, 100);
}

// ---------------------------------------------------------------------------
// Modification state
// ---------------------------------------------------------------------------

BOOL Rich_GetModified(HWND h) {
    return h && SendMessageW(h, EM_GETMODIFY, 0, 0);
}

void Rich_SetModified(HWND h, BOOL modified) {
    if (h) SendMessageW(h, EM_SETMODIFY, modified ? TRUE : FALSE, 0);
}

// ---------------------------------------------------------------------------
// Find and replace
// ---------------------------------------------------------------------------

int Rich_FindText(HWND h, const WCHAR* text, BOOL matchCase, BOOL wholeWord, BOOL forward) {
    if (!h || !text || !text[0]) return -1;

    int selStart, selEnd;
    Rich_GetSelection(h, &selStart, &selEnd);

    DWORD flags = 0;
    if (matchCase) flags |= FR_MATCHCASE;
    if (wholeWord) flags |= FR_WHOLEWORD;
    if (forward)   flags |= FR_DOWN;

    FINDTEXTEXW ft = {0};
    if (forward) {
        ft.chrg.cpMin = selEnd;
        ft.chrg.cpMax = -1;
    } else {
        ft.chrg.cpMin = selStart;
        ft.chrg.cpMax = 0;
    }
    ft.lpstrText = (LPWSTR)text;

    LONG found = (LONG)SendMessageW(h, EM_FINDTEXTEXW, flags, (LPARAM)&ft);

    if (found < 0) {
        // Wrap around, which is what every other find in this application does.
        if (forward) {
            ft.chrg.cpMin = 0;
            ft.chrg.cpMax = -1;
        } else {
            ft.chrg.cpMin = Rich_GetTextLength(h);
            ft.chrg.cpMax = 0;
        }
        found = (LONG)SendMessageW(h, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    }

    if (found >= 0) {
        Rich_SetSelection(h, ft.chrgText.cpMin, ft.chrgText.cpMax);
        SendMessageW(h, EM_SCROLLCARET, 0, 0);
    }
    return found;
}

int Rich_ReplaceText(HWND h, const WCHAR* findText, const WCHAR* replaceText,
                     BOOL matchCase, BOOL wholeWord) {
    if (!h || !findText || !replaceText) return -1;

    // Replace what is selected only if it is actually a match; otherwise this
    // would overwrite whatever the user happened to have highlighted.
    WCHAR* selected = Rich_GetSelectedText(h);
    if (selected) {
        BOOL isMatch = matchCase ? (wcscmp(selected, findText) == 0)
                                 : (_wcsicmp(selected, findText) == 0);
        free(selected);
        if (isMatch) {
            Rich_ReplaceSelection(h, replaceText);
        }
    }

    return Rich_FindText(h, findText, matchCase, wholeWord, TRUE);
}

int Rich_ReplaceAll(HWND h, const WCHAR* findText, const WCHAR* replaceText,
                    BOOL matchCase, BOOL wholeWord) {
    if (!h || !findText || !findText[0] || !replaceText) return 0;

    DWORD flags = FR_DOWN;
    if (matchCase) flags |= FR_MATCHCASE;
    if (wholeWord) flags |= FR_WHOLEWORD;

    // One undo step for the whole operation rather than one per replacement.
    SendMessageW(h, EM_SETSEL, 0, 0);

    int count = 0;
    LONG from = 0;

    for (;;) {
        FINDTEXTEXW ft = {0};
        ft.chrg.cpMin = from;
        ft.chrg.cpMax = -1;
        ft.lpstrText = (LPWSTR)findText;

        LONG found = (LONG)SendMessageW(h, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
        if (found < 0) break;

        Rich_SetSelection(h, ft.chrgText.cpMin, ft.chrgText.cpMax);
        SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)replaceText);

        // Continue past the replacement, or a replacement containing the search
        // term would loop forever.
        from = ft.chrgText.cpMin + (LONG)wcslen(replaceText);
        count++;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Character formatting
// ---------------------------------------------------------------------------

static void ApplyCharFormat(HWND h, DWORD mask, DWORD effects, LONG height,
                            COLORREF color, const WCHAR* face) {
    CHARFORMAT2W cf = { .cbSize = sizeof(cf) };
    cf.dwMask = mask;
    cf.dwEffects = effects;
    cf.yHeight = height;
    cf.crTextColor = color;
    if (face) wcscpy_s(cf.szFaceName, LF_FACESIZE, face);

    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void GetCharFormat(HWND h, CHARFORMAT2W* cf) {
    memset(cf, 0, sizeof(*cf));
    cf->cbSize = sizeof(*cf);
    SendMessageW(h, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)cf);
}

BOOL Rich_HasEffect(HWND h, DWORD effect) {
    if (!h) return FALSE;

    CHARFORMAT2W cf;
    GetCharFormat(h, &cf);

    // A selection spanning both states reports the bit as not-set in dwMask;
    // treat that as "not on", which is what the toolbar should show.
    if (!(cf.dwMask & effect)) return FALSE;
    return (cf.dwEffects & effect) != 0;
}

void Rich_ToggleEffect(HWND h, DWORD effect) {
    if (!h) return;
    BOOL on = Rich_HasEffect(h, effect);
    ApplyCharFormat(h, effect, on ? 0 : effect, 0, 0, NULL);
}

void Rich_SetFontName(HWND h, const WCHAR* name) {
    if (!h || !name) return;
    ApplyCharFormat(h, CFM_FACE, 0, 0, 0, name);
}

void Rich_SetFontSize(HWND h, int points) {
    if (!h || points < 1) return;
    ApplyCharFormat(h, CFM_SIZE, 0, points * 20, 0, NULL);  // twips
}

void Rich_SetTextColor(HWND h, COLORREF color) {
    if (!h) return;
    ApplyCharFormat(h, CFM_COLOR, 0, 0, color, NULL);
}

void Rich_SetHighlightColor(HWND h, COLORREF color, BOOL none) {
    if (!h) return;

    CHARFORMAT2W cf = { .cbSize = sizeof(cf) };
    cf.dwMask = CFM_BACKCOLOR;
    cf.dwEffects = none ? CFE_AUTOBACKCOLOR : 0;
    cf.crBackColor = color;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

BOOL Rich_GetFontName(HWND h, WCHAR* out, size_t outChars) {
    if (!h || !out || outChars == 0) return FALSE;
    out[0] = L'\0';

    CHARFORMAT2W cf;
    GetCharFormat(h, &cf);
    if (!(cf.dwMask & CFM_FACE)) return FALSE;

    wcsncpy_s(out, outChars, cf.szFaceName, _TRUNCATE);
    return TRUE;
}

int Rich_GetFontSize(HWND h) {
    if (!h) return 0;

    CHARFORMAT2W cf;
    GetCharFormat(h, &cf);
    if (!(cf.dwMask & CFM_SIZE)) return 0;

    return cf.yHeight / 20;
}

// ---------------------------------------------------------------------------
// Paragraph formatting
// ---------------------------------------------------------------------------

static void GetParaFormat(HWND h, PARAFORMAT2* pf) {
    memset(pf, 0, sizeof(*pf));
    pf->cbSize = sizeof(*pf);
    SendMessageW(h, EM_GETPARAFORMAT, 0, (LPARAM)pf);
}

void Rich_SetAlignment(HWND h, WORD alignment) {
    if (!h) return;
    PARAFORMAT2 pf = { .cbSize = sizeof(pf) };
    pf.dwMask = PFM_ALIGNMENT;
    pf.wAlignment = alignment;
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

WORD Rich_GetAlignment(HWND h) {
    if (!h) return PFA_LEFT;
    PARAFORMAT2 pf;
    GetParaFormat(h, &pf);
    return (pf.dwMask & PFM_ALIGNMENT) ? pf.wAlignment : PFA_LEFT;
}

void Rich_SetListStyle(HWND h, WORD numbering) {
    if (!h) return;

    PARAFORMAT2 pf = { .cbSize = sizeof(pf) };
    pf.dwMask = PFM_NUMBERING | PFM_OFFSET;
    pf.wNumbering = numbering;
    // Without a hanging indent the marker and the text sit on top of each other.
    pf.dxOffset = numbering ? 360 : 0;  // 360 twips = 0.25"
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

WORD Rich_GetListStyle(HWND h) {
    if (!h) return 0;
    PARAFORMAT2 pf;
    GetParaFormat(h, &pf);
    return (pf.dwMask & PFM_NUMBERING) ? pf.wNumbering : 0;
}

void Rich_Indent(HWND h, BOOL increase) {
    if (!h) return;

    PARAFORMAT2 cur;
    GetParaFormat(h, &cur);

    LONG step = 720;  // 720 twips = 0.5", the step WordPad used
    LONG indent = (cur.dwMask & PFM_STARTINDENT) ? cur.dxStartIndent : 0;

    indent += increase ? step : -step;
    if (indent < 0) indent = 0;

    PARAFORMAT2 pf = { .cbSize = sizeof(pf) };
    pf.dwMask = PFM_STARTINDENT;
    pf.dxStartIndent = indent;
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

void Rich_SetLineSpacing(HWND h, int spacingTenths) {
    if (!h) return;

    PARAFORMAT2 pf = { .cbSize = sizeof(pf) };
    pf.dwMask = PFM_LINESPACING;
    // Rule 5: dyLineSpacing is in twentieths of a line, so single spacing is 20.
    pf.bLineSpacingRule = 5;
    pf.dyLineSpacing = spacingTenths * 2;
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

// ---------------------------------------------------------------------------
// Common dialogs
// ---------------------------------------------------------------------------

void Rich_ChooseFont(HWND h, HWND hOwner) {
    if (!h) return;

    CHARFORMAT2W cf;
    GetCharFormat(h, &cf);

    HDC hdc = GetDC(NULL);
    int logY = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);

    LOGFONTW lf = {0};
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, cf.szFaceName);
    lf.lfHeight = -MulDiv(cf.yHeight / 20, logY, 72);
    lf.lfWeight = (cf.dwEffects & CFE_BOLD) ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (cf.dwEffects & CFE_ITALIC) ? TRUE : FALSE;
    lf.lfUnderline = (cf.dwEffects & CFE_UNDERLINE) ? TRUE : FALSE;
    lf.lfStrikeOut = (cf.dwEffects & CFE_STRIKEOUT) ? TRUE : FALSE;

    CHOOSEFONTW cfd = {
        .lStructSize = sizeof(cfd),
        .hwndOwner = hOwner,
        .lpLogFont = &lf,
        .Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS,
        .rgbColors = cf.crTextColor
    };

    if (!ChooseFontW(&cfd)) return;

    CHARFORMAT2W out = { .cbSize = sizeof(out) };
    out.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC |
                 CFM_UNDERLINE | CFM_STRIKEOUT | CFM_COLOR;
    wcscpy_s(out.szFaceName, LF_FACESIZE, lf.lfFaceName);
    out.yHeight = cfd.iPointSize * 2;  // iPointSize is tenths of a point
    out.crTextColor = cfd.rgbColors;
    out.dwEffects = 0;
    if (lf.lfWeight >= FW_BOLD) out.dwEffects |= CFE_BOLD;
    if (lf.lfItalic)            out.dwEffects |= CFE_ITALIC;
    if (lf.lfUnderline)         out.dwEffects |= CFE_UNDERLINE;
    if (lf.lfStrikeOut)         out.dwEffects |= CFE_STRIKEOUT;

    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&out);
}

void Rich_ChooseColor(HWND h, HWND hOwner) {
    if (!h) return;

    static COLORREF customColors[16] = {0};

    CHARFORMAT2W cf;
    GetCharFormat(h, &cf);

    CHOOSECOLORW cc = {
        .lStructSize = sizeof(cc),
        .hwndOwner = hOwner,
        .rgbResult = cf.crTextColor,
        .lpCustColors = customColors,
        .Flags = CC_FULLOPEN | CC_RGBINIT
    };

    if (ChooseColorW(&cc)) {
        Rich_SetTextColor(h, cc.rgbResult);
    }
}

void Rich_ClearFormatting(HWND h) {
    if (!h) return;

    CHARFORMAT2W cf = { .cbSize = sizeof(cf) };
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT |
                CFM_COLOR | CFM_BACKCOLOR | CFM_SUBSCRIPT | CFM_SUPERSCRIPT;
    cf.dwEffects = CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    PARAFORMAT2 pf = { .cbSize = sizeof(pf) };
    pf.dwMask = PFM_ALIGNMENT | PFM_NUMBERING | PFM_STARTINDENT |
                PFM_OFFSET | PFM_LINESPACING;
    pf.wAlignment = PFA_LEFT;
    pf.wNumbering = 0;
    pf.dxStartIndent = 0;
    pf.dxOffset = 0;
    pf.bLineSpacingRule = 5;
    pf.dyLineSpacing = 20;
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

// ---------------------------------------------------------------------------
// Pictures
//
// RichEdit will not insert an image from a plain message; the documented route
// is an RTF fragment carrying the bitmap bits, which EM_STREAMIN accepts with
// SFF_SELECTION. That avoids pulling in an OLE object store for what is, in
// the end, a picture in a document.
// ---------------------------------------------------------------------------

BOOL Rich_InsertPicture(HWND h, const WCHAR* path) {
    if (!h || !path) return FALSE;

    // The picture is embedded as a Windows metafile-wrapped DIB, which is the
    // form RTF readers other than RichEdit also understand -- so a document
    // saved here still shows its images in Word.
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    HBITMAP hBmp = (HBITMAP)LoadImageW(NULL, path, IMAGE_BITMAP, 0, 0,
                                       LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    CloseHandle(hFile);
    if (!hBmp) return FALSE;

    BITMAP bm;
    GetObjectW(hBmp, sizeof(bm), &bm);

    HDC hdcScreen = GetDC(NULL);
    BITMAPINFOHEADER bi = {
        .biSize = sizeof(BITMAPINFOHEADER),
        .biWidth = bm.bmWidth,
        .biHeight = bm.bmHeight,
        .biPlanes = 1,
        .biBitCount = 24,
        .biCompression = BI_RGB
    };

    DWORD rowBytes = (((DWORD)bm.bmWidth * 3 + 3) & ~3u);
    DWORD imageBytes = rowBytes * (DWORD)bm.bmHeight;

    BYTE* bits = (BYTE*)malloc(imageBytes);
    if (!bits) {
        ReleaseDC(NULL, hdcScreen);
        DeleteObject(hBmp);
        return FALSE;
    }

    GetDIBits(hdcScreen, hBmp, 0, (UINT)bm.bmHeight, bits,
              (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdcScreen);
    DeleteObject(hBmp);

    // \wbitmap0 \wbmbitspixel24 plus the DIB header and bits as hex.
    size_t hdrBytes = sizeof(BITMAPINFOHEADER);
    size_t hexChars = (hdrBytes + imageBytes) * 2;
    size_t rtfCap = hexChars + 512;

    char* rtf = (char*)malloc(rtfCap);
    if (!rtf) {
        free(bits);
        return FALSE;
    }

    // Twips at 96 dpi: one pixel is 15 twips.
    int wGoal = bm.bmWidth * 15;
    int hGoal = bm.bmHeight * 15;

    int n = snprintf(rtf, rtfCap,
        "{\\rtf1\\ansi{\\pict\\dibitmap0\\wbmbitspixel24"
        "\\picw%ld\\pich%ld\\picwgoal%d\\pichgoal%d ",
        bm.bmWidth, bm.bmHeight, wGoal, hGoal);

    static const char hexDigits[] = "0123456789abcdef";
    char* p = rtf + n;

    const BYTE* hdrBytesPtr = (const BYTE*)&bi;
    for (size_t i = 0; i < hdrBytes; i++) {
        *p++ = hexDigits[hdrBytesPtr[i] >> 4];
        *p++ = hexDigits[hdrBytesPtr[i] & 0x0F];
    }
    for (size_t i = 0; i < imageBytes; i++) {
        *p++ = hexDigits[bits[i] >> 4];
        *p++ = hexDigits[bits[i] & 0x0F];
    }
    *p++ = '}';
    *p++ = '}';
    *p = '\0';

    free(bits);

    MemStream ms = { rtf, (size_t)(p - rtf), 0, FALSE };
    EDITSTREAM es = { (DWORD_PTR)&ms, 0, ReadFromMem };

    SendMessageW(h, EM_STREAMIN, SF_RTF | SFF_SELECTION, (LPARAM)&es);

    BOOL ok = (es.dwError == 0);
    free(rtf);
    return ok;
}

// ---------------------------------------------------------------------------
// Printing
//
// EM_FORMATRANGE renders as much of the document as fits the given rectangle
// and reports where it stopped, so paging is a loop over that.
// ---------------------------------------------------------------------------

int Rich_PrintToDC(HWND h, HDC hDC, const RECT* rcPageTwips, const WCHAR* docTitle,
                   const WCHAR* outputFile) {
    if (!h || !hDC || !rcPageTwips) return 0;

    // Naming the output is what sends a print-to-file device -- the PDF
    // printer -- straight to that path instead of asking where to put it.
    DOCINFOW di = { sizeof(di), docTitle ? docTitle : L"Document", outputFile, NULL, 0 };
    if (StartDocW(hDC, &di) <= 0) return 0;

    FORMATRANGE fr = {0};
    fr.hdc = hDC;
    fr.hdcTarget = hDC;
    fr.rc = *rcPageTwips;
    fr.rcPage = *rcPageTwips;
    fr.chrg.cpMin = 0;
    fr.chrg.cpMax = -1;

    int pages = 0;
    LONG textLength = Rich_GetTextLength(h);

    while (fr.chrg.cpMin < textLength) {
        if (StartPage(hDC) <= 0) break;

        LONG next = (LONG)SendMessageW(h, EM_FORMATRANGE, TRUE, (LPARAM)&fr);

        if (EndPage(hDC) <= 0) break;
        pages++;

        // No forward progress means the rectangle is too small to hold even one
        // line; stopping beats emitting blank pages until the printer runs out.
        if (next <= fr.chrg.cpMin) break;
        fr.chrg.cpMin = next;

        if (pages > 2000) break;  // runaway guard
    }

    // Release the cached information EM_FORMATRANGE built up.
    SendMessageW(h, EM_FORMATRANGE, FALSE, 0);

    EndDoc(hDC);
    return pages;
}

// ---------------------------------------------------------------------------
// Self-check. Run with: OpenNote.exe --selftest
//
// Creates a real hidden RichEdit control, because the things worth checking
// here -- that RTF survives a round trip, that formatting actually applies --
// are behaviours of the control rather than of this file's arithmetic.
// ---------------------------------------------------------------------------

BOOL Rich_SelfTest(char* failure, size_t failureSize) {
#define FAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        if (h) DestroyWindow(h); \
        return FALSE; \
    } while (0)

    HWND h = NULL;

    if (!EnsureRichEditLoaded()) {
        strncpy_s(failure, failureSize, "Msftedit.dll would not load", _TRUNCATE);
        return FALSE;
    }

    // A message-only parent keeps the control off screen during the check.
    h = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
                        WS_POPUP | ES_MULTILINE | ES_NOHIDESEL,
                        0, 0, 100, 100, HWND_MESSAGE, NULL,
                        GetModuleHandleW(NULL), NULL);
    if (!h) {
        strncpy_s(failure, failureSize, "could not create a RichEdit control", _TRUNCATE);
        return FALSE;
    }

    SendMessageW(h, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO, 0);
    SendMessageW(h, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);

    // Plain text in and out.
    Rich_SetText(h, L"Hello world");
    if (Rich_GetTextLength(h) != 11) FAIL("Rich_GetTextLength disagrees with what was set");

    WCHAR* text = Rich_GetText(h);
    if (!text || wcscmp(text, L"Hello world") != 0) {
        free(text);
        FAIL("Rich_GetText did not return what Rich_SetText stored");
    }
    free(text);

    // Setting text must not leave the document looking modified, or every
    // freshly opened file would prompt to save on close.
    if (Rich_GetModified(h)) FAIL("Rich_SetText left the document marked modified");

    // Formatting has to actually stick, and has to be reported back.
    Rich_SetSelection(h, 0, 5);
    Rich_ToggleEffect(h, CFE_BOLD);
    if (!Rich_HasEffect(h, CFE_BOLD)) FAIL("bold did not apply to the selection");

    Rich_ToggleEffect(h, CFE_BOLD);
    if (Rich_HasEffect(h, CFE_BOLD)) FAIL("bold did not toggle back off");

    Rich_SetSelection(h, 0, 5);
    Rich_SetFontSize(h, 18);
    if (Rich_GetFontSize(h) != 18) FAIL("font size did not round trip");

    // The whole point of this view: RTF out, RTF back in, formatting intact.
    Rich_SetSelection(h, 0, 5);
    Rich_ToggleEffect(h, CFE_ITALIC);

    char* rtf = Rich_GetRtf(h);
    if (!rtf) FAIL("Rich_GetRtf returned nothing");
    if (strncmp(rtf, "{\\rtf", 5) != 0) {
        free(rtf);
        FAIL("Rich_GetRtf output does not start with an RTF header");
    }

    Rich_SetText(h, L"");
    if (!Rich_SetRtf(h, rtf)) {
        free(rtf);
        FAIL("Rich_SetRtf rejected output from Rich_GetRtf");
    }
    free(rtf);

    text = Rich_GetText(h);
    if (!text || wcsncmp(text, L"Hello world", 11) != 0) {
        free(text);
        FAIL("text did not survive the RTF round trip");
    }
    free(text);

    Rich_SetSelection(h, 0, 5);
    if (!Rich_HasEffect(h, CFE_ITALIC)) FAIL("italic did not survive the RTF round trip");
    if (Rich_GetFontSize(h) != 18)      FAIL("font size did not survive the RTF round trip");

    // Paragraph attributes.
    Rich_SetAlignment(h, PFA_CENTER);
    if (Rich_GetAlignment(h) != PFA_CENTER) FAIL("alignment did not round trip");

    Rich_SetListStyle(h, PFN_BULLET);
    if (Rich_GetListStyle(h) != PFN_BULLET) FAIL("bullet list did not round trip");
    Rich_SetListStyle(h, 0);

    // Replace-all must terminate even when the replacement contains the needle,
    // which is the case that loops forever if the scan restarts at the match.
    Rich_SetText(h, L"aaa");
    int replaced = Rich_ReplaceAll(h, L"a", L"aa", FALSE, FALSE);
    if (replaced != 3) FAIL("replace all did not replace each occurrence exactly once");

    text = Rich_GetText(h);
    if (!text || wcscmp(text, L"aaaaaa") != 0) {
        free(text);
        FAIL("replace all produced the wrong result");
    }
    free(text);

    // The file path, which is what actually saves and opens a document -- the
    // string round trip above does not touch EM_STREAMIN's file callbacks.
    {
        WCHAR tmpDir[MAX_PATH], tmpFile[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmpDir) == 0 ||
            GetTempFileNameW(tmpDir, L"ont", 0, tmpFile) == 0) {
            FAIL("could not make a temporary file for the RTF file check");
        }

        Rich_SetText(h, L"Round trip");
        Rich_SetSelection(h, 0, 5);
        Rich_ToggleEffect(h, CFE_BOLD);
        Rich_SetFontSize(h, 22);

        if (!Rich_SaveRtfFile(h, tmpFile)) {
            DeleteFileW(tmpFile);
            FAIL("Rich_SaveRtfFile failed");
        }

        Rich_SetText(h, L"");
        if (!Rich_LoadRtfFile(h, tmpFile)) {
            DeleteFileW(tmpFile);
            FAIL("Rich_LoadRtfFile failed");
        }
        DeleteFileW(tmpFile);

        WCHAR* back = Rich_GetText(h);
        if (!back || wcsncmp(back, L"Round trip", 10) != 0) {
            free(back);
            FAIL("text did not survive a save and load through a file");
        }
        free(back);

        Rich_SetSelection(h, 0, 5);
        if (!Rich_HasEffect(h, CFE_BOLD)) FAIL("bold did not survive the file round trip");
        if (Rich_GetFontSize(h) != 22)    FAIL("font size did not survive the file round trip");

        // A load must leave the document clean, or opening a file would
        // immediately prompt to save it on close.
        if (Rich_GetModified(h)) FAIL("Rich_LoadRtfFile left the document modified");
    }

    // Loading something that is not RTF at all must fail rather than fill the
    // document with the raw bytes.
    {
        WCHAR tmpDir[MAX_PATH], tmpFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpDir);
        GetTempFileNameW(tmpDir, L"onb", 0, tmpFile);

        HANDLE f = CreateFileW(tmpFile, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(f, "\x01\x02 not rtf \x03", 15, &wrote, NULL);
            CloseHandle(f);
        }
        Rich_LoadRtfFile(h, tmpFile);   // may succeed or fail; must not crash
        DeleteFileW(tmpFile);
    }

    // Word lookup, which the spell-check context menu depends on.
    Rich_SetText(h, L"alpha beta gamma");
    int ws = 0, we = 0;
    WCHAR* word = Rich_GetWordAt(h, 7, &ws, &we);
    if (!word || wcscmp(word, L"beta") != 0) {
        free(word);
        FAIL("Rich_GetWordAt did not find the word under the position");
    }
    free(word);
    if (ws != 6 || we != 10) FAIL("Rich_GetWordAt reported the wrong bounds");

    DestroyWindow(h);
    failure[0] = '\0';
    return TRUE;

#undef FAIL
}
