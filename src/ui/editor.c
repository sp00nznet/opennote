#include "supernote.h"
#include "res/resource.h"
#include <Scintilla.h>
#include <SciLexer.h>
#include <Lexilla.h>
#include <spellcheck.h>
#include "ui/editor_rich.h"

// ---------------------------------------------------------------------------
// Which view is behind this HWND
//
// Two document views now exist: the Scintilla one in this file and the
// RichEdit one in editor_rich.c. Everything outside keeps calling Editor_*
// with an HWND, and the functions below dispatch. The kind is stamped on the
// window at creation rather than derived from its class name, so answering is
// a property lookup rather than a string compare on every call.
// ---------------------------------------------------------------------------

#define EDITOR_KIND_PROP L"OpenNoteEditorKind"

static void Editor_SetKind(HWND hEditor, EditorKind kind) {
    // +1 so the stored value is never NULL, which GetPropW cannot distinguish
    // from "no such property".
    SetPropW(hEditor, EDITOR_KIND_PROP, (HANDLE)(INT_PTR)(kind + 1));
}

EditorKind Editor_GetKind(HWND hEditor) {
    if (!hEditor) return EDITOR_KIND_PLAIN;
    INT_PTR v = (INT_PTR)GetPropW(hEditor, EDITOR_KIND_PROP);
    return v ? (EditorKind)(v - 1) : EDITOR_KIND_PLAIN;
}

BOOL Editor_IsRich(HWND hEditor) {
    return Editor_GetKind(hEditor) == EDITOR_KIND_RICH;
}

// Define GUIDs for spell checker (Windows 8+)
// {7AB36653-1796-484B-BDFA-E74F1DB7C1DC}
static const GUID CLSID_SpellCheckerFactoryLocal =
    { 0x7AB36653, 0x1796, 0x484B, { 0xBD, 0xFA, 0xE7, 0x4F, 0x1D, 0xB7, 0xC1, 0xDC } };

// {8E018A9D-2415-4677-BF08-794EA61F94BB}
static const GUID IID_ISpellCheckerFactoryLocal =
    { 0x8E018A9D, 0x2415, 0x4677, { 0xBF, 0x08, 0x79, 0x4E, 0xA6, 0x1F, 0x94, 0xBB } };

// Indicator numbers
#define INDICATOR_LINK      8
#define INDICATOR_SPELL     9

// Spell checker instance (Windows 8+ ISpellChecker)
static ISpellChecker* g_spellChecker = NULL;
static BOOL g_spellCheckInit = FALSE;

// Scintilla direct function for better performance
static SciFnDirect g_sciFunc = NULL;
static sptr_t g_sciPtr = 0;

// Helper macro for sending Scintilla messages
#define SciCall(hwnd, msg, wp, lp) SendMessage(hwnd, msg, (WPARAM)(wp), (LPARAM)(lp))

// Theme structure
typedef struct {
    const WCHAR* name;
    COLORREF background;
    COLORREF foreground;
    COLORREF comment;
    COLORREF string;
    COLORREF number;
    COLORREF keyword;
    COLORREF keyword2;
    COLORREF operatorColor;
    COLORREF identifier;
    COLORREF preprocessor;
    COLORREF caret;
    COLORREF selectionBg;
    COLORREF lineNumber;
    COLORREF lineNumBg;
} EditorTheme;

