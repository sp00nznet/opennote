# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **A document group on the toolbar** — New, Open, Save and Print, and a button that
  opens the page layout view. The bar used to appear only for a rich text document and
  hold nothing but formatting; those four mean the same thing whatever is being edited,
  so the bar now stays and the formatting half greys out on a plain text file rather than
  vanishing and shuffling everything along.
- **View → Toolbar**, which hides it entirely and is remembered between runs, the way the
  status bar already was.

### Changed
- **A new icon.** The old one was a leather notebook and a pen, painted at 256 pixels;
  at the size an icon is actually seen -- 16 pixels, in the title bar and the taskbar --
  it was a brown blob. The new one is a page with a folded corner, a heading and two
  lines of text: flat colour, hard edges, and drawn by `tools/make_icon.py` so it can be
  changed by editing numbers rather than repainted. Below 20 pixels it drops to a heading
  and one line, because three bars in sixteen pixels is a smudge.
- **The product is written "opennote"** — lower case, matching the repository. The title
  bar, every message box, the About box, the Help menu, the version resource and the
  installer's display name all say it that way now. The executable, the `%APPDATA%`
  directory and the registry entries keep their capitals: renaming those would orphan
  every existing installation's database.
- **The About box** says what this program is -- it still described "a tabbed text editor
  with SQLite note storage" -- and links to the repository and the releases page. Its
  version number now comes from `APP_VERSION` rather than being typed into the dialog and
  left behind at the next release.

### Added
- **A screen for your own OAuth credentials** — Settings → Default Settings… → Sync
  Accounts… → Credentials…. OpenNote ships no API keys and never has: an installed copy
  had no way to be given any, so cloud sync could only be enabled by building from source.
  Client IDs are stored as they are, because they are public identifiers; the Google
  client secret is wrapped with DPAPI like an access token. The sign-in buttons follow
  the credentials rather than being offered and then failing.

### Fixed
- **A stored credential that cannot be unwrapped is now deleted rather than left.** A
  token written by a different Windows account, or left in the clear by a build from
  before v0.2.0, could not be read and so could not be used — but it stayed in the
  database indefinitely, which for a cleartext token is the whole of the vulnerability
  v0.2.0 set out to remove. See SECURITY.md: revoke any token from such a build.

### Changed
- `docs/cloud-sync-setup.md` rewritten: it had been telling people to pass client secrets
  to CMake, which fails the build on purpose, and to register a Google *Web application*
  client, which PKCE with a loopback redirect does not want.

## [0.9.0] - 2026-09-20

**Document fidelity.** The reader used to guess at a good deal of what a `.docx` says: a
heading was recognised by its style being called "Heading1", every list was assumed to be
bulleted, pictures were dropped, the page setup was ignored, and headers, footers and
footnotes did not exist. All of it is read now, laid out, and written back out.

The measure of this version is a number rather than a claim: **51158/51158 properties
survive a document going through the model and back to a file**, pictures compared byte
for byte, across a corpus of ten documents and 196 conformance checks.

### Added
- **`styles.xml`** - document defaults, the style table, and `basedOn` resolved to the
  root, so a style based on a style based on Normal comes out with what each of them
  stated. Paragraphs carry the resolved properties; the table is kept beside them so a
  document written back out says "Quote" where it said "Quote".
- **`numbering.xml`** - `w:numId` to abstract numbering to level, which is where the
  format and the marker pattern live. The engine counts: a list restarts when another
  begins, a deeper level starts again under each parent, letters run a, b, c and then aa,
  roman numerals work, and "%1.%2)" names both levels.
- **Pictures**, in both spellings: DrawingML and VML. The bytes go into the model exactly
  as the file had them -- a PNG stays a PNG -- so a round trip cannot re-encode them. The
  layout engine places them, and the page view, the printer and the PDF all draw them
  through WIC.
