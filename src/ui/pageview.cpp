// The page view: the laid-out document, drawn and edited.
//
// The engine answers "where does everything go" and knows nothing about
// windows. This is the other half -- a scrollable window that paints those
// pages with Direct2D, and, since v0.8, the window that edits them: a click
// names a character, the arrows walk the laid-out lines rather than the model,
// and every keystroke changes the model and lays it out again.
//
// Nothing here knows how to break a line or where a paragraph ends up. It asks
// the engine, which is the point of the engine being free of any window: the
// screen and the printed page cannot disagree, because neither of them decides
// anything on its own.
//
// C++ for the same reason the engine is: Direct2D and DirectWrite have no C
// bindings. Everything it exposes to the rest of the project is plain C.

#include "supernote.h"
#include "res/resource.h"
#include "layout/layout.h"
#include "layout/layout_internal.h"
#include "layout/layoutimage.h"
#include "ui/pageview.h"
#include "ui/editor_rich.h"

#include <d2d1.h>
#include <windowsx.h>
#include <math.h>

#pragma comment(lib, "d2d1.lib")

#define PAGEVIEW_CLASS L"OpenNotePageView"
#define PAGE_GAP_DIP   16.0f
#define RULER_H        24.0f    // the ruler band across the top, in DIPs
#define TWIPS_PER_DIP  15.0f
#define TWIPS_PER_INCH 1440
#define CARET_TIMER    1
#define UNDO_MAX       64

struct PageViewState {
    LayoutResult*         layout;
    DocModel*             doc;          // owned; the layout points into it
    HWND                  hRich;        // where the document came from, and goes back
    char*                 openedWith;   // the control's RTF when this window opened

    ID2D1Factory*         d2d;
    ID2D1HwndRenderTarget* target;
    ID2D1SolidColorBrush* textBrush;
    ID2D1SolidColorBrush* pageBrush;
    ID2D1SolidColorBrush* lineBrush;
    ID2D1SolidColorBrush* selBrush;

    float zoom;
    BOOL  userZoomed;     // once true, stop fitting the page to the window
    int   scrollY;        // in DIPs
    int   contentHeight;  // total, at current zoom

    // The caret, and the other end of the selection. Held as paragraph indices
    // rather than pointers because undo puts a different model in place, and
    // an index survives that where a pointer would dangle.
    int      caretPara;
    unsigned caretOff;
    int      anchorPara;
    unsigned anchorOff;

    BOOL  focused;
    BOOL  caretOn;
    BOOL  dirty;

    // Dragging an indent marker on the ruler. 0 = not dragging, 1 = the
    // first-line marker, 2 = the left indent (which carries the first line
    // with it, the way every ruler does).
    int   dragMarker;

    IDWriteFactory*    dwrite;      // for the ruler's numbers, and nothing else
    IDWriteTextFormat* rulerFont;

    // Pictures, decoded once for the device they are drawn on. A document
    // with the same picture ten times decodes it once.
    struct {
        const DocImage* image;
        ID2D1Bitmap*    bitmap;
    } pictures[64];
    int pictureCount;

    DocModel* undo[UNDO_MAX];
    int       undoCount;
    DocModel* redo[UNDO_MAX];
    int       redoCount;
};

// The client area in DIPs rather than pixels.
//
// A Direct2D window render target works in DIPs at the system DPI, while
// GetClientRect answers in physical pixels. On a scaled display those are not
// the same number, and treating them as if they were drew the page about 1.5x
// too large and ran it off the right of the window.
static void ClientSizeDip(HWND hwnd, ID2D1RenderTarget* rt, float* w, float* h) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    float dpiX = 96.0f, dpiY = 96.0f;
    if (rt) rt->GetDpi(&dpiX, &dpiY);
    if (dpiX <= 0.0f) dpiX = 96.0f;
    if (dpiY <= 0.0f) dpiY = 96.0f;

    if (w) *w = (rc.right - rc.left) * 96.0f / dpiX;
    if (h) *h = (rc.bottom - rc.top) * 96.0f / dpiY;
}

// A mouse position arrives in physical pixels; everything drawn is in DIPs.
static void PixelsToDip(ID2D1RenderTarget* rt, int px, int py, float* x, float* y) {
    float dpiX = 96.0f, dpiY = 96.0f;
    if (rt) rt->GetDpi(&dpiX, &dpiY);
    if (dpiX <= 0.0f) dpiX = 96.0f;
    if (dpiY <= 0.0f) dpiY = 96.0f;

    *x = px * 96.0f / dpiX;
    *y = py * 96.0f / dpiY;
}

// ---------------------------------------------------------------------------
// Device resources
// ---------------------------------------------------------------------------

// A bitmap belongs to the render target it was made for, so both go together.
static void DiscardPictures(PageViewState* st) {
    for (int i = 0; i < st->pictureCount; i++) {
        if (st->pictures[i].bitmap) st->pictures[i].bitmap->Release();
    }
    st->pictureCount = 0;
}

static ID2D1Bitmap* PictureFor(PageViewState* st, const DocImage* image) {
    for (int i = 0; i < st->pictureCount; i++) {
        if (st->pictures[i].image == image) return st->pictures[i].bitmap;
    }
    if (st->pictureCount >= 64) return NULL;

    ID2D1Bitmap* bitmap = LayoutImage_Create(st->target, image);
    st->pictures[st->pictureCount].image = image;
    st->pictures[st->pictureCount].bitmap = bitmap;   // NULL is remembered too
    st->pictureCount++;
    return bitmap;
}

static void DiscardTarget(PageViewState* st) {
    DiscardPictures(st);
    if (st->textBrush) { st->textBrush->Release(); st->textBrush = NULL; }
    if (st->pageBrush) { st->pageBrush->Release(); st->pageBrush = NULL; }
    if (st->lineBrush) { st->lineBrush->Release(); st->lineBrush = NULL; }
    if (st->selBrush)  { st->selBrush->Release();  st->selBrush = NULL; }
    if (st->target)    { st->target->Release();    st->target = NULL; }
}

