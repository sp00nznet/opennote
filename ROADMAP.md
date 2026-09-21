# Roadmap

Where this is going, in order, and what is deliberately not being built.

The short version: **Windows shipped a rich text editor for thirty years and removed it
in 2024.** WordPad is gone from Windows 11 24H2 and from Windows Server 2025. The
replacement Microsoft points you at is a subscription. OpenNote is aiming at that hole.

**v0.6.0 is released.** Published binaries — a bare executable and an installer — are
built by CI from the tagged commit.

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

### v0.8.0 — The layout engine

The big one. Windows supplies the hard half: `IDWriteTextLayout` does shaping, line
breaking, justification, bidi and font fallback. What has to be written is the part
above it — flowing those laid-out lines into columns and pages.

- [x] Block layout: measure and flow paragraphs into a page, break, continue. A
      paragraph too tall for the space left is split at a line boundary and continues
      on the next page, however many that takes
- [x] Page model: paper size and margins, from the document or from Page Setup, which
      now reaches the engine, the printer and the `.docx` writer at once
- [ ] Widow and orphan control
- [x] **Table layout** — column widths from `w:tblGrid`, scaled to the page, cells
      measured and boxed. A row moves to the next page whole rather than splitting
      across the boundary, and merged cells are not handled yet. Honestly the nastiest
      part of the whole project, and this is the easy half of it
- [x] Rendering through Direct2D, to the screen and to a printer. Not through a printer
      DC: Direct2D refuses one outright, and its actual printing path is
      `ID2D1PrintControl`, which keeps the output vector rather than rasterising a page
- [ ] Hit testing, caret movement and selection across the laid-out model. Routinely
      underestimated; comparable in size to layout itself
- [ ] Editing against the model, with undo
- [ ] Ruler and tab stops, which need a page width to mean anything
- [x] Print preview, which is just the page renderer in a window
- [x] Print-to-PDF through the in-box printer, which turned out not to be free — but is
      still a printing path and not a PDF library, and the text in the file is text

The engine is deliberately free of any window and of Direct2D: it answers where
everything goes, and drawing it is somebody else's job. That is what lets one layout
serve the screen, the printer and the exported PDF, and it is what makes it checkable
without a screen — `--layout-report` prints the geometry, `--export-pdf` writes the file,
and the conformance harness asserts on every document that nothing lands below the
bottom margin and that no table cell goes missing.

What is left in this version is the editing half: a caret that moves through laid-out
text, a selection, and editing the model rather than the control. Until that lands the
engine renders the document rather than hosting it, and the RichEdit view is still where
typing happens.

Once this lands, Scintilla can go: the plain text view becomes a degenerate case of the
rich one, and roughly a thousand vendored files leave with it.

### v0.9.0 — Document fidelity

The things real `.docx` files contain that are currently dropped or approximated.

- [ ] `styles.xml` — named styles, inheritance, document defaults, instead of the
      current approximation of what a heading ought to look like
- [ ] `numbering.xml` — real numbered and multi-level lists, instead of reading every
      list as bulleted
- [ ] Images: DrawingML and VML, read and written
- [ ] Headers and footers, which need pagination to place
- [ ] Sections, page breaks and columns
- [ ] Footnotes and endnotes

### v0.10.0 — The business tier

The reason a company cannot leave Word, as distinct from the reason a person cannot.

- [ ] Track changes as model state, with accept and reject. Deletions are currently
      dropped on read, which is correct for display and lossy for a round-trip
- [ ] Comments
- [ ] Fields: page numbers, dates, cross-references
- [ ] Table of contents

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
