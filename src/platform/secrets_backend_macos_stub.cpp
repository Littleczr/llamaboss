// secrets_backend_macos_stub.cpp — macOS Keychain stub (Phase 0: no-op in-memory)
#include "secrets_backend.h"

#include <unordered_map>
#include <mutex>

namespace secrets_backend {

static std::unordered_map<std::string, std::string> g_store;
static std::mutex g_mutex;

bool Initialize() { return true; }

bool Set(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_store[std::string(key)] = std::string(value);
    return true;
}

std::optional<std::string> Get(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_store.find(std::string(key));
    if (it == g_store.end()) return std::nullopt;
    return it->second;
}

bool Delete(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_store.erase(std::string(key)) > 0;
}

std::vector<std::string> ListKeys() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<std::string> keys;
    keys.reserve(g_store.size());
    for (const auto& kv : g_store) keys.push_back(kv.first);
    return keys;
}

} // namespace secrets_backend