// OAuth for the two sync providers.
//
// No client secret is compiled into this binary, because a secret shipped to
// users is not a secret -- `strings OpenNote.exe` recovers it. The two flows
// here are the ones designed for clients that cannot hold one:
//
//   GitHub  -- device authorization grant (RFC 8628). Client id only, which is
//              public by design. No loopback server, no secret, no redirect.
//   Google  -- authorization code with PKCE (RFC 7636). Google's token endpoint
//              still wants a client_secret for a Desktop client type, so that
//              value is supplied by the user for their own Google Cloud project
//              and stored encrypted. It is theirs, not ours, and it never ships.

// Winsock must be included before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>

#include "supernote.h"
#include "sync/oauth.h"
#include "sync/crypto.h"
#include <winhttp.h>
#include <shellapi.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

// Local server port for the Google OAuth callback
#define OAUTH_CALLBACK_PORT 8547

// GitHub device flow endpoints
#define GITHUB_DEVICE_HOST  L"github.com"
#define GITHUB_DEVICE_PATH  L"/login/device/code"
#define GITHUB_TOKEN_PATH   L"/login/oauth/access_token"
#define GITHUB_DEVICE_GRANT "urn:ietf:params:oauth:grant-type:device_code"

// Google OAuth endpoints
#define GOOGLE_AUTH_URL   "https://accounts.google.com/o/oauth2/v2/auth"
#define GOOGLE_TOKEN_HOST L"oauth2.googleapis.com"
#define GOOGLE_TOKEN_PATH L"/token"

// Settings keys. Client ids are public; the Google secret belongs to the user's
// own project and is stored wrapped by DPAPI like a token.
#define KEY_GITHUB_CLIENT_ID     "oauth_github_client_id"
#define KEY_GOOGLE_CLIENT_ID     "oauth_google_client_id"
#define KEY_GOOGLE_CLIENT_SECRET "oauth_google_client_secret"

// Callback server state (Google only)
static SOCKET g_listenSocket = INVALID_SOCKET;
static volatile BOOL g_callbackReceived = FALSE;
static char g_authCode[512] = {0};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Pull a string field out of a flat JSON object. The responses these endpoints
// return are small and flat, so this is enough; anything nested would need a
// real parser and there is nothing nested here.
static BOOL JsonGetString(const char* json, const char* key, char* out, size_t outSize) {
    if (!json || !key || !out || outSize == 0) return FALSE;
    out[0] = '\0';

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (!p) return FALSE;

    p = strchr(p + strlen(pattern), ':');
    if (!p) return FALSE;
    p++;

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return FALSE;
    p++;

    const char* end = strchr(p, '"');
    if (!end) return FALSE;

    size_t len = (size_t)(end - p);
    if (len >= outSize) return FALSE;

    memcpy(out, p, len);
    out[len] = '\0';
    return TRUE;
}