// Available themes
static const EditorTheme g_themes[] = {
    // Dark (VS Code style)
    {
        L"Dark",
        0x1E1E1E,  // background
        0xD4D4D4,  // foreground
        0x6A9955,  // comment (green)
        0xCE9178,  // string (orange)
        0xB5CEA8,  // number (light green)
        0x569CD6,  // keyword (blue)
        0xC586C0,  // keyword2 (purple)
        0xD4D4D4,  // operator
        0x9CDCFE,  // identifier (light blue)
        0xC586C0,  // preprocessor (purple)
        0xFFFFFF,  // caret (white)
        0x264F78,  // selection bg
        0x858585,  // line number
        0x1E1E1E   // line number bg
    },
    // Light
    {
        L"Light",
        0xFFFFFF,  // background (white)
        0x000000,  // foreground (black)
        0x008000,  // comment (green)
        0xA31515,  // string (dark red)
        0x098658,  // number (teal)
        0x0000FF,  // keyword (blue)
        0xAF00DB,  // keyword2 (purple)
        0x000000,  // operator (black)
        0x001080,  // identifier (dark blue)
        0xAF00DB,  // preprocessor (purple)
        0x000000,  // caret (black)
        0xADD6FF,  // selection bg (light blue)
        0x237893,  // line number
        0xF3F3F3   // line number bg
    },
    // Monokai
    {
        L"Monokai",
        0x272822,  // background
        0xF8F8F2,  // foreground
        0x75715E,  // comment (gray)
        0xE6DB74,  // string (yellow)
        0xAE81FF,  // number (purple)
        0xF92672,  // keyword (pink)
        0x66D9EF,  // keyword2 (cyan)
        0xF8F8F2,  // operator
        0xA6E22E,  // identifier (green)
        0xF92672,  // preprocessor (pink)
        0xF8F8F0,  // caret
        0x49483E,  // selection bg
        0x90908A,  // line number
        0x272822   // line number bg
    },
    // Solarized Dark
    {
        L"Solarized Dark",
        0x002B36,  // background
        0x839496,  // foreground
        0x586E75,  // comment
        0x2AA198,  // string (cyan)
        0xD33682,  // number (magenta)
        0x268BD2,  // keyword (blue)
        0x6C71C4,  // keyword2 (violet)
        0x839496,  // operator
        0xB58900,  // identifier (yellow)
        0xCB4B16,  // preprocessor (orange)
        0x839496,  // caret
        0x073642,  // selection bg
        0x586E75,  // line number
        0x002B36   // line number bg
    },
    // Solarized Light
    {
        L"Solarized Light",
        0xFDF6E3,  // background
        0x657B83,  // foreground
        0x93A1A1,  // comment
        0x2AA198,  // string (cyan)
        0xD33682,  // number (magenta)
        0x268BD2,  // keyword (blue)
        0x6C71C4,  // keyword2 (violet)
        0x657B83,  // operator
        0xB58900,  // identifier (yellow)
        0xCB4B16,  // preprocessor (orange)
        0x657B83,  // caret
        0xEEE8D5,  // selection bg
        0x93A1A1,  // line number
        0xFDF6E3   // line number bg
    },
    // Nord
    {
        L"Nord",
        0x2E3440,  // background
        0xD8DEE9,  // foreground
        0x616E88,  // comment
        0xA3BE8C,  // string (green)
        0xB48EAD,  // number (purple)
        0x81A1C1,  // keyword (blue)
        0x5E81AC,  // keyword2 (darker blue)
        0xD8DEE9,  // operator
        0x88C0D0,  // identifier (cyan)
        0xBF616A,  // preprocessor (red)
        0xD8DEE9,  // caret
        0x434C5E,  // selection bg
        0x4C566A,  // line number
        0x2E3440   // line number bg
    },
    // Dracula
    {
        L"Dracula",
        0x282A36,  // background
        0xF8F8F2,  // foreground
        0x6272A4,  // comment
        0xF1FA8C,  // string (yellow)
        0xBD93F9,  // number (purple)
        0xFF79C6,  // keyword (pink)
        0x8BE9FD,  // keyword2 (cyan)
        0xF8F8F2,  // operator
        0x50FA7B,  // identifier (green)
        0xFFB86C,  // preprocessor (orange)
        0xF8F8F0,  // caret
        0x44475A,  // selection bg
        0x6272A4,  // line number
        0x282A36   // line number bg
    },
    // One Dark
    {
        L"One Dark",
        0x282C34,  // background
        0xABB2BF,  // foreground
        0x5C6370,  // comment
        0x98C379,  // string (green)
        0xD19A66,  // number (orange)
        0xC678DD,  // keyword (purple)
        0x61AFEF,  // keyword2 (blue)
        0xABB2BF,  // operator
        0xE06C75,  // identifier (red)
        0xE5C07B,  // preprocessor (yellow)
        0x528BFF,  // caret (bright blue)
        0x3E4451,  // selection bg
        0x4B5263,  // line number
        0x282C34   // line number bg
    },
    // Gruvbox Dark
    {
        L"Gruvbox Dark",
        0x282828,  // background
        0xEBDBB2,  // foreground
        0x928374,  // comment
        0xB8BB26,  // string (green)
        0xD3869B,  // number (purple)
        0xFB4934,  // keyword (red)
        0xFE8019,  // keyword2 (orange)
        0xEBDBB2,  // operator
        0x83A598,  // identifier (blue)
        0xFABD2F,  // preprocessor (yellow)
        0xEBDBB2,  // caret
        0x504945,  // selection bg
        0x7C6F64,  // line number
        0x282828   // line number bg
    },
    // Gruvbox Light
    {
        L"Gruvbox Light",
        0xFBF1C7,  // background
        0x3C3836,  // foreground
        0x928374,  // comment
        0x79740E,  // string (green)
        0x8F3F71,  // number (purple)
        0x9D0006,  // keyword (red)
        0xAF3A03,  // keyword2 (orange)
        0x3C3836,  // operator
        0x076678,  // identifier (blue)
        0xB57614,  // preprocessor (yellow)
        0x3C3836,  // caret
        0xD5C4A1,  // selection bg
        0x7C6F64,  // line number
        0xFBF1C7   // line number bg
    },
    // GitHub Dark
    {
        L"GitHub Dark",
        0x0D1117,  // background
        0xC9D1D9,  // foreground
        0x8B949E,  // comment
        0xA5D6FF,  // string (light blue)
        0x79C0FF,  // number (blue)
        0xFF7B72,  // keyword (red)
        0xD2A8FF,  // keyword2 (purple)
        0xC9D1D9,  // operator
        0x7EE787,  // identifier (green)
        0xFFA657,  // preprocessor (orange)
        0x58A6FF,  // caret (blue)
        0x388BFD,  // selection bg
        0x6E7681,  // line number
        0x0D1117   // line number bg
    },
    // GitHub Light
    {
        L"GitHub Light",
        0xFFFFFF,  // background
        0x24292F,  // foreground
        0x6E7781,  // comment
        0x0A3069,  // string (dark blue)
        0x0550AE,  // number (blue)
        0xCF222E,  // keyword (red)
        0x8250DF,  // keyword2 (purple)
        0x24292F,  // operator
        0x116329,  // identifier (green)
        0x953800,  // preprocessor (orange)
        0x0969DA,  // caret (blue)
        0xDDF4FF,  // selection bg
        0x57606A,  // line number
        0xF6F8FA   // line number bg
    },
    // Catppuccin Mocha
    {
        L"Catppuccin",
        0x1E1E2E,  // background
        0xCDD6F4,  // foreground
        0x6C7086,  // comment
        0xA6E3A1,  // string (green)
        0xFAB387,  // number (peach)
        0xCBA6F7,  // keyword (mauve)
        0x89B4FA,  // keyword2 (blue)
        0xCDD6F4,  // operator
        0xF38BA8,  // identifier (red)
        0xF9E2AF,  // preprocessor (yellow)
        0xF5E0DC,  // caret
        0x45475A,  // selection bg
        0x7F849C,  // line number
        0x1E1E2E   // line number bg
    },
    // Oceanic
    {
        L"Oceanic",
        0x1B2B34,  // background
        0xD8DEE9,  // foreground
        0x65737E,  // comment
        0x99C794,  // string (green)
        0xF99157,  // number (orange)
        0xC594C5,  // keyword (purple)
        0x6699CC,  // keyword2 (blue)
        0xD8DEE9,  // operator
        0x5FB3B3,  // identifier (cyan)
        0xEC5F67,  // preprocessor (red)
        0xD8DEE9,  // caret
        0x4F5B66,  // selection bg
        0x65737E,  // line number
        0x1B2B34   // line number bg
    },
    // Tomorrow Night
    {
        L"Tomorrow Night",
        0x1D1F21,  // background
        0xC5C8C6,  // foreground
        0x969896,  // comment
        0xB5BD68,  // string (green)
        0xDE935F,  // number (orange)
        0xB294BB,  // keyword (purple)
        0x81A2BE,  // keyword2 (blue)
        0xC5C8C6,  // operator
        0xCC6666,  // identifier (red)
        0xF0C674,  // preprocessor (yellow)
        0xC5C8C6,  // caret
        0x373B41,  // selection bg
        0x969896,  // line number
        0x1D1F21   // line number bg
    },
    // Ayu Dark
    {
        L"Ayu Dark",
        0x0A0E14,  // background
        0xB3B1AD,  // foreground
        0x626A73,  // comment
        0xC2D94C,  // string (green)
        0xE6B450,  // number (yellow)
        0xFF8F40,  // keyword (orange)
        0x59C2FF,  // keyword2 (blue)
        0xB3B1AD,  // operator
        0xF07178,  // identifier (red)
        0xFFB454,  // preprocessor (gold)
        0xE6B450,  // caret
        0x273747,  // selection bg
        0x626A73,  // line number
        0x0A0E14   // line number bg
    },
    // Ayu Light
    {
        L"Ayu Light",
        0xFAFAFA,  // background
        0x5C6166,  // foreground
        0xABB0B6,  // comment
        0x86B300,  // string (green)
        0xA37ACC,  // number (purple)
        0xFA8D3E,  // keyword (orange)
        0x399EE6,  // keyword2 (blue)
        0x5C6166,  // operator
        0xF07171,  // identifier (red)
        0xF2AE49,  // preprocessor (gold)
        0xFF9940,  // caret
        0xD1E4F4,  // selection bg
        0xABB0B6,  // line number
        0xFAFAFA   // line number bg
    }
};

#define THEME_COUNT (sizeof(g_themes) / sizeof(g_themes[0]))

// Get current theme
static const EditorTheme* GetCurrentTheme(void) {
    int themeIndex = g_app ? g_app->themeIndex : 0;
    if (themeIndex < 0 || themeIndex >= (int)THEME_COUNT) themeIndex = 0;
    return &g_themes[themeIndex];
}

// Get theme count
int Editor_GetThemeCount(void) {
    return (int)THEME_COUNT;
}

// Get theme name by index
const WCHAR* Editor_GetThemeName(int index) {
    if (index < 0 || index >= (int)THEME_COUNT) return L"Unknown";
    return g_themes[index].name;
}

// Initialize Scintilla DLL/module
static BOOL g_scintillaInit = FALSE;

// Scintilla registration function (from Scintilla.h)
extern int Scintilla_RegisterClasses(void *hInstance);

static BOOL InitScintilla(void) {
    if (g_scintillaInit) return TRUE;

    // Register Scintilla window class (required for static linking)
    if (Scintilla_RegisterClasses(g_app->hInstance) == 0) {
        return FALSE;
    }

    g_scintillaInit = TRUE;
    return TRUE;
}

// Set up common editor styles
static void Editor_SetupStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    // Use UTF-8 encoding
    SciCall(hEditor, SCI_SETCODEPAGE, SC_CP_UTF8, 0);

    // Get font settings from app state
    int fontSize = 12;  // Default
    const char* fontName = "Consolas";
    char fontNameBuf[LF_FACESIZE];

    if (g_app && g_app->editorFont.lfHeight != 0) {
        fontSize = abs(g_app->editorFont.lfHeight);
        if (fontSize < 6 || fontSize > 72) fontSize = 12;
    }
    if (g_app && g_app->editorFont.lfFaceName[0] != L'\0') {
        WideCharToMultiByte(CP_ACP, 0, g_app->editorFont.lfFaceName, -1, fontNameBuf, LF_FACESIZE, NULL, NULL);
        fontName = fontNameBuf;
    }

    // Default style - use user's font settings
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_DEFAULT, theme->foreground);
    SciCall(hEditor, SCI_STYLESETBACK, STYLE_DEFAULT, theme->background);
    SciCall(hEditor, SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, fontSize * 100);
    SciCall(hEditor, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)fontName);

    // Apply default to all styles
    SciCall(hEditor, SCI_STYLECLEARALL, 0, 0);

    // Line numbers
    SciCall(hEditor, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    SciCall(hEditor, SCI_SETMARGINWIDTHN, 0, 50);
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_LINENUMBER, theme->lineNumber);
    SciCall(hEditor, SCI_STYLESETBACK, STYLE_LINENUMBER, theme->lineNumBg);

    // Caret
    SciCall(hEditor, SCI_SETCARETFORE, theme->caret, 0);
    SciCall(hEditor, SCI_SETCARETWIDTH, 2, 0);

    // Selection
    SciCall(hEditor, SCI_SETSELBACK, TRUE, theme->selectionBg);

    // Indentation
    SciCall(hEditor, SCI_SETTABWIDTH, 4, 0);
    SciCall(hEditor, SCI_SETINDENT, 4, 0);
    SciCall(hEditor, SCI_SETUSETABS, FALSE, 0);

    // Enable auto-indent
    SciCall(hEditor, SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH, 0);

    // Enable brace matching
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_BRACELIGHT, 0x00FF00);
    SciCall(hEditor, SCI_STYLESETBACK, STYLE_BRACELIGHT, theme->background);
    SciCall(hEditor, SCI_STYLESETBOLD, STYLE_BRACELIGHT, TRUE);
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_BRACEBAD, 0x0000FF);

    // Scroll width
    SciCall(hEditor, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
    SciCall(hEditor, SCI_SETSCROLLWIDTH, 1, 0);

    // Multi-line undo
    SciCall(hEditor, SCI_SETUNDOCOLLECTION, TRUE, 0);
    SciCall(hEditor, SCI_EMPTYUNDOBUFFER, 0, 0);
}

