// The toolbar: documents on the left, formatting on the right.
//
// ponytail: the buttons are custom-drawn rather than loaded from a bitmap
// strip. A toolbar this size needs about twenty glyphs, and drawing a styled
// letter, a floppy disk or three lines in a rectangle is less code than
// authoring, shipping and DPI-scaling an icon resource -- and it follows the
// system text colour into dark mode for free, which a bitmap does not.
//
// The same reasoning as `tools/make_icon.py`: the shapes are geometry, so they
// are sharp at any size and carry nothing platform-specific but the drawing
// calls themselves.

#include "supernote.h"
#include "res/resource.h"
#include "ui/toolbar.h"

#define BAR_HEIGHT     30
#define COMBO_FONT_W   170
#define COMBO_SIZE_W   60
#define BTN_SIZE       26

// The combos live in a separator-shaped hole in the middle of the button run.
#define COMBO_GAP_INDEX 6
#define COMBO_GAP_W     (COMBO_FONT_W + 4 + COMBO_SIZE_W + 8)

static HWND g_hBar       = NULL;
static HWND g_hFontCombo = NULL;
static HWND g_hSizeCombo = NULL;
static BOOL g_visible    = FALSE;

// Set while pushing state into the combos, so the resulting CBN_SELCHANGE is
// not mistaken for the user choosing a font.
static BOOL g_syncing = FALSE;

HWND FormatBar_Handle(void) { return g_hBar; }

int FormatBar_Height(void) { return g_visible ? BAR_HEIGHT : 0; }

// ---------------------------------------------------------------------------
// Button table
// ---------------------------------------------------------------------------

typedef enum {
    GLYPH_LETTER,     // a styled character, for B / I / U / S
    GLYPH_ALIGN,      // horizontal rules, ragged on one side
    GLYPH_LIST,       // marker plus rules
    GLYPH_INDENT,     // arrow plus rules
    GLYPH_COLOR,      // an "A" over a colour swatch
    GLYPH_NEW,        // a blank page
    GLYPH_OPEN,       // a folder
    GLYPH_SAVE,       // a floppy disk, still what "save" looks like
    GLYPH_PRINT,      // a printer
    GLYPH_PAGE        // a page with lines: the laid-out view
} GlyphKind;

typedef struct {
    int       id;
    GlyphKind kind;
    WCHAR     letter;   // GLYPH_LETTER
    int       param;    // alignment/list/indent variant
    const WCHAR* tip;
} BarButton;

static const BarButton BUTTONS[] = {
    { IDM_FILE_NEW,             GLYPH_NEW,    0, 0, L"New (Ctrl+N)" },
    { IDM_FILE_OPEN,            GLYPH_OPEN,   0, 0, L"Open (Ctrl+O)" },
    { IDM_FILE_SAVE,            GLYPH_SAVE,   0, 0, L"Save (Ctrl+S)" },
    { IDM_FILE_PRINT,           GLYPH_PRINT,  0, 0, L"Print (Ctrl+P)" },
    { 0, 0, 0, 0, NULL },
    { IDM_VIEW_PAGE_LAYOUT,     GLYPH_PAGE,   0, 0, L"Page layout (Ctrl+Shift+L)" },
    { 0, 0, 0, 0, NULL },       // the wide one: the combos sit in this gap
    { IDM_FORMAT_BOLD,          GLYPH_LETTER, L'B', 0, L"Bold (Ctrl+B)" },
    { IDM_FORMAT_ITALIC,        GLYPH_LETTER, L'I', 0, L"Italic (Ctrl+I)" },
    { IDM_FORMAT_UNDERLINE,     GLYPH_LETTER, L'U', 0, L"Underline (Ctrl+U)" },
    { IDM_FORMAT_STRIKE,        GLYPH_LETTER, L'S', 0, L"Strikethrough" },
    { 0, 0, 0, 0, NULL },  // separator
    { IDM_FORMAT_TEXTCOLOR,     GLYPH_COLOR,  L'A', 0, L"Text colour" },
    { 0, 0, 0, 0, NULL },
    { IDM_FORMAT_ALIGN_LEFT,    GLYPH_ALIGN,  0, PFA_LEFT,    L"Align left" },
    { IDM_FORMAT_ALIGN_CENTER,  GLYPH_ALIGN,  0, PFA_CENTER,  L"Centre" },
    { IDM_FORMAT_ALIGN_RIGHT,   GLYPH_ALIGN,  0, PFA_RIGHT,   L"Align right" },
    { IDM_FORMAT_ALIGN_JUSTIFY, GLYPH_ALIGN,  0, PFA_JUSTIFY, L"Justify" },
    { 0, 0, 0, 0, NULL },
    { IDM_FORMAT_BULLETS,       GLYPH_LIST,   0, 0, L"Bullet list" },
    { IDM_FORMAT_NUMBERING,     GLYPH_LIST,   0, 1, L"Numbered list" },
    { IDM_FORMAT_INDENT_LESS,   GLYPH_INDENT, 0, 0, L"Decrease indent" },
    { IDM_FORMAT_INDENT_MORE,   GLYPH_INDENT, 0, 1, L"Increase indent" },
};

