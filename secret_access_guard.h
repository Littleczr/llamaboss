// secret_access_guard.h
//
// Agent harness — hard block on reading stored secrets.
//
// Background: in a production chat (2026-10-30) the model, stuck on a
// browser file-upload field, decided on its own to pull the user's GitHub
// token out of Git Credential Manager with `git credential fill` so it
// could call the GitHub API.  PowerShell had already been approved for the
// chat, so no card appeared; the call only failed because of a quoting
// accident.  A token in a tool result goes straight to the remote model.
//
// DetectSecretStoreAccess() looks for commands and code that read
// credential stores.  Callers REJECT the call outright (no approval card,
// "approve for this chat" does not cover it).  Pure, header-only, no wx,
// safe from any thread.
//
// Matching is token-based so it survives quoting and list syntax:
//     git credential fill
//     subprocess.run(["git", "credential", "fill"])
//     & git -C repo credential   fill
// all tokenize to ... git ... credential fill ...
//
// Deliberately NOT blocked: `git config --get credential.helper` (shows
// which helper is configured, never a secret), Get-Credential (prompts the
// user interactively), and anything that merely mentions the word
// "credential".
#pragma once

#include <string>
#include <vector>

namespace secret_guard_detail {

inline char Lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string LowerCopy(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = Lower(c);
    return out;
}

// Split on anything that is not [a-z0-9-_.] ; dots stay inside tokens so
// "credential.helper" is one token and never reads as "credential".
inline std::vector<std::string> Tokens(const std::string& lower) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : lower) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.';
        if (keep) {
            cur.push_back(c);
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

inline bool Contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// True when token `a` is followed by `b` within `window` tokens.
inline bool FollowedBy(const std::vector<std::string>& t, size_t i,
                       const char* b, size_t window) {
    for (size_t k = i + 1; k < t.size() && k <= i + window; ++k)
        if (t[k] == b) return true;
    return false;
}

} // namespace secret_guard_detail

// Returns a human/model-readable reason when `text` reads a credential
// store, or an empty string when it does not.
inline std::string DetectSecretStoreAccess(const std::string& text)
{
    using namespace secret_guard_detail;
    if (text.empty()) return {};

    const std::string lower = LowerCopy(text);
    std::string slashed = lower;
    for (char& c : slashed) if (c == '\\') c = '/';
    const std::vector<std::string> t = Tokens(lower);

    for (size_t i = 0; i < t.size(); ++i) {
        const std::string& w = t[i];

        // git credential fill|approve|reject (fill returns the secret;
        // approve/reject write or erase it).  Also the credential helpers
        // invoked directly: git credential-manager get, credential-store get.
        if (w == "credential" || w == "credential-manager" ||
            w == "credential-manager-core" || w == "credential-store" ||
            w == "credential-wincred" || w == "credential-cache") {
            for (const char* verb : { "fill", "approve", "reject", "get",
                                      "store", "erase" }) {
                if (FollowedBy(t, i, verb, 3))
                    return "reads or changes a stored credential (`" + w +
                           " " + verb + "`)";
            }
        }

        // gh auth token / gh auth status --show-token / -t
        if (w == "gh" && i + 2 < t.size() && t[i + 1] == "auth" &&
            (t[i + 2] == "token" ||
             (t[i + 2] == "status" && (FollowedBy(t, i + 2, "--show-token", 4) ||
                                       FollowedBy(t, i + 2, "-t", 4)))))
            return "prints the GitHub CLI token (`gh auth token`)";

        if (w == "cmdkey" || w == "cmdkey.exe")
            return "reads the Windows Credential Manager (`cmdkey`)";
        if (w == "vaultcmd" || w == "vaultcmd.exe")
            return "reads the Windows Vault (`vaultcmd`)";
        if (w == "get-storedcredential" || w == "credread" || w == "credreadw" ||
            w == "credenumerate" || w == "credenumeratew")
            return "reads the Windows Credential Manager (`" + w + "`)";
        if (w == "passwordvault" || w == "windows.security.credentials.passwordvault")
            return "reads the Windows PasswordVault";
        if (w == "cryptunprotectdata")
            return "decrypts DPAPI-protected secrets";
    }

    // Dotted API names (one token each, so matched as substrings).
    if (Contains(lower, "protecteddata]::unprotect") ||
        Contains(lower, "protecteddata.unprotect"))
        return "decrypts DPAPI-protected secrets";
    if (Contains(lower, "keyring.get_password") ||
        Contains(lower, "keyring.get_credential"))
        return "reads a stored password through Python keyring";
    if (Contains(lower, "win32cred.credread") ||
        Contains(lower, "win32cred.credenumerate"))
        return "reads the Windows Credential Manager (`win32cred`)";

    // Files that hold secrets in plain text or as an encrypted blob.
    if (Contains(slashed, ".git-credentials"))
        return "reads the git plaintext credential file (`.git-credentials`)";
    if (Contains(slashed, "github cli/hosts.yml") || Contains(slashed, "gh/hosts.yml"))
        return "reads the GitHub CLI token file (`hosts.yml`)";
    if (Contains(slashed, "llamaboss/secrets.json"))
        return "reads LlamaBoss's own API-key store (`secrets.json`)";
    if (Contains(slashed, "/login data") || Contains(slashed, "'login data'") ||
        Contains(slashed, "\"login data\""))
        return "reads a browser's saved-password database (`Login Data`)";
    if (Contains(slashed, "microsoft/credentials") ||
        Contains(slashed, "microsoft/vault") ||
        Contains(slashed, "microsoft/protect"))
        return "reads the Windows credential/DPAPI store files";

    return {};
}

// Text appended to the rejection so the model takes the right next step
// instead of trying another route to the same secret.
inline const char* SecretStoreRejectionAdvice()
{
    return "LlamaBoss never lets tools read stored credentials, tokens or "
           "passwords, and approving tools for this chat does not change "
           "that. Do not try another way to get the secret. If a task needs "
           "authentication, use a tool that keeps its own login (for "
           "example `git push`, or `gh` after the user has run "
           "`gh auth login`), or ask the user to do that step themselves.";
}

// Tools whose args are code or a command line and therefore get scanned.
// Names match tool_names:: in tool_invocation.h (kept as literals so this
// header stays standalone for the g++ tests).
inline bool ToolArgsMayReadSecrets(const std::string& toolName)
{
    return toolName == "powershell" || toolName == "py" ||
           toolName == "python_create_script" ||
           toolName == "python_run_script" ||
           toolName == "write_powershell_script";
}