// Set up C/C++ lexer styles
static void Editor_SetupCppStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    // Get the C++ lexer
    ILexer5* lexer = CreateLexer("cpp");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    // Keywords (C/C++)
    SciCall(hEditor, SCI_SETKEYWORDS, 0, (LPARAM)
        "auto break case char const continue default do double else enum extern float for goto "
        "if inline int long register restrict return short signed sizeof static struct switch "
        "typedef union unsigned void volatile while _Bool _Complex _Imaginary bool class "
        "catch const_cast delete dynamic_cast explicit export false friend mutable namespace "
        "new operator private protected public reinterpret_cast static_cast template this "
        "throw true try typeid typename using virtual wchar_t nullptr constexpr decltype "
        "noexcept override final alignas alignof thread_local static_assert");

    // Types
    SciCall(hEditor, SCI_SETKEYWORDS, 1, (LPARAM)
        "size_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
        "BOOL BYTE WORD DWORD LPSTR LPWSTR LPCSTR LPCWSTR HWND HANDLE HINSTANCE HDC HFONT "
        "WPARAM LPARAM LRESULT RECT POINT SIZE TRUE FALSE NULL");

    // Style colors
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_COMMENT, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_COMMENTLINE, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_COMMENTDOC, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_WORD, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_WORD2, theme->keyword2);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_STRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_CHARACTER, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_PREPROCESSOR, theme->preprocessor);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_OPERATOR, theme->operatorColor);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_IDENTIFIER, theme->foreground);

    // Make keywords bold
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_C_WORD, TRUE);
}

// Set up Python lexer styles
static void Editor_SetupPythonStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("python");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_SETKEYWORDS, 0, (LPARAM)
        "and as assert async await break class continue def del elif else except finally for "
        "from global if import in is lambda None nonlocal not or pass raise return True False "
        "try while with yield");

    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_COMMENTLINE, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_COMMENTBLOCK, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_STRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_CHARACTER, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_TRIPLE, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_TRIPLEDOUBLE, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_WORD, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_WORD2, theme->keyword2);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_P_DECORATOR, theme->preprocessor);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_P_WORD, TRUE);
}

// Set up JavaScript lexer styles
static void Editor_SetupJavaScriptStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("cpp");  // JavaScript uses CPP lexer
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_SETKEYWORDS, 0, (LPARAM)
        "async await break case catch class const continue debugger default delete do else "
        "export extends false finally for from function get if import in instanceof let new "
        "null of return set static super switch this throw true try typeof undefined var "
        "void while with yield");

    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_COMMENT, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_COMMENTLINE, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_WORD, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_STRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_C_STRINGEOL, theme->string);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_C_WORD, TRUE);
}

// Set up HTML lexer styles
static void Editor_SetupHtmlStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("hypertext");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_TAG, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_TAGEND, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_ATTRIBUTE, theme->identifier);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_ATTRIBUTEUNKNOWN, theme->identifier);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_DOUBLESTRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_SINGLESTRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_H_COMMENT, theme->comment);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_H_TAG, TRUE);
}

// Set up JSON lexer styles
static void Editor_SetupJsonStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("json");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_DEFAULT, theme->foreground);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_STRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_STRINGEOL, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_PROPERTYNAME, theme->identifier);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_KEYWORD, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_OPERATOR, theme->operatorColor);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_LINECOMMENT, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_JSON_BLOCKCOMMENT, theme->comment);
}

// Set up SQL lexer styles
static void Editor_SetupSqlStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("sql");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_SETKEYWORDS, 0, (LPARAM)
        "select insert update delete from where and or not null is in like between exists "
        "create drop alter table index view trigger procedure function begin end if else "
        "case when then as join inner outer left right on group by order having distinct "
        "union all limit offset values set primary key foreign references default cascade "
        "constraint unique check autoincrement");

    SciCall(hEditor, SCI_STYLESETFORE, SCE_SQL_COMMENT, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SQL_COMMENTLINE, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SQL_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SQL_WORD, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SQL_STRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SQL_CHARACTER, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SQL_OPERATOR, theme->operatorColor);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_SQL_WORD, TRUE);
}

// Set up Markdown styles
static void Editor_SetupMarkdownStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("markdown");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_DEFAULT, theme->foreground);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_LINE_BEGIN, theme->foreground);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_STRONG1, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_STRONG2, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_EM1, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_EM2, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_HEADER1, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_HEADER2, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_HEADER3, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_CODE, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_CODE2, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_CODEBK, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_MARKDOWN_LINK, theme->identifier);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_MARKDOWN_STRONG1, TRUE);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_MARKDOWN_STRONG2, TRUE);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_MARKDOWN_HEADER1, TRUE);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_MARKDOWN_HEADER2, TRUE);
}

// Set up YAML styles
static void Editor_SetupYamlStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("yaml");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_DEFAULT, theme->foreground);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_COMMENT, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_IDENTIFIER, theme->identifier);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_KEYWORD, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_REFERENCE, theme->keyword2);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_DOCUMENT, theme->preprocessor);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_TEXT, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_YAML_ERROR, 0x0000FF);
}

// Set up Bash/Shell styles
static void Editor_SetupBashStyles(HWND hEditor) {
    const EditorTheme* theme = GetCurrentTheme();

    ILexer5* lexer = CreateLexer("bash");
    SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);

    SciCall(hEditor, SCI_SETKEYWORDS, 0, (LPARAM)
        "case do done elif else esac fi for function if in select then until while "
        "break continue exit return shift trap exec eval export readonly unset "
        "declare local typeset");

    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_COMMENTLINE, theme->comment);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_NUMBER, theme->number);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_WORD, theme->keyword);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_STRING, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_CHARACTER, theme->string);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_OPERATOR, theme->operatorColor);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_PARAM, theme->identifier);
    SciCall(hEditor, SCI_STYLESETFORE, SCE_SH_BACKTICKS, theme->keyword2);
    SciCall(hEditor, SCI_STYLESETBOLD, SCE_SH_WORD, TRUE);
}