#define BUTTON_COUNT (int)(sizeof(BUTTONS) / sizeof(BUTTONS[0]))

static const BarButton* FindButton(int id) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (BUTTONS[i].id == id) return &BUTTONS[i];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Font list
// ---------------------------------------------------------------------------

static int CALLBACK AddFontProc(const LOGFONTW* lf, const TEXTMETRICW* tm,
                                DWORD type, LPARAM param) {
    (void)tm; (void)type;
    HWND combo = (HWND)param;

    // Skip the vertical-writing aliases Windows lists with a leading '@'.
    if (lf->lfFaceName[0] == L'@') return 1;

    if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                     (LPARAM)lf->lfFaceName) == CB_ERR) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)lf->lfFaceName);
    }
    return 1;
}

static void PopulateFonts(HWND combo) {
    HDC hdc = GetDC(NULL);
    LOGFONTW lf = { .lfCharSet = DEFAULT_CHARSET };
    EnumFontFamiliesExW(hdc, &lf, AddFontProc, (LPARAM)combo, 0);
    ReleaseDC(NULL, hdc);
}

static const int SIZES[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

HWND FormatBar_Create(HWND hParent) {
    g_hBar = CreateWindowExW(
        0, TOOLBARCLASSNAMEW, NULL,
        WS_CHILD | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NODIVIDER | CCS_NORESIZE,
        0, 0, 100, BAR_HEIGHT,
        hParent, (HMENU)(INT_PTR)IDC_TOOLBAR, g_app->hInstance, NULL);
    if (!g_hBar) return NULL;

    SendMessageW(g_hBar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(g_hBar, TB_SETBITMAPSIZE, 0, MAKELONG(0, 0));
    SendMessageW(g_hBar, TB_SETBUTTONSIZE, 0, MAKELONG(BTN_SIZE, BTN_SIZE));

    TBBUTTON tbb[BUTTON_COUNT];
    memset(tbb, 0, sizeof(tbb));

    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (BUTTONS[i].id == 0) {
            tbb[i].fsStyle = BTNS_SEP;
            // The second separator is the space the font and size combos are
            // placed over: a separator is the only toolbar item whose width
            // can simply be stated.
            tbb[i].iBitmap = (i == COMBO_GAP_INDEX) ? COMBO_GAP_W : 6;
        } else {
            tbb[i].idCommand = BUTTONS[i].id;
            tbb[i].fsState = TBSTATE_ENABLED;

            // Only the buttons that show a state latch. Everything else --
            // saving, printing, indenting, opening a view -- is an action and
            // must come back up.
            BOOL latches = BUTTONS[i].kind == GLYPH_LETTER ||
                           BUTTONS[i].kind == GLYPH_ALIGN ||
                           BUTTONS[i].kind == GLYPH_LIST ||
                           BUTTONS[i].kind == GLYPH_PAGE;
            tbb[i].fsStyle = latches ? BTNS_CHECK : BTNS_BUTTON;
            tbb[i].iBitmap = I_IMAGENONE;
        }
    }

    SendMessageW(g_hBar, TB_ADDBUTTONS, BUTTON_COUNT, (LPARAM)tbb);

    // The combos sit to the left of the buttons, so the buttons are pushed
    // right by an equivalent run of separators sized to cover them.
    g_hFontCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT,
        0, 0, COMBO_FONT_W, 300, g_hBar, (HMENU)(INT_PTR)IDC_FONT_COMBO,
        g_app->hInstance, NULL);

    g_hSizeCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
        0, 0, COMBO_SIZE_W, 300, g_hBar, (HMENU)(INT_PTR)IDC_SIZE_COMBO,
        g_app->hInstance, NULL);

    HFONT hUi = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(g_hFontCombo, WM_SETFONT, (WPARAM)hUi, TRUE);
    SendMessageW(g_hSizeCombo, WM_SETFONT, (WPARAM)hUi, TRUE);

    PopulateFonts(g_hFontCombo);

    for (int i = 0; i < (int)(sizeof(SIZES) / sizeof(SIZES[0])); i++) {
        WCHAR buf[8];
        swprintf_s(buf, 8, L"%d", SIZES[i]);
        SendMessageW(g_hSizeCombo, CB_ADDSTRING, 0, (LPARAM)buf);
    }

    return g_hBar;
}

