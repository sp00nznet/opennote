#ifndef OAUTH_H
#define OAUTH_H

#include <windows.h>

// OAuth state
typedef struct {
    char accessToken[1024];
    char refreshToken[1024];
    char tokenType[32];
    int expiresIn;
    BOOL isAuthenticated;
} OAuthToken;

// GitHub OAuth
BOOL OAuth_GitHubLogin(HWND hParent, OAuthToken* tokenOut);
BOOL OAuth_GitHubRefresh(const char* refreshToken, OAuthToken* tokenOut);
void OAuth_GitHubLogout(void);

// Google OAuth
BOOL OAuth_GoogleLogin(HWND hParent, OAuthToken* tokenOut);
BOOL OAuth_GoogleRefresh(const char* refreshToken, OAuthToken* tokenOut);
void OAuth_GoogleLogout(void);

// The OAuth application the user registered for themselves.
//
// Nothing here ships with the program: no key is compiled into a released
// build, and the settings screen is how an installed copy is given one. A
// client id is a public identifier and is stored as it is; Google's desktop
// "client secret" is not confidential in the RFC 8252 sense but belongs to the
// user's own project, so it is wrapped with DPAPI like a token.
typedef struct {
    char githubClientId[256];
    char googleClientId[256];
    char googleSecret[512];
} OAuthCredentials;

// Load what the user entered. The Google secret comes back decrypted. Fields
// nobody has filled in come back empty, which is not a failure.
void OAuth_LoadCredentials(OAuthCredentials* out);

// Store them. An empty field clears that setting rather than writing a blank,
// so clearing a credential really removes it.
BOOL OAuth_SaveCredentials(const OAuthCredentials* creds);

// Is there enough to attempt a sign-in? GitHub's device flow needs only a
// client id; Google needs both halves of its own project's credentials.
BOOL OAuth_HasGitHubCredentials(void);
BOOL OAuth_HasGoogleCredentials(void);

// Token storage
BOOL OAuth_SaveToken(const char* provider, const OAuthToken* token);
BOOL OAuth_LoadToken(const char* provider, OAuthToken* token);
void OAuth_DeleteToken(const char* provider);

// Self-check, run by `OpenNote.exe --selftest`. Returns FALSE and fills
// `failure` with the first check that did not hold.
BOOL OAuth_SelfTest(char* failure, size_t failureSize);

#endif // OAUTH_H