// Set lexer based on file extension
void Editor_SetLexerFromExtension(HWND hEditor, const WCHAR* filename) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor || !filename) return;

    const WCHAR* ext = wcsrchr(filename, L'.');
    if (!ext) {
        // No extension - use plain text
        SciCall(hEditor, SCI_SETILEXER, 0, 0);
        return;
    }
    ext++;  // Skip the dot

    // C/C++
    if (_wcsicmp(ext, L"c") == 0 || _wcsicmp(ext, L"h") == 0 ||
        _wcsicmp(ext, L"cpp") == 0 || _wcsicmp(ext, L"hpp") == 0 ||
        _wcsicmp(ext, L"cc") == 0 || _wcsicmp(ext, L"cxx") == 0 ||
        _wcsicmp(ext, L"cs") == 0 || _wcsicmp(ext, L"java") == 0) {
        Editor_SetupCppStyles(hEditor);
    }
    // Python
    else if (_wcsicmp(ext, L"py") == 0 || _wcsicmp(ext, L"pyw") == 0) {
        Editor_SetupPythonStyles(hEditor);
    }
    // JavaScript/TypeScript
    else if (_wcsicmp(ext, L"js") == 0 || _wcsicmp(ext, L"jsx") == 0 ||
             _wcsicmp(ext, L"ts") == 0 || _wcsicmp(ext, L"tsx") == 0) {
        Editor_SetupJavaScriptStyles(hEditor);
    }
    // HTML
    else if (_wcsicmp(ext, L"html") == 0 || _wcsicmp(ext, L"htm") == 0 ||
             _wcsicmp(ext, L"xhtml") == 0 || _wcsicmp(ext, L"php") == 0) {
        Editor_SetupHtmlStyles(hEditor);
    }
    // JSON
    else if (_wcsicmp(ext, L"json") == 0) {
        Editor_SetupJsonStyles(hEditor);
    }
    // SQL
    else if (_wcsicmp(ext, L"sql") == 0) {
        Editor_SetupSqlStyles(hEditor);
    }
    // Markdown
    else if (_wcsicmp(ext, L"md") == 0 || _wcsicmp(ext, L"markdown") == 0) {
        Editor_SetupMarkdownStyles(hEditor);
    }
    // YAML
    else if (_wcsicmp(ext, L"yaml") == 0 || _wcsicmp(ext, L"yml") == 0) {
        Editor_SetupYamlStyles(hEditor);
    }
    // Bash/Shell
    else if (_wcsicmp(ext, L"sh") == 0 || _wcsicmp(ext, L"bash") == 0) {
        Editor_SetupBashStyles(hEditor);
    }
    // CSS
    else if (_wcsicmp(ext, L"css") == 0 || _wcsicmp(ext, L"scss") == 0 ||
             _wcsicmp(ext, L"less") == 0) {
        const EditorTheme* theme = GetCurrentTheme();
        ILexer5* lexer = CreateLexer("css");
        SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_COMMENT, theme->comment);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_TAG, theme->keyword);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_CLASS, theme->identifier);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_ID, theme->identifier);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_OPERATOR, theme->operatorColor);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_VALUE, theme->string);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_DOUBLESTRING, theme->string);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_CSS_SINGLESTRING, theme->string);
    }
    // Rust
    else if (_wcsicmp(ext, L"rs") == 0) {
        const EditorTheme* theme = GetCurrentTheme();
        ILexer5* lexer = CreateLexer("rust");
        SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_RUST_COMMENTBLOCK, theme->comment);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_RUST_COMMENTLINE, theme->comment);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_RUST_WORD, theme->keyword);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_RUST_STRING, theme->string);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_RUST_NUMBER, theme->number);
        SciCall(hEditor, SCI_STYLESETBOLD, SCE_RUST_WORD, TRUE);
    }
    // XML
    else if (_wcsicmp(ext, L"xml") == 0 || _wcsicmp(ext, L"xsl") == 0 ||
             _wcsicmp(ext, L"xslt") == 0 || _wcsicmp(ext, L"svg") == 0) {
        const EditorTheme* theme = GetCurrentTheme();
        ILexer5* lexer = CreateLexer("xml");
        SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_H_TAG, theme->keyword);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_H_TAGEND, theme->keyword);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_H_ATTRIBUTE, theme->identifier);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_H_DOUBLESTRING, theme->string);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_H_SINGLESTRING, theme->string);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_H_COMMENT, theme->comment);
    }
    // Batch files
    else if (_wcsicmp(ext, L"bat") == 0 || _wcsicmp(ext, L"cmd") == 0) {
        const EditorTheme* theme = GetCurrentTheme();
        ILexer5* lexer = CreateLexer("batch");
        SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_BAT_COMMENT, theme->comment);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_BAT_WORD, theme->keyword);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_BAT_LABEL, theme->identifier);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_BAT_COMMAND, theme->keyword2);
        SciCall(hEditor, SCI_STYLESETBOLD, SCE_BAT_WORD, TRUE);
    }
    // INI/Config files
    else if (_wcsicmp(ext, L"ini") == 0 || _wcsicmp(ext, L"cfg") == 0 ||
             _wcsicmp(ext, L"conf") == 0 || _wcsicmp(ext, L"properties") == 0) {
        const EditorTheme* theme = GetCurrentTheme();
        ILexer5* lexer = CreateLexer("props");
        SciCall(hEditor, SCI_SETILEXER, 0, (LPARAM)lexer);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_PROPS_COMMENT, theme->comment);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_PROPS_SECTION, theme->keyword);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_PROPS_KEY, theme->identifier);
        SciCall(hEditor, SCI_STYLESETFORE, SCE_PROPS_ASSIGNMENT, theme->operatorColor);
        SciCall(hEditor, SCI_STYLESETBOLD, SCE_PROPS_SECTION, TRUE);
    }
    else {
        // Plain text - no lexer
        SciCall(hEditor, SCI_SETILEXER, 0, 0);
    }
}

// Apply current theme to an editor (used when theme changes)
void Editor_ApplyTheme(HWND hEditor, const WCHAR* filename) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor) return;

    // Reapply base styles with new theme
    Editor_SetupStyles(hEditor);

    // Reapply font from settings
    if (g_app && g_app->hEditorFont) {
        Editor_SetFont(hEditor, g_app->hEditorFont);
    }

    // Reapply lexer styles if there's a filename
    if (filename && filename[0]) {
        Editor_SetLexerFromExtension(hEditor, filename);
    }

    // Force redraw
    InvalidateRect(hEditor, NULL, TRUE);
}

// Fully initialize an editor with consistent settings
static void Editor_FullInitialize(HWND hEditor) {
    if (!hEditor) return;

    const EditorTheme* theme = GetCurrentTheme();

    // Get font settings
    int fontSize = 12;
    char fontName[LF_FACESIZE] = "Consolas";

    if (g_app) {
        if (g_app->editorFont.lfFaceName[0] != L'\0') {
            WideCharToMultiByte(CP_ACP, 0, g_app->editorFont.lfFaceName, -1, fontName, LF_FACESIZE, NULL, NULL);
        }
        if (g_app->editorFont.lfHeight != 0) {
            fontSize = abs(g_app->editorFont.lfHeight);
            if (fontSize < 6 || fontSize > 72) fontSize = 12;
        }
    }

    // UTF-8 encoding
    SciCall(hEditor, SCI_SETCODEPAGE, SC_CP_UTF8, 0);

    // Set STYLE_DEFAULT
    SciCall(hEditor, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)fontName);
    SciCall(hEditor, SCI_STYLESETSIZE, STYLE_DEFAULT, fontSize);
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_DEFAULT, theme->foreground);
    SciCall(hEditor, SCI_STYLESETBACK, STYLE_DEFAULT, theme->background);

    // Propagate to ALL styles
    SciCall(hEditor, SCI_STYLECLEARALL, 0, 0);

    // Now set specific style colors (font is already inherited)
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_LINENUMBER, theme->lineNumber);
    SciCall(hEditor, SCI_STYLESETBACK, STYLE_LINENUMBER, theme->lineNumBg);

    // Caret and selection
    SciCall(hEditor, SCI_SETCARETFORE, theme->caret, 0);
    SciCall(hEditor, SCI_SETCARETWIDTH, 2, 0);
    SciCall(hEditor, SCI_SETSELBACK, TRUE, theme->selectionBg);

    // Line number margin
    SciCall(hEditor, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    SciCall(hEditor, SCI_SETMARGINWIDTHN, 0, 50);

    // Indentation
    SciCall(hEditor, SCI_SETTABWIDTH, 4, 0);
    SciCall(hEditor, SCI_SETINDENT, 4, 0);
    SciCall(hEditor, SCI_SETUSETABS, FALSE, 0);
    SciCall(hEditor, SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH, 0);

    // Brace matching
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_BRACELIGHT, 0x00FF00);
    SciCall(hEditor, SCI_STYLESETBACK, STYLE_BRACELIGHT, theme->background);
    SciCall(hEditor, SCI_STYLESETBOLD, STYLE_BRACELIGHT, TRUE);
    SciCall(hEditor, SCI_STYLESETFORE, STYLE_BRACEBAD, 0x0000FF);

    // Scrolling
    SciCall(hEditor, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
    SciCall(hEditor, SCI_SETSCROLLWIDTH, 1, 0);

    // Undo
    SciCall(hEditor, SCI_SETUNDOCOLLECTION, TRUE, 0);
    SciCall(hEditor, SCI_EMPTYUNDOBUFFER, 0, 0);

    // Indicators for links and spell check
    Editor_SetupLinkIndicator(hEditor);

    // Notifications
    SciCall(hEditor, SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT, 0);

    // Mouse dwell for links
    SciCall(hEditor, SCI_SETMOUSEDWELLTIME, 500, 0);

    // Disable built-in context menu
    SciCall(hEditor, SCI_USEPOPUP, SC_POPUP_NEVER, 0);
}