static BOOL EnsureTarget(HWND hwnd, PageViewState* st) {
    if (st->target) return TRUE;
    if (!st->d2d) return FALSE;

    RECT rc;
    GetClientRect(hwnd, &rc);

    D2D1_SIZE_U size = D2D1::SizeU((UINT32)(rc.right - rc.left),
                                   (UINT32)(rc.bottom - rc.top));
    if (size.width == 0 || size.height == 0) return FALSE;

    if (FAILED(st->d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &st->target))) {
        return FALSE;
    }

    st->target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &st->textBrush);
    st->target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &st->pageBrush);
    st->target->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.70f, 0.72f), &st->lineBrush);

    // The selection is drawn under the text rather than over it, so coloured
    // text keeps its colour and nothing has to be drawn twice.
    st->target->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.45f, 0.85f, 0.30f), &st->selBrush);

    return st->textBrush && st->pageBrush && st->lineBrush && st->selBrush;
}

// Colour is carried beside the layout rather than inside it, because the
// engine has no Direct2D to make a brush with. Applying it here is the other
// end of that arrangement. Done once per layout; the effect sticks.
static void ApplyColors(PageViewState* st, LaidText* t) {
    if (!t->colorCount || !t->colors) return;

    for (int i = 0; i < t->colorCount; i++) {
        COLORREF c = t->colors[i].color;
        ID2D1SolidColorBrush* brush = NULL;
        if (FAILED(st->target->CreateSolidColorBrush(
                D2D1::ColorF(GetRValue(c) / 255.0f,
                             GetGValue(c) / 255.0f,
                             GetBValue(c) / 255.0f),
                &brush))) {
            continue;
        }

        DWRITE_TEXT_RANGE range = { t->colors[i].start, t->colors[i].len };
        t->layout->SetDrawingEffect(brush, range);
        brush->Release();   // the layout holds its own reference
    }

    // Applied; do not do it again on the next paint.
    free(t->colors);
    t->colors = NULL;
    t->colorCount = 0;
}

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------

static LayoutPos PosOf(PageViewState* st, int paraIndex, unsigned offset) {
    LayoutPos p = { NULL, 0 };

    DocPara* para = Doc_ParaAt(st->doc, paraIndex);
    if (!para) {
        int last = Doc_CountParas(st->doc) - 1;
        para = Doc_ParaAt(st->doc, last < 0 ? 0 : last);
        offset = para ? Doc_ParaLength(para) : 0;
    }
    if (!para) return p;

    unsigned len = Doc_ParaLength(para);
    p.para = para;
    p.offset = offset > len ? len : offset;
    return p;
}

static LayoutPos CaretPos(PageViewState* st)  { return PosOf(st, st->caretPara, st->caretOff); }
static LayoutPos AnchorPos(PageViewState* st) { return PosOf(st, st->anchorPara, st->anchorOff); }

// The engine hands back positions into a model it borrowed as const; editing
// one needs that pointer back. It is the same paragraph, and this window owns
// the model it was laid out from.
static DocPos ToDocPos(LayoutPos p) {
    DocPos d = { (DocPara*)p.para, p.offset };
    return d;
}

static BOOL HasSelection(PageViewState* st) {
    return st->caretPara != st->anchorPara || st->caretOff != st->anchorOff;
}

static void SetCaret(PageViewState* st, LayoutPos pos, BOOL extend) {
    if (!pos.para) return;

    st->caretPara = Doc_ParaIndexOf(st->doc, pos.para);
    st->caretOff = pos.offset;
    if (!extend) {
        st->anchorPara = st->caretPara;
        st->anchorOff = st->caretOff;
    }
    st->caretOn = TRUE;
}

// The selection in document order, which is not the order it was made in.
static void Selection(PageViewState* st, LayoutPos* a, LayoutPos* b) {
    LayoutPos caret = CaretPos(st);
    LayoutPos anchor = AnchorPos(st);

    if (Layout_ComparePos(st->layout, anchor, caret) <= 0) {
        *a = anchor;
        *b = caret;
    } else {
        *a = caret;
        *b = anchor;
    }
}

// ---------------------------------------------------------------------------
// Forward declarations for the editing half
// ---------------------------------------------------------------------------

static void SetTitle(HWND hwnd, PageViewState* st, const WCHAR* docTitle);
static void UpdateScrollRange(HWND hwnd, PageViewState* st);
static void EnsureCaretVisible(HWND hwnd, PageViewState* st);

