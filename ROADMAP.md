# Roadmap

Where this is going, in order, and what is deliberately not being built.

The short version: **Windows shipped a rich text editor for thirty years and removed it
in 2024.** WordPad is gone from Windows 11 24H2 and from Windows Server 2025. The
replacement Microsoft points you at is a subscription. OpenNote is aiming at that hole.

**v0.10.0.** Published binaries — a bare executable and an installer — are built by CI
from the tagged commit.

Nothing below is a promise of a date.

---

## v0.2.0 — Compliance and security

Housekeeping and the three security problems described in `SECURITY.md`. No user-visible
features.

- [x] `LICENSE`, `CHANGELOG.md`, `ROADMAP.md`, `SECURITY.md`, `CONTRIBUTING.md`
- [x] GitHub Actions replacing GitLab CI; releases built by CI, not a workstation, and
      held unpublished until v0.5.0
- [x] Remove personal-host references from the README and the installer
- [x] Remove the dead `Msftedit.dll` load that could abort startup for nothing
- [x] GitHub device flow and Google PKCE — no client secret in the shipped binary
- [x] DPAPI for token storage; bound parameters on all settings access
- [ ] Rename the repository to `opennote`
- [x] Route every editing operation through the `Editor_*` API (the Scintilla
      coupling turned out to be far thinner than feared — see below)

On that last item: the coupling was much thinner than a first look suggested. Scintilla
was never spread across `mainwindow.c`, `tabcontrol.c` and `editor.c` — it was 282 uses
inside `editor.c` and eleven leaks in `mainwindow.c`, with everything else already talking
through `Editor_*(HWND, ...)`, which is view-agnostic as it stands. The eleven leaks are
now closed, so the only Scintilla-specific code left outside `editor.c` is the
`SCNotification` dispatch — the control's notification protocol rather than an editing
operation, and the single place that needs a branch when a second view lands.

No dispatch layer has been built, deliberately. There is one view type; a factory for one
product is scaffolding. The seam is where it needs to be and the rest waits for v0.5.

## v0.3.0 — Notes vault

The notes half of OpenNote aimed squarely at what Evernote (~$130/yr, free tier capped at
50 notes), Obsidian Sync ($4–8/mo for sync and version history alone) and Standard Notes
(~$90/yr, where encryption *is* the paid tier) charge for.

- [ ] Encryption at rest — AES-GCM via CNG, key derived from a passphrase
- [ ] Version history for every note, in the same SQLite file
- [ ] Conflict-safe sync — content hashing and three-way merge, not last-writer-wins
- [ ] Export to a plain Markdown folder in one action, so the data is never hostage
- [ ] Harness: kill the process mid-sync across a corpus, assert zero note loss

## v0.4.0 — Editor tier

The part UltraEdit (~$80/yr), EmEditor ($40–80/yr) and Beyond Compare ($35–70) sell.

- [ ] Open multi-gigabyte files — memory-mapped, chunked, lazy line index
- [ ] Column/block selection and multiple carets
- [ ] Three-way diff and folder comparison
- [ ] Regex find-in-files across a directory tree
- [ ] Harness: fixed file corpus with open time and peak working set tracked in CI, and
      the current figures in the README

## v0.5.0 — The WordPad replacement — **done**

RTF is WordPad's native format, its specification (RTF 1.9.1) is published, and Windows'
in-box RichEdit control reads and writes it — which is essentially how WordPad itself
worked.

- [x] Rich text document type alongside the plain text one
- [x] RTF round-trip: character and paragraph formatting, lists, images
- [x] Formatting toolbar
- [x] Print and page setup, paging across as many sheets as the document needs
- [ ] Ruler and tab stops — deferred, they want the pagination v0.6 brings
- [ ] Spell check in the rich view — the `ISpellChecker` plumbing is shared, but the
      squiggle indicators are Scintilla's; the rich view needs its own

Built on RichEdit deliberately, to get a working replacement out while the removal is
still recent. RichEdit's ceiling is real — weak tables, no true pagination, and word wrap
that cannot be turned off without a page width to wrap to — and the upgrade path is the
DirectWrite engine in v0.6.0.

## v0.6.0 — `.docx` — **done**

This version originally bundled `.docx`, a layout engine and `.doc` together. That was
three separate efforts in one entry, so they are now three versions.

- [x] `.docx` reading — ECMA-376, via `IOpcFactory` and `IXmlReader`
- [x] `.docx` writing, with the Save As file type choosing the storage format
- [x] Conformance harness with a pass/fail count, run in CI, figure in the README
- [x] Corpus generated rather than committed
- [ ] `styles.xml` resolution, `numbering.xml` list markers, images

## The road to a Word replacement

Everything from here is one problem wearing several hats. Pagination, headers and
footers, footnotes, real tables, columns, print preview and faithful PDF export are not
seven features — they are seven things you cannot have until the document is laid out
onto pages, and right now nothing lays anything out. RichEdit flows text into a window.

So the ordering below is not a wish list in priority order. It is a dependency chain.