// Create editor control (Scintilla)
HWND Editor_Create(HWND hParent) {
    InitScintilla();

    HWND hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"Scintilla",
        NULL,
        WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN,
        0, 0, 100, 100,
        hParent,
        NULL,
        g_app->hInstance,
        NULL
    );

    if (hEdit) {
        Editor_SetKind(hEdit, EDITOR_KIND_PLAIN);
        // Single initialization path for all editors
        Editor_FullInitialize(hEdit);
    }

    return hEdit;
}

// The rich text view. Creation lives in editor_rich.c; what happens here is
// stamping the kind so every Editor_* call afterwards routes correctly.
HWND Editor_CreateRich(HWND hParent) {
    HWND hEdit = Rich_Create(hParent);
    if (hEdit) {
        Editor_SetKind(hEdit, EDITOR_KIND_RICH);
    }
    return hEdit;
}

// Set text content
void Editor_SetText(HWND hEditor, const WCHAR* text) {
    if (Editor_IsRich(hEditor)) { Rich_SetText(hEditor, text); return; }
    if (!hEditor) return;

    // Convert to UTF-8 for Scintilla
    if (text) {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
        char* utf8 = (char*)malloc(utf8Len);
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);
            SciCall(hEditor, SCI_SETTEXT, 0, (LPARAM)utf8);
            free(utf8);
        }
    } else {
        SciCall(hEditor, SCI_SETTEXT, 0, (LPARAM)"");
    }

    SciCall(hEditor, SCI_EMPTYUNDOBUFFER, 0, 0);
    SciCall(hEditor, SCI_SETSAVEPOINT, 0, 0);
}

// Replace everything, as an edit rather than as a load.
//
// Editor_SetText is what loading a file does: it drops the undo history and
// says "this is the saved content". Text coming back from the page layout view
// is neither of those -- it is something the user typed, so it has to be
// undoable and it has to leave the document modified.
void Editor_SetTextAsEdit(HWND hEditor, const WCHAR* text) {
    if (Editor_IsRich(hEditor)) {
        Rich_SetText(hEditor, text);
        Rich_SetModified(hEditor, TRUE);
        return;
    }
    if (!hEditor) return;

    if (!text) text = L"";
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)malloc(utf8Len);
    if (!utf8) return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);

    SciCall(hEditor, SCI_SETTARGETRANGE, 0, SciCall(hEditor, SCI_GETLENGTH, 0, 0));
    SciCall(hEditor, SCI_REPLACETARGET, utf8Len - 1, (LPARAM)utf8);
    free(utf8);
}

// Get text content (caller must free)
WCHAR* Editor_GetText(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_GetText(hEditor);
    if (!hEditor) return NULL;

    int utf8Len = (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0) + 1;
    char* utf8 = (char*)malloc(utf8Len);
    if (!utf8) return NULL;

    SciCall(hEditor, SCI_GETTEXT, utf8Len, (LPARAM)utf8);

    // Convert to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    WCHAR* buffer = (WCHAR*)malloc(wideLen * sizeof(WCHAR));
    if (buffer) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buffer, wideLen);
    }

    free(utf8);
    return buffer;
}

// Get text length
int Editor_GetTextLength(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_GetTextLength(hEditor);
    if (!hEditor) return 0;
    return (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0);
}

// Get selection
void Editor_GetSelection(HWND hEditor, int* start, int* end) {
    if (Editor_IsRich(hEditor)) { Rich_GetSelection(hEditor, start, end); return; }
    if (!hEditor) return;
    if (start) *start = (int)SciCall(hEditor, SCI_GETSELECTIONSTART, 0, 0);
    if (end) *end = (int)SciCall(hEditor, SCI_GETSELECTIONEND, 0, 0);
}

// Set selection
void Editor_SetSelection(HWND hEditor, int start, int end) {
    if (Editor_IsRich(hEditor)) { Rich_SetSelection(hEditor, start, end); return; }
    if (!hEditor) return;
    SciCall(hEditor, SCI_SETSEL, start, end);
}

// Replace selection
void Editor_ReplaceSelection(HWND hEditor, const WCHAR* text) {
    if (Editor_IsRich(hEditor)) { Rich_ReplaceSelection(hEditor, text); return; }
    if (!hEditor) return;

    // Convert to UTF-8
    char* utf8 = NULL;
    if (text) {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
        utf8 = (char*)malloc(utf8Len);
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);
        }
    }

    SciCall(hEditor, SCI_REPLACESEL, 0, (LPARAM)(utf8 ? utf8 : ""));
    free(utf8);
}

// Get selected text (caller must free)
WCHAR* Editor_GetSelectedText(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_GetSelectedText(hEditor);
    if (!hEditor) return NULL;

    int start, end;
    Editor_GetSelection(hEditor, &start, &end);
    if (start == end) return NULL;

    int utf8Len = end - start + 1;
    char* utf8 = (char*)malloc(utf8Len);
    if (!utf8) return NULL;

    SciCall(hEditor, SCI_GETSELTEXT, 0, (LPARAM)utf8);

    // Convert to wide
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    WCHAR* buffer = (WCHAR*)malloc(wideLen * sizeof(WCHAR));
    if (buffer) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buffer, wideLen);
    }

    free(utf8);
    return buffer;
}

// Get cursor position (line and column)
void Editor_GetCursorPos(HWND hEditor, int* line, int* column) {
    if (Editor_IsRich(hEditor)) { Rich_GetCursorPos(hEditor, line, column); return; }
    if (!hEditor) return;

    int pos = (int)SciCall(hEditor, SCI_GETCURRENTPOS, 0, 0);
    int lineNum = (int)SciCall(hEditor, SCI_LINEFROMPOSITION, pos, 0);
    int lineStart = (int)SciCall(hEditor, SCI_POSITIONFROMLINE, lineNum, 0);

    if (line) *line = lineNum + 1;  // 1-based
    if (column) *column = pos - lineStart + 1;  // 1-based
}

// Go to line
void Editor_GotoLine(HWND hEditor, int line) {
    if (Editor_IsRich(hEditor)) { Rich_GotoLine(hEditor, line); return; }
    if (!hEditor || line < 1) return;

    int lineIndex = (int)SciCall(hEditor, SCI_POSITIONFROMLINE, line - 1, 0);
    SciCall(hEditor, SCI_GOTOPOS, lineIndex, 0);
}

// Get line count
int Editor_GetLineCount(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_GetLineCount(hEditor);
    if (!hEditor) return 0;
    return (int)SciCall(hEditor, SCI_GETLINECOUNT, 0, 0);
}

// Get current line number
int Editor_GetCurrentLine(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_GetCurrentLine(hEditor);
    int line, col;
    Editor_GetCursorPos(hEditor, &line, &col);
    return line;
}

// Undo/Redo
BOOL Editor_CanUndo(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_CanUndo(hEditor);
    if (!hEditor) return FALSE;
    return (BOOL)SciCall(hEditor, SCI_CANUNDO, 0, 0);
}

BOOL Editor_CanRedo(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_CanRedo(hEditor);
    if (!hEditor) return FALSE;
    return (BOOL)SciCall(hEditor, SCI_CANREDO, 0, 0);
}

void Editor_Undo(HWND hEditor) {
    if (Editor_IsRich(hEditor)) { Rich_Undo(hEditor); return; }
    if (!hEditor) return;
    SciCall(hEditor, SCI_UNDO, 0, 0);
}

void Editor_Redo(HWND hEditor) {
    if (Editor_IsRich(hEditor)) { Rich_Redo(hEditor); return; }
    if (!hEditor) return;
    SciCall(hEditor, SCI_REDO, 0, 0);
}

// Clipboard operations
void Editor_Cut(HWND hEditor) {
    if (Editor_IsRich(hEditor)) { Rich_Cut(hEditor); return; }
    if (!hEditor) return;
    SciCall(hEditor, SCI_CUT, 0, 0);
}