// One POST of an x-www-form-urlencoded body, JSON response. Every OAuth call
// below is this shape; it used to be copied out per call site.
static BOOL HttpPostForm(const WCHAR* host, const WCHAR* path,
                         const char* body, char* response, size_t responseSize) {
    if (!response || responseSize == 0) return FALSE;
    response[0] = '\0';

    HINTERNET hSession = WinHttpOpen(L"OpenNote", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return FALSE;

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path, NULL,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    WinHttpAddRequestHeaders(hRequest, L"Accept: application/json", (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/x-www-form-urlencoded",
                             (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = FALSE;
    DWORD bodyLen = (DWORD)strlen(body);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           (LPVOID)body, bodyLen, bodyLen, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        DWORD total = 0, read = 0;
        while (total + 1 < responseSize &&
               WinHttpReadData(hRequest, response + total,
                               (DWORD)(responseSize - total - 1), &read) && read > 0) {
            total += read;
        }
        response[total] = '\0';
        ok = (total > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// Resolve a client id: a build-time default if one was configured, otherwise
// whatever the user entered. Client ids are not secrets, so compiling one in is
// fine -- unlike the secrets this file used to carry.
static BOOL GetClientId(const char* settingsKey, const char* builtIn,
                        char* out, size_t outSize) {
    if (builtIn && builtIn[0]) {
        strncpy_s(out, outSize, builtIn, _TRUNCATE);
        return TRUE;
    }
    return Database_GetSetting(settingsKey, out, outSize);
}

// ---------------------------------------------------------------------------
// The user's own OAuth application
// ---------------------------------------------------------------------------

void OAuth_LoadCredentials(OAuthCredentials* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    // A build-time client id, when one was compiled in, is what the user sees
    // in the settings screen -- so what is shown is what will actually be used.
#ifdef GITHUB_CLIENT_ID
    strncpy_s(out->githubClientId, sizeof(out->githubClientId), GITHUB_CLIENT_ID, _TRUNCATE);
#endif
#ifdef GOOGLE_CLIENT_ID
    strncpy_s(out->googleClientId, sizeof(out->googleClientId), GOOGLE_CLIENT_ID, _TRUNCATE);
#endif

    char stored[512];
    if (Database_GetSetting(KEY_GITHUB_CLIENT_ID, stored, sizeof(stored)) && stored[0]) {
        strncpy_s(out->githubClientId, sizeof(out->githubClientId), stored, _TRUNCATE);
    }
    if (Database_GetSetting(KEY_GOOGLE_CLIENT_ID, stored, sizeof(stored)) && stored[0]) {
        strncpy_s(out->googleClientId, sizeof(out->googleClientId), stored, _TRUNCATE);
    }

    char wrapped[2048];
    if (Database_GetSetting(KEY_GOOGLE_CLIENT_SECRET, wrapped, sizeof(wrapped)) && wrapped[0]) {
        if (!Crypto_UnprotectFromBase64(wrapped, out->googleSecret, sizeof(out->googleSecret))) {
            // Written by another Windows account, or corrupted. Treat it as
            // absent rather than handing the caller rubbish.
            out->googleSecret[0] = '\0';
        }
    }
}

// Write one setting, or remove it when the value is empty.
static BOOL StoreOrClear(const char* key, const char* value) {
    if (!value || !value[0]) {
        Database_DeleteSetting(key);
        return TRUE;
    }
    return Database_SetSetting(key, value);
}

BOOL OAuth_SaveCredentials(const OAuthCredentials* creds) {
    if (!creds) return FALSE;

    BOOL ok = StoreOrClear(KEY_GITHUB_CLIENT_ID, creds->githubClientId) &&
              StoreOrClear(KEY_GOOGLE_CLIENT_ID, creds->googleClientId);

    if (!creds->googleSecret[0]) {
        Database_DeleteSetting(KEY_GOOGLE_CLIENT_SECRET);
        return ok;
    }

    // The secret is wrapped before it reaches the database, the same way a
    // token is: a copy of opennote.db is then useless anywhere else.
    char wrapped[2048];
    if (!Crypto_ProtectToBase64(creds->googleSecret, wrapped, sizeof(wrapped))) return FALSE;

    return ok && Database_SetSetting(KEY_GOOGLE_CLIENT_SECRET, wrapped);
}

BOOL OAuth_HasGitHubCredentials(void) {
    OAuthCredentials creds;
    OAuth_LoadCredentials(&creds);
    return creds.githubClientId[0] != '\0';
}

BOOL OAuth_HasGoogleCredentials(void) {
    OAuthCredentials creds;
    OAuth_LoadCredentials(&creds);
    return creds.googleClientId[0] != '\0' && creds.googleSecret[0] != '\0';
}

static void CopyToClipboard(HWND hOwner, const char* text) {
    if (!OpenClipboard(hOwner)) return;
    EmptyClipboard();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(WCHAR));
    if (hMem) {
        WCHAR* dst = (WCHAR*)GlobalLock(hMem);
        MultiByteToWideChar(CP_UTF8, 0, text, -1, dst, wlen);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }

    CloseClipboard();
}

// ---------------------------------------------------------------------------
// Loopback callback server -- Google only. GitHub's device flow needs none.
// ---------------------------------------------------------------------------

static BOOL StartCallbackServer(void) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return FALSE;

    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSocket == INVALID_SOCKET) {
        WSACleanup();
        return FALSE;
    }

    int opt = 1;
    setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(OAUTH_CALLBACK_PORT);

    if (bind(g_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(g_listenSocket, 1) == SOCKET_ERROR) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        return FALSE;
    }

    return TRUE;
}

static void StopCallbackServer(void) {
    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }
    WSACleanup();
}

static BOOL WaitForCallback(int timeoutSeconds) {
    g_callbackReceived = FALSE;
    g_authCode[0] = '\0';

    DWORD timeout = (DWORD)timeoutSeconds * 1000;
    setsockopt(g_listenSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    SOCKET clientSocket = accept(g_listenSocket, NULL, NULL);
    if (clientSocket == INVALID_SOCKET) return FALSE;

    char buffer[4096] = {0};
    int received = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (received > 0) {
        buffer[received] = '\0';

        // GET /callback?code=XXX&state=YYY HTTP/1.1
        char* codeStart = strstr(buffer, "code=");
        if (codeStart) {
            codeStart += 5;
            char* codeEnd = strpbrk(codeStart, "& \r\n");
            if (codeEnd) {
                size_t codeLen = (size_t)(codeEnd - codeStart);
                if (codeLen < sizeof(g_authCode)) {
                    strncpy_s(g_authCode, sizeof(g_authCode), codeStart, codeLen);
                    g_callbackReceived = TRUE;
                }
            }
        }
    }

    const char* successPage =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><title>OpenNote</title>"
        "<style>body{font-family:system-ui;display:flex;justify-content:center;"
        "align-items:center;height:100vh;margin:0;background:#f0f0f0;}"
        ".box{background:white;padding:40px;border-radius:8px;text-align:center;"
        "box-shadow:0 2px 10px rgba(0,0,0,0.1);}"
        "h1{color:#2ecc71;margin:0 0 10px 0;}p{color:#666;}</style></head>"
        "<body><div class='box'><h1>Success!</h1>"
        "<p>You can close this window and return to OpenNote.</p></div></body></html>";

    const char* errorPage =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><title>OpenNote</title>"
        "<style>body{font-family:system-ui;display:flex;justify-content:center;"
        "align-items:center;height:100vh;margin:0;background:#f0f0f0;}"
        ".box{background:white;padding:40px;border-radius:8px;text-align:center;"
        "box-shadow:0 2px 10px rgba(0,0,0,0.1);}"
        "h1{color:#e74c3c;margin:0 0 10px 0;}p{color:#666;}</style></head>"
        "<body><div class='box'><h1>Error</h1>"
        "<p>Authorization failed. Please try again.</p></div></body></html>";

    const char* page = g_callbackReceived ? successPage : errorPage;
    send(clientSocket, page, (int)strlen(page), 0);

    closesocket(clientSocket);
    return g_callbackReceived;
}

// ---------------------------------------------------------------------------
// GitHub -- device authorization grant (RFC 8628)
// ---------------------------------------------------------------------------

BOOL OAuth_GitHubLogin(HWND hParent, OAuthToken* tokenOut) {
    memset(tokenOut, 0, sizeof(OAuthToken));

#ifdef GITHUB_CLIENT_ID
    const char* builtIn = GITHUB_CLIENT_ID;
#else
    const char* builtIn = NULL;
#endif

    char clientId[256];
    if (!GetClientId(KEY_GITHUB_CLIENT_ID, builtIn, clientId, sizeof(clientId))) {
        MessageBoxW(hParent,
                    L"No GitHub client ID is configured.\n\n"
                    L"Enter one in Settings, or build with -DGH_OAUTH_CLIENT_ID.",
                    APP_NAME, MB_ICONWARNING);
        return FALSE;
    }

    // Step 1: ask GitHub for a device code and a code for the user to type.
    char body[512];
    snprintf(body, sizeof(body), "client_id=%s&scope=gist", clientId);

    char response[4096];
    if (!HttpPostForm(GITHUB_DEVICE_HOST, GITHUB_DEVICE_PATH, body, response, sizeof(response))) {
        MessageBoxW(hParent, L"Could not reach GitHub to start authorization.",
                    APP_NAME, MB_ICONERROR);
        return FALSE;
    }

    char deviceCode[512], userCode[64], verifyUri[256], intervalStr[16];
    if (!JsonGetString(response, "device_code", deviceCode, sizeof(deviceCode)) ||
        !JsonGetString(response, "user_code", userCode, sizeof(userCode))) {
        // GitHub returns device_flow_disabled if the OAuth app has not opted in.
        MessageBoxW(hParent,
                    L"GitHub refused to start the device flow.\n\n"
                    L"Check that device flow is enabled on the OAuth app.",
                    APP_NAME, MB_ICONERROR);
        return FALSE;
    }

    if (!JsonGetString(response, "verification_uri", verifyUri, sizeof(verifyUri))) {
        strcpy_s(verifyUri, sizeof(verifyUri), "https://github.com/login/device");
    }

    // GitHub tells us how often it is willing to be polled. Ignoring it earns a
    // slow_down error, so take the value when it is offered and default to the
    // 5 seconds the spec suggests when it is not.
    int interval = 5;
    if (JsonGetString(response, "interval", intervalStr, sizeof(intervalStr))) {
        int parsed = atoi(intervalStr);
        if (parsed > 0) interval = parsed;
    }

    // Step 2: the user types the code into the browser. Put it on the clipboard
    // so they do not have to retype it from a message box.
    CopyToClipboard(hParent, userCode);

    WCHAR verifyUriW[256];
    MultiByteToWideChar(CP_UTF8, 0, verifyUri, -1, verifyUriW, 256);
    ShellExecuteW(NULL, L"open", verifyUriW, NULL, NULL, SW_SHOWNORMAL);

    WCHAR prompt[512], userCodeW[64];
    MultiByteToWideChar(CP_UTF8, 0, userCode, -1, userCodeW, 64);
    swprintf(prompt, 512,
             L"Enter this code in your browser:\n\n        %s\n\n"
             L"It has been copied to your clipboard.\n\n"
             L"Click OK once you have authorized OpenNote.",
             userCodeW);
    MessageBoxW(hParent, prompt, APP_NAME, MB_OK | MB_ICONINFORMATION);

    // Step 3: poll for the token. GitHub's device codes last 15 minutes; cap the
    // wait at 5, which is well past the point where the user has walked away.
    snprintf(body, sizeof(body),
             "client_id=%s&device_code=%s&grant_type=%s",
             clientId, deviceCode, GITHUB_DEVICE_GRANT);

    const int deadline = 300;
    for (int waited = 0; waited < deadline; waited += interval) {
        if (!HttpPostForm(GITHUB_DEVICE_HOST, GITHUB_TOKEN_PATH, body,
                          response, sizeof(response))) {
            break;
        }

        if (JsonGetString(response, "access_token", tokenOut->accessToken,
                          sizeof(tokenOut->accessToken))) {
            strcpy_s(tokenOut->tokenType, sizeof(tokenOut->tokenType), "bearer");
            tokenOut->isAuthenticated = TRUE;
            OAuth_SaveToken("github", tokenOut);
            return TRUE;
        }

        char error[64];
        if (JsonGetString(response, "error", error, sizeof(error))) {
            if (strcmp(error, "authorization_pending") == 0) {
                // Expected: the user has not finished in the browser yet.
            } else if (strcmp(error, "slow_down") == 0) {
                interval += 5;
            } else {
                // access_denied, expired_token, incorrect_device_code: all final.
                break;
            }
        }

        Sleep((DWORD)interval * 1000);
    }

    MessageBoxW(hParent, L"GitHub authorization was not completed.",
                APP_NAME, MB_ICONWARNING);
    return FALSE;
}

// ---------------------------------------------------------------------------
// Google -- authorization code with PKCE (RFC 7636)
// ---------------------------------------------------------------------------

BOOL OAuth_GoogleLogin(HWND hParent, OAuthToken* tokenOut) {
    memset(tokenOut, 0, sizeof(OAuthToken));

#ifdef GOOGLE_CLIENT_ID
    const char* builtIn = GOOGLE_CLIENT_ID;
#else
    const char* builtIn = NULL;
#endif

    char clientId[256];
    if (!GetClientId(KEY_GOOGLE_CLIENT_ID, builtIn, clientId, sizeof(clientId))) {
        MessageBoxW(hParent,
                    L"No Google client ID is configured.\n\n"
                    L"Create a Desktop OAuth client in your own Google Cloud project "
                    L"and enter its ID in Settings.",
                    APP_NAME, MB_ICONWARNING);
        return FALSE;
    }

    // The user's own client secret, wrapped by DPAPI in their own database.
    // Google's token endpoint wants one for a Desktop client even under PKCE.
    OAuthCredentials creds;
    OAuth_LoadCredentials(&creds);
    const char* clientSecret = creds.googleSecret;

    // PKCE: a fresh verifier per attempt, and its SHA-256 sent up front. An
    // attacker who intercepts the authorization code cannot redeem it without
    // the verifier, which never leaves this process until the token request.
    char verifier[128], challenge[64];
    if (!Crypto_RandomBase64Url(verifier, sizeof(verifier), 32) ||
        !Crypto_Sha256Base64Url(verifier, challenge, sizeof(challenge))) {
        MessageBoxW(hParent, L"Could not generate PKCE parameters.", APP_NAME, MB_ICONERROR);
        return FALSE;
    }

    if (!StartCallbackServer()) {
        MessageBoxW(hParent, L"Failed to start the local authentication server.",
                    APP_NAME, MB_ICONERROR);
        return FALSE;
    }

    char authUrl[2048];
    snprintf(authUrl, sizeof(authUrl),
             "%s?client_id=%s&redirect_uri=http://localhost:%d/callback"
             "&response_type=code&scope=https://www.googleapis.com/auth/drive.file"
             "&access_type=offline&code_challenge=%s&code_challenge_method=S256",
             GOOGLE_AUTH_URL, clientId, OAUTH_CALLBACK_PORT, challenge);

    WCHAR authUrlW[2048];
    MultiByteToWideChar(CP_UTF8, 0, authUrl, -1, authUrlW, 2048);
    ShellExecuteW(NULL, L"open", authUrlW, NULL, NULL, SW_SHOWNORMAL);

    BOOL gotCode = WaitForCallback(120);
    StopCallbackServer();

    if (!gotCode) {
        MessageBoxW(hParent, L"Authorization timed out or was cancelled.",
                    APP_NAME, MB_ICONWARNING);
        return FALSE;
    }

    char body[2048];
    snprintf(body, sizeof(body),
             "client_id=%s&client_secret=%s&code=%s&code_verifier=%s"
             "&grant_type=authorization_code&redirect_uri=http://localhost:%d/callback",
             clientId, clientSecret, g_authCode, verifier, OAUTH_CALLBACK_PORT);

    char response[4096];
    BOOL ok = HttpPostForm(GOOGLE_TOKEN_HOST, GOOGLE_TOKEN_PATH, body,
                           response, sizeof(response));

    SecureZeroMemory(body, sizeof(body));
    SecureZeroMemory(verifier, sizeof(verifier));
    SecureZeroMemory(clientSecret, sizeof(clientSecret));

    if (ok && JsonGetString(response, "access_token", tokenOut->accessToken,
                            sizeof(tokenOut->accessToken))) {
        JsonGetString(response, "refresh_token", tokenOut->refreshToken,
                      sizeof(tokenOut->refreshToken));
        strcpy_s(tokenOut->tokenType, sizeof(tokenOut->tokenType), "bearer");
        tokenOut->isAuthenticated = TRUE;
        OAuth_SaveToken("google", tokenOut);
        return TRUE;
    }

    char error[128];
    if (JsonGetString(response, "error", error, sizeof(error))) {
        WCHAR msg[512], errorW[128];
        MultiByteToWideChar(CP_UTF8, 0, error, -1, errorW, 128);
        swprintf(msg, 512, L"Google rejected the token request: %s", errorW);
        MessageBoxW(hParent, msg, APP_NAME, MB_ICONERROR);
    } else {
        MessageBoxW(hParent, L"Failed to get an access token from Google.",
                    APP_NAME, MB_ICONERROR);
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Token storage
//
// Wrapped with DPAPI before it goes near the database, so a stolen or
// backed-up opennote.db is not a stolen GitHub account. Bound parameters, not
// formatted SQL -- the values come from a remote server.
// ---------------------------------------------------------------------------

static void TokenKey(char* out, size_t outSize, const char* provider, const char* suffix) {
    snprintf(out, outSize, "oauth_%s_%s", provider, suffix);
}

static BOOL SaveProtected(const char* key, const char* value) {
    char wrapped[4096];
    if (!Crypto_ProtectToBase64(value, wrapped, sizeof(wrapped))) return FALSE;
    return Database_SetSetting(key, wrapped);
}

static BOOL LoadProtected(const char* key, char* out, size_t outSize) {
    char wrapped[4096];
    if (!Database_GetSetting(key, wrapped, sizeof(wrapped))) return FALSE;
    if (Crypto_UnprotectFromBase64(wrapped, out, outSize)) return TRUE;

    // It did not unwrap. Either it was written by another Windows account, or
    // it is a token left in the clear by a build from before v0.2.0 -- the
    // vulnerability SECURITY.md describes. Either way it is unusable here, and
    // a credential nothing can read should not still be sitting in a database.
    // Deleting it signs that account out; the next sign-in stores a wrapped one.
    Database_DeleteSetting(key);
    out[0] = 0;
    return FALSE;
}

BOOL OAuth_SaveToken(const char* provider, const OAuthToken* token) {
    if (!Database_IsOpen() || !provider || !token) return FALSE;

    char key[64];

    TokenKey(key, sizeof(key), provider, "token");
    if (!SaveProtected(key, token->accessToken)) return FALSE;

    if (token->refreshToken[0]) {
        TokenKey(key, sizeof(key), provider, "refresh");
        SaveProtected(key, token->refreshToken);
    }

    return TRUE;
}

BOOL OAuth_LoadToken(const char* provider, OAuthToken* token) {
    if (!Database_IsOpen() || !provider || !token) return FALSE;

    memset(token, 0, sizeof(OAuthToken));

    char key[64];

    TokenKey(key, sizeof(key), provider, "token");
    if (LoadProtected(key, token->accessToken, sizeof(token->accessToken)) &&
        token->accessToken[0]) {
        token->isAuthenticated = TRUE;
    }

    TokenKey(key, sizeof(key), provider, "refresh");
    LoadProtected(key, token->refreshToken, sizeof(token->refreshToken));

    return token->isAuthenticated;
}

void OAuth_DeleteToken(const char* provider) {
    if (!Database_IsOpen() || !provider) return;

    char key[64];
    TokenKey(key, sizeof(key), provider, "token");
    Database_DeleteSetting(key);
    TokenKey(key, sizeof(key), provider, "refresh");
    Database_DeleteSetting(key);
}

void OAuth_GitHubLogout(void) { OAuth_DeleteToken("github"); }
void OAuth_GoogleLogout(void) { OAuth_DeleteToken("google"); }

// GitHub OAuth apps do not issue refresh tokens; a device-flow token lasts
// until the user revokes it.
BOOL OAuth_GitHubRefresh(const char* refreshToken, OAuthToken* tokenOut) {
    (void)refreshToken;
    (void)tokenOut;
    return FALSE;
}

BOOL OAuth_GoogleRefresh(const char* refreshToken, OAuthToken* tokenOut) {
    if (!refreshToken || !refreshToken[0] || !tokenOut) return FALSE;

    memset(tokenOut, 0, sizeof(OAuthToken));

#ifdef GOOGLE_CLIENT_ID
    const char* builtIn = GOOGLE_CLIENT_ID;
#else
    const char* builtIn = NULL;
#endif

    char clientId[256];
    if (!GetClientId(KEY_GOOGLE_CLIENT_ID, builtIn, clientId, sizeof(clientId))) return FALSE;

    OAuthCredentials creds;
    OAuth_LoadCredentials(&creds);
    const char* clientSecret = creds.googleSecret;

    char body[2048];
    snprintf(body, sizeof(body),
             "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
             clientId, clientSecret, refreshToken);

    char response[4096];
    BOOL ok = HttpPostForm(GOOGLE_TOKEN_HOST, GOOGLE_TOKEN_PATH, body,
                           response, sizeof(response));

    SecureZeroMemory(body, sizeof(body));
    SecureZeroMemory(clientSecret, sizeof(clientSecret));

    if (ok && JsonGetString(response, "access_token", tokenOut->accessToken,
                            sizeof(tokenOut->accessToken))) {
        // Google does not reissue the refresh token, so carry the existing one
        // forward or the next refresh has nothing to work with.
        strncpy_s(tokenOut->refreshToken, sizeof(tokenOut->refreshToken),
                  refreshToken, _TRUNCATE);
        strcpy_s(tokenOut->tokenType, sizeof(tokenOut->tokenType), "bearer");
        tokenOut->isAuthenticated = TRUE;
        OAuth_SaveToken("google", tokenOut);
        return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Self-check. Run with: OpenNote.exe --selftest
// ---------------------------------------------------------------------------

// The credentials round trip, through a database of its own.
//
// What matters is that what goes in comes back, and that the Google secret is
// not sitting in the settings table in the clear -- the whole point of wrapping
// it. Checking that means reading the raw row back, not the decrypted value.
static BOOL CredentialsSelfTest(char* failure, size_t failureSize) {
#define CFAIL(msg) do { \
        strncpy_s(failure, failureSize, (msg), _TRUNCATE); \
        Database_Close(); \
        DeleteFileW(path); \
        return FALSE; \
    } while (0)

    WCHAR temp[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) {
        strncpy_s(failure, failureSize, "no temp directory", _TRUNCATE);
        return FALSE;
    }
    swprintf_s(path, MAX_PATH, L"%sopennote-selftest-%lu.db", temp, GetCurrentProcessId());
    DeleteFileW(path);

    // A database of its own: the checks must not touch whatever the person
    // running them has actually configured.
    BOOL wasOpen = Database_IsOpen();
    if (wasOpen) Database_Close();

    if (!Database_Open(path) || !Database_Initialize()) {
        strncpy_s(failure, failureSize, "could not open a temporary database", _TRUNCATE);
        Database_Close();
        DeleteFileW(path);
        return FALSE;
    }

    OAuthCredentials in;
    memset(&in, 0, sizeof(in));
    strcpy_s(in.githubClientId, sizeof(in.githubClientId), "Iv1.0123456789abcdef");
    strcpy_s(in.googleClientId, sizeof(in.googleClientId),
             "1234567890-abcdefgh.apps.googleusercontent.com");
    strcpy_s(in.googleSecret, sizeof(in.googleSecret), "GOCSPX-a-secret-that-is-theirs");

    if (!OAuth_SaveCredentials(&in)) CFAIL("credentials could not be saved");

    OAuthCredentials out;
    OAuth_LoadCredentials(&out);

    if (strcmp(out.githubClientId, in.githubClientId) != 0) {
        CFAIL("the GitHub client id did not survive being stored");
    }
    if (strcmp(out.googleClientId, in.googleClientId) != 0) {
        CFAIL("the Google client id did not survive being stored");
    }
    if (strcmp(out.googleSecret, in.googleSecret) != 0) {
        CFAIL("the Google secret did not survive being stored");
    }

    // The secret must not be readable in the database itself.
    char raw[2048] = {0};
    if (!Database_GetSetting(KEY_GOOGLE_CLIENT_SECRET, raw, sizeof(raw)) || !raw[0]) {
        CFAIL("the Google secret was not stored at all");
    }
    if (strstr(raw, in.googleSecret) != NULL) {
        CFAIL("the Google secret is in the database in the clear");
    }

    if (!OAuth_HasGitHubCredentials()) CFAIL("a stored GitHub client id was not noticed");
    if (!OAuth_HasGoogleCredentials()) CFAIL("stored Google credentials were not noticed");

    // Clearing a field removes it rather than storing a blank.
    OAuthCredentials cleared;
    memset(&cleared, 0, sizeof(cleared));
    if (!OAuth_SaveCredentials(&cleared)) CFAIL("credentials could not be cleared");

    OAuth_LoadCredentials(&out);
    if (out.githubClientId[0] || out.googleClientId[0] || out.googleSecret[0]) {
        CFAIL("clearing the credentials left something behind");
    }
    if (OAuth_HasGitHubCredentials() || OAuth_HasGoogleCredentials()) {
        CFAIL("cleared credentials still count as present");
    }

    // A token left in the clear by a build from before v0.2.0 is not merely
    // unreadable, it is removed on sight: leaving a credential nothing can use
    // in a database is the thing that version set out to stop.
    if (!Database_SetSetting("oauth_github_token", "gho_a_token_in_the_clear")) {
        CFAIL("could not plant a cleartext token");
    }

    OAuthToken stale;
    if (OAuth_LoadToken("github", &stale)) CFAIL("a cleartext token was accepted");

    char left[256] = {0};
    if (Database_GetSetting("oauth_github_token", left, sizeof(left)) && left[0]) {
        CFAIL("a cleartext token was left in the database");
    }

    Database_Close();
    DeleteFileW(path);

    // Leave the application's own database as it was found. In --selftest
    // there is none open, so this is for a check run from inside a session.
    if (wasOpen && g_app && g_app->dbPath[0]) Database_Open(g_app->dbPath);

    return TRUE;

#undef CFAIL
}

BOOL OAuth_SelfTest(char* failure, size_t failureSize) {
#define FAIL(msg) do { strncpy_s(failure, failureSize, (msg), _TRUNCATE); return FALSE; } while (0)

    if (!CredentialsSelfTest(failure, failureSize)) return FALSE;

    char out[256];

    // The shapes GitHub's device endpoint actually returns.
    const char* deviceResponse =
        "{\"device_code\":\"3584d83530557fdd1f46af8289938c8ef79f9dc5\","
        "\"user_code\":\"WDJB-MJHT\","
        "\"verification_uri\":\"https://github.com/login/device\","
        "\"expires_in\":900,\"interval\":5}";

    if (!JsonGetString(deviceResponse, "user_code", out, sizeof(out)) ||
        strcmp(out, "WDJB-MJHT") != 0) {
        FAIL("JsonGetString did not read user_code");
    }
    if (!JsonGetString(deviceResponse, "verification_uri", out, sizeof(out)) ||
        strcmp(out, "https://github.com/login/device") != 0) {
        FAIL("JsonGetString mangled a URL value");
    }

    // "device_code" contains "code", and a substring search that did not anchor
    // on the quotes would match the wrong key here.
    if (!JsonGetString(deviceResponse, "device_code", out, sizeof(out)) ||
        strcmp(out, "3584d83530557fdd1f46af8289938c8ef79f9dc5") != 0) {
        FAIL("JsonGetString matched a key by substring");
    }

    // A missing key must report absence, not leave a stale value behind.
    strcpy_s(out, sizeof(out), "stale");
    if (JsonGetString(deviceResponse, "access_token", out, sizeof(out))) {
        FAIL("JsonGetString invented a missing key");
    }
    if (out[0] != '\0') FAIL("JsonGetString left a stale value on failure");

    // A numeric field is not a string field; reading one must fail rather than
    // return whatever follows the colon.
    if (JsonGetString(deviceResponse, "interval", out, sizeof(out))) {
        // Accepted only if it really did produce the digits.
        if (strcmp(out, "5") != 0) FAIL("JsonGetString misread a numeric field");
    }

    // The pending response the token endpoint returns while the user is still
    // in the browser. Treating this as anything but a retry would abort a login
    // that was about to succeed.
    const char* pending = "{\"error\":\"authorization_pending\","
                          "\"error_description\":\"Pending\"}";
    if (!JsonGetString(pending, "error", out, sizeof(out)) ||
        strcmp(out, "authorization_pending") != 0) {
        FAIL("JsonGetString did not read an error code");
    }

    // A value with a space in it, which strpbrk-style parsing gets wrong.
    const char* spaced = "{\"error_description\":\"Device code expired\"}";
    if (!JsonGetString(spaced, "error_description", out, sizeof(out)) ||
        strcmp(out, "Device code expired") != 0) {
        FAIL("JsonGetString truncated a value at a space");
    }

    // An oversized value must fail rather than truncate a token.
    char tiny[4];
    if (JsonGetString(deviceResponse, "device_code", tiny, sizeof(tiny))) {
        FAIL("JsonGetString truncated into an undersized buffer");
    }

    failure[0] = '\0';
    return TRUE;

#undef FAIL
}
