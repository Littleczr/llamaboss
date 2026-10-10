// chatgpt_auth.h
//
// "Sign in with ChatGPT" for LlamaBoss: lets a ChatGPT Plus/Pro user run
// LlamaBoss requests on their ChatGPT plan instead of an API key.
//
// What this owns
// --------------
//   * The one-time agent registration and every later sign-in: system
//     browser + loopback callback on 127.0.0.1, PKCE S256, state, OIDC
//     nonce, RS256 ID-token verification against OpenAI's JWKS.
//   * The credential file:
//         %LOCALAPPDATA%\LlamaBoss\chatgpt_accounts.json
//     Non-secret fields (issued client id, email, granted scopes,
//     expiry, this computer's stable host id) are plain JSON.  The ID,
//     access and refresh tokens are encrypted with Windows DPAPI
//     (CryptProtectData, current-user scope), so the file is useless
//     when copied to another Windows account or machine.  The refresh
//     token is a long-lived credential for the user's ChatGPT account.
//   * Access-token refresh.  Access tokens live one hour; refresh
//     tokens rotate on every use (30-day lifetime each).  Refreshes are
//     serialized process-wide so two windows can never race a rotating
//     token -- the loser would present a token that was just retired
//     and the server would revoke the whole grant ("refresh_token_reused").
//
// Threading
// ---------
//   * Init() and BeginSignIn() run on the UI thread.
//   * CompleteSignIn(), GetAccessToken() and RevokeAsync's worker block
//     on the network and MUST run on a worker thread.
//   * Everything else is a short, mutex-guarded in-memory read and is
//     safe on any thread.  No method holds the data lock across a
//     network call, so the UI never waits on a refresh in progress.
//
// Identity model (from the siwc docs)
// -----------------------------------
//   client id : the issued "oaiapp_..." id returned by the FIRST sign-in
//               for one ChatGPT account + workspace.  It is the account
//               key everywhere in LlamaBoss (endpoints.json stores it as
//               "chatgpt_account").  "dynamic_agent_client" is only the
//               registration entry point and is never stored.
//   host id   : one opaque "urn:uuid:..." per computer, created once and
//               reused for every account and every sign-in.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lb_chatgpt {

struct AccountSummary {
    std::string clientId;       // issued id, the account key
    std::string email;
    std::string name;
    bool signedIn = false;      // a refresh token is on file
    bool planEnabled = false;   // chatgpt.tokens.use.direct was granted
};

// One in-progress browser sign-in.  Opaque outside chatgpt_auth.cpp.
struct SignInAttempt;
// Process-wide state behind Auth.  Opaque outside chatgpt_auth.cpp.
struct AuthImpl;

struct SignInResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;          // readable, never contains a token
    AccountSummary account;
};

class Auth {
public:
    static Auth& Get();

    // UI thread, before any other use: the folder that holds
    // endpoints.json.  Loads the credential file (missing = empty).
    // Idempotent; later calls with the same folder are no-ops.
    void Init(const std::string& dataDir);
    bool IsInitialized() const;

    std::vector<AccountSummary> Accounts() const;
    bool FindAccount(const std::string& clientId, AccountSummary& out) const;

    // UI thread.  Binds the loopback listener and builds the authorize
    // URL to open in the browser.  An empty reauthClientId registers a
    // NEW agent for whichever account the user picks; a saved client id
    // re-authorizes that account (no consent screen, same registration).
    // Returns nullptr with `error` set on failure.
    std::shared_ptr<SignInAttempt> BeginSignIn(const std::string& reauthClientId,
                                               std::string& authorizeUrl,
                                               std::string& error);

    // Worker thread.  Waits (up to 10 minutes) for the browser to come
    // back, then exchanges the code, verifies the ID token, checks the
    // plan scope, and saves the account.  Returns cancelled=true when
    // CancelSignIn() ran first.
    SignInResult CompleteSignIn(const std::shared_ptr<SignInAttempt>& attempt);

    // Any thread.  Makes CompleteSignIn return within ~250 ms.
    static void CancelSignIn(const std::shared_ptr<SignInAttempt>& attempt);

    // Worker thread.  A bearer token for the plan route, refreshed when
    // it is within two minutes of expiry.  On failure `error` is a
    // complete sentence for the chat error card.
    bool GetAccessToken(const std::string& clientId,
                        std::string& accessToken,
                        std::string& error);

    // Any thread.  The server rejected the current access token (HTTP
    // 401); the next GetAccessToken() refreshes instead of reusing it.
    void InvalidateAccessToken(const std::string& clientId);

    // UI thread.  Forgets this account's tokens immediately (the issued
    // client id and email stay, so signing in again reuses the same
    // registration), then revokes the refresh token on a detached
    // worker.  Revocation is best effort; the user can always
    // disconnect LlamaBoss under ChatGPT Settings > Security.
    void SignOut(const std::string& clientId);

private:
    Auth() = default;
    Auth(const Auth&) = delete;
    Auth& operator=(const Auth&) = delete;

    // Created on the first Init(); never replaced afterwards, so worker
    // threads may read the pointer without the init lock once
    // IsInitialized() has returned true.
    std::shared_ptr<AuthImpl> m_impl;
    mutable std::mutex m_initMutex;
    std::shared_ptr<AuthImpl> Impl() const;
};

} // namespace lb_chatgpt