void Editor_Copy(HWND hEditor) {
    if (Editor_IsRich(hEditor)) { Rich_Copy(hEditor); return; }
    if (!hEditor) return;
    SciCall(hEditor, SCI_COPY, 0, 0);
}

void Editor_Paste(HWND hEditor) {
    if (Editor_IsRich(hEditor)) { Rich_Paste(hEditor); return; }
    if (!hEditor) return;
    SciCall(hEditor, SCI_PASTE, 0, 0);
}

// Set word wrap
void Editor_SetWordWrap(HWND hEditor, BOOL wrap) {
    if (Editor_IsRich(hEditor)) { Rich_SetWordWrap(hEditor, wrap); return; }
    if (!hEditor) return;
    SciCall(hEditor, SCI_SETWRAPMODE, wrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
}

// Get DPI for a window (or primary monitor if NULL)
static int GetWindowDPI(HWND hwnd) {
    // Try GetDpiForWindow (Windows 10 1607+)
    typedef UINT (WINAPI *GetDpiForWindowFunc)(HWND);
    static GetDpiForWindowFunc pGetDpiForWindow = NULL;
    static BOOL tried = FALSE;

    if (!tried) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            pGetDpiForWindow = (GetDpiForWindowFunc)GetProcAddress(hUser32, "GetDpiForWindow");
        }
        tried = TRUE;
    }

    if (pGetDpiForWindow && hwnd) {
        UINT dpi = pGetDpiForWindow(hwnd);
        if (dpi > 0) return (int)dpi;
    }

    // Fallback: get system DPI
    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);
    return dpi > 0 ? dpi : 96;
}

// Set font
void Editor_SetFont(HWND hEditor, HFONT hFont) {
    if (Editor_IsRich(hEditor)) { Rich_SetFont(hEditor, hFont); return; }
    if (!hEditor) return;
    (void)hFont;  // We read directly from g_app now

    // Get font settings directly from app state for reliability
    int pointSize = 12;  // Default
    char fontName[LF_FACESIZE] = "Consolas";

    if (g_app && g_app->editorFont.lfFaceName[0] != L'\0') {
        WideCharToMultiByte(CP_ACP, 0, g_app->editorFont.lfFaceName, -1, fontName, LF_FACESIZE, NULL, NULL);
    }

    if (g_app && g_app->editorFont.lfHeight != 0) {
        // lfHeight is stored as negative point size (e.g., -14 for 14pt)
        pointSize = abs(g_app->editorFont.lfHeight);
    }

    // Sanity check
    if (pointSize < 6 || pointSize > 72) pointSize = 12;

    // Apply font to ALL styles
    for (int i = 0; i <= STYLE_LASTPREDEFINED; i++) {
        SciCall(hEditor, SCI_STYLESETFONT, i, (LPARAM)fontName);
        SciCall(hEditor, SCI_STYLESETSIZE, i, pointSize);
    }

    // Also set for common style ranges used by lexers (0-127)
    for (int i = 0; i < 128; i++) {
        SciCall(hEditor, SCI_STYLESETFONT, i, (LPARAM)fontName);
        SciCall(hEditor, SCI_STYLESETSIZE, i, pointSize);
    }
}

// Set tab size
void Editor_SetTabSize(HWND hEditor, int tabSize) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor) return;
    SciCall(hEditor, SCI_SETTABWIDTH, tabSize, 0);
    SciCall(hEditor, SCI_SETINDENT, tabSize, 0);
}

// Set zoom level
void Editor_SetZoom(HWND hEditor, int zoomPercent) {
    if (Editor_IsRich(hEditor)) { Rich_SetZoom(hEditor, zoomPercent); return; }
    if (!hEditor) return;
    // Scintilla zoom is in points relative to normal (-10 to +20)
    int zoom = (zoomPercent - 100) / 10;
    SciCall(hEditor, SCI_SETZOOM, zoom, 0);
}

// Get/Set modified state
BOOL Editor_GetModified(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return Rich_GetModified(hEditor);
    if (!hEditor) return FALSE;
    return (BOOL)SciCall(hEditor, SCI_GETMODIFY, 0, 0);
}

void Editor_SetModified(HWND hEditor, BOOL modified) {
    if (Editor_IsRich(hEditor)) { Rich_SetModified(hEditor, modified); return; }
    if (!hEditor) return;
    if (!modified) {
        SciCall(hEditor, SCI_SETSAVEPOINT, 0, 0);
    }
}

// Find text
int Editor_FindText(HWND hEditor, const WCHAR* text, BOOL matchCase, BOOL wholeWord, BOOL forward) {
    if (Editor_IsRich(hEditor)) return Rich_FindText(hEditor, text, matchCase, wholeWord, forward);
    if (!hEditor || !text || !text[0]) return -1;

    // Convert search text to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)malloc(utf8Len);
    if (!utf8) return -1;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);

    int flags = 0;
    if (matchCase) flags |= SCFIND_MATCHCASE;
    if (wholeWord) flags |= SCFIND_WHOLEWORD;

    int docLen = (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0);
    int selStart = (int)SciCall(hEditor, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SciCall(hEditor, SCI_GETSELECTIONEND, 0, 0);

    int searchStart, searchEnd;
    if (forward) {
        searchStart = selEnd;
        searchEnd = docLen;
    } else {
        searchStart = selStart;
        searchEnd = 0;
    }

    SciCall(hEditor, SCI_SETTARGETSTART, searchStart, 0);
    SciCall(hEditor, SCI_SETTARGETEND, searchEnd, 0);
    SciCall(hEditor, SCI_SETSEARCHFLAGS, flags, 0);

    int pos = (int)SciCall(hEditor, SCI_SEARCHINTARGET, utf8Len - 1, (LPARAM)utf8);

    // Wrap around
    if (pos < 0 && g_app->wrapAround) {
        if (forward) {
            SciCall(hEditor, SCI_SETTARGETSTART, 0, 0);
            SciCall(hEditor, SCI_SETTARGETEND, selStart, 0);
        } else {
            SciCall(hEditor, SCI_SETTARGETSTART, docLen, 0);
            SciCall(hEditor, SCI_SETTARGETEND, selEnd, 0);
        }
        pos = (int)SciCall(hEditor, SCI_SEARCHINTARGET, utf8Len - 1, (LPARAM)utf8);
    }

    free(utf8);

    if (pos >= 0) {
        int targetEnd = (int)SciCall(hEditor, SCI_GETTARGETEND, 0, 0);
        SciCall(hEditor, SCI_SETSEL, pos, targetEnd);
        SciCall(hEditor, SCI_SCROLLCARET, 0, 0);
        return pos;
    }

    MessageBoxW(g_app->hMainWindow, L"Cannot find the specified text.", APP_NAME, MB_ICONINFORMATION);
    return -1;
}

// Replace text
int Editor_ReplaceText(HWND hEditor, const WCHAR* findText, const WCHAR* replaceText, BOOL matchCase, BOOL wholeWord) {
    if (Editor_IsRich(hEditor)) return Rich_ReplaceText(hEditor, findText, replaceText, matchCase, wholeWord);
    if (!hEditor || !findText || !findText[0]) return -1;

    // Check if current selection matches
    WCHAR* selected = Editor_GetSelectedText(hEditor);
    BOOL selectionMatches = FALSE;

    if (selected) {
        if (matchCase) {
            selectionMatches = (wcscmp(selected, findText) == 0);
        } else {
            selectionMatches = (_wcsicmp(selected, findText) == 0);
        }
        free(selected);
    }

    if (selectionMatches) {
        Editor_ReplaceSelection(hEditor, replaceText ? replaceText : L"");
    }

    // Find next
    return Editor_FindText(hEditor, findText, matchCase, wholeWord, TRUE);
}

