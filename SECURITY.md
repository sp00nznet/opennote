# Security

OpenNote stores your notes, your editor session and — if you connect a sync provider —
an OAuth access token, in a SQLite database at `%APPDATA%\OpenNote\opennote.db`.

That file is as sensitive as the notes you put in it. This document says plainly what
protects it today and what does not.

## Reporting a vulnerability

Open a GitHub issue at <https://github.com/sp00nznet/opennote/issues>. If the issue
would put existing users at risk before a fix ships, use GitHub's **private vulnerability
reporting** on the repository's Security tab instead.

There is no bounty. Expect a reply within a week.

## Fixed in v0.2.0

Three problems were present in earlier code. All three are fixed; they are described here
rather than quietly deleted, because anyone running a build from before v0.2.0 is still
affected by them.

### 1. OAuth client secrets were compiled into the executable — fixed

`CMakeLists.txt` accepted `GH_OAUTH_CLIENT_SECRET` and `GOOGLE_CLIENT_SECRET` and baked
them into the binary as preprocessor defines. A secret inside a program distributed to
users is not a secret — `strings OpenNote.exe` recovers it.

GitHub now uses the [device authorization grant][device] (RFC 8628), which needs only a
client ID — public by design. Google uses [PKCE][pkce] (RFC 7636); Google's token endpoint
still wants a `client_secret` for a Desktop client, so that value is supplied by the user
for their own Google Cloud project and stored encrypted. It never ships.

`CMakeLists.txt` now fails the build if either secret variable is passed at all, so this
cannot come back by accident. Because no secret is needed, **CI uses no credentials**, and
anyone can reproduce a published binary from its tagged commit.

### 2. Access tokens were stored in cleartext — fixed

`OAuth_SaveToken` wrote the token into the `settings` table as plain text. Anything that
could read `%APPDATA%\OpenNote\opennote.db` — any process running as you, any backup, any
sync tool that picked that directory up — could read the token and use it against your
account.

**If you ran a build from before v0.2.0 and signed in, your access token is still in that
database in the clear.** Revoke it — GitHub under Settings → Applications, Google under
Account → Security → Third-party access — and sign in again. From v0.9.1 OpenNote deletes
any stored credential it cannot unwrap the moment it reads one, so such a token is removed
on sight rather than left to sit there; it was unusable to the program either way, which
is precisely why it went unnoticed.

Tokens are now wrapped with `CryptProtectData` (DPAPI) before they reach the database,
which ties the stored blob to your Windows account. A copy of `opennote.db` taken to
another machine, or opened by another user, yields nothing.

**If you connected an account before v0.2.0**, the old cleartext value was written to
disk. Revoke that token in your provider's settings and reconnect.

### 3. The token write path built SQL by string formatting — fixed

The same function formatted the token directly into an `INSERT OR REPLACE` statement
rather than binding it. The value came from the provider rather than from an attacker, so
it was not a live injection path — but it was the wrong construction. Settings access now
goes through `Database_SetSetting` / `Database_GetSetting`, which bind their parameters.

### Still true: notes are not encrypted at rest

The notes database is an ordinary SQLite file. Anyone with the file has the notes.

This is not a bug, it is the current design, and it is stated here so nobody assumes
otherwise. Encryption at rest is scheduled for v0.3.0 — see `ROADMAP.md`.

## What OpenNote does not do

- No telemetry, no analytics, no crash reporting, no update check. The application makes
  no network request at all unless you connect a sync provider.
- No account. There is nothing to sign up for.
- Sync talks to GitHub or Google directly. There is no server in between, and none is
  operated by this project.

## Self-check

`OpenNote.exe --selftest` runs the checks that guard these paths — the PKCE S256 challenge
against the RFC 7636 appendix B test vector, the DPAPI round trip including its truncation
and malformed-input cases, and the JSON field reader against the responses GitHub's device
endpoint actually returns. It prints a line per check and exits non-zero on any failure.
CI runs it on every build.

## Verifying what you run

Released binaries are built by GitHub Actions from a tagged commit — see
`.github/workflows/ci.yml`. The build log for any release is public and shows exactly
which commit produced it.

The executables are **not** code-signed. A certificate costs money annually and this
project does not take money, so SmartScreen will warn on first run. Building from source
takes one command and avoids the question entirely.

[device]: https://docs.github.com/apps/oauth-apps/building-oauth-apps/authorizing-oauth-apps#device-flow
[pkce]: https://developers.google.com/identity/protocols/oauth2/native-app
