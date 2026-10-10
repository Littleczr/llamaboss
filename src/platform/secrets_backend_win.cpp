// secrets_backend_win.cpp — Windows DPAPI implementation
#include "secrets_backend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "crypt32.lib")

namespace secrets_backend {

static std::mutex g_mutex;

static std::string GetSecretsFilePath() {
    PWSTR pszPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
    if (FAILED(hr) || !pszPath) return "";

    std::filesystem::path p(pszPath);
    CoTaskMemFree(pszPath);
    p /= "LlamaBoss";
    p /= "secrets.dat";
    return p.u8string();
}

static bool EnsureDirectory(const std::string& path) {
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    return !ec;
}

static std::vector<BYTE> ProtectData(const std::vector<BYTE>& data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{0, nullptr};
    if (!CryptProtectData(&in, L"LlamaBoss Secrets", nullptr, nullptr, nullptr, 0, &out)) {
        return {};
    }
    std::vector<BYTE> result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

static std::vector<BYTE> UnprotectData(const std::vector<BYTE>& data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{0, nullptr};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return {};
    }
    std::vector<BYTE> result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

bool Initialize() {
    std::string path = GetSecretsFilePath();
    if (path.empty()) return false;
    return EnsureDirectory(path);
}

bool Set(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lk(g_mutex);

    std::string path = GetSecretsFilePath();
    if (path.empty()) return false;

    // Read existing data
    std::vector<BYTE> encrypted;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            size_t size = in.tellg();
            in.seekg(0, std::ios::beg);
            encrypted.resize(size);
            in.read(reinterpret_cast<char*>(encrypted.data()), size);
        }
    }

    // Decrypt
    std::vector<BYTE> decrypted = UnprotectData(encrypted);
    if (decrypted.empty() && !encrypted.empty()) return false; // Corrupted

    // Parse as simple key=value\n format
    std::string content(decrypted.begin(), decrypted.end());
    std::string newContent;
    bool found = false;
    std::string searchKey = std::string(key) + "=";

    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (line.rfind(searchKey, 0) == 0) {
            newContent += searchKey + std::string(value) + "\n";
            found = true;
        } else {
            newContent += line + "\n";
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (!found) {
        newContent += searchKey + std::string(value) + "\n";
    }

    // Encrypt and write
    std::vector<BYTE> newEncrypted = ProtectData(
        std::vector<BYTE>(newContent.begin(), newContent.end()));
    if (newEncrypted.empty()) return false;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(newEncrypted.data()), newEncrypted.size());
    return out.good();
}

std::optional<std::string> Get(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);

    std::string path = GetSecretsFilePath();
    if (path.empty()) return std::nullopt;

    std::vector<BYTE> encrypted;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        in.seekg(0, std::ios::end);
        size_t size = in.tellg();
        in.seekg(0, std::ios::beg);
        encrypted.resize(size);
        in.read(reinterpret_cast<char*>(encrypted.data()), size);
    }
    if (encrypted.empty()) return std::nullopt;

    std::vector<BYTE> decrypted = UnprotectData(encrypted);
    if (decrypted.empty()) return std::nullopt;

    std::string content(decrypted.begin(), decrypted.end());
    std::string searchKey = std::string(key) + "=";

    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (line.rfind(searchKey, 0) == 0) {
            return line.substr(searchKey.size());
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return std::nullopt;
}

bool Delete(std::string_view key) {
    std::lock_guard<std::mutex> lk(g_mutex);

    std::string path = GetSecretsFilePath();
    if (path.empty()) return false;

    std::vector<BYTE> encrypted;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return true; // Nothing to delete
        in.seekg(0, std::ios::end);
        size_t size = in.tellg();
        in.seekg(0, std::ios::beg);
        encrypted.resize(size);
        in.read(reinterpret_cast<char*>(encrypted.data()), size);
    }
    if (encrypted.empty()) return true;

    std::vector<BYTE> decrypted = UnprotectData(encrypted);
    if (decrypted.empty()) return false;

    std::string content(decrypted.begin(), decrypted.end());
    std::string newContent;
    std::string searchKey = std::string(key) + "=";

    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (line.rfind(searchKey, 0) != 0) {
            newContent += line + "\n";
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    std::vector<BYTE> newEncrypted = ProtectData(
        std::vector<BYTE>(newContent.begin(), newContent.end()));
    if (newEncrypted.empty()) return false;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(newEncrypted.data()), newEncrypted.size());
    return out.good();
}

std::vector<std::string> ListKeys() {
    std::lock_guard<std::mutex> lk(g_mutex);

    std::string path = GetSecretsFilePath();
    if (path.empty()) return {};

    std::vector<BYTE> encrypted;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        in.seekg(0, std::ios::end);
        size_t size = in.tellg();
        in.seekg(0, std::ios::beg);
        encrypted.resize(size);
        in.read(reinterpret_cast<char*>(encrypted.data()), size);
    }
    if (encrypted.empty()) return {};

    std::vector<BYTE> decrypted = UnprotectData(encrypted);
    if (decrypted.empty()) return {};

    std::string content(decrypted.begin(), decrypted.end());
    std::vector<std::string> keys;

    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            keys.push_back(line.substr(0, eq));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return keys;
}

} // namespace secrets_backend