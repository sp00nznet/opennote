# Cloud Sync Setup

**opennote ships no API keys.** Not a client secret, and not a client ID: the published
binaries contain neither, CI passes no credentials, and the build refuses a secret if one
is offered. If you want sync, you register the OAuth application yourself and it stays
yours — your quota, your consent screen, your revocation.

That is a deliberate choice, not an oversight. A secret inside a program handed to users
is not a secret; `strings OpenNote.exe` recovers it. See [SECURITY.md](../SECURITY.md).

---

## What each provider needs

| | Flow | What you need | Who holds it |
|---|---|---|---|
| **GitHub** | Device authorization grant ([RFC 8628][device]) | Client ID only | Public by design — it is in every request the browser can see |
| **Google Drive** | Authorization code with [PKCE][pkce] (RFC 7636) | Client ID, and the "client secret" Google issues for a Desktop client | Yours. Entered at runtime, wrapped with DPAPI, never in the binary |

Google's token endpoint still asks for a `client_secret` for a Desktop client type even
under PKCE. [RFC 8252][native] is explicit that such a value is not confidential — it
cannot be, in an application anybody can download — which is exactly why it belongs to
*your* project rather than to opennote's.

## GitHub

1. Register an OAuth App: <https://github.com/settings/developers> → **New OAuth App**.
2. Any homepage URL will do. **Leave the callback URL blank or set anything** — the device
   flow never redirects, so it is not used.
3. In the app's settings, enable **Device flow**. Without it GitHub rejects the request.
4. Copy the **Client ID**. There is no secret to copy; do not generate one.

The scope opennote asks for is `gist` — enough to sync notes as gists, and nothing else.

## Google Drive

1. Create a project: <https://console.cloud.google.com>.
2. **APIs & Services → Library →** enable the **Google Drive API**.
3. **Credentials → Create credentials → OAuth client ID → Desktop app.**
   Not *Web application*: a desktop client is what PKCE and the loopback redirect want.
4. Copy the **Client ID** and the **Client secret** that Google shows you. Both belong to
   your project.
5. On the consent screen, add yourself as a test user. Google requires
   [verification][verify] before an app with Drive scopes can be used by people outside
   that list — another reason these credentials are yours rather than this project's.

The loopback listener runs on `http://localhost:8547/callback`. Google accepts any
loopback port for a desktop client, so this needs no configuration.

## Giving opennote the credentials

**In an installed copy** — no rebuild, no command line:

> **Settings → Default Settings… → Sync Accounts… → Credentials…**

Paste the GitHub client ID, and the Google client ID and secret, then **Save**. The
sign-in buttons stay greyed out until there is something to sign in with, so the dialog
tells you what is missing rather than failing at the consent screen. **Clear All** forgets
them again; signing out is separate, under Sync Accounts.

**When building from source** the client IDs can be compiled in instead:

```powershell
cmake -B build -DGH_OAUTH_CLIENT_ID="your_github_client_id" `
               -DGOOGLE_CLIENT_ID="your_google_client_id"
cmake --build build --config Release
```

Passing `GH_OAUTH_CLIENT_SECRET` or `GOOGLE_CLIENT_SECRET` **fails the build on purpose**.
The Google secret is entered in the dialog, not compiled in. A compiled-in ID shows up in
the dialog as the current value, so what you see there is always what will be used.

## What is stored, and where

`%APPDATA%\OpenNote\opennote.db`, in the `settings` table:

- **Client IDs** — plain, because they are public identifiers.
- **The Google client secret and every access token** — wrapped with `CryptProtectData`
  (DPAPI) first, so a copy of the database taken to another machine or another account is
  useless.

Revoking access at the provider is always enough to cut a build off entirely: GitHub under
**Settings → Applications**, Google under **Account → Security → Third-party access**.

A stored credential that cannot be unwrapped — because it was written by a different
Windows account, or because it was left in the clear by a build from before v0.2.0 — is
**deleted the moment it is read**. It could not have been used anyway, and a credential
nothing can read has no business staying in a database. The account simply appears signed
out, and signing in again stores a wrapped one.

## Why not just ship a key?

It would work, and it is what most desktop applications do. It also means:

- every user's sync depends on one OAuth app that can be rate-limited or suspended as a
  unit, by a provider, over behaviour by somebody else;
- the consent screen names this project rather than the person whose data it is;
- Google's verification for Drive scopes — and whatever review it asks for that year —
  lands on this project, which does not operate a server and would rather not start.

A client ID is not a secret and could be shipped safely from a build-time variable if that
ever changes. A client secret could not be, whatever it was injected from.

[device]: https://datatracker.ietf.org/doc/html/rfc8628
[pkce]: https://datatracker.ietf.org/doc/html/rfc7636
[native]: https://datatracker.ietf.org/doc/html/rfc8252#section-8.5
[verify]: https://support.google.com/cloud/answer/13463073
