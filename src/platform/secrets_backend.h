#pragma once
// secrets_backend.h — Encrypted secrets storage abstraction
// Windows: DPAPI (CryptProtectData / CryptUnprotectData)
// macOS: Keychain (SecItemAdd / SecItemCopyMatching / SecItemDelete)
// Linux: libsecret (secret_password_store / secret_password_lookup) or encrypted file fallback

#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace secrets_backend {

// Initialize backend. Returns false on fatal error (e.g., Keychain unavailable and no fallback).
bool Initialize();

// Store a secret (key-value). Overwrites existing.
bool Set(std::string_view key, std::string_view value);

// Retrieve a secret. Returns nullopt if not found.
std::optional<std::string> Get(std::string_view key);

// Delete a secret.
bool Delete(std::string_view key);

// List all secret keys.
std::vector<std::string> ListKeys();

} // namespace secrets_backend