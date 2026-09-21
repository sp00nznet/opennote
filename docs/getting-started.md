# Getting started

opennote is one executable. Download it, run it, type. Nothing to install, no account, no
service running when it is closed. This page is the twenty minutes after that.

If you want the background — what this is, where it came from, what it is trying to be —
read [overview.md](overview.md). If you want the full list of keys, that is
[controls.md](controls.md).

---

## Installing

Two downloads on the [releases page](https://github.com/sp00nznet/opennote/releases/latest):

- **`opennote.exe`** — the bare executable. Put it wherever you like and run it. It writes
  one file, `%APPDATA%\OpenNote\opennote.db`, and nothing else.
- **The installer** — the same executable, plus Start menu entries and file associations
  for `.txt`, `.rtf` and `.docx` if you want them.

Windows 10 or 11, 64-bit. There is no .NET, no Visual C++ redistributable, no runtime of
any kind to chase down. Building it yourself is in [BUILDING.md](../BUILDING.md).

The first run has nothing to configure. There is no setup wizard because there is no
setup.

---

## The first document

`Ctrl+N` opens a new tab holding a plain text document — the Notepad case. Type, `Ctrl+S`,
done.

**File → New Rich Text Document** opens a formatted one instead — the WordPad case. This
is the one with fonts, bold, colours, alignment, lists, indents, pictures and tables.

The difference is only what the file can hold. A plain `.txt` has nowhere to keep a font,
so the formatting half of the toolbar greys out. Save a plain document **As** `.rtf` or
`.docx` and the formatting comes back on — nothing is lost in the other direction either,
because there was nothing there to lose.

**Opening** is `Ctrl+O`, and opennote reads:

| Format | What happens |
|--------|--------------|
| `.txt`, `.md`, source files | Plain text, with syntax highlighting where it recognises the extension |
| `.rtf` | Rich text — fonts, colours, lists, pictures, tables |
| `.docx` | Word documents — styles, numbering, pictures, headers and footers, footnotes, tables, tracked changes, comments, fields |
| `.pdf` | Shown, a page at a time. Read-only for now |
| anything else | As text |

Everything it opens, it saves back in the same format.

Each document gets a tab. `Ctrl+W` closes one, the `+` at the end of the strip adds one,
and **Settings → Default Settings…** can have the whole set come back next time you start.

---

## Seeing the pages

`Ctrl+Shift+L`, or the page button on the toolbar, or **File → Print Preview** — all the
same thing. The tab stops flowing the document into the window and lays it out on paper
instead: the page size and margins from **File → Page Setup**, real page breaks, headers
and footers, footnotes at the foot of the page that referred to them, tables with the
column widths the file states.

**You can edit there.** It is not a preview you have to leave to change something. Click
to put the caret somewhere, type, select with the mouse or `Shift`+arrows, `Ctrl+Z` to
undo. The arrows walk the lines you can see rather than the invisible structure behind
them, `Page Up` and `Page Down` move by an actual page, and the ruler's markers set the
paragraph's indents.

`Esc` goes back to the flowing view. Each tab remembers which view it is in, so one
document can be on its pages while the next is a wall of text.

This works on a plain text file too: one paragraph per line, in the editor's font. That
is all printing a note from Notepad has ever been.

---

## Printing, and PDF

`Ctrl+P` prints. The pages that come out are the pages the layout view showed you — the
same engine measures both, so they cannot disagree.

**File → Export to PDF…** writes a PDF. There is no PDF library in opennote; it goes
through Windows' own *Microsoft Print to PDF*, and what comes out is vector — the text in
it is still text, selectable and searchable, with the fonts embedded.

**File → Page Setup…** sets paper size, orientation and margins, and the layout view
follows it immediately.

**Opening a PDF** works too, as of v0.11: it gets a tab of its own and shows its pages,
fitted to the window, `Page Up` and `Page Down` by a page, `Ctrl`+wheel to zoom. It is
read-only — filling in a form and signing one are what comes next.

---

## Working on someone else's document

A document that has been through a review carries more than its text, and opennote keeps
all of it.

**Tracked changes** arrive intact. What you see is the document as it will read once the
changes are accepted; the deleted text is still in the file, and it is still there when
you save. **Review → Accept All Changes** and **Reject All Changes** settle them.

**Comments** arrive too. **Review → Comments…** lists them — who said what, and when —
and **Review → New Comment…** adds one to the paragraph you are in.

**Insert → Page Numbers** puts "Page N of M" in the footer, and **Insert → Table of
Contents** builds one from the document's headings. Both are fields: the numbers are
worked out each time the document is laid out, so they stay right when the document
changes underneath them rather than describing where things used to be.

---

## Notes

Alongside files there is a note store: one SQLite database at
`%APPDATA%\OpenNote\opennote.db`, holding notes that have a title rather than a path.

**File → Browse Notes…** (or **View → Notes Browser…**) lists them, searches them, and
makes, renames and deletes them. A note opens in a tab like anything else.

Use whichever suits: files for things that belong in a folder, notes for the ones that
never deserved a filename.

---

## Cloud sync (optional)

opennote ships **no API keys**. Sync is off until you give it credentials for an OAuth
application you registered yourself — your GitHub account, your Google project, your quota,
your revoke button.

**Settings → Default Settings… → Sync Accounts… → Credentials…** is where they go, and the
dialog's **Setup Guide** button opens [cloud-sync-setup.md](cloud-sync-setup.md), which
walks through registering the app with each provider. Notes then sync to a private GitHub
gist or a folder in your Google Drive.

Nothing is sent anywhere until you do this, and there is no opennote server to send it to
in any case.

---

## Making it yours

**Settings → Default Settings…** holds the few things worth setting:

- Default font size, and light or dark theme
- Minimise to the system tray
- Save the session on exit, and restore it on startup
- The sync accounts described above

**View → Toolbar** and **View → Status Bar** hide either one, and both are remembered.
**Format → Word Wrap** and **Format → Tab Size** apply to plain text; **Format → Font…**
sets the editor font.

---

## What next

- [controls.md](controls.md) — every key and gesture, including the page layout view's own
- [overview.md](overview.md) — what opennote is like WordPad, what it is like Word, and
  where it is going
- [ROADMAP.md](../ROADMAP.md) — the ordering, version by version
- [SECURITY.md](../SECURITY.md) — the known issues, stated rather than buried

Problems go to the [issue tracker](https://github.com/sp00nznet/opennote/issues). A
document that does not come through correctly is the most useful kind of report: attach
it if you can.
