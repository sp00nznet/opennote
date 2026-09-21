# Contributing

Issues and pull requests are welcome.

## Building

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Requires Windows 10/11, Visual Studio 2022 and CMake 3.16+. Nothing else — SQLite and
Scintilla are vendored in `lib/`.

Do **not** pass the `GH_OAUTH_CLIENT_ID` / `GOOGLE_CLIENT_SECRET` CMake variables for a
normal build. Without them the binary contains no credentials and cloud sync is simply
absent. See `SECURITY.md`.

## Pull requests

- One change per pull request.
- Imperative-mood commit subjects. Explain *why* in the body when the change is not
  self-evident. No `wip`, `fix` or `stuff`.
- CI must be green: the build runs with `/W4` and warnings are treated as something to
  fix, not to scroll past.
- Update `CHANGELOG.md` under `## [Unreleased]` as part of the change, not afterwards.

## Working on file formats

opennote is growing into a document editor, which means implementing published formats:
RTF 1.9.1, ECMA-376 (`.docx`), and eventually [MS-DOC] over [MS-CFB] (`.doc`). All of
these specifications are public and downloadable. Read them.

Disassembly of Microsoft's own converters is a legitimate way to resolve a point where a
specification is vague or where real-world files disagree with it. It is a tie-breaker,
not a starting point.

**Nothing derived from a proprietary binary is ever committed.** Specifically, none of
the following may enter this repository, in any form, including test fixtures and CI
artifacts:

- Decompiler output, disassembly listings, or lifted IR.
- Struct definitions, headers or symbol maps reconstructed from a proprietary binary.
- Any portion of a Microsoft binary or SDK.

Keep analysis notes outside the repository. What lands in the tree is hand-written C with
a comment citing the **specification section** that justifies it. If the only reason a
line of code is correct is "the disassembly does this", say so in the comment and cite
the spec section it contradicts — that is a finding worth recording, and it still does
not license pasting the listing.

## Test corpora

Document files used for conformance testing must be freely redistributable. Anything that
is not lives outside the repository; the harness fetches it or skips with a clear message,
and the path is documented in the README.

Never commit a document containing anyone's real content.