// ---------------------------------------------------------------------------
// Layout and visibility
// ---------------------------------------------------------------------------

// Lays out the bar's children. The bar's own position belongs to the window
// that owns it -- setting it here as well moved it to y=0 and then back on
// every resize, which left the custom-drawn buttons half painted.
void FormatBar_Layout(int width) {
    if (!g_hBar) return;
    (void)width;

    int y = (BAR_HEIGHT - 22) / 2;

    // Put the combos wherever the gap ended up, rather than assuming: the
    // buttons before it are a different width on a different DPI.
    RECT gap = {0};
    SendMessageW(g_hBar, TB_GETITEMRECT, COMBO_GAP_INDEX, (LPARAM)&gap);

    int x = gap.left + 4;
    SetWindowPos(g_hFontCombo, NULL, x, y, COMBO_FONT_W, 300, SWP_NOZORDER);
    SetWindowPos(g_hSizeCombo, NULL, x + COMBO_FONT_W + 4, y, COMBO_SIZE_W, 300, SWP_NOZORDER);

    // Custom-drawn buttons do not repaint themselves after the indent changes.
    InvalidateRect(g_hBar, NULL, TRUE);
}

void FormatBar_UpdateVisibility(HWND hEditor, BOOL pageLayout) {
    if (!g_hBar) return;

    // The bar is the user's choice now, not the document's: New, Open, Save
    // and Print mean the same thing whatever is being edited. What the
    // document decides is which buttons are usable -- a plain text file has no
    // bold and no pages, so those go grey rather than disappearing and moving
    // everything else along the bar.
    BOOL want = g_app->showFormatBar;
    if (want != g_visible) {
        g_visible = want;
        ShowWindow(g_hBar, want ? SW_SHOW : SW_HIDE);
    }

    BOOL rich = Editor_IsRich(hEditor);

    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (BUTTONS[i].id == 0) continue;

        BOOL usable;
        switch (BUTTONS[i].kind) {
            case GLYPH_NEW:
            case GLYPH_OPEN:
            case GLYPH_SAVE:
            case GLYPH_PRINT:
                usable = TRUE;              // these mean the same in any view
                break;
            case GLYPH_PAGE:
                usable = rich;              // ...and this is how you get back
                break;
            default:
                usable = rich && !pageLayout;
                break;
        }

        SendMessageW(g_hBar, TB_ENABLEBUTTON, BUTTONS[i].id, MAKELONG(usable, 0));
    }

    // The page button shows which view the tab is in.
    SendMessageW(g_hBar, TB_CHECKBUTTON, IDM_VIEW_PAGE_LAYOUT, MAKELONG(pageLayout, 0));

    EnableWindow(g_hFontCombo, rich && !pageLayout);
    EnableWindow(g_hSizeCombo, rich && !pageLayout);
}

