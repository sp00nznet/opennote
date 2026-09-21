# What opennote is

A word processor and a notes editor for Windows, in one executable of a few megabytes,
with no runtime to install, no account, no telemetry and nothing running when it is
closed. It reads and writes `.txt`, `.rtf` and `.docx`, lays documents out on real pages,
prints them and exports PDF.

This page is the long answer: where it came from, what part of it is WordPad, what part of
it is Word, and what it is deliberately not.

For using it, start at [getting-started.md](getting-started.md). For the keys, see
[controls.md](controls.md).

---

## Where it came from

**Windows shipped a rich text editor for thirty years and then removed it.** WordPad is
gone from Windows 11 24H2 and from Windows Server 2025. It is not deprecated, not
optional, not a feature-on-demand — it is deleted, and the installer will remove the copy
you had.

Microsoft's suggested replacement for editing `.rtf` and `.docx` is Microsoft 365, at
$70–100 a year, or Office 2024 at about $150 once. For Notepad's job it suggests Notepad,
which cannot open the files WordPad opened.

So a capability that was in the box since Windows 95 now has a subscription in front of
it. That is the gap this fills, and it is the same idea as this project's siblings —
[futureburn](https://github.com/sp00nznet/futureburn),
[pstfree](https://github.com/sp00nznet/pstfree),
[vncfree](https://github.com/sp00nznet/vncfree),
[bulkhead](https://github.com/sp00nznet/bulkhead): find the Windows payware, read the
published specification it is hiding behind, give it away.

The second reason is older. **Nothing free reads a legacy `.doc` well**, and the last
in-box reader left with WordPad. [MS-DOC] and [MS-CFB] are published documents anyone can
download. Twenty-five years of files are sitting in folders waiting for someone to bother.

---

## What is WordPad about it

Everything WordPad did, opennote does, and in the same shape — because both are built on
the parts of Windows that were always there for this.

- **Rich text.** Fonts, sizes, colours, bold, italic, underline, strikethrough,
  super- and subscript, alignment, line spacing, bullets and numbering, indents, pictures.
  Saved as `.rtf`, which is what WordPad saved.
- **A formatting toolbar**, a ruler, page setup and multi-page printing.
- **Spell checking**, through `ISpellChecker` — the dictionary Windows already has.
- **It opens in a second and closes in less.** One executable, a few megabytes, no splash
  screen, no update service, no first-run sign-in.

WordPad itself was built on the RichEdit control that ships with Windows, and for the
rich text view so is this. That control has a known ceiling: tables are weak, and it flows
text into a window rather than showing you pages. Everything below is about lifting that
ceiling.

It also does Notepad's job, since the two were always the same program to most people: a
plain text tab, with syntax highlighting where Scintilla recognises the file, word wrap,
go-to-line, find in all open tabs, and a local note store for the things that never
deserved a filename.

---

## What is Word about it

The part Word actually gets paid for is `.docx` fidelity and page layout. Both are here.

- **`.docx` is read and written directly** — ECMA-376 WordprocessingML. Not converted,
  not approximated: styles resolved through `basedOn` to the document defaults, numbering
  that counts properly (letters, roman numerals, `1.2.` at a nested level), pictures,
  tables with their stated column widths, sections, columns, page breaks, headers and
  footers, footnotes and endnotes, tracked changes, comments, fields, bookmarks, and
  Unicode throughout.

  There is no third-party dependency for any of it. A `.docx` is an Open Packaging
  Conventions container, and Windows ships the API for exactly that shape (`IOpcFactory`)
  next to a pull XML reader (`IXmlReader`). The container and the parser were never this
  project's code to own.

- **A layout engine.** A document is measured onto pages: paragraphs broken at a line when
  they do not fit, widows and orphans kept off the page, tables boxed, footnotes at the
  foot of the page whose text referred to them, pages sized by Page Setup.
  `IDWriteTextLayout` does the shaping, line breaking and font fallback — the part above
  it is this project's, and it knows nothing about windows. The same geometry draws the
  page view, the printout and the PDF, so the three cannot disagree.

- **You edit on the page.** The laid-out view is not a preview. A click names a character,
  the arrows walk the laid-out lines rather than the structure behind them, `Page Up` means
  a page, the ruler's markers set indents, and undo is a stack of model snapshots.

- **What a working document carries.** Tracked changes are model state: a deletion is
  kept rather than dropped, shown nowhere, and written back where it came from, with
  accept and reject to settle them. Comments live in their own part with markers in the
  text. Fields — `PAGE`, `NUMPAGES`, `DATE`, `TIME`, `REF`, `PAGEREF` — are answered when
  the document is laid out, which is the only moment the answer is known, and a table of
  contents is built from the headings with each entry pointing at its own.

- **PDF export** that is vector — the text stays text, the fonts are embedded — through
  Windows' own PDF printer. There is no PDF library in here either.

**Fidelity is measured, not claimed.** A generated corpus of documents goes through
load → model → save → reload, and every property the source stated is compared with what
came back. The number is in the README, a build fails when it gets worse, and the current
reading is `230/230` conformance checks across 12 documents, `84327/84327` properties
surviving the model round-trip and `84189/84191` surviving a load, edit and save — with
both remaining losses named rather than rounded away.

---

## The goals

1. **Replace WordPad completely**, for someone who never wanted more than WordPad.
2. **Be honest about `.docx`.** The bar is: someone emails you a document, you open it, it
   looks right, you edit it, you save it, and the person who gets it back cannot tell which
   program touched it. Not feature parity — nobody is waiting on mail merge.
3. **Read the files nothing free reads.** Legacy `.doc`, from the published specification.
4. **Stay one executable.** No runtime, no installer requirement, no service, no account.
5. **Keep the data local.** One SQLite file in `%APPDATA%`. Sync, if you turn it on, talks
   to your own GitHub or Google account with credentials you registered — there is no
   opennote server and there will not be one.
6. **Measure instead of claiming.** Every fidelity assertion in this project has a number
   behind it that a regression can make worse.

---

## What it is not

- **Not a code editor.** VS Code and Notepad++ are free, excellent and enormously further
  along. Syntax highlighting is here because Scintilla provides it, not because anyone
  should switch.
- **Not a competitor to LibreOffice.** LibreOffice Writer is genuinely free, genuinely
  good, and years ahead of this. Use it today. opennote exists because a 350MB office
  suite is not what most people wanted when they opened WordPad.
- **Not a sync service.** There is no server, no hosted account, no cloud tier.
- **Not cross-platform, not mobile, not a web app.** It is a Win32 program on purpose.
- **Not funded.** No telemetry, no ads, no paid tier, MIT licensed. It is also not code
  signed, because a certificate is an annual fee and this project does not take money —
  expect SmartScreen to complain on first run.

---

## Where it is going

[ROADMAP.md](../ROADMAP.md) has the ordering. The short version:

- **v0.11 — PDF, the other direction.** opennote writes a PDF and cannot read one.
  Rendering a page (Windows ships `Windows.Data.Pdf`), filling in a form, and stamping a
  visible signature on it — the form you have to fill in and send back arrives as a PDF,
  and the free tools for it are adware or a web upload. A cryptographic signature is a
  separate, later job.
- **v1.0 — `.doc`.** The legacy binary format over compound file storage. The widest gap,
  and the one that needs format archaeology rather than careful reading.

The honest risk is fidelity. LibreOffice has worked on `.docx` for twenty years and still
mangles complex documents, with far more people on it than this has. The discipline is
scope: be excellent on the documents people actually exchange — letters, reports, resumes,
contracts — and measure the loss rather than pretend there is none.