// Replace all
int Editor_ReplaceAll(HWND hEditor, const WCHAR* findText, const WCHAR* replaceText, BOOL matchCase, BOOL wholeWord) {
    if (Editor_IsRich(hEditor)) return Rich_ReplaceAll(hEditor, findText, replaceText, matchCase, wholeWord);
    if (!hEditor || !findText || !findText[0]) return 0;

    // Convert to UTF-8
    int findUtf8Len = WideCharToMultiByte(CP_UTF8, 0, findText, -1, NULL, 0, NULL, NULL);
    char* findUtf8 = (char*)malloc(findUtf8Len);
    if (!findUtf8) return 0;
    WideCharToMultiByte(CP_UTF8, 0, findText, -1, findUtf8, findUtf8Len, NULL, NULL);

    int replaceUtf8Len = 0;
    char* replaceUtf8 = NULL;
    if (replaceText) {
        replaceUtf8Len = WideCharToMultiByte(CP_UTF8, 0, replaceText, -1, NULL, 0, NULL, NULL);
        replaceUtf8 = (char*)malloc(replaceUtf8Len);
        if (replaceUtf8) {
            WideCharToMultiByte(CP_UTF8, 0, replaceText, -1, replaceUtf8, replaceUtf8Len, NULL, NULL);
        }
    }

    int flags = 0;
    if (matchCase) flags |= SCFIND_MATCHCASE;
    if (wholeWord) flags |= SCFIND_WHOLEWORD;

    int count = 0;
    int docLen = (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0);

    // Begin undo action
    SciCall(hEditor, SCI_BEGINUNDOACTION, 0, 0);

    SciCall(hEditor, SCI_SETTARGETSTART, 0, 0);
    SciCall(hEditor, SCI_SETTARGETEND, docLen, 0);
    SciCall(hEditor, SCI_SETSEARCHFLAGS, flags, 0);

    while (TRUE) {
        int pos = (int)SciCall(hEditor, SCI_SEARCHINTARGET, findUtf8Len - 1, (LPARAM)findUtf8);
        if (pos < 0) break;

        SciCall(hEditor, SCI_REPLACETARGET, replaceUtf8Len - 1, (LPARAM)(replaceUtf8 ? replaceUtf8 : ""));
        count++;

        // Update search range
        int newEnd = pos + (replaceUtf8Len - 1);
        docLen = (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0);
        SciCall(hEditor, SCI_SETTARGETSTART, newEnd, 0);
        SciCall(hEditor, SCI_SETTARGETEND, docLen, 0);
    }

    // End undo action
    SciCall(hEditor, SCI_ENDUNDOACTION, 0, 0);

    free(findUtf8);
    free(replaceUtf8);

    // Show result
    WCHAR msg[64];
    swprintf_s(msg, sizeof(msg)/sizeof(WCHAR), L"Replaced %d occurrence(s).", count);
    MessageBoxW(g_app->hMainWindow, msg, APP_NAME, MB_ICONINFORMATION);

    return count;
}

// Insert time/date
void Editor_InsertTimeDate(HWND hEditor) {
    if (!hEditor) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    WCHAR timeDate[128];
    int len = GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, timeDate, 64);
    if (len > 0) {
        timeDate[len - 1] = L' ';
        GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, timeDate + len, 64);
    }

    Editor_ReplaceSelection(hEditor, timeDate);
}

// ============================================================================
// Link Indicators
// ============================================================================

// Setup link indicator style
void Editor_SetupLinkIndicator(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor) return;

    // Configure link indicator - composited box style for visibility
    SciCall(hEditor, SCI_INDICSETSTYLE, INDICATOR_LINK, INDIC_COMPOSITIONTHIN);
    SciCall(hEditor, SCI_INDICSETFORE, INDICATOR_LINK, 0xCC6600);  // Orange
    SciCall(hEditor, SCI_INDICSETALPHA, INDICATOR_LINK, 70);
    SciCall(hEditor, SCI_INDICSETOUTLINEALPHA, INDICATOR_LINK, 150);
    SciCall(hEditor, SCI_INDICSETUNDER, INDICATOR_LINK, TRUE);

    // Set hover style - makes indicator respond to clicks with SCN_INDICATORCLICK
    SciCall(hEditor, SCI_INDICSETHOVERSTYLE, INDICATOR_LINK, INDIC_FULLBOX);
    SciCall(hEditor, SCI_INDICSETHOVERFORE, INDICATOR_LINK, 0xFF8800);

    // Configure spell indicator - squiggly red underline
    SciCall(hEditor, SCI_INDICSETSTYLE, INDICATOR_SPELL, INDIC_SQUIGGLE);
    SciCall(hEditor, SCI_INDICSETFORE, INDICATOR_SPELL, 0x0000FF);  // Red
    SciCall(hEditor, SCI_INDICSETALPHA, INDICATOR_SPELL, 200);
    SciCall(hEditor, SCI_INDICSETUNDER, INDICATOR_SPELL, TRUE);
}

// Add link indicator at range
void Editor_AddLinkIndicator(HWND hEditor, int start, int end) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor || start >= end) return;

    SciCall(hEditor, SCI_SETINDICATORCURRENT, INDICATOR_LINK, 0);
    SciCall(hEditor, SCI_INDICATORFILLRANGE, start, end - start);
}

// Clear all link indicators
void Editor_ClearLinkIndicators(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor) return;

    int docLen = (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0);
    SciCall(hEditor, SCI_SETINDICATORCURRENT, INDICATOR_LINK, 0);
    SciCall(hEditor, SCI_INDICATORCLEARRANGE, 0, docLen);
}

// Refresh links from database for current document
void Editor_RefreshLinks(HWND hEditor, Document* doc) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor || !doc) return;

    // Clear existing link indicators
    Editor_ClearLinkIndicators(hEditor);

    // Get links from database
    LinkArray* links = NULL;
    if (doc->type == DOC_TYPE_FILE) {
        links = Links_GetForSource(DOC_TYPE_FILE, doc->filePath, 0);
    } else if (doc->type == DOC_TYPE_NOTE) {
        links = Links_GetForSource(DOC_TYPE_NOTE, NULL, doc->noteId);
    }

    if (links) {
        for (int i = 0; i < links->count; i++) {
            Editor_AddLinkIndicator(hEditor, links->items[i].startPos, links->items[i].endPos);
        }
        Links_FreeArray(links);
    }
}

// Get position from screen coordinates
int Editor_GetPositionFromPoint(HWND hEditor, int x, int y) {
    if (Editor_IsRich(hEditor)) return Rich_GetPositionFromPoint(hEditor, x, y);
    if (!hEditor) return -1;

    // Convert screen to client coordinates
    POINT pt = { x, y };
    ScreenToClient(hEditor, &pt);

    return (int)SciCall(hEditor, SCI_POSITIONFROMPOINT, pt.x, pt.y);
}

// ============================================================================
// Spell Check (Windows ISpellChecker API)
// ============================================================================

// Initialize spell checker
void Editor_InitSpellCheck(void) {
    if (g_spellCheckInit) return;
    g_spellCheckInit = TRUE;  // Mark as init attempted

    // Try to initialize COM - don't fail if already initialized
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        // COM initialization failed, spell check won't be available
        return;
    }

    // Try to create spell checker factory - this may fail on older Windows
    __try {
        ISpellCheckerFactory* factory = NULL;
        hr = CoCreateInstance(&CLSID_SpellCheckerFactoryLocal, NULL, CLSCTX_INPROC_SERVER,
                              &IID_ISpellCheckerFactoryLocal, (void**)&factory);

        if (SUCCEEDED(hr) && factory) {
            // Check if spell checking is supported
            BOOL supported = FALSE;
            hr = factory->lpVtbl->IsSupported(factory, L"en-US", &supported);

            if (SUCCEEDED(hr) && supported) {
                hr = factory->lpVtbl->CreateSpellChecker(factory, L"en-US", &g_spellChecker);
            }

            factory->lpVtbl->Release(factory);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        // Spell check initialization failed, continue without it
        g_spellChecker = NULL;
    }
}

// Shutdown spell checker
void Editor_ShutdownSpellCheck(void) {
    if (g_spellChecker) {
        g_spellChecker->lpVtbl->Release(g_spellChecker);
        g_spellChecker = NULL;
    }
    g_spellCheckInit = FALSE;
}

// Add spell indicator at range
void Editor_AddSpellIndicator(HWND hEditor, int start, int end) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor || start >= end) return;

    SciCall(hEditor, SCI_SETINDICATORCURRENT, INDICATOR_SPELL, 0);
    SciCall(hEditor, SCI_INDICATORFILLRANGE, start, end - start);
}