// ---------------------------------------------------------------------------
// Reflecting what is under the caret
// ---------------------------------------------------------------------------

// SetWindowTextW on a combo's edit field leaves the whole value selected, so
// the bar ends up looking like two highlighted fields the user is editing.
static void SetComboText(HWND combo, const WCHAR* text) {
    // Write to the combo's edit child rather than the combo. A combo's own
    // WM_SETTEXT handler selects the whole value afterwards.
    //
    // ponytail: this does not fully win. The two combos still show their value
    // highlighted until they are first clicked, even though the selection is
    // cleared here and via CB_SETEDITSEL. Cosmetic only -- the values are
    // correct and editing works. The likely real fix is parenting the combos to
    // the main window instead of to the toolbar, which also simplifies command
    // routing; not worth the z-order churn until the bar is revisited.
    HWND edit = FindWindowExW(combo, NULL, L"Edit", NULL);
    if (edit) {
        SetWindowTextW(edit, text);
        SendMessageW(edit, EM_SETSEL, 0, 0);
    } else {
        SetWindowTextW(combo, text);
    }
}

void FormatBar_SyncFromEditor(HWND hEditor) {
    if (!g_hBar || !g_visible || !Editor_IsRich(hEditor)) return;

    g_syncing = TRUE;

    struct { int id; DWORD effect; } effects[] = {
        { IDM_FORMAT_BOLD,      CFE_BOLD },
        { IDM_FORMAT_ITALIC,    CFE_ITALIC },
        { IDM_FORMAT_UNDERLINE, CFE_UNDERLINE },
        { IDM_FORMAT_STRIKE,    CFE_STRIKEOUT },
    };
    for (int i = 0; i < 4; i++) {
        SendMessageW(g_hBar, TB_CHECKBUTTON, effects[i].id,
                     MAKELONG(Rich_HasEffect(hEditor, effects[i].effect), 0));
    }

    WORD align = Rich_GetAlignment(hEditor);
    SendMessageW(g_hBar, TB_CHECKBUTTON, IDM_FORMAT_ALIGN_LEFT,    MAKELONG(align == PFA_LEFT, 0));
    SendMessageW(g_hBar, TB_CHECKBUTTON, IDM_FORMAT_ALIGN_CENTER,  MAKELONG(align == PFA_CENTER, 0));
    SendMessageW(g_hBar, TB_CHECKBUTTON, IDM_FORMAT_ALIGN_RIGHT,   MAKELONG(align == PFA_RIGHT, 0));
    SendMessageW(g_hBar, TB_CHECKBUTTON, IDM_FORMAT_ALIGN_JUSTIFY, MAKELONG(align == PFA_JUSTIFY, 0));

    WORD list = Rich_GetListStyle(hEditor);
    SendMessageW(g_hBar, TB_CHECKBUTTON, IDM_FORMAT_BULLETS,
                 MAKELONG(list == PFN_BULLET, 0));
    SendMessageW(g_hBar, TB_CHECKBUTTON, IDM_FORMAT_NUMBERING,
                 MAKELONG(list != 0 && list != PFN_BULLET, 0));

    WCHAR face[LF_FACESIZE];
    if (Rich_GetFontName(hEditor, face, LF_FACESIZE)) {
        WCHAR current[LF_FACESIZE];
        GetWindowTextW(g_hFontCombo, current, LF_FACESIZE);
        if (wcscmp(current, face) != 0) SetComboText(g_hFontCombo, face);
    }

    int pt = Rich_GetFontSize(hEditor);
    if (pt > 0) {
        WCHAR buf[8], current[8];
        swprintf_s(buf, 8, L"%d", pt);
        GetWindowTextW(g_hSizeCombo, current, 8);
        if (wcscmp(current, buf) != 0) SetComboText(g_hSizeCombo, buf);
    }

    g_syncing = FALSE;
}

// ---------------------------------------------------------------------------
// Commands from the bar's own controls
// ---------------------------------------------------------------------------