- **The page a document is set on**: paper size, orientation, margins and columns, from
  `w:sectPr`, which the reader had been ignoring entirely. A page is a row of columns
  now; the one-column case is the same code with the loop running once.
- **Page breaks**, both spellings: `w:pageBreakBefore` on a paragraph and a
  `w:br w:type="page"` run.
- **Headers and footers**, read from their own parts and placed on every page once
  pagination has decided how many there are.
- **Footnotes and endnotes.** A footnote goes at the foot of the page its reference
  landed on: the page owes it the room, and a paragraph that no longer fits because of
  its own footnote is taken back off the page and laid out again on the next one.
  Endnotes go after everything else. The marks are numbered from the order the references
  appear in, which is where they come from in a `.docx` too -- the file does not hold
  them.
- **`src/core/imagedib.c`**, **`src/layout/layoutimage.cpp`** and **`src/ui/richole.c`**:
  decoding a picture for the view, for the page, and giving the control somewhere to keep
  one.
- Five new corpus fixtures -- styles and numbering, pictures, the page setup, headers and
  footers, notes -- and self-checks for the counting, the columns, the page breaks, the
  picture placement and the footnote placement.

### Fixed
- **`SB_AddF` truncated anything longer than 512 characters without a word**, which cut a
  picture's drawing in half and produced XML no reader would accept.
- A list paragraph's left indent came back 360 twips short, because the control reports
  where the first line starts rather than where the paragraph does.
- Two lists in one document were written out as one, so a bulleted list following a
  numbered one rewrote the numbered one's format.
- A model captured from the editor now takes its page, its margins, its header, its
  footer, its pictures and its notes from the document it was loaded from. The control
  holds none of those, so without it, saving an A4 document from the editor put it
  quietly onto Letter and dropped everything else.

### Known issues
- Three things the RichEdit view cannot hold, so the editor path loses them and the
  harness names them on every run: a paragraph's style and heading level, a lettered or
  roman list (which comes back numbered -- the control's RTF reader takes `\pndec` and
  nothing else), and a page-break run in the middle of a paragraph.
- A picture in a paragraph whose text was edited in that view is lost on the way back
  out: the control displays a picture and then will not say where it is, so a picture is
  matched to the paragraph it came from and an edited paragraph has nowhere to put it.
- One page setup per document: a document with several sections keeps the last one, and
  the section breaks become page breaks.
- One header and one footer: not a different first page, not different left and right
  pages. Both are per-section.
- Tab stops are every half inch for the whole document; per-paragraph `w:tabs` are not
  carried.
- Nested tables still read as further rows of the outer table.
- Fields -- page numbers, dates, cross-references -- are not read, so a footer that says
  "Page 1 of 12" arrives as whatever text was last saved with it. That is v0.10.

## [0.8.0] - 2026-09-20

**The layout engine.** The document is laid out onto pages - paragraphs measured and
flowed, broken at a line when they do not fit, tables boxed - and the result is drawn to
the screen, to a printer and to a PDF from one set of geometry. The page view is also
where a document can be edited: a click names a character, the arrows walk lines rather
than runs, and every keystroke changes the model and lays it out again.

The engine knows nothing about windows and nothing about Direct2D. It answers where
everything goes; drawing it is somebody else's job. That is what lets one layout serve
the screen, the printer and the exported PDF, and what makes it checkable without a
screen.

### Added
- **`src/layout/layout.cpp`** - the engine. `IDWriteTextLayout` shapes, breaks and
  justifies one paragraph at a time; this flows those paragraphs down a page, splits one
  across a page boundary at a line, places tables with the column widths the document's
  `w:tblGrid` states, honours left indents, and keeps widows and orphans off the page: a
  broken paragraph leaves at least two lines on each side of the break, or moves to the
  next page whole.
- **Geometry for editing** - `Layout_HitTest`, `Layout_PosRect`, `Layout_RangeRects`,
  `Layout_MoveLine` and `Layout_LineEdge`. A page of text becomes an editor only when a
  point can name a character and a character can name a point, and both of those are
  questions about the laid-out page rather than about the tree.