### v0.7.0 — The document model — **done**

**The unglamorous one, and the one that came first.**

Today a document *is* whatever the RichEdit control happens to be holding, and `.docx`
round-trips through RTF. That is why a table survives being read and then flattens when
it is written: there is nowhere to keep a table between the two.

A real tree — sections holding paragraphs and tables, tables holding rows and cells,
paragraphs holding runs — makes every file format a serializer over the same structure
and fixes the flattening as a side effect. It is also the thing the layout engine needs
to exist before it can be written, so building it first turns v0.8 from
"model *and* renderer" into "renderer".

- [x] Document tree: sections, paragraphs, runs, tables, with properties on each
- [x] `.docx` reader and writer against the model rather than through RTF
- [x] Round-trip harness reporting what was lost as a number, not a pass:
      **2445/2445** through the serializer, **2440/2442** through the editor
- [x] The editor's contents captured back into a model, tables included
- [ ] `.rtf` against the model. Deferred deliberately: that path already round-trips
      losslessly because RichEdit is both its reader and its writer, so writing an RTF
      parser to replace it would be work for no fidelity gain. It becomes worthwhile
      when the layout engine replaces the control.

The two properties the editor still loses are named by the harness on every run: a
paragraph's heading level, which RichEdit has no way to represent, and a table's column
widths, which it does not hand back. They are fixed by the two versions below, and until
then they are measured rather than assumed away.

### v0.8.0 — The layout engine — **done**

The big one. Windows supplies the hard half: `IDWriteTextLayout` does shaping, line
breaking, justification, bidi and font fallback. What had to be written is the part
above it — flowing those laid-out lines into columns and pages, and then letting
somebody edit the result.

- [x] Block layout: measure and flow paragraphs into a page, break, continue. A
      paragraph too tall for the space left is split at a line boundary and continues
      on the next page, however many that takes
- [x] Page model: paper size and margins, from the document or from Page Setup, which
      reaches the engine, the printer and the `.docx` writer at once
- [x] Widow and orphan control: a broken paragraph leaves at least two lines on each
      side of the break, or moves to the next page whole
- [x] **Table layout** — column widths from `w:tblGrid`, scaled to the page, cells
      measured and boxed. A row moves to the next page whole rather than splitting
      across the boundary, and merged cells are not handled yet. Honestly the nastiest
      part of the whole project, and this is the easy half of it
- [x] Rendering through Direct2D, to the screen and to a printer. Not through a printer
      DC: Direct2D refuses one outright, and its actual printing path is
      `ID2D1PrintControl`, which keeps the output vector rather than rasterising a page
- [x] Hit testing, caret movement and selection across the laid-out model. Routinely
      underestimated, and it was: a click names a character, the arrows walk lines
      rather than runs, Home and End mean the wrapped line, and Page Up and Page Down
      mean an actual page
- [x] Editing against the model, with undo. Typing, Enter, Backspace and Delete,
      selection, cut, copy and paste, and undo as a stack of model snapshots
- [x] Ruler and tab stops, which need a page width to mean anything: margins, half-inch
      tab stops and draggable indent markers
- [x] Print preview, which is just the page renderer in a window — and, now that the
      window edits, the same window
- [x] Print-to-PDF through the in-box printer, which turned out not to be free — but is
      still a printing path and not a PDF library, and the text in the file is text

The engine is deliberately free of any window and of Direct2D: it answers where
everything goes, and drawing it is somebody else's job. That is what lets one layout
serve the screen, the printer and the exported PDF, and it is what makes it checkable
without a screen — `--layout-report` prints the geometry, `--export-pdf` writes the
file, and the checks assert that nothing lands below the bottom margin, that no table
cell goes missing, that a click on a character and that character's own position agree,
and that no broken paragraph leaves a line stranded.

What v0.8 does *not* do is replace the editor. The page view edits a model captured
from the rich text view and writes it back through RTF when it closes; typing in the
main window still goes through RichEdit. Making the laid-out view the only view means
character formatting, spell check, images and find-and-replace all move across with it,
which is v0.9 work and is what finally retires Scintilla — roughly a thousand vendored
files leaving with it.

### v0.9.0 — Document fidelity — **done**

The things real `.docx` files contain that used to be dropped or approximated. The
measure of this version is a number: 51158/51158 properties survive a document going
through the model and back to a file, across ten documents and 196 conformance checks.

- [x] `styles.xml` — named styles, inheritance through `basedOn`, document defaults,
      instead of the reader's guess at what a heading ought to look like
- [x] `numbering.xml` — real numbered and multi-level lists that count: decimal, letters,
      roman numerals, and `1.2.` for a nested level, instead of reading every list as
      bulleted
- [x] Images: DrawingML and VML, read and written, carried as the bytes the file held so
      nothing is re-encoded on the way through. Drawn by the page view, the printer and
      the PDF
- [x] Headers and footers, placed once pagination has decided how many pages there are
- [x] Sections, page breaks and columns. One page setup per document: several sections
      keep the last one, and the section breaks become page breaks