// Every edit ends the same way: the model changed, so lay it out again. A
// whole-document layout per keystroke is not as expensive as it sounds -- a
// hundred pages measure in a few milliseconds, because DirectWrite does the
// per-paragraph work and that is the part that costs.
//
// ponytail: relays out everything. Lay out from the changed paragraph onward
// when a document gets big enough for the typing to lag.
static void Relayout(HWND hwnd, PageViewState* st) {
    Layout_Free(st->layout);
    st->layout = Layout_Build(st->doc, L"Calibri", 11.0f);

    UpdateScrollRange(hwnd, st);
    SetTitle(hwnd, st, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
}

// ---------------------------------------------------------------------------
// Undo
//
// A snapshot of the model per edit. That sounds wasteful and is not: the model
// is small, and a snapshot restores exactly what was there, where an inverse
// operation can be subtly wrong in a way that only shows up three edits later.
//
// ponytail: no coalescing, so undo steps back a character at a time. Group
// consecutive typing into one step when somebody complains.
// ---------------------------------------------------------------------------

static void ClearStack(DocModel** stack, int* count) {
    for (int i = 0; i < *count; i++) Doc_Free(stack[i]);
    *count = 0;
}

static void PushModel(DocModel** stack, int* count, DocModel* model) {
    if (!model) return;
    if (*count == UNDO_MAX) {
        Doc_Free(stack[0]);
        memmove(stack, stack + 1, (UNDO_MAX - 1) * sizeof(DocModel*));
        (*count)--;
    }
    stack[(*count)++] = model;
}

// Called before a change, not after: what goes on the stack is the document as
// it was.
static void PushUndo(PageViewState* st) {
    PushModel(st->undo, &st->undoCount, Doc_Clone(st->doc));
    ClearStack(st->redo, &st->redoCount);
    st->dirty = TRUE;
}

// An edit that changed nothing should not leave a step to undo past.
static void DropLastUndo(PageViewState* st) {
    if (st->undoCount > 0) Doc_Free(st->undo[--st->undoCount]);
}

static void SwapModel(HWND hwnd, PageViewState* st, DocModel** from, int* fromCount,
                      DocModel** to, int* toCount) {
    if (*fromCount == 0) return;

    PushModel(to, toCount, st->doc);      // the current one becomes the other side's step
    st->doc = from[--(*fromCount)];

    int count = Doc_CountParas(st->doc);
    if (st->caretPara >= count) st->caretPara = count - 1;
    if (st->anchorPara >= count) st->anchorPara = count - 1;
    if (st->caretPara < 0) st->caretPara = 0;
    if (st->anchorPara < 0) st->anchorPara = 0;
    st->anchorPara = st->caretPara;
    st->anchorOff = st->caretOff;

    st->dirty = TRUE;
    Relayout(hwnd, st);
    EnsureCaretVisible(hwnd, st);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

// Delete what is selected. Answers FALSE when there was nothing selected, or
// when the model refused: a range running into a table is not a splice it can
// make, and half-doing it would be worse than not doing it.
static BOOL DeleteSelection(HWND hwnd, PageViewState* st) {
    if (!HasSelection(st)) return FALSE;

    LayoutPos a = {}, b = {};
    Selection(st, &a, &b);

    PushUndo(st);

    DocPos from = ToDocPos(a);
    DocPos to = ToDocPos(b);
    DocPos out = {};
    if (!DocEdit_DeleteRange(st->doc, from, to, &out)) {
        DropLastUndo(st);
        MessageBeep(MB_ICONWARNING);
        return FALSE;
    }

    LayoutPos at = { out.para, out.offset };
    SetCaret(st, at, FALSE);
    Relayout(hwnd, st);
    EnsureCaretVisible(hwnd, st);
    return TRUE;
}

static void InsertText(HWND hwnd, PageViewState* st, const WCHAR* text, int len) {
    if (!text || !*text) return;
    if (len < 0) len = (int)wcslen(text);

    if (HasSelection(st) && !DeleteSelection(hwnd, st)) return;

    PushUndo(st);

    LayoutPos caret = CaretPos(st);
    DocPos at = ToDocPos(caret);

    // Pasted text carries line breaks, and each one is a new paragraph.
    int start = 0;
    for (int i = 0; i <= len; i++) {
        BOOL isBreak = (i < len) && (text[i] == L'\n' || text[i] == L'\r');
        if (i < len && !isBreak) continue;

        if (i > start) DocEdit_Insert(st->doc, &at, text + start, i - start);
        if (isBreak) {
            DocEdit_SplitPara(st->doc, &at);
            if (text[i] == L'\r' && i + 1 < len && text[i + 1] == L'\n') i++;
        }
        start = i + 1;
    }

    LayoutPos after = { at.para, at.offset };
    SetCaret(st, after, FALSE);
    Relayout(hwnd, st);
    EnsureCaretVisible(hwnd, st);
}

static void SplitAtCaret(HWND hwnd, PageViewState* st) {
    if (HasSelection(st) && !DeleteSelection(hwnd, st)) return;

    PushUndo(st);

    LayoutPos caret = CaretPos(st);
    DocPos at = ToDocPos(caret);
    if (!DocEdit_SplitPara(st->doc, &at)) {
        DropLastUndo(st);
        return;
    }

    LayoutPos after = { at.para, at.offset };
    SetCaret(st, after, FALSE);
    Relayout(hwnd, st);
    EnsureCaretVisible(hwnd, st);
}

// Backspace and Delete are the same operation over a one-character range --
// including when that character is the paragraph break itself, which is how
// two paragraphs become one.
static void DeleteCharacter(HWND hwnd, PageViewState* st, BOOL forward) {
    if (HasSelection(st)) {
        DeleteSelection(hwnd, st);
        return;
    }

    LayoutPos caret = CaretPos(st);
    if (!caret.para) return;

    LayoutPos a = caret, b = caret;

    if (forward) {
        if (caret.offset < Doc_ParaLength(caret.para)) {
            b.offset = caret.offset + 1;
        } else {
            DocPara* next = Doc_ParaAt(st->doc, st->caretPara + 1);
            if (!next) return;
            b.para = next;
            b.offset = 0;
        }
    } else {
        if (caret.offset > 0) {
            a.offset = caret.offset - 1;
        } else {
            DocPara* prev = Doc_ParaAt(st->doc, st->caretPara - 1);
            if (!prev) return;
            a.para = prev;
            a.offset = Doc_ParaLength(prev);
        }
    }

    PushUndo(st);

    DocPos from = ToDocPos(a);
    DocPos to = ToDocPos(b);
    DocPos out = {};
    if (!DocEdit_DeleteRange(st->doc, from, to, &out)) {
        DropLastUndo(st);
        MessageBeep(MB_ICONWARNING);
        return;
    }

    LayoutPos at = { out.para, out.offset };
    SetCaret(st, at, FALSE);
    Relayout(hwnd, st);
    EnsureCaretVisible(hwnd, st);
}

// ---------------------------------------------------------------------------
// The clipboard
//
// Text only, in both directions. Formatted copy wants a second serializer and
// a second reader, and RTF on the clipboard is what the rich text view is
// already for.
// ---------------------------------------------------------------------------

static void CopySelection(HWND hwnd, PageViewState* st, BOOL cut) {
    if (!HasSelection(st)) return;

    LayoutPos a = {}, b = {};
    Selection(st, &a, &b);

    DocPos from = ToDocPos(a);
    DocPos to = ToDocPos(b);
    WCHAR* text = DocEdit_RangeText(st->doc, from, to);
    if (!text) return;

    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dest = GlobalLock(mem);
        if (dest) {
            memcpy(dest, text, bytes);
            GlobalUnlock(mem);

            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
                CloseClipboard();
            } else {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }

    free(text);
    if (cut) DeleteSelection(hwnd, st);
}

static void PasteText(HWND hwnd, PageViewState* st) {
    if (!OpenClipboard(hwnd)) return;

    WCHAR* copy = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const WCHAR* text = (const WCHAR*)GlobalLock(h);
        if (text) {
            // Taken away before the clipboard closes: the handle is only ours
            // until then.
            size_t len = wcslen(text);
            copy = (WCHAR*)malloc((len + 1) * sizeof(WCHAR));
            if (copy) memcpy(copy, text, (len + 1) * sizeof(WCHAR));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();

    if (copy) {
        InsertText(hwnd, st, copy, -1);
        free(copy);
    }
}

// ---------------------------------------------------------------------------
// Back into the document
//
// The page view edits a model captured from the rich text view, so what it
// changes has to go back there for saving to mean anything. RTF is the way
// in: the control is its own reader, and that path is already lossless.
// ---------------------------------------------------------------------------

static void ApplyToDocument(HWND hwnd, PageViewState* st) {
    if (!st->dirty || !st->hRich || !IsWindow(st->hRich)) return;

    // Somebody may have typed in the main window while this one was open.
    // Overwriting that silently would lose it, so ask first.
    char* now = Rich_GetRtf(st->hRich);
    if (now && st->openedWith && strcmp(now, st->openedWith) != 0) {
        int answer = MessageBoxW(hwnd,
            L"The document was also changed in the main window.\n\n"
            L"Apply the changes made here on top of it?",
            APP_NAME, MB_ICONWARNING | MB_YESNO);
        if (answer != IDYES) {
            free(now);
            return;
        }
    }
    free(now);

    char* rtf = DocRtf_Emit(st->doc);
    if (!rtf) return;

    Rich_SetRtf(st->hRich, rtf);
    free(rtf);

    // What the control holds now is what this window put there, so a later
    // write-back has nothing to warn about.
    free(st->openedWith);
    st->openedWith = Rich_GetRtf(st->hRich);
    st->dirty = FALSE;
}

// ---------------------------------------------------------------------------
// Scrolling and page geometry
// ---------------------------------------------------------------------------

// Where a page's top-left corner sits in the window, in DIPs. The one place
// that knows how pages are arranged; painting and hit testing both ask it, so
// a click cannot land somewhere other than where the page was drawn.
static void PageOrigin(HWND hwnd, PageViewState* st, int page, float* left, float* top) {
    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(st->layout, &pw, &ph);

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);

    float l = (clientW - pw * st->zoom) / 2.0f;
    if (l < PAGE_GAP_DIP) l = PAGE_GAP_DIP;

    *left = l;
    *top = RULER_H + (PAGE_GAP_DIP + page * (ph + PAGE_GAP_DIP)) * st->zoom - st->scrollY;
}

static void UpdateScrollRange(HWND hwnd, PageViewState* st) {
    if (!st->layout) return;

    float pw = 0, ph = 0;
    Layout_PageSize(st->layout, &pw, &ph);

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);

    int pages = Layout_PageCount(st->layout);
    st->contentHeight = (int)((ph + PAGE_GAP_DIP) * pages * st->zoom + PAGE_GAP_DIP);

    int visible = (int)(clientH - RULER_H);
    if (visible < 1) visible = 1;

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = st->contentHeight > 0 ? st->contentHeight : 0;
    si.nPage = (UINT)visible;

    if (st->scrollY > st->contentHeight - visible) st->scrollY = st->contentHeight - visible;
    if (st->scrollY < 0) st->scrollY = 0;
    si.nPos = st->scrollY;

    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static void EnsureCaretVisible(HWND hwnd, PageViewState* st) {
    if (!st->layout) return;

    int page = 0;
    LayoutRect caret = {};
    if (!Layout_PosRect(st->layout, CaretPos(st), &page, &caret)) return;

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);
    if (clientH <= 0.0f) return;

    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(st->layout, &pw, &ph);

    // Where the caret is in the whole stack of pages, before scrolling.
    float docY = (PAGE_GAP_DIP + page * (ph + PAGE_GAP_DIP) + caret.y) * st->zoom;
    float height = caret.height * st->zoom;
    float visible = clientH - RULER_H;

    if (docY - st->scrollY < 0) {
        st->scrollY = (int)(docY - 8.0f);
    } else if (docY + height - st->scrollY > visible) {
        st->scrollY = (int)(docY + height - visible + 8.0f);
    } else {
        return;
    }

    UpdateScrollRange(hwnd, st);
    InvalidateRect(hwnd, NULL, FALSE);
}

// Which page a point in the window is over, and where on that page it falls.
// The nearest page when it is over the gap between two, so a drag that runs
// off the paper still selects.
static BOOL PointToPage(HWND hwnd, PageViewState* st, int px, int py,
                        int* pageOut, float* pageX, float* pageY) {
    if (!st->layout) return FALSE;

    float x = 0.0f, y = 0.0f;
    PixelsToDip(st->target, px, py, &x, &y);

    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(st->layout, &pw, &ph);

    int pages = Layout_PageCount(st->layout);
    int best = -1;
    float bestDistance = 0.0f;

    for (int i = 0; i < pages; i++) {
        float left = 0.0f, top = 0.0f;
        PageOrigin(hwnd, st, i, &left, &top);

        float bottom = top + ph * st->zoom;
        float distance = 0.0f;
        if (y < top) distance = top - y;
        else if (y > bottom) distance = y - bottom;

        if (best < 0 || distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
        if (distance == 0.0f) break;
    }

    if (best < 0) return FALSE;

    float left = 0.0f, top = 0.0f;
    PageOrigin(hwnd, st, best, &left, &top);

    *pageOut = best;
    *pageX = (x - left) / st->zoom;
    *pageY = (y - top) / st->zoom;
    return TRUE;
}

// ---------------------------------------------------------------------------
// The ruler
//
// A ruler needs a page width to mean anything, which is why it arrives with
// the layout engine rather than with the rich text view. It shows where the
// margins are, marks the tab stops the engine uses, and carries the two
// markers that set a paragraph's indent -- dragging one is an edit like any
// other, undo included.
// ---------------------------------------------------------------------------

// The paragraph the ruler is about: the one the caret is in.
static DocPara* RulerPara(PageViewState* st) {
    return Doc_ParaAt(st->doc, st->caretPara);
}

// Where a twips offset from the left margin sits in the window.
static float RulerX(HWND hwnd, PageViewState* st, int twips) {
    float left = 0.0f, top = 0.0f;
    PageOrigin(hwnd, st, 0, &left, &top);
    float margin = st->doc->section.marginLeft / TWIPS_PER_DIP;
    return left + (margin + twips / TWIPS_PER_DIP) * st->zoom;
}

// ...and back again, snapped to a sixteenth of an inch, which is fine enough
// to place an indent and coarse enough to hit twice running.
static int RulerTwips(HWND hwnd, PageViewState* st, float x) {
    float left = 0.0f, top = 0.0f;
    PageOrigin(hwnd, st, 0, &left, &top);
    float margin = st->doc->section.marginLeft / TWIPS_PER_DIP;

    float dips = (x - left) / st->zoom - margin;
    int twips = (int)(dips * TWIPS_PER_DIP);

    int step = TWIPS_PER_INCH / 16;
    twips = ((twips + (twips < 0 ? -step / 2 : step / 2)) / step) * step;
    return twips;
}

static void DrawTriangle(ID2D1RenderTarget* rt, ID2D1Factory* factory,
                         float cx, float cy, float size, BOOL pointUp,
                         ID2D1Brush* brush) {
    ID2D1PathGeometry* path = NULL;
    if (FAILED(factory->CreatePathGeometry(&path))) return;

    ID2D1GeometrySink* sink = NULL;
    if (SUCCEEDED(path->Open(&sink))) {
        float tip = pointUp ? cy - size : cy + size;
        sink->BeginFigure(D2D1::Point2F(cx, tip), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(cx - size, pointUp ? cy + size * 0.6f : cy - size * 0.6f));
        sink->AddLine(D2D1::Point2F(cx + size, pointUp ? cy + size * 0.6f : cy - size * 0.6f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();

        rt->FillGeometry(path, brush);
    }

    path->Release();
}

static void PaintRuler(HWND hwnd, PageViewState* st, float clientW) {
    ID2D1RenderTarget* rt = st->target;

    ID2D1SolidColorBrush* face = NULL;
    ID2D1SolidColorBrush* ink = NULL;
    ID2D1SolidColorBrush* margin = NULL;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.97f), &face);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.35f, 0.38f), &ink);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.82f, 0.82f, 0.84f), &margin);
    if (!face || !ink || !margin) {
        if (face) face->Release();
        if (ink) ink->Release();
        if (margin) margin->Release();
        return;
    }

    rt->FillRectangle(D2D1::RectF(0, 0, clientW, RULER_H), face);

    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(st->layout, &pw, &ph);

    float pageLeft = 0.0f, pageTop = 0.0f;
    PageOrigin(hwnd, st, 0, &pageLeft, &pageTop);

    float leftMargin = RulerX(hwnd, st, 0);
    float rightMargin = pageLeft + (pw - st->doc->section.marginRight / TWIPS_PER_DIP) * st->zoom;

    // The margins, shaded, so the writing area is obvious at a glance.
    rt->FillRectangle(D2D1::RectF(pageLeft, 4.0f, leftMargin, RULER_H - 6.0f), margin);
    rt->FillRectangle(D2D1::RectF(rightMargin, 4.0f,
                                  pageLeft + pw * st->zoom, RULER_H - 6.0f), margin);

    // Ticks every half inch -- where the tab stops are -- and a taller one on
    // the inch, numbered.
    int halves = (int)((pw / 96.0f) * 2.0f) + 1;
    for (int i = 0; i <= halves; i++) {
        float x = leftMargin + i * 48.0f * st->zoom;
        if (x > pageLeft + pw * st->zoom) break;
        if (x < pageLeft) continue;

        BOOL inch = (i % 2) == 0;
        float half = RULER_H / 2.0f;
        rt->DrawLine(D2D1::Point2F(x, half - (inch ? 5.0f : 2.5f)),
                     D2D1::Point2F(x, half + (inch ? 5.0f : 2.5f)), ink, 1.0f);

        if (inch && i > 0 && st->rulerFont) {
            WCHAR label[8];
            swprintf_s(label, 8, L"%d", i / 2);
            rt->DrawTextW(label, (UINT32)wcslen(label), st->rulerFont,
                          D2D1::RectF(x - 10.0f, 2.0f, x + 10.0f, RULER_H - 2.0f),
                          ink, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    // The indent markers for the paragraph the caret is in.
    DocPara* para = RulerPara(st);
    if (para) {
        int left = para->props.indentLeft;
        int first = left + para->props.indentFirst;

        DrawTriangle(rt, st->d2d, RulerX(hwnd, st, first), 6.0f, 5.0f, TRUE, ink);
        DrawTriangle(rt, st->d2d, RulerX(hwnd, st, left), RULER_H - 6.0f, 5.0f, FALSE, ink);
    }

    rt->DrawLine(D2D1::Point2F(0, RULER_H), D2D1::Point2F(clientW, RULER_H), ink, 1.0f);

    face->Release();
    ink->Release();
    margin->Release();
}

// Which marker, if any, is under a point in the ruler band.
static int MarkerAt(HWND hwnd, PageViewState* st, float x, float y) {
    DocPara* para = RulerPara(st);
    if (!para || y > RULER_H) return 0;

    float first = RulerX(hwnd, st, para->props.indentLeft + para->props.indentFirst);
    float left = RulerX(hwnd, st, para->props.indentLeft);

    // The top half belongs to the first-line marker and the bottom half to the
    // left indent, so the two can sit on top of each other and still be told
    // apart -- which they do, in every paragraph without a hanging indent.
    if (y < RULER_H / 2.0f) {
        if (fabsf(x - first) <= 7.0f) return 1;
    } else {
        if (fabsf(x - left) <= 7.0f) return 2;
    }
    return 0;
}

// Dragging a marker sets the indent on every paragraph the selection touches,
// or on the caret's paragraph when there is no selection.
static void SetIndentFromRuler(HWND hwnd, PageViewState* st, int marker, float x) {
    int twips = RulerTwips(hwnd, st, x);
    if (twips < 0) twips = 0;

    int from = st->caretPara, to = st->anchorPara;
    if (from > to) { int swap = from; from = to; to = swap; }

    for (int i = from; i <= to; i++) {
        DocPara* para = Doc_ParaAt(st->doc, i);
        if (!para) continue;

        if (marker == 1) {
            // The first line, measured from the left indent: dragging it left
            // of the indent is a hanging indent, which is how a list looks.
            para->props.indentFirst = twips - para->props.indentLeft;
        } else {
            // The left indent takes the first line with it, so the shape of
            // the paragraph survives the drag.
            para->props.indentLeft = twips;
        }
    }

    Relayout(hwnd, st);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

// Until the user zooms, the page is sized to fit the window's width, the way
// every print preview does -- otherwise a Letter page is wider than a default
// window and the reader only ever sees its left half.
//
// This has to run against the DPI the drawing will use, which means after the
// render target exists. Computing it earlier assumed 96 DPI and the real DPI
// then undid it.
static void FitIfNeeded(HWND hwnd, PageViewState* st) {
    if (st->userZoomed || !st->layout || !st->target) return;

    float pw = 0.0f, ph = 0.0f;
    Layout_PageSize(st->layout, &pw, &ph);
    if (pw <= 0.0f) return;

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);
    if (clientW <= 0.0f) return;

    float fit = (clientW - 2 * PAGE_GAP_DIP) / pw;
    (void)clientH;
    if (fit < 0.15f) fit = 0.15f;
    if (fit > 2.0f)  fit = 2.0f;

    if (fit != st->zoom) {
        st->zoom = fit;
        UpdateScrollRange(hwnd, st);
        SetTitle(hwnd, st, NULL);
    }
}

static void Paint(HWND hwnd, PageViewState* st) {
    if (!EnsureTarget(hwnd, st)) {
        ValidateRect(hwnd, NULL);
        return;
    }

    FitIfNeeded(hwnd, st);

    float clientW = 0.0f, clientH = 0.0f;
    ClientSizeDip(hwnd, st->target, &clientW, &clientH);

    st->target->BeginDraw();
    st->target->SetTransform(D2D1::Matrix3x2F::Identity());
    st->target->Clear(D2D1::ColorF(0.45f, 0.45f, 0.48f));

    if (st->layout) {
        float pw = 0, ph = 0;
        Layout_PageSize(st->layout, &pw, &ph);

        int pages = Layout_PageCount(st->layout);
        float z = st->zoom;

        LayoutPos selA = {}, selB = {};
        BOOL selecting = HasSelection(st);
        if (selecting) Selection(st, &selA, &selB);

        int caretPage = -1;
        LayoutRect caretRect = {};
        BOOL haveCaret = Layout_PosRect(st->layout, CaretPos(st), &caretPage, &caretRect);

        for (int i = 0; i < pages; i++) {
            float pageLeft = 0.0f, pageTop = 0.0f;
            PageOrigin(hwnd, st, i, &pageLeft, &pageTop);

            // Skip pages entirely off screen: a hundred-page document should
            // not cost a hundred pages of drawing to show one.
            if (pageTop + ph * z < 0) continue;
            if (pageTop > clientH) break;

            D2D1_RECT_F paper = D2D1::RectF(pageLeft, pageTop,
                                            pageLeft + pw * z, pageTop + ph * z);
            st->target->FillRectangle(paper, st->pageBrush);
            st->target->DrawRectangle(paper, st->lineBrush, 1.0f);

            // Draw the page's contents in page coordinates.
            st->target->SetTransform(
                D2D1::Matrix3x2F::Scale(z, z) *
                D2D1::Matrix3x2F::Translation(pageLeft, pageTop));

            const LaidPage* page = &st->layout->pages[i];

            for (int c = 0; c < page->cellCount; c++) {
                const LaidCell* cell = &page->cells[c];
                st->target->DrawRectangle(
                    D2D1::RectF(cell->x, cell->y,
                                cell->x + cell->width, cell->y + cell->height),
                    st->lineBrush, 0.75f);
            }

            if (selecting) {
                LayoutRect rects[128];
                int n = Layout_RangeRects(st->layout, i, selA, selB, rects, 128);
                for (int k = 0; k < n; k++) {
                    st->target->FillRectangle(
                        D2D1::RectF(rects[k].x, rects[k].y,
                                    rects[k].x + rects[k].width,
                                    rects[k].y + rects[k].height),
                        st->selBrush);
                }
            }

            for (int t = 0; t < page->textCount; t++) {
                LaidText* lt = &page->texts[t];
                if (!lt->layout) continue;
                ApplyColors(st, lt);
                st->target->DrawTextLayout(D2D1::Point2F(lt->x, lt->y),
                                           lt->layout, st->textBrush,
                                           D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            for (int m = 0; m < page->imageCount; m++) {
                const LaidImage* pic = &page->images[m];
                ID2D1Bitmap* bitmap = PictureFor(st, pic->image);
                if (!bitmap) continue;

                st->target->DrawBitmap(
                    bitmap,
                    D2D1::RectF(pic->x, pic->y, pic->x + pic->width, pic->y + pic->height),
                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
            }

            if (haveCaret && caretPage == i && st->focused && st->caretOn && !selecting) {
                st->target->FillRectangle(
                    D2D1::RectF(caretRect.x, caretRect.y,
                                caretRect.x + 1.2f, caretRect.y + caretRect.height),
                    st->textBrush);
            }

            st->target->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    if (st->layout) PaintRuler(hwnd, st, clientW);

    HRESULT hr = st->target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // The device was lost -- a display change, a remote session. Drop the
        // resources and the next paint rebuilds them.
        DiscardTarget(st);
    }

    ValidateRect(hwnd, NULL);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void SetTitle(HWND hwnd, PageViewState* st, const WCHAR* docTitle) {
    // Remembered, so a retitle on zoom does not drop the document's name.
    static WCHAR remembered[256] = L"Document";
    if (docTitle && docTitle[0]) wcsncpy_s(remembered, 256, docTitle, _TRUNCATE);

    WCHAR title[512];
    swprintf_s(title, 512, L"Page Layout - %s%s - %d page%s at %d%%",
               remembered,
               st->dirty ? L" *" : L"",
               Layout_PageCount(st->layout),
               Layout_PageCount(st->layout) == 1 ? L"" : L"s",
               (int)(st->zoom * 100.0f + 0.5f));
    SetWindowTextW(hwnd, title);
}

// Arrows, Home, End and the page keys. All of them are questions about the
// laid-out page rather than about the model: "up" means the line above on the
// paper, which is not the run before in the tree, and "End" means the end of a
// wrapped line, which the model has no opinion about at all.
static void MoveCaret(HWND hwnd, PageViewState* st, WPARAM key, BOOL extend) {
    LayoutPos caret = CaretPos(st);
    LayoutPos target = caret;
    BOOL moved = FALSE;

    switch (key) {
        case VK_LEFT:
            if (caret.offset > 0) {
                target.offset = caret.offset - 1;
                moved = TRUE;
            } else {
                DocPara* prev = Doc_ParaAt(st->doc, st->caretPara - 1);
                if (prev) {
                    target.para = prev;
                    target.offset = Doc_ParaLength(prev);
                    moved = TRUE;
                }
            }
            break;

        case VK_RIGHT:
            if (caret.offset < Doc_ParaLength(caret.para)) {
                target.offset = caret.offset + 1;
                moved = TRUE;
            } else {
                DocPara* next = Doc_ParaAt(st->doc, st->caretPara + 1);
                if (next) {
                    target.para = next;
                    target.offset = 0;
                    moved = TRUE;
                }
            }
            break;

        case VK_UP:   moved = Layout_MoveLine(st->layout, caret, -1, &target); break;
        case VK_DOWN: moved = Layout_MoveLine(st->layout, caret, 1, &target);  break;

        case VK_HOME:
            if (GetKeyState(VK_CONTROL) < 0) {
                target.para = Doc_ParaAt(st->doc, 0);
                target.offset = 0;
                moved = target.para != NULL;
            } else {
                moved = Layout_LineEdge(st->layout, caret, FALSE, &target);
            }
            break;

        case VK_END:
            if (GetKeyState(VK_CONTROL) < 0) {
                int last = Doc_CountParas(st->doc) - 1;
                target.para = Doc_ParaAt(st->doc, last);
                target.offset = target.para ? Doc_ParaLength(target.para) : 0;
                moved = target.para != NULL;
            } else {
                moved = Layout_LineEdge(st->layout, caret, TRUE, &target);
            }
            break;

        case VK_PRIOR:
        case VK_NEXT: {
            // A page key moves a page, which the engine can answer exactly:
            // the same point on the page before or after.
            int page = 0;
            LayoutRect rect = {};
            if (!Layout_PosRect(st->layout, caret, &page, &rect)) break;

            int want = page + (key == VK_NEXT ? 1 : -1);
            if (want < 0 || want >= Layout_PageCount(st->layout)) break;

            moved = Layout_HitTest(st->layout, want, rect.x, rect.y, &target);
            break;
        }
    }

    if (!moved) return;

    SetCaret(st, target, extend);
    EnsureCaretVisible(hwnd, st);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void SelectWordAt(PageViewState* st, LayoutPos pos) {
    unsigned len = 0;
    WCHAR* text = Doc_ParaText(pos.para, &len);
    if (!text) return;

    unsigned from = pos.offset, to = pos.offset;
    while (from > 0 && iswalnum(text[from - 1])) from--;
    while (to < len && iswalnum(text[to])) to++;
    free(text);

    if (to == from) return;

    st->anchorPara = st->caretPara = Doc_ParaIndexOf(st->doc, pos.para);
    st->anchorOff = from;
    st->caretOff = to;
}

// A click, a drag or a double click, all of which start the same way: what
// character is under this point?
static void CaretFromPoint(HWND hwnd, PageViewState* st, LPARAM lParam,
                           BOOL extend, BOOL word) {
    int page = 0;
    float px = 0.0f, py = 0.0f;
    if (!PointToPage(hwnd, st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                     &page, &px, &py)) {
        return;
    }

    LayoutPos pos = {};
    if (!Layout_HitTest(st->layout, page, px, py, &pos)) return;

    if (word) SelectWordAt(st, pos);
    else      SetCaret(st, pos, extend);

    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK PageViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PageViewState* st = (PageViewState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            SetTimer(hwnd, CARET_TIMER, GetCaretBlinkTime(), NULL);
            return 0;
        }

        case WM_SIZE:
            if (st) {
                if (st->target) {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    D2D1_SIZE_U size = D2D1::SizeU((UINT32)(rc.right - rc.left),
                                                   (UINT32)(rc.bottom - rc.top));
                    st->target->Resize(size);
                }
                UpdateScrollRange(hwnd, st);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_PAINT:
            if (st) {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                Paint(hwnd, st);
                EndPaint(hwnd, &ps);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;   // the render target covers everything

        case WM_SETFOCUS:
            if (st) {
                st->focused = TRUE;
                st->caretOn = TRUE;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_KILLFOCUS:
            if (st) {
                st->focused = FALSE;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_TIMER:
            if (st && wParam == CARET_TIMER && st->focused) {
                st->caretOn = !st->caretOn;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (st) {
                SetFocus(hwnd);

                float x = 0.0f, y = 0.0f;
                PixelsToDip(st->target, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &x, &y);

                if (y <= RULER_H) {
                    st->dragMarker = MarkerAt(hwnd, st, x, y);
                    if (st->dragMarker) {
                        // One undo step for the whole drag, not one per pixel.
                        PushUndo(st);
                        SetCapture(hwnd);
                    }
                    return 0;
                }

                CaretFromPoint(hwnd, st, lParam, GetKeyState(VK_SHIFT) < 0, FALSE);
                SetCapture(hwnd);
            }
            return 0;

        case WM_LBUTTONDBLCLK:
            if (st) CaretFromPoint(hwnd, st, lParam, FALSE, TRUE);
            return 0;

        case WM_MOUSEMOVE:
            if (st && (wParam & MK_LBUTTON) && GetCapture() == hwnd) {
                if (st->dragMarker) {
                    float x = 0.0f, y = 0.0f;
                    PixelsToDip(st->target, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &x, &y);
                    SetIndentFromRuler(hwnd, st, st->dragMarker, x);
                } else {
                    CaretFromPoint(hwnd, st, lParam, TRUE, FALSE);
                }
            }
            return 0;

        case WM_LBUTTONUP:
            if (st) st->dragMarker = 0;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;

        case WM_SETCURSOR:
            if (st && LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);

                float x = 0.0f, y = 0.0f;
                PixelsToDip(st->target, pt.x, pt.y, &x, &y);

                SetCursor(LoadCursorW(NULL, y <= RULER_H ? IDC_ARROW : IDC_IBEAM));
                return TRUE;
            }
            break;

        case WM_CHAR:
            if (st) {
                WCHAR ch = (WCHAR)wParam;

                if (ch == L'\r' || ch == L'\n') {
                    SplitAtCaret(hwnd, st);
                } else if (ch == L'\b') {
                    DeleteCharacter(hwnd, st, FALSE);
                } else if (ch == L'\t') {
                    InsertText(hwnd, st, L"\t", 1);
                } else if (ch >= 32) {
                    WCHAR text[2] = { ch, 0 };
                    InsertText(hwnd, st, text, 1);
                }
                // Anything else is a control key, handled in WM_KEYDOWN.
            }
            return 0;

        case WM_KEYDOWN:
            if (st) {
                BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
                BOOL shift = GetKeyState(VK_SHIFT) < 0;

                switch (wParam) {
                    case VK_ESCAPE:
                        SendMessageW(hwnd, WM_CLOSE, 0, 0);
                        return 0;

                    case VK_DELETE:
                        DeleteCharacter(hwnd, st, TRUE);
                        return 0;

                    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
                        MoveCaret(hwnd, st, wParam, shift);
                        return 0;

                    case 'A':
                        if (ctrl) {
                            int last = Doc_CountParas(st->doc) - 1;
                            st->anchorPara = 0;
                            st->anchorOff = 0;
                            st->caretPara = last < 0 ? 0 : last;
                            st->caretOff = Doc_ParaLength(Doc_ParaAt(st->doc, st->caretPara));
                            InvalidateRect(hwnd, NULL, FALSE);
                            return 0;
                        }
                        break;

                    case 'C': if (ctrl) { CopySelection(hwnd, st, FALSE); return 0; } break;
                    case 'X': if (ctrl) { CopySelection(hwnd, st, TRUE);  return 0; } break;
                    case 'V': if (ctrl) { PasteText(hwnd, st);            return 0; } break;

                    case 'Z':
                        if (ctrl) {
                            SwapModel(hwnd, st, st->undo, &st->undoCount,
                                      st->redo, &st->redoCount);
                            return 0;
                        }
                        break;

                    case 'Y':
                        if (ctrl) {
                            SwapModel(hwnd, st, st->redo, &st->redoCount,
                                      st->undo, &st->undoCount);
                            return 0;
                        }
                        break;

                    case 'S':
                        if (ctrl) {
                            // Saving means saving the document, so the edits go
                            // back to the view first and the main window does
                            // the rest.
                            ApplyToDocument(hwnd, st);
                            SetTitle(hwnd, st, NULL);
                            PostMessageW(GetWindow(hwnd, GW_OWNER), WM_COMMAND,
                                         MAKEWPARAM(IDM_FILE_SAVE, 0), 0);
                            return 0;
                        }
                        break;

                    case 'P':
                        if (ctrl) {
                            // Printing from a page view is what it is for. The
                            // owner already knows how, and prints the same
                            // model through the same engine.
                            ApplyToDocument(hwnd, st);
                            SetTitle(hwnd, st, NULL);
                            PostMessageW(GetWindow(hwnd, GW_OWNER), WM_COMMAND,
                                         MAKEWPARAM(IDM_FILE_PRINT, 0), 0);
                            return 0;
                        }
                        break;

                    case '0':
                        if (ctrl) {
                            // Back to fitting the window, for when a zoom has
                            // gone somewhere unhelpful.
                            st->userZoomed = FALSE;
                            UpdateScrollRange(hwnd, st);
                            InvalidateRect(hwnd, NULL, FALSE);
                            return 0;
                        }
                        break;
                }
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (st) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);

                if (GetKeyState(VK_CONTROL) < 0) {
                    float old = st->zoom;
                    st->userZoomed = TRUE;
                    st->zoom *= (delta > 0) ? 1.1f : (1.0f / 1.1f);
                    if (st->zoom < 0.2f) st->zoom = 0.2f;
                    if (st->zoom > 4.0f) st->zoom = 4.0f;
                    // Keep roughly the same place in the document in view.
                    st->scrollY = (int)(st->scrollY * (st->zoom / old));
                } else {
                    st->scrollY -= delta;
                }

                UpdateScrollRange(hwnd, st);
                SetTitle(hwnd, st, NULL);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_VSCROLL:
            if (st) {
                SCROLLINFO si = {};
                si.cbSize = sizeof(si);
                si.fMask = SIF_ALL;
                GetScrollInfo(hwnd, SB_VERT, &si);

                int pos = si.nPos;
                switch (LOWORD(wParam)) {
                    case SB_LINEUP:   pos -= 40; break;
                    case SB_LINEDOWN: pos += 40; break;
                    case SB_PAGEUP:   pos -= si.nPage; break;
                    case SB_PAGEDOWN: pos += si.nPage; break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                }

                st->scrollY = pos;
                UpdateScrollRange(hwnd, st);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_CLOSE:
            if (st) ApplyToDocument(hwnd, st);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (st) {
                KillTimer(hwnd, CARET_TIMER);
                ApplyToDocument(hwnd, st);

                DiscardTarget(st);
                if (st->rulerFont) st->rulerFont->Release();
                if (st->dwrite) st->dwrite->Release();
                if (st->d2d) st->d2d->Release();
                Layout_Free(st->layout);
                Doc_Free(st->doc);
                ClearStack(st->undo, &st->undoCount);
                ClearStack(st->redo, &st->redoCount);
                free(st->openedWith);
                free(st);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureClass(void) {
    static BOOL registered = FALSE;
    if (registered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = PageViewProc;
    wc.hInstance = g_app->hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    wc.lpszClassName = PAGEVIEW_CLASS;
    wc.hIcon = LoadIconW(g_app->hInstance, MAKEINTRESOURCEW(IDI_SUPERNOTE));

    RegisterClassExW(&wc);
    registered = TRUE;
}

extern "C" BOOL PageView_Show(HWND hOwner, HWND hRichEdit, const WCHAR* docTitle,
                              const DocModel* source) {
    if (!hRichEdit) return FALSE;

    DocModel* doc = DocView_CaptureWith(hRichEdit, source);
    if (!doc) return FALSE;

    PageViewState* st = (PageViewState*)calloc(1, sizeof(PageViewState));
    if (!st) {
        Doc_Free(doc);
        return FALSE;
    }

    st->doc = doc;
    st->hRich = hRichEdit;

    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      (IUnknown**)&st->dwrite))) {
        st->dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     9.0f, L"", &st->rulerFont);
        if (st->rulerFont) {
            st->rulerFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            st->rulerFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    st->openedWith = Rich_GetRtf(hRichEdit);
    st->zoom = 1.0f;
    st->focused = TRUE;
    st->caretOn = TRUE;

    st->layout = Layout_Build(doc, L"Calibri", 11.0f);
    if (!st->layout) {
        free(st->openedWith);
        Doc_Free(doc);
        free(st);
        return FALSE;
    }

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &st->d2d))) {
        Layout_Free(st->layout);
        free(st->openedWith);
        Doc_Free(doc);
        free(st);
        return FALSE;
    }

    EnsureClass();

    HWND hwnd = CreateWindowExW(
        0, PAGEVIEW_CLASS, L"Page Layout",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 820,
        hOwner, NULL, g_app->hInstance, st);

    if (!hwnd) {
        DiscardTarget(st);
        st->d2d->Release();
        Layout_Free(st->layout);
        free(st->openedWith);
        Doc_Free(doc);
        free(st);
        return FALSE;
    }

    SetTitle(hwnd, st, docTitle);
    UpdateScrollRange(hwnd, st);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(hwnd);
    return TRUE;
}