- **Editing the model** - `DocEdit_Insert`, `DocEdit_SplitPara`, `DocEdit_DeleteRange`,
  `DocEdit_RangeText` and `Doc_Clone`, with positions measured in the same offsets the
  engine flattens a paragraph to, so the two cannot disagree about where anything is.
- **`src/ui/pageview.cpp`** - the page view: real pages with real margins, drawn with
  Direct2D and edited in place. Caret and selection, click and drag, double click for a
  word, typing, Enter, Backspace and Delete, cut, copy and paste, select all, undo and
  redo, Ctrl+wheel zoom, fit to width, Ctrl+P to print and Ctrl+S to save. Edits go back
  to the document when the window closes. View > Page Layout, or Ctrl+Shift+L.
- **A ruler**, which needed a page width to mean anything: margins, half inch tab stops,
  and indent markers that can be dragged - one undo step per drag, applied to every
  paragraph the selection touches.
- **`src/layout/layoutprint.cpp`** - printing, and **export to PDF** (File > Export to
  PDF). Windows' own PDF printer does the writing, so there is no PDF library here; the
  output is vector, with the text still text and the fonts subset into the file.
- **`--layout-report <file.docx>`** prints a document's pagination: page size, page
  count, and per page how much was placed and how far down the page it reached.
- **`--export-pdf <file.docx> <out.pdf>`** does the export without a window.
- Checks for all of it. `--selftest` gains the engine's invariants - nothing below the
  bottom margin, a paragraph longer than a page split rather than dropped, a left indent
  narrowing the column, Page Setup reaching the page size, a position and a point
  agreeing about where a character is, and no broken paragraph leaving a line stranded -
  and the editing operations get their own: typing inherits the right formatting,
  splitting keeps the paragraph's shape, deleting across paragraphs merges them, a range
  running into a table is refused rather than half done, and a clone is equal and
  separate. The conformance harness asserts the margin invariant and "every cell in the
  model reached a page" over the whole corpus.
- **Page Setup now reaches the engine.** Paper size and margins chosen in the dialog
  become the page a new model starts on, so the page view, the printout, the PDF and a
  saved `.docx` all agree with it. Previously only the printed page used them.

### Fixed
- Printing measured from the printable area rather than from the paper, which shifted
  every margin by the strip the printer cannot reach.

### Known issues
- The page view is a second view, not the only one: it edits a model captured from the
  rich text view and writes it back through RTF when it closes. Typing in the main
  window still goes through RichEdit, and the two are not live linked - if the document
  changed in the main window while the page view was open, it asks before applying.
  Making the laid out view the only view is v0.9.
- No character formatting in the page view: no bold, no font, no colour. Those commands
  still belong to the rich text view.
- Tab stops are every half inch for the whole document. Per paragraph stops are a
  `w:tabs` list the model does not carry yet.
- No first line indent in the engine (DirectWrite lays out one rectangle; a first line
  set differently needs its own), table rows do not split across a page boundary, and
  merged cells are not handled.
- Undo steps back one character at a time; consecutive typing is not grouped.
- A document holding a picture still prints through the control rather than the engine:
  the model carries no images until v0.9, and dropping a picture off a printout is a
  worse trade than losing the engine's pagination.
- Paper size and orientation come from Page Setup rather than from the print dialog's
  own paper list: the engine lays the document out before a printer is chosen.

## [0.7.0] - 2026-09-20

**The document model.** A document is now a tree rather than whatever the editor control
happens to be holding, and every file format is a serializer over that one structure.
This ships no new feature on purpose; it is what the layout engine in v0.8 needs to exist
before it can be written, and it fixes a real defect on the way.

### Added
- **`src/core/doctree.*`** - sections, paragraphs, runs, tables, rows and cells, with
  character and paragraph properties on each.