BOOL FormatBar_OnCommand(HWND hEditor, int id, int notifyCode, HWND hCtl) {
    if (!Editor_IsRich(hEditor)) return FALSE;

    if (id == IDC_FONT_COMBO || id == IDC_SIZE_COMBO) {
        // CBN_SELENDOK covers picking from the list; CBN_KILLFOCUS covers a
        // value typed straight into the edit field.
        if (g_syncing) return TRUE;
        if (notifyCode != CBN_SELENDOK && notifyCode != CBN_KILLFOCUS) return TRUE;

        WCHAR text[LF_FACESIZE] = {0};

        if (notifyCode == CBN_SELENDOK) {
            int sel = (int)SendMessageW(hCtl, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) return TRUE;
            SendMessageW(hCtl, CB_GETLBTEXT, sel, (LPARAM)text);
        } else {
            GetWindowTextW(hCtl, text, LF_FACESIZE);
        }
        if (!text[0]) return TRUE;

        if (id == IDC_FONT_COMBO) {
            Rich_SetFontName(hEditor, text);
        } else {
            int pt = _wtoi(text);
            // A typo of 0 or 900 would otherwise be applied literally.
            if (pt >= 1 && pt <= 999) Rich_SetFontSize(hEditor, pt);
        }
        SetFocus(hEditor);
        return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Custom draw
// ---------------------------------------------------------------------------

static void DrawRules(HDC hdc, const RECT* rc, COLORREF color, int alignment) {
    HBRUSH hb = CreateSolidBrush(color);

    int w = rc->right - rc->left;
    int lines = 4;
    int lineH = 2;
    int gap = 3;
    int totalH = lines * lineH + (lines - 1) * (gap - lineH);
    int y = rc->top + ((rc->bottom - rc->top) - totalH) / 2;

    for (int i = 0; i < lines; i++) {
        // Every other line is short, which is what makes the ragged edge read
        // as left/right/centre rather than as four identical bars.
        int lw = (i % 2 == 1 && alignment != PFA_JUSTIFY) ? (w * 6 / 10) : w;
        int x = rc->left;

        if (alignment == PFA_RIGHT)       x = rc->right - lw;
        else if (alignment == PFA_CENTER) x = rc->left + (w - lw) / 2;

        RECT line = { x, y, x + lw, y + lineH };
        FillRect(hdc, &line, hb);
        y += gap;
    }

    DeleteObject(hb);
}

static void DrawListGlyph(HDC hdc, const RECT* rc, COLORREF color, BOOL numbered) {
    HBRUSH hb = CreateSolidBrush(color);

    int markerW = 5;
    int y = rc->top + 3;

    for (int i = 0; i < 3; i++) {
        if (numbered) {
            // A tick of increasing length stands in for 1. 2. 3. at this size,
            // where an actual digit would be a smudge.
            RECT tick = { rc->left, y, rc->left + 2 + i, y + 2 };
            FillRect(hdc, &tick, hb);
        } else {
            RECT dot = { rc->left, y, rc->left + 3, y + 3 };
            FillRect(hdc, &dot, hb);
        }
        RECT line = { rc->left + markerW + 2, y, rc->right, y + 2 };
        FillRect(hdc, &line, hb);
        y += 5;
    }

    DeleteObject(hb);
}

static void DrawIndentGlyph(HDC hdc, const RECT* rc, COLORREF color, BOOL increase) {
    HBRUSH hb = CreateSolidBrush(color);

    int y = rc->top + 3;
    for (int i = 0; i < 4; i++) {
        int left = (i == 1 || i == 2) ? rc->left + 6 : rc->left;
        RECT line = { left, y, rc->right, y + 2 };
        FillRect(hdc, &line, hb);
        y += 5;
    }

    // A triangle pointing the way the indent moves.
    int cy = (rc->top + rc->bottom) / 2;
    for (int i = 0; i < 4; i++) {
        int x = increase ? rc->left + i : rc->left + 4 - i;
        RECT t = { x, cy - (4 - i), x + 1, cy + (4 - i) };
        FillRect(hdc, &t, hb);
    }

    DeleteObject(hb);
}

// A document: a page with its top right corner turned down, the same mark the
// application icon uses, so the button and the icon are recognisably the same
// program.
static void DrawPageGlyph(HDC hdc, const RECT* rc, COLORREF fg, BOOL withText) {
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;
    int fold = w / 3;

    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    POINT page[5] = {
        { rc->left,             rc->top },
        { rc->right - fold,     rc->top },
        { rc->right,            rc->top + fold },
        { rc->right,            rc->bottom },
        { rc->left,             rc->bottom },
    };
    Polygon(hdc, page, 5);

    // The fold itself.
    MoveToEx(hdc, rc->right - fold, rc->top, NULL);
    LineTo(hdc, rc->right - fold, rc->top + fold);
    LineTo(hdc, rc->right + 1, rc->top + fold);

    if (withText) {
        for (int i = 0; i < 3; i++) {
            int y = rc->top + fold + 3 + i * ((h - fold - 4) / 3);
            if (y >= rc->bottom - 1) break;
            MoveToEx(hdc, rc->left + 2, y, NULL);
            LineTo(hdc, rc->right - (i == 2 ? 5 : 2), y);
        }
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// A folder, drawn as a body with a tab along the top left.
static void DrawFolderGlyph(HDC hdc, const RECT* rc, COLORREF fg) {
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;

    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    POINT folder[6] = {
        { rc->left,          rc->bottom },
        { rc->left,          rc->top + h / 4 },
        { rc->left + w / 2,  rc->top + h / 4 },
        { rc->left + w / 2 + 2, rc->top + h / 4 - 3 },
        { rc->right,         rc->top + h / 4 - 3 },
        { rc->right,         rc->bottom },
    };
    Polygon(hdc, folder, 6);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// A floppy disk. Nobody has seen one for twenty years and everybody still
// reads it as "save", which is the only thing a toolbar glyph has to do.
static void DrawSaveGlyph(HDC hdc, const RECT* rc, COLORREF fg) {
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;

    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    // The body, with the corner clipped the way a disk's is.
    POINT body[5] = {
        { rc->left,            rc->top },
        { rc->right - 3,       rc->top },
        { rc->right,           rc->top + 3 },
        { rc->right,           rc->bottom },
        { rc->left,            rc->bottom },
    };
    Polygon(hdc, body, 5);

    // The shutter at the top, and the label at the bottom.
    RECT shutter = { rc->left + w / 4, rc->top, rc->right - w / 4, rc->top + h / 3 };
    Rectangle(hdc, shutter.left, shutter.top, shutter.right, shutter.bottom);

    RECT label = { rc->left + 3, rc->bottom - h / 3, rc->right - 3, rc->bottom };
    Rectangle(hdc, label.left, label.top, label.right, label.bottom);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// A printer: paper going in at the top, the body, paper coming out.
static void DrawPrintGlyph(HDC hdc, const RECT* rc, COLORREF fg) {
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;

    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Rectangle(hdc, rc->left + w / 6, rc->top, rc->right - w / 6, rc->top + h / 3);
    Rectangle(hdc, rc->left, rc->top + h / 3, rc->right, rc->bottom - h / 4);
    Rectangle(hdc, rc->left + w / 6, rc->bottom - h / 3, rc->right - w / 6, rc->bottom);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

LRESULT FormatBar_OnCustomDraw(LPNMTBCUSTOMDRAW nm) {
    if (nm->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (nm->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

    const BarButton* btn = FindButton((int)nm->nmcd.dwItemSpec);
    if (!btn) return CDRF_DODEFAULT;

    HDC hdc = nm->nmcd.hdc;
    RECT rc = nm->nmcd.rc;

    // Ask the toolbar whether the button is usable rather than reading
    // CDIS_DISABLED: a flat toolbar does not set that flag, because it expects
    // to grey the button's bitmap itself -- and these buttons have no bitmap.
    BOOL disabled = !SendMessageW(nm->nmcd.hdr.hwndFrom, TB_ISBUTTONENABLED,
                                  (WPARAM)btn->id, 0);

    BOOL checked = !disabled && (nm->nmcd.uItemState & CDIS_CHECKED) != 0;
    BOOL hot     = !disabled && (nm->nmcd.uItemState & CDIS_HOT) != 0;

    // Background first, since the default would paint over the glyph.
    if (checked || hot) {
        COLORREF fill = GetSysColor(checked ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
        if (checked) {
            // Lighten the highlight so a latched button does not read as a
            // solid block with an invisible glyph on it.
            fill = RGB((GetRValue(fill) + 510) / 3,
                       (GetGValue(fill) + 510) / 3,
                       (GetBValue(fill) + 510) / 3);
        }
        HBRUSH hb = CreateSolidBrush(fill);
        FillRect(hdc, &rc, hb);
        DeleteObject(hb);

        HBRUSH border = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
        FrameRect(hdc, &rc, border);
        DeleteObject(border);
    }

    // A button that cannot be pressed has to look like it: the formatting half
    // of the bar is disabled on a plain text document.
    COLORREF fg = GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);

    // Inset so glyphs do not touch the button border.
    RECT inner = rc;
    InflateRect(&inner, -6, -6);

    switch (btn->kind) {
        case GLYPH_LETTER: {
            LOGFONTW lf = {0};
            lf.lfHeight = -(rc.bottom - rc.top) + 10;
            lf.lfWeight = (btn->letter == L'B') ? FW_BOLD : FW_NORMAL;
            lf.lfItalic = (btn->letter == L'I');
            lf.lfUnderline = (btn->letter == L'U');
            lf.lfStrikeOut = (btn->letter == L'S');
            lf.lfCharSet = DEFAULT_CHARSET;
            wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Times New Roman");

            HFONT hf = CreateFontIndirectW(&lf);
            HFONT old = (HFONT)SelectObject(hdc, hf);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, fg);

            WCHAR s[2] = { btn->letter, 0 };
            DrawTextW(hdc, s, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, old);
            DeleteObject(hf);
            break;
        }

        case GLYPH_COLOR: {
            RECT letter = rc;
            letter.bottom -= 6;

            LOGFONTW lf = {0};
            lf.lfHeight = -(rc.bottom - rc.top) + 12;
            lf.lfWeight = FW_BOLD;
            lf.lfCharSet = DEFAULT_CHARSET;
            wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Times New Roman");

            HFONT hf = CreateFontIndirectW(&lf);
            HFONT old = (HFONT)SelectObject(hdc, hf);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, fg);
            DrawTextW(hdc, L"A", 1, &letter, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old);
            DeleteObject(hf);

            // The swatch shows the colour the button would apply.
            RECT swatch = { rc.left + 5, rc.bottom - 8, rc.right - 5, rc.bottom - 4 };
            HBRUSH hb = CreateSolidBrush(disabled ? fg : RGB(200, 30, 30));
            FillRect(hdc, &swatch, hb);
            DeleteObject(hb);
            break;
        }

        case GLYPH_ALIGN:
            DrawRules(hdc, &inner, fg, btn->param);
            break;

        case GLYPH_LIST:
            DrawListGlyph(hdc, &inner, fg, btn->param == 1);
            break;

        case GLYPH_INDENT:
            DrawIndentGlyph(hdc, &inner, fg, btn->param == 1);
            break;

        case GLYPH_NEW:
            DrawPageGlyph(hdc, &inner, fg, FALSE);
            break;

        case GLYPH_PAGE:
            DrawPageGlyph(hdc, &inner, fg, TRUE);
            break;

        case GLYPH_OPEN:
            DrawFolderGlyph(hdc, &inner, fg);
            break;

        case GLYPH_SAVE:
            DrawSaveGlyph(hdc, &inner, fg);
            break;

        case GLYPH_PRINT:
            DrawPrintGlyph(hdc, &inner, fg);
            break;
    }

    return CDRF_SKIPDEFAULT;
}

// Tooltips. The toolbar owns a tooltip control because of TBSTYLE_TOOLTIPS;
// it asks the parent for the text, and the parent forwards to here.
void FormatBar_OnGetTooltip(NMTTDISPINFOW* info) {
    if (!info) return;

    const BarButton* btn = FindButton((int)info->hdr.idFrom);
    if (!btn || !btn->tip) return;

    info->lpszText = (LPWSTR)btn->tip;
    info->hinst = NULL;
}
