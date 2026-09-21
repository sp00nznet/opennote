# OpenNote

```
   ____                   _   _       _
  / __ \                 | \ | |     | |
 | |  | |_ __   ___ _ __ |  \| | ___ | |_ ___
 | |  | | '_ \ / _ \ '_ \| . ` |/ _ \| __/ _ \
 | |__| | |_) |  __/ | | | |\  | (_) | ||  __/
  \____/| .__/ \___|_| |_|_| \_|\___/ \__\___|
        | |
        |_|
```

[![Build](https://github.com/sp00nznet/opennote/actions/workflows/ci.yml/badge.svg)](https://github.com/sp00nznet/opennote/actions/workflows/ci.yml)
[![Platform](https://img.shields.io/badge/platform-Windows-blue.svg)](https://github.com/sp00nznet/opennote)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C-orange.svg)](https://en.wikipedia.org/wiki/C_(programming_language))

A tabbed Windows editor with a local SQLite note store behind it. Native C and Win32 —
one executable, a few megabytes, no runtime to install and nothing running when it is
closed.

Sibling project to [futureburn](https://github.com/sp00nznet/futureburn),
[pstfree](https://github.com/sp00nznet/pstfree),
[vncfree](https://github.com/sp00nznet/vncfree) and
[bulkhead](https://github.com/sp00nznet/bulkhead) — same attitude: find the Windows
payware, read the published spec it is hiding behind, give it away.

## Status

**v0.7.0 — alpha. Reads and writes `.docx`, through a real document model.** Downloads are on the
[releases page](https://github.com/sp00nznet/opennote/releases/latest): a bare executable
and an installer, with the release notes saying what each one does and does not give you.

Three security issues are known and documented in [SECURITY.md](SECURITY.md). A build
made from source without the optional OAuth variables is not affected by any of them.

### Word documents

As of v0.6.0 OpenNote opens and saves **`.docx`** — ECMA-376 WordprocessingML.
Formatting, alignment, lists, indents, tables, tracked-deletion handling and Unicode all
come through. There is no new dependency: a `.docx` is an Open Packaging Conventions
container, and Windows ships the API for exactly that shape (`IOpcFactory`) alongside a
pull XML reader (`IXmlReader`). The container and the parser were never this project's
code to own.

**Conformance:** `79/79 checks across 5 documents`, and fidelity is measured rather than claimed — see
[Conformance](#conformance) below.

As of v0.7 a document is a real tree (sections, paragraphs, runs, tables, cells) rather
than whatever the editor control happens to be holding, and every format is a serializer
over it. **Tables now survive being saved back to `.docx`**, which they did not before.

### The WordPad replacement

**Windows shipped a rich text editor for thirty years and removed it.** WordPad is gone
from Windows 11 24H2 and Windows Server 2025. Microsoft's suggested replacement is a
subscription: Microsoft 365 is $70–100/yr, or $150 once for Office 2024.

As of v0.5.0 OpenNote opens, edits and saves `.rtf` — fonts, sizes, colours, bold, italic,
underline, strikethrough, super/subscript, alignment, line spacing, bullets, numbering,
indent, pictures — with a formatting toolbar, spell check, page setup and multi-page
printing. The whole application is a **3.6MB executable** with no runtime to install.

![Rich text editing](gfx/rich-text.png)

It is built on Windows' own RichEdit control, which is how WordPad itself worked. That
has a known ceiling: tables are weak and the view flows rather than showing page
boundaries. The layout engine lifts both — see [ROADMAP.md](ROADMAP.md).

### Page layout

A document is laid out onto pages rather than flowed into a window: paragraphs measured
and broken at a line when they do not fit, tables boxed with the column widths the file
states, pages sized by Page Setup. `IDWriteTextLayout` does the shaping, line breaking
and font fallback; what OpenNote adds is the part above it, and it knows nothing about
windows — the same geometry draws the print preview, the printout and the PDF, so they
cannot disagree.

**Export to PDF** goes through Windows' own PDF printer, so there is no PDF library in
here; the output is vector and the text in it is still text.

The stretch is the part Word actually gets paid for: `.docx`, real page layout, `.doc`,
track changes. Everything needed for that is already in Windows and already paid for.
DirectWrite does the text shaping, `ISpellChecker` does spelling, Microsoft Print to PDF
does the export, and [MS-DOC], [MS-CFB] and ECMA-376 are all published specifications
anyone can download.

[LibreOffice](https://www.libreoffice.org) Writer is genuinely free, genuinely good, and
further along than this will be for a long time. Use it today. OpenNote exists because a
350MB office suite is not what most people wanted when they opened WordPad — and because
nothing free reads a legacy `.doc` well, now that the last in-box reader has left with it.

Likewise on the editor side: **VS Code and Notepad++ are free and excellent**, and this
does not compete with them. Syntax highlighting is here because Scintilla provides it, not
because anyone should switch.

See [ROADMAP.md](ROADMAP.md) for the ordering.

---

## Screenshots

| Main Editor | Notes Browser |
|-------------|---------------|
| ![Main Editor](gfx/main-editor.png) | ![Notes Browser](gfx/notes-browser.png) |

---

## Getting Started

From a clean machine:

1. Install **Visual Studio 2022** with the *Desktop development with C++* workload, and
   **CMake 3.16+** (the Visual Studio installer can provide both). Windows 10 or 11.

2. Clone and build:

   ```powershell
   git clone https://github.com/sp00nznet/opennote.git
   cd opennote
   cmake -B build -A x64
   cmake --build build --config Release
   ```

   SQLite and Scintilla are vendored in `lib/`. There is nothing else to fetch.

3. Run it:

   ```powershell
   .\build\bin\OpenNote.exe
   .\build\bin\OpenNote.exe --selftest   # check the crypto and RTF paths
   ```

   An empty tab opens. Your notes database is created at
   `%APPDATA%\OpenNote\opennote.db` the first time you save a note.

---

## Usage

```powershell
OpenNote.exe                 # empty tab
OpenNote.exe notes.txt       # open a file in a tab
OpenNote.exe report.rtf      # open a rich text document
OpenNote.exe report.docx     # open a Word document
OpenNote.exe --selftest      # run the built-in checks and exit
```

### What it does

| | |
|---|---|
| **Word documents** | `.docx` read and write — ECMA-376, via Windows' own packaging API |
| **Rich text** | `.rtf` documents: fonts, colours, alignment, lists, indent, pictures, printing |
| **Tabs** | Several documents at once, with the session restored on next launch |
| **Syntax highlighting** | Via Scintilla — 100+ languages |
| **Note store** | SQLite with FTS5 full-text search, browsable, import and export |
| **Compare** | Side-by-side diff between any two open documents |
| **Cross-tab search** | Find and replace across every open tab |
| **Shell integration** | Run selected text through CMD or PowerShell |
| **Cloud sync** | Optional, to your own GitHub or Google Drive. See the caveats below |

### Keyboard

| | | | |
|---|---|---|---|
| `Ctrl+N` | New file | `Ctrl+F` | Find |
| `Ctrl+O` | Open file | `Ctrl+H` | Replace |
| `Ctrl+S` | Save | `Ctrl+G` | Go to line |
| `Ctrl+W` | Close tab | `F3` | Find next |
| `Ctrl+Tab` | Next tab | `Ctrl++` | Zoom in |
| `Ctrl+Shift+Tab` | Previous tab | `Ctrl+-` | Zoom out |
| `Ctrl+B` | Bold | `Ctrl+I` | Italic |
| `Ctrl+U` | Underline | `Ctrl+P` | Print |

### Cloud sync

Off unless you connect an account, and it talks to GitHub or Google directly — there is
no server in between and this project does not operate one.

It is also the least finished part of the codebase. Tokens are currently stored in
cleartext and a released build would carry an OAuth client secret inside it. Both are
being fixed in v0.2.0; both are described in full in [SECURITY.md](SECURITY.md). A build
made without the optional `GH_OAUTH_CLIENT_ID` / `GOOGLE_CLIENT_ID` CMake variables has
no credentials in it and simply does not offer sync.

### Configuration

Everything lives in `%APPDATA%\OpenNote\opennote.db` — settings, notes and session. There
is no config file to edit and no registry key to find. Deleting that one file resets the
application completely.

---

## Building from source

Covered under [Getting Started](#getting-started) above; that is the only way to run it
right now. `cmake -B build && cmake --build build --config Release` is the whole thing.

Do not pass the OAuth CMake variables for a normal build — see
[CONTRIBUTING.md](CONTRIBUTING.md).

---

## Conformance

`.docx` handling is checked against a corpus on every build. The count is reported rather
than a bare "tests passed", and **fidelity is a number, not a claim**:

```
> OpenNote.exe --docx-check build/corpus
docx conformance: 79/79 checks across 5 documents
model fidelity:   2445/2445 properties survive .docx -> model -> .docx
editor fidelity:  2440/2442 properties survive a load, edit and save
```

Fidelity counts what the *source document stated* and the round trip failed to preserve.
A document that never named a font and comes back saying Calibri has lost nothing — the
reader resolved a default — so that is not counted. A font it did name and lost, is.

The two numbers are kept apart because conflating them hides which half broke: the first
is the serializer with no editor involved, the second is a real load-edit-save through
the control.

**The two properties the editor currently loses, both named by the harness:**

| Loss | Why | Fixed by |
|---|---|---|
| A paragraph's heading *level* | RichEdit has no notion of named styles, so `Heading1` comes back as bold 18pt body text. The appearance survives; the style does not | v0.9, `styles.xml` |
| A table's column widths | The control does not give them back, so the reader auto-sizes | v0.8, the layout engine's table model |

The corpus is **generated, not committed**, so the repository carries no binary Office
documents:

```powershell
python tests/make_fixtures.py build/corpus     # build the corpus
.\build\bin\OpenNote.exe --docx-check build/corpus
python tests/validate_docx.py build/corpus/out # check what the writer produced
```

`Doc_Compare` is itself covered by `--selftest`: it deliberately breaks a model and
requires each change to be noticed. A comparison that never reported anything would make
every fidelity number above a lie.

Each document has a `.expect` file listing what the converted RTF must and must not
contain — that a tracked deletion is absent, that a heading keeps its weight, that cell
edges come from `w:tblGrid`, that non-ASCII survives as `\uN`. Every document is also
written back out as `.docx` and re-read, asserting no text is lost, and
`tests/validate_docx.py` then checks those packages with Python rather than with
OpenNote's own reader.

`--docx2rtf <file.docx>` prints the converted RTF, which is the thing to look at when a
document comes out wrong. `--layout-report <file.docx>` prints where the layout engine
put everything — page size, page count, and per page how much it placed and how far down
the page it reached — and `--export-pdf <file.docx> <out.pdf>` writes the PDF without a
window.

---

## Project structure

```
src/ui/      Window, tabs, editors (plain + rich), page view, toolbar, dialogs
src/core/    Document model, serializers (.docx, RTF), file I/O, search
src/layout/  The layout engine: document model -> pages, and printing them
src/db/      SQLite, notes and links repositories
src/sync/    OAuth, GitHub and Google Drive sync
res/         Icons, dialogs, manifest
lib/         Vendored SQLite and Scintilla
```

See [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Contributing

Issues and pull requests welcome — [CONTRIBUTING.md](CONTRIBUTING.md). It also carries the
rule that matters most for the format work ahead: nothing derived from a proprietary
binary is ever committed.

## License

MIT — see [LICENSE](LICENSE).