- **`Doc_Compare`** - compares two models property by property and reports what did not
  survive. Fidelity counts what the *source stated* and the round trip lost; a default
  the reader resolved is not a loss, which is the distinction that makes the number mean
  anything.
- **`src/core/doctree_view.c`** - captures the editor's contents back into a model,
  including table structure, which the control does expose (PFE_TABLE, U+FFF9/U+FFFB row
  markers, BEL between cells).
- **`src/core/doctree_rtf.c`** - the only place RTF is now generated.
- **`src/core/strbuf.*`** - the growable string builder, previously private to docx.c.
- Fidelity reporting in the conformance harness, split between the serializer and the
  editor so a regression says which half broke:
  `model fidelity: 2445/2445` and `editor fidelity: 2440/2442`.
- A self-check for `Doc_Compare` itself, which deliberately breaks a model and requires
  each change to be detected. The fidelity numbers are worthless if the comparison cannot
  see a change.

### Fixed
- **Tables survive being saved to `.docx`.** They previously flattened to tab-separated
  text, because the reader produced RTF and the writer walked the control - there was
  nowhere to keep a table in between. Measured consequence: the serializer now loses
  nothing at all, and the editor path loses two named properties instead of every table
  in the document.
- A row's trailing cell separator produced a phantom empty cell, so every captured table
  had one column too many.

### Changed
- `.docx` reading and writing go through the model. `Docx_ReadToRtf` is now a convenience
  over `Docx_ReadToModel` plus `DocRtf_Emit`.
- `_RICHEDIT_VER` raised to 0x0800, which exposes EM_GETTABLEPARMS. Safe because this
  targets Windows 10 and 11, where the control behind MSFTEDIT_CLASS is RichEdit 8
  regardless of the 4.1-era class name.
- `tests/validate_docx.py` checks every package the writer produced, not only the ones
  that came through the editor.

### Known issues
- The editor loses a paragraph's heading *level* (RichEdit has no named styles; the
  appearance survives) and a table's column widths (the control does not return them).
  Both are named by the harness on every run, and both are fixed by v0.8 and v0.9.
- `.rtf` files still load and save through the control directly rather than through the
  model. That path is already lossless in both directions - RichEdit is both the reader
  and the writer - so replacing it with an RTF parser would be work for no fidelity gain.
- Nested tables are read as further rows of the outer table rather than as nested ones.

## [0.6.1] - 2026-09-18

### Fixed
- The `.docx` conformance harness wrote its round-trip output into the directory it was
  enumerating, which makes `FindNextFile`'s behaviour undefined. Depending on the run it
  processed a different number of documents, and on a second run it would treat its own
  output as corpus input. It now lists the corpus before writing anything and puts the
  output in a separate `out` directory, so repeat runs give the same count.

  Found by running the published v0.6.0 binary against a freshly generated corpus, which
  reported fewer documents than the same binary did locally. A harness that quietly
  changes what it checks is worth less than no harness.

## [0.6.0] - 2026-09-18

**Word documents.** OpenNote reads and writes `.docx` — ECMA-376 WordprocessingML —
with no new dependency, because Windows already ships an API for Open Packaging
Conventions containers and a pull XML reader.

### Added
- **`.docx` reading.** The package is opened with `IOpcFactory` and the main document
  part located through its `officeDocument` relationship rather than a hardcoded path.
  `IXmlReader` walks the XML. Supported: character formatting (bold, italic, underline,
  strikethrough, colour, size, font, super/subscript), paragraph formatting (alignment,
  indent, spacing, lists), heading styles, tables with cell widths taken from
  `w:tblGrid`, tabs, line breaks, and Unicode.
- **Tracked deletions are honoured** — content inside `w:del` is revision history and
  does not appear in the document.
- **`.docx` writing.** The document is read back out of the rich text view and emitted as
  WordprocessingML into an OPC package. The Save As dialog's file type decides whether a
  rich document is stored as `.rtf` or `.docx`.