// Clear all spell indicators
void Editor_ClearSpellIndicators(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor) return;

    int docLen = (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0);
    SciCall(hEditor, SCI_SETINDICATORCURRENT, INDICATOR_SPELL, 0);
    SciCall(hEditor, SCI_INDICATORCLEARRANGE, 0, docLen);
}

// Check spelling of entire document
void Editor_CheckSpelling(HWND hEditor) {
    if (Editor_IsRich(hEditor)) return;  // rich view has no squiggle indicators yet
    if (!hEditor || !g_spellChecker) return;

    // Clear existing spell indicators
    Editor_ClearSpellIndicators(hEditor);

    // Get document text
    int docLen = (int)SciCall(hEditor, SCI_GETLENGTH, 0, 0);
    if (docLen == 0) return;

    char* utf8 = (char*)malloc(docLen + 1);
    if (!utf8) return;

    SciCall(hEditor, SCI_GETTEXT, docLen + 1, (LPARAM)utf8);

    // Convert to wide string for spell checker
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    WCHAR* wideText = (WCHAR*)malloc(wideLen * sizeof(WCHAR));
    if (!wideText) {
        free(utf8);
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wideText, wideLen);

    // Check each word
    IEnumSpellingError* errors = NULL;
    HRESULT hr = g_spellChecker->lpVtbl->Check(g_spellChecker, wideText, &errors);

    if (SUCCEEDED(hr) && errors) {
        ISpellingError* error = NULL;

        while (errors->lpVtbl->Next(errors, &error) == S_OK && error) {
            ULONG startIndex = 0;
            ULONG length = 0;

            error->lpVtbl->get_StartIndex(error, &startIndex);
            error->lpVtbl->get_Length(error, &length);

            // Convert wide char positions back to UTF-8 byte positions
            // This is approximate - for accurate results we'd need a proper mapping
            int utf8Start = WideCharToMultiByte(CP_UTF8, 0, wideText, startIndex, NULL, 0, NULL, NULL);
            int utf8End = WideCharToMultiByte(CP_UTF8, 0, wideText, startIndex + length, NULL, 0, NULL, NULL);

            Editor_AddSpellIndicator(hEditor, utf8Start, utf8End);

            error->lpVtbl->Release(error);
        }

        errors->lpVtbl->Release(errors);
    }

    free(wideText);
    free(utf8);
}

// Get spell suggestions for a word
WCHAR** Editor_GetSpellSuggestions(const WCHAR* word, int* count) {
    *count = 0;
    if (!g_spellChecker || !word || !word[0]) return NULL;

    IEnumString* suggestions = NULL;
    HRESULT hr = g_spellChecker->lpVtbl->Suggest(g_spellChecker, word, &suggestions);

    if (FAILED(hr) || !suggestions) return NULL;

    // Allocate array for suggestions (max 10)
    WCHAR** result = (WCHAR**)calloc(10, sizeof(WCHAR*));
    if (!result) {
        suggestions->lpVtbl->Release(suggestions);
        return NULL;
    }

    LPOLESTR suggestion;
    while (*count < 10 && suggestions->lpVtbl->Next(suggestions, 1, &suggestion, NULL) == S_OK) {
        result[*count] = _wcsdup(suggestion);
        CoTaskMemFree(suggestion);
        (*count)++;
    }

    suggestions->lpVtbl->Release(suggestions);
    return result;
}

// Free spell suggestions
void Editor_FreeSpellSuggestions(WCHAR** suggestions, int count) {
    if (!suggestions) return;
    for (int i = 0; i < count; i++) {
        free(suggestions[i]);
    }
    free(suggestions);
}

// Add word to user dictionary
BOOL Editor_AddWordToDictionary(const WCHAR* word) {
    if (!g_spellChecker || !word || !word[0]) return FALSE;

    HRESULT hr = g_spellChecker->lpVtbl->Add(g_spellChecker, word);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Operations that callers outside this file used to reach for by sending
// Scintilla messages directly. They are here so that mainwindow.c does not
// need to know which control is behind the HWND -- see ROADMAP.md v0.5, where
// a second document view arrives and this is the seam it slots into.
// ---------------------------------------------------------------------------

static int IndicatorNumber(EditorIndicator which) {
    return (which == EDITOR_INDICATOR_LINK) ? INDICATOR_LINK : INDICATOR_SPELL;
}

BOOL Editor_HasIndicatorAt(HWND hEditor, int pos, EditorIndicator which) {
    if (Editor_IsRich(hEditor)) return FALSE;  // no indicators in the rich view
    if (!hEditor) return FALSE;

    int indicators = (int)SciCall(hEditor, SCI_INDICATORALLONFOR, pos, 0);
    return (indicators & (1 << IndicatorNumber(which))) != 0;
}

void Editor_ReplaceRange(HWND hEditor, int start, int end, const WCHAR* text) {
    if (Editor_IsRich(hEditor)) { Rich_ReplaceRange(hEditor, start, end, text); return; }
    if (!hEditor || !text || start > end) return;

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (utf8Len <= 0) return;

    char* utf8 = (char*)malloc(utf8Len);
    if (!utf8) return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);

    SciCall(hEditor, SCI_SETTARGETSTART, start, 0);
    SciCall(hEditor, SCI_SETTARGETEND, end, 0);
    SciCall(hEditor, SCI_REPLACETARGET, -1, utf8);

    free(utf8);
}

void Editor_ClearSpellIndicatorRange(HWND hEditor, int start, int end) {
    if (Editor_IsRich(hEditor)) return;
    if (!hEditor || start >= end) return;

    SciCall(hEditor, SCI_SETINDICATORCURRENT, INDICATOR_SPELL, 0);
    SciCall(hEditor, SCI_INDICATORCLEARRANGE, start, end - start);
}

// Word under a position, for the spell-check context menu. Positions are byte
// offsets into the UTF-8 document, which is why the scan happens on the UTF-8
// line rather than after converting to wide characters.
WCHAR* Editor_GetWordAt(HWND hEditor, int pos, int* wordStart, int* wordEnd) {
    if (Editor_IsRich(hEditor)) return Rich_GetWordAt(hEditor, pos, wordStart, wordEnd);
    if (!hEditor || !wordStart || !wordEnd) return NULL;

    int lineNum   = (int)SciCall(hEditor, SCI_LINEFROMPOSITION, pos, 0);
    int lineStart = (int)SciCall(hEditor, SCI_POSITIONFROMLINE, lineNum, 0);
    int lineEnd   = (int)SciCall(hEditor, SCI_GETLINEENDPOSITION, lineNum, 0);

    int lineLen = lineEnd - lineStart;
    if (lineLen <= 0) return NULL;

    char* lineUtf8 = (char*)malloc((size_t)lineLen + 1);
    if (!lineUtf8) return NULL;

    struct Sci_TextRange tr;
    tr.chrg.cpMin = lineStart;
    tr.chrg.cpMax = lineEnd;
    tr.lpstrText = lineUtf8;
    SciCall(hEditor, SCI_GETTEXTRANGE, 0, &tr);

    int relPos = pos - lineStart;
    if (relPos < 0) relPos = 0;
    if (relPos > lineLen) relPos = lineLen;

    int wStart = relPos;
    int wEnd = relPos;

    while (wStart > 0 &&
           (isalnum((unsigned char)lineUtf8[wStart - 1]) || lineUtf8[wStart - 1] == '\'')) {
        wStart--;
    }
    while (wEnd < lineLen &&
           (isalnum((unsigned char)lineUtf8[wEnd]) || lineUtf8[wEnd] == '\'')) {
        wEnd++;
    }

    if (wStart >= wEnd) {
        free(lineUtf8);
        return NULL;
    }

    char wordUtf8[256];
    size_t wordLen = (size_t)(wEnd - wStart);
    if (wordLen >= sizeof(wordUtf8)) wordLen = sizeof(wordUtf8) - 1;
    memcpy(wordUtf8, lineUtf8 + wStart, wordLen);
    wordUtf8[wordLen] = '\0';

    free(lineUtf8);

    *wordStart = lineStart + wStart;
    *wordEnd = lineStart + wEnd;

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, wordUtf8, -1, NULL, 0);
    WCHAR* word = (WCHAR*)malloc((size_t)wideLen * sizeof(WCHAR));
    if (word) {
        MultiByteToWideChar(CP_UTF8, 0, wordUtf8, -1, word, wideLen);
    }
    return word;
}