- [x] Footnotes and endnotes. A footnote goes at the foot of the page its reference
      landed on, which means the page owes it the room before the text may use it

What none of this changes is where typing happens. The RichEdit view cannot hold a
picture's bytes, a style's name, a header or a note, so a document opened from a file
keeps its model beside the view and hands back what the view cannot say. That works, and
it is measured — the editor path loses nine properties out of fifty thousand, each one
named — but it is scaffolding until the laid-out view is the only view.

### v0.10.0 — The business tier — **done**

The reason a company cannot leave Word, as distinct from the reason a person cannot.

- [x] Track changes as model state, with accept and reject. A deletion is kept rather
      than dropped, invisible everywhere text is measured, and written back out
- [x] Comments, in a part of their own with markers in the document
- [x] Fields: page numbers, dates, cross-references — answered when the document is laid
      out, which is the only time the answer is known
- [x] Table of contents, built from the headings, each entry pointing at a bookmark

Two things it does not do, both layout work rather than model work: show markup on the
page (insertions underlined, deletions struck through) and show a comment in the margin.
The round trip was what mattered first — a comment dropped on read is a comment deleted
on save.

### v0.11.0 — PDF, the other direction

opennote writes a PDF today and cannot read one. That is a one-way door with a lot of
people standing at it: the form you have to fill in and send back arrives as a PDF, and
the free tools for it are adware or a web upload.

- [x] **Render.** Windows ships `Windows.Data.Pdf`, which turns a page into a bitmap.
      No parser needed to *show* a PDF, and it is the fastest way to something usable.
      Done: a `.pdf` opens in a tab, `--pdf-info` reports it on the command line, and the
      self-check writes a PDF by hand, opens it and asserts there is ink on the render.
- [x] **Fill in a form.** Done for the files this reads: classic cross-reference tables,
      text fields, appearance streams written per field so the value shows everywhere.
      The update is appended, so the original bytes never move.
- [x] **Inflate**, and with it the PDFs written this century: a cross-reference stream
      with a PNG predictor on it, and the object streams that a 1.5 file keeps its
      dictionaries in. About 300 lines, no new dependency. An update written for such a
      file is a cross-reference stream too, because a reader that understands only
      streams would not see a classic table appended after one.
- [x] **A visible signature.** File > Sign PDF takes a picture of one and the next drag
      on the page says where it goes. It is stamped in as an image with its transparency
      kept -- a signature PNG is mostly transparent, and dropping that puts a black box
      on the paper -- carried by an annotation, which is the same incremental-update
      machinery as a filled field.
- [ ] **A cryptographic signature**, separately and later: a PKCS#7 detached signature
      over a byte range, from a certificate in the Windows store. Windows has the crypto
      (`CryptMsg`); what it costs is the `/ByteRange` placeholder dance and the care that
      anything claiming to verify a signature deserves. Reading and *verifying* someone
      else's signature is a different job again, and is not promised here.

Everything above uses what Windows already ships. Nothing in it needs a PDF library.

### v1.0.0 — `.doc`

Legacy binary `.doc`: [MS-DOC] over [MS-CFB]. Compound file storage, the FIB, piece
tables, formatting run arrays. Both specifications are published.

This is the one that needs format archaeology rather than careful reading, and it is
also the widest gap: twenty-five years of files, nothing free reads them well, and the
last in-box reader left with WordPad. Microsoft's own abandoned converters
(`msconv97.dll`, `mswrd632.wpc`) are small, self-contained and do exactly this
conversion — useful as a tie-breaker where the specification is vague, under the rule in
`CONTRIBUTING.md` that nothing derived from them is ever committed.

---

## What "Word replacement" actually means

Not feature parity. Nobody is waiting on mail merge.

The bar is: **someone emails you a `.docx`, you open it, it looks right, you edit it, you
save it, and the person who gets it back cannot tell which program touched it.** That is
v0.7 through v0.9. The business tier (v0.10) is what a company needs on top. `.doc` is a
separate gift to everyone with an archive.

The honest risk remains fidelity. LibreOffice has worked on `.docx` for twenty years and
still mangles complex documents, and it has far more people on it than this has. The
discipline is scope — be excellent on the documents people actually exchange, letters,
reports, resumes and contracts — and measure the loss rather than claim there is none.
That is what the round-trip harness in v0.7 is for: a number in the README that can get
worse, and a build that fails when it does.

---

## Deferred

- Code signing. A certificate is an annual fee and this project does not take money.
- Plugin API. Not until the core is stable enough that an API would not be rewritten.
- Macros and scripting.

## Out of scope

- **Being a code editor.** VS Code and Notepad++ are free, excellent and enormously
  further along. Syntax highlighting exists here because Scintilla provides it, not
  because this competes.
- **A hosted sync service.** Sync talks to your GitHub or Google account directly. No
  server exists and none will.
- Mobile, web, and non-Windows builds.
- Mail merge, equation editing, and the rest of the Word long tail.