- **A conformance harness**, per the house rules. `OpenNote.exe --docx-check <dir>`
  converts every document in a corpus, checks the assertions in its `.expect` sidecar,
  writes it back out as `.docx`, re-reads it and asserts no text was lost. It reports a
  pass/fail count and exits non-zero on regression. CI runs it on every build; the
  current figure is in the README.
- **The corpus is generated, not committed** — `tests/make_fixtures.py` builds it, so the
  repository carries no binary Office documents.
- **`tests/validate_docx.py`** checks the packages the writer produced using Python's
  zipfile and XML parser, so a file only OpenNote's own reader accepts does not pass.
- `OpenNote.exe --docx2rtf <file.docx>` prints the converted RTF, for looking at when a
  document comes out wrong.

### Fixed
- The writer emitted RichEdit's internal table markers (U+FFF9..U+FFFC) and cell
  separators into the document as literal text. They are structure, not content; cell
  breaks now become tabs and row breaks become line breaks.
- A `Heading` style lost its weight and size the moment the paragraph's first run opened,
  because runs reset their formatting to nothing rather than to what the paragraph style
  implied.
- Table cells carried a paragraph mark as well as their cell terminator, leaving a blank
  line in every cell. Cell edges were also emitted after the row's content instead of
  before it, and ignored `w:tblGrid`.
- `Rich_EnsureLoaded` is now public. The `.docx` self-check created a RichEdit control
  directly and only worked because an earlier check happened to have loaded the library
  first; the conformance harness, which runs on its own, did not. It silently skipped its
  round-trip checks rather than reporting them — a harness that skips quietly is worse
  than one that fails.

### Known issues
- Tables read correctly but flatten to tab-separated text when written back to `.docx`.
  Rebuilding the grid on write needs the table model the layout engine brings in v0.7.
  Text is never lost, and the harness enforces that.
- Numbered lists are read as bulleted. Which marker a list uses lives in `numbering.xml`
  behind two levels of indirection, which is not yet followed.
- Styles are not resolved from `styles.xml`; heading levels are given the shape readers
  expect rather than the document's own definition.
- Images inside a `.docx` are not yet read.
- Legacy binary `.doc` is a different format ([MS-DOC] over [MS-CFB]) and is not claimed.

## [0.5.0] - 2026-09-18

**The WordPad replacement.** Windows 11 24H2 removed WordPad; this is the milestone that
answers it. OpenNote now opens, edits, prints and saves rich text documents.

### Added
- **Rich text documents.** `.rtf` files open in a RichEdit 4.1 view alongside the existing
  plain text view, load and save through `EM_STREAMIN`/`EM_STREAMOUT`, and are offered in
  the Open and Save dialogs. **File > New Rich Text Document** starts an empty one.
- **Character formatting**: font family and size, text colour, bold, italic, underline,
  strikethrough, superscript and subscript.
- **Paragraph formatting**: left/centre/right/justified alignment, single, 1.5 and double
  line spacing, bulleted and numbered lists, and increase/decrease indent.
- **Formatting toolbar**, shown only while a rich text tab is active, with font and size
  combos that follow the caret. Its buttons are drawn rather than loaded from a bitmap, so
  they follow the system text colour.
- **Insert picture** (uncompressed bitmaps), embedded as RTF so the image survives being
  opened in other readers.
- **Page Setup**, and **printing that pages properly** — the rich view renders through
  `EM_FORMATRANGE` across as many sheets as the document needs, with margins measured from
  the paper edge rather than from wherever the driver starts.
- `Ctrl+B`, `Ctrl+I` and `Ctrl+U` accelerators. **Clear Formatting** on the Format menu.
- Self-check coverage for the rich view, run by CI: RTF round trips through both a string
  and a real file with bold, italic and font size intact; alignment and bullets round trip;
  replace-all terminates when the replacement contains the search term; word lookup
  reports correct bounds.

