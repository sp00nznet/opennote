# Controls

Every key, button and mouse gesture opennote answers to. Nothing here is hidden behind a
setting; if a command has a shortcut, the menu shows the same one.

The application has two places you can be: the **text view**, where a document flows into
the window, and the **page layout view**, where the same document is laid out on pages.
Most of this table is the text view, because that is where the menus live. The page
layout view has its own section — it is a different window with its own keys, and it
answers a deliberately small set of them.

---

## File

| Key | What it does |
|-----|--------------|
| `Ctrl+N` | New plain text document, in a new tab |
| — | **File → New Rich Text Document** for a formatted one |
| `Ctrl+O` | Open a file — `.txt`, `.rtf`, `.docx`, or anything else as text |
| — | **File → Browse Notes…** opens the note store instead of the file system |
| `Ctrl+S` | Save. A document that has never been saved asks where |
| `Ctrl+Shift+S` | Save As — this is also how a plain note becomes `.rtf` or `.docx` |
| `Ctrl+W` | Close the tab. An unsaved document asks first |
| `Ctrl+P` | Print |
| — | **File → Print Preview** opens the page layout view (the same thing) |
| — | **File → Page Setup…** — paper size, orientation, margins |
| — | **File → Export to PDF…** |
| — | **File → Recent Files** |

## Editing

| Key | What it does |
|-----|--------------|
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+X` / `Ctrl+C` / `Ctrl+V` | Cut / copy / paste |
| `Del` | Delete the selection, or the character ahead of the caret |
| `Ctrl+A` | Select all |
| `Ctrl+F` | Find |
| `F3` / `Shift+F3` | Find next / find previous |
| `Ctrl+H` | Replace |
| — | **Edit → Find in All Tabs…** and **Replace in All Tabs…** search every open document |
| `Ctrl+G` | Go to line |
| `F5` | Insert the time and date |

## Formatting

Formatting applies to a rich text document (`.rtf` or `.docx`). On a plain text file the
formatting half of the toolbar greys out, because a `.txt` has nowhere to keep it.

| Key | What it does |
|-----|--------------|
| `Ctrl+B` / `Ctrl+I` / `Ctrl+U` | Bold / italic / underline |
| — | **Format → Strikethrough**, **Superscript**, **Subscript**, **Text Colour…** |
| — | **Format → Alignment** — left, centre, right, justify |
| — | **Format → Line Spacing** — single, 1.5, double |
| — | **Format → Bulleted List** / **Numbered List** |
| — | **Format → Increase Indent** / **Decrease Indent** |
| — | **Format → Insert Picture…** |
| — | **Format → Clear Formatting** |
| — | **Format → Font…**, **Word Wrap**, **Tab Size** (plain text) |

## View

| Key | What it does |
|-----|--------------|
| `Ctrl+Shift+L` | Switch this tab between the text view and page layout |
| `Ctrl++` / `Ctrl+-` | Zoom in / out |
| `Ctrl+0` | Reset zoom |
| `F6` | Markdown preview |
| — | **View → Toolbar**, **Status Bar** — both remembered between runs |
| — | **View → Notes Browser…** |
| — | **View → Always on Top** |

## The toolbar

Left to right: **New**, **Open**, **Save**, **Print**, then **Page layout** — which
latches, and shows which view the tab is in. Those five mean the same thing in any
document. After the separator, the font and size boxes and the formatting buttons, which
apply to rich text and grey out otherwise.

**View → Toolbar** hides it entirely; the setting is remembered.

---

## The page layout view

`Ctrl+Shift+L`, the toolbar's page button, or **File → Print Preview**. The document is
laid out on pages, and you can edit it there — this is not a preview you can only look at.

### Moving

| Key | What it does |
|-----|--------------|
| `←` `→` | A character. Crosses paragraphs at the ends |
| `↑` `↓` | A laid-out line — the wrapped line you can see, not the paragraph |
| `Home` / `End` | Start / end of the line |
| `Ctrl+Home` / `Ctrl+End` | Start / end of the document |
| `Page Up` / `Page Down` | An actual page |
| `Shift` + any of the above | Extends the selection instead of moving |

### Editing

| Key | What it does |
|-----|--------------|
| Typing | Inserts at the caret, replacing the selection |
| `Backspace` / `Del` | Delete backwards / forwards |
| `Enter` | Split the paragraph |
| `Tab` | Insert a tab |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo. The stack is model snapshots, not keystrokes |
| `Ctrl+X` / `Ctrl+C` / `Ctrl+V` | Cut / copy / paste, as plain text |
| `Ctrl+A` | Select the whole document |
| `Ctrl+S` | Save — the edits go back to the document first |
| `Ctrl+P` | Print the pages you are looking at |
| `Esc` | Back to the text view |

Character formatting — bold, fonts, colour — is not applied here. It belongs to the text
view, and that half of the toolbar is grey while you are on the page.

### Mouse

| Gesture | What it does |
|---------|--------------|
| Click | Put the caret at that character |
| Drag | Select |
| Double-click | Select the word |
| Shift+click | Extend the selection to there |
| Wheel | Scroll |
| `Ctrl` + wheel | Zoom |
| Drag a ruler marker | Top half: the first-line indent. Bottom half: the left indent, which carries the first line with it |

The status bar shows the page count and the zoom while you are in this view.

---

## Where things are kept

- Notes, settings, recent files and the session: `%APPDATA%\OpenNote\opennote.db`
- Nothing else. No registry keys beyond file associations set by the installer, and
  nothing written next to the executable.

Sync credentials, if you set any up, live in the same database — the OAuth secret and
any access token wrapped with DPAPI, which ties them to your Windows account on this
machine. See [cloud-sync-setup.md](cloud-sync-setup.md).
