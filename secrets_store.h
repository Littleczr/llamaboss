// secrets_store.h
//
// Phase 1 secrets layer for service-tool API keys.
//
// SecretsStore owns one JSON file at:
//     %LOCALAPPDATA%\LlamaBoss\secrets.json
//
// The file shape mirrors OpenClaw's openclaw.json — a flat mapping
// from provider name to a small {key: value} map.  Values may be
// raw strings or env-var indirections of the form {"$env": "NAME"}.
//
//     {
//       "version": 1,
//       "providers": {
//         "gmail":      { "api_key": "sk-abc123" },
//         "smartsheet": { "api_key": { "$env": "SMARTSHEET_TOKEN" } }
//       }
//     }
//
// Storage policy for Phase 1 follows OpenClaw's threat model:
// plaintext on disk, file ACL inherited from the user-only
// %LOCALAPPDATA%\LlamaBoss directory, no OS-keychain encryption.
// SecretRef ($env) lets users keep keys out of the JSON when they
// prefer.  DPAPI encryption is a deliberate Phase 2 option, not a
// Phase 1 requirement.
//
// Lifetime:
//   * Owned by AppState (one per process).
//   * PythonRunner holds a non-owning pointer for env-var injection.
//   * ConnectionsDialog mutates through Set/Remove and calls Save().
//
// Thread-safety:
//   The store is intended for UI-thread mutation.  Reads from worker
//   threads (PythonRunner's spawn path) must happen via the snapshot
//   helpers (BuildEnvInjections) on the UI thread before the worker
//   is started.  Do NOT call SetSecret/RemoveSecret/Save from worker
//   threads.
//
#pragma once

#include <string>
#include <vector>
#include <map>

class SecretsStore
{
public:
    SecretsStore() = default;
    ~SecretsStore() = default;

    // Returns the absolute path to secrets.json (creates parent dirs
    // on first call; never creates the file itself).
    static std::string GetSecretsFilePath();

    // Load from disk.  Missing file is not an error — the store
    // simply starts empty.  Malformed JSON logs a warning and the
    // store starts empty so the user can re-enter credentials.
    bool Load();

    // Atomically rewrite secrets.json from the in-memory map.
    // Writes to a temp file alongside, then renames.
    bool Save();

    // ── Per-secret access ────────────────────────────────────────
    // Resolves $env / raw / (future) $file references.  Returns the
    // empty string when no secret is configured or the indirection
    // points at a missing env var.
    std::string GetSecret(const std::string& provider,
                          const std::string& key) const;

    // Raw setter — stores the literal value.  Use SetSecretEnvRef
    // when the user picks "use env var" in the UI.
    void SetSecret(const std::string& provider,
                   const std::string& key,
                   const std::string& value);

    // Stores {"$env": envVarName} as the value.  Resolution happens
    // at read time via GetSecret.
    void SetSecretEnvRef(const std::string& provider,
                         const std::string& key,
                         const std::string& envVarName);

    // Removes one key under a provider.  Removes the whole provider
    // entry if it becomes empty.
    void RemoveSecret(const std::string& provider,
                      const std::string& key);

    // Removes an entire provider and every key under it.
    void RemoveProvider(const std::string& provider);

    // ── UI helpers ───────────────────────────────────────────────
    // Snapshot of (provider, key, isEnvRef, displayHint) tuples for
    // the Connections dialog.  isEnvRef is true when the stored
    // value is a $env reference; displayHint is the env var name for
    // refs and a masked "••••••••" for raw values.
    struct ConnectionRow {
        std::string provider;
        std::string key;
        bool        isEnvRef = false;
        std::string displayHint;   // env var name or masked dots
    };
    std::vector<ConnectionRow> ListConnections() const;

    // ── Worker integration ───────────────────────────────────────
    // Build the env-var injections for a python_run_script call.
    // Returns a snapshot vector of (env_var_name, resolved_value)
    // pairs covering every configured provider/key pair.
    //
    // Naming convention: uppercase the provider name, append the
    // uppercased key, and normalize non-alphanumeric characters to
    // underscores so UI-friendly names still become usable env vars. Examples:
    //     gmail.api_key      -> GMAIL_API_KEY
    //     smartsheet.token   -> SMARTSHEET_TOKEN
    //     runpod.api_key     -> RUNPOD_API_KEY
    //
    // Empty values are omitted from the result (an env ref pointing
    // at a missing env var won't shadow whatever the user has set
    // in their shell).
    //
    // Phase 2 will narrow this to "only providers declared in the
    // running skill's `## Connections` section."
    std::vector<std::pair<std::string, std::string>>
        BuildEnvInjections() const;

private:
    // provider -> (key -> value-as-JSON-string).  We store the raw
    // JSON sub-tree so $env refs survive round-trips without us
    // having to introduce a value-variant type.  When a value is a
    // plain string the JSON is `"the value"`; when a $env ref it's
    // `{"$env":"NAME"}`.
    std::map<std::string,
             std::map<std::string, std::string>> m_providers;

    bool m_loaded = false;
};