### Changed
- `Editor_*` now dispatches between the two views, so everything outside `editor.c` keeps
  calling the same functions with an `HWND` and does not know which control is behind it.
- Opening a document reuses the current tab only when that tab's view matches the
  document's format. Four copies of that logic became one function; previously an `.rtf`
  dropped onto a plain tab would have shown its markup.
- The editor font, word wrap and tab size settings apply to the plain text view only. A
  rich document carries its own fonts, and forcing the code font over it flattened the
  document. New rich documents start on Calibri 11, as WordPad did.

### Known issues
- The toolbar's font and size combos show their value highlighted until first clicked.
  Cosmetic only — the values are correct and editing works.
- The rich view always wraps to the window. Turning wrapping off needs a page width to
  wrap to instead, which arrives with pagination in v0.6.
- Tables are weak and the view flows rather than showing page boundaries. Both are
  RichEdit's limits, and both are lifted by the layout engine `.docx` requires — see
  `ROADMAP.md`.

## [Unreleased]

### Added
- `LICENSE` (MIT), `CHANGELOG.md`, `ROADMAP.md`, `SECURITY.md`, `CONTRIBUTING.md`.
- GitHub Actions pipeline (`.github/workflows/ci.yml`): builds on every push and pull
  request to `main`, publishes a GitHub Release with both the bare executable and the
  installer on a `v*` tag.
- Installer now registers OpenNote as an *additional* handler for `.txt`, `.log`, `.md`
  and `.ini` rather than only adding a generic "Open with" verb, and no longer seizes
  the default association for any extension.

### Changed
- Releases are built by CI and published on GitHub Releases. Downloads no longer come
  from a personal host. No binaries will be published until v0.5.0; the pipeline only
  fires on a `v*` tag and none will be cut before then.
- Installer is per-user by default and no longer requires administrator rights; an
  all-users install is still available from the elevation prompt.
- Installer version is supplied by the build rather than hardcoded.
- Google Drive sync now needs a client ID and secret from the user's own Google Cloud
  project, entered in Settings. Google issues no credential that a public client can
  safely ship, so the alternative was shipping one anyway.

### Changed
- Every editing operation now goes through the `Editor_*` API. `mainwindow.c` no longer
  sends Scintilla messages directly; `Editor_HasIndicatorAt`, `Editor_ReplaceRange`,
  `Editor_ClearSpellIndicatorRange` and `Editor_GetWordAt` cover what it was reaching for.
  The only Scintilla-specific code left outside `editor.c` is the notification dispatch.

### Fixed
- Startup no longer fails when `Msftedit.dll` is unavailable. The RichEdit control has
  been unused since the Scintilla port; the library was still being loaded, and a load
  failure aborted the application for no reason.

### Removed
- GitLab CI configuration, which carried an internal host address and share credentials
  in its deploy stage.

### Security
- **OAuth client secrets are no longer compiled into the binary.** GitHub now uses the
  device authorization grant (RFC 8628), which needs only a public client ID. Google uses
  PKCE (RFC 7636) with the user's own Google Cloud credentials, stored encrypted. The
  build now fails outright if `GH_OAUTH_CLIENT_SECRET` or `GOOGLE_CLIENT_SECRET` is
  passed, so it cannot regress. CI consequently needs no secrets, and a published binary
  is reproducible from its tagged commit.
- **Access tokens are wrapped with DPAPI** (`CryptProtectData`) before reaching the
  database, tying them to the Windows account. Anyone who connected an account before
  v0.2.0 should revoke that token and reconnect — the old value was written in cleartext.
- **Settings access binds its parameters.** `Database_SetSetting` / `Database_GetSetting`
  replace the formatted `INSERT OR REPLACE` the token path used.
- `OpenNote.exe --selftest` checks the PKCE S256 challenge against the RFC 7636 test
  vector, the DPAPI round trip, and the JSON field reader. CI runs it on every build.
