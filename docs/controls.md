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

## Insert

Everything here belongs to a rich text document — a `.txt` has nowhere to keep it.

| | What it does |
|---|--------------|
| **Insert → Picture…** | A picture at the caret |
| **Insert → Page Numbers** | A centred "Page N of M" footer. They are fields, so they are right after you add a page rather than right when you typed them |
| **Insert → Table of Contents** | Built from the document's headings, at the top. Each entry points at its heading, so the page numbers follow the document. Run it again after editing and the old one is replaced rather than added to |
| **Insert → Date Field** | A date that updates when the document is laid out. `F5` inserts today's date as plain text instead, which is what Notepad does |

## Review

| | What it does |
|---|--------------|
| **Review → New Comment…** | A comment on the paragraph you are in — the page view's caret when that is the view, the text view's selection otherwise. It is signed with your Windows account name |
| **Review → Comments…** | Every comment in the document, with who wrote it and when. Delete removes one and its markers |
| **Review → Accept All Changes** | Tracked deletions go, tracked insertions become ordinary text. Nothing on screen changes, because that is already what was being shown |
| **Review → Reject All Changes** | The other way round: deleted text comes back and inserted text goes |

A document with tracked changes reads as it will once they are accepted. Deleted text is
carried, not shown — it is in the file, and it goes back into the file when you save.
There is no mode that shows insertions underlined and deletions struck through yet.

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

## The PDF view

A `.pdf` opens in a tab of its own and is **shown, not edited** — filling in a form and
signing one are the next thing along, and the commands that would change the document say
so rather than half-working.

| Key | What it does |
|-----|--------------|
| `↑` `↓` | Scroll a little |
| `Page Up` / `Page Down` | A page |
| `Home` / `End` | The first page / the last |
| Wheel | Scroll |
| `Ctrl` + wheel | Zoom |
| `Ctrl+0` | Fit the page across the window, which is how it opens |

**File → Sign PDF…** puts your signature on a page. The first time, it opens a box to
sign in with the mouse — tick "keep it for next time" and it is saved, so every form
after that is two clicks. Then drag a box where the signature goes (or click to drop one
at a sensible size). It is written to a
copy, with the original bytes untouched underneath. This is a *visible* signature — what
it proves is what ink on paper proves, and no more.

**File → Sign PDF with a Certificate…** is the other kind of signature: it signs the
bytes with a certificate you hold the private key for, so that a change to the document
afterwards can be detected. A signed PDF says so in the status bar when it opens — and
says whether its bytes still match. That is all it says: whether the certificate is one
anybody should trust is a separate question, and opennote does not answer it.

**Use answers**, in the fill dialog, puts back what you told a previous form — matched on
the field's name with case and punctuation ignored, so `Full Name` and `full_name` are the
same question. **Remember these** keeps this form's answers for the next one.
**Settings → Remembered Answers** shows everything kept, and forgets it.

**File → Fill PDF Form…** lists the fields the PDF carries — boxes to type in, boxes to
tick and lists to choose from. Double-click one to fill it in (a tick box turns over; a
list tells you what it will take), then **Save As** writes the filled copy — the original file is not changed, and the
copy is the original with the answers appended, which is how PDF is meant to be edited.
Buttons that run something when they are clicked are listed but not filled.

The status bar shows the page count and the zoom.

---

## Where things are kept

- Notes, settings, recent files and the session: `%APPDATA%\OpenNote\opennote.db`, or
  wherever `OPENNOTE_DB` points if it is set — which is how a copy run off a stick keeps
  its notes beside itself
- Nothing else. No registry keys beyond file associations set by the installer, and
  nothing written next to the executable.

Sync credentials, if you set any up, live in the same database — the OAuth secret and
any access token wrapped with DPAPI, which ties them to your Windows account on this
machine. See [cloud-sync-setup.md](cloud-sync-setup.md).
