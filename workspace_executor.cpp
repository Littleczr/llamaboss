// workspace_executor.cpp
// Implementation of workspace file search and chat saving.

#include "workspace_executor.h"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <ctime>
#include <regex>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════
//  Internal helpers
// ═══════════════════════════════════════════════════════════════════

// Convert a string to lowercase (ASCII-safe).
static std::string ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Split a string on whitespace into non-empty tokens.
static std::vector<std::string> SplitTerms(const std::string& query)
{
    std::vector<std::string> terms;
    std::istringstream stream(query);
    std::string word;
    while (stream >> word) {
        terms.push_back(ToLower(word));
    }
    return terms;
}

// Check if `haystack` contains `needle` (both already lowercase).
static bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ═══════════════════════════════════════════════════════════════════
//  ExecuteWorkspaceSearch
// ═══════════════════════════════════════════════════════════════════

WorkspaceSearchResult ExecuteWorkspaceSearch(
    const std::string& query,
    const std::string& workspacePath,
    int maxResults)
{
    WorkspaceSearchResult result;
    result.success = false;
    result.totalFilesScanned = 0;

    // ── Validate workspace path ──────────────────────────────────
    std::error_code ec;
    fs::path wsRoot(workspacePath);

    if (!fs::exists(wsRoot, ec)) {
        result.errorMessage = "Workspace folder does not exist: " + workspacePath;
        return result;
    }

    if (!fs::is_directory(wsRoot, ec)) {
        result.errorMessage = "Workspace path is not a directory: " + workspacePath;
        return result;
    }

    // ── Parse query into search terms ────────────────────────────
    std::vector<std::string> terms = SplitTerms(query);
    if (terms.empty()) {
        result.errorMessage = "Search query is empty.";
        return result;
    }

    // ── Recursively scan the workspace ───────────────────────────
    try {
        for (auto it = fs::recursive_directory_iterator(wsRoot, 
                 fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it)
        {
            if (ec) {
                // Skip entries that cause errors (permission denied, etc.)
                ec.clear();
                continue;
            }

            const auto& entry = *it;

            // Only match regular files (skip directories, symlinks, etc.)
            if (!entry.is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }

            result.totalFilesScanned++;

            // Check filename against search terms (ANY term matches)
            std::string fileName = entry.path().filename().string();
            std::string fileNameLower = ToLower(fileName);

            bool matched = false;
            for (const auto& term : terms) {
                if (Contains(fileNameLower, term)) {
                    matched = true;
                    break;
                }
            }

            if (!matched)
                continue;

            // ── Build the match entry ────────────────────────────
            WorkspaceFileMatch match;
            match.fileName = fileName;

            // Compute relative path from workspace root
            fs::path relPath = fs::relative(entry.path(), wsRoot, ec);
            match.relativePath = ec ? fileName : relPath.string();
            ec.clear();

            // Get file size (0 if unavailable)
            match.fileSize = entry.file_size(ec);
            if (ec) {
                match.fileSize = 0;
                ec.clear();
            }

            result.matches.push_back(std::move(match));

            // Stop if we hit the cap
            if (static_cast<int>(result.matches.size()) >= maxResults)
                break;
        }
    }
    catch (const fs::filesystem_error& ex) {
        result.errorMessage = std::string("Filesystem error: ") + ex.what();
        return result;
    }
    catch (const std::exception& ex) {
        result.errorMessage = std::string("Error during search: ") + ex.what();
        return result;
    }

    // ── Sort matches by relative path ────────────────────────────
    std::sort(result.matches.begin(), result.matches.end(),
              [](const WorkspaceFileMatch& a, const WorkspaceFileMatch& b) {
                  return a.relativePath < b.relativePath;
              });

    result.success = true;
    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  FormatFileSize
// ═══════════════════════════════════════════════════════════════════

std::string FormatFileSize(std::uintmax_t bytes)
{
    if (bytes == 0)
        return "0 bytes";

    const char* units[] = { "bytes", "KB", "MB", "GB" };
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 3) {
        size /= 1024.0;
        unitIndex++;
    }

    std::ostringstream oss;
    if (unitIndex == 0) {
        // bytes — no decimals
        oss << bytes << " bytes";
    } else if (size >= 100.0) {
        // large numbers — no decimals
        oss << static_cast<int>(size + 0.5) << " " << units[unitIndex];
    } else if (size >= 10.0) {
        // medium — one decimal
        oss << std::fixed << std::setprecision(1) << size << " " << units[unitIndex];
    } else {
        // small — one decimal
        oss << std::fixed << std::setprecision(1) << size << " " << units[unitIndex];
    }

    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  FormatSearchResult
// ═══════════════════════════════════════════════════════════════════

std::string FormatSearchResult(const WorkspaceSearchResult& result,
                               const std::string& query,
                               const std::string& workspacePath)
{
    std::ostringstream oss;

    if (!result.success) {
        oss << "Workspace search failed.\n"
            << "  " << result.errorMessage;
        return oss.str();
    }

    if (result.matches.empty()) {
        oss << "No files found matching \"" << query << "\"\n"
            << "  Scanned " << result.totalFilesScanned
            << " file" << (result.totalFilesScanned != 1 ? "s" : "")
            << " in " << workspacePath;
        return oss.str();
    }

    // Header
    oss << "Found " << result.matches.size()
        << " file" << (result.matches.size() != 1 ? "s" : "")
        << " matching \"" << query << "\":\n";

    // File list
    for (size_t i = 0; i < result.matches.size(); ++i) {
        const auto& m = result.matches[i];
        oss << "  " << (i + 1) << ". " << m.relativePath;
        if (m.fileSize > 0)
            oss << "  (" << FormatFileSize(m.fileSize) << ")";
        oss << "\n";
    }

    // Footer
    oss << "Scanned " << result.totalFilesScanned
        << " file" << (result.totalFilesScanned != 1 ? "s" : "")
        << " in " << workspacePath;

    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  Save Chat helpers
// ═══════════════════════════════════════════════════════════════════

// Generate a timestamp string like "2026-04-05_103045"
static std::string TimestampNow()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d_%H%M%S");
    return oss.str();
}

// Generate a human-readable date string like "April 5, 2026 at 10:30 AM"
static std::string DateTimeNow()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%B %d, %Y at %I:%M %p");
    return oss.str();
}

// Sanitize a user-provided name for use as a filename.
// Replaces spaces with underscores, strips anything not alphanumeric/underscore/hyphen.
static std::string SanitizeFilename(const std::string& name)
{
    std::string result;
    result.reserve(name.size());

    for (char c : name) {
        if (c == ' ' || c == '\t')
            result += '_';
        else if (std::isalnum(static_cast<unsigned char>(c)) ||
                 c == '_' || c == '-' || c == '.')
            result += c;
        // else: skip the character
    }

    // Trim leading/trailing underscores
    size_t start = result.find_first_not_of('_');
    size_t end = result.find_last_not_of('_');
    if (start == std::string::npos)
        return "";
    return result.substr(start, end - start + 1);
}

// ═══════════════════════════════════════════════════════════════════
//  ExecuteWorkspaceSaveChat
// ═══════════════════════════════════════════════════════════════════

WorkspaceSaveChatResult ExecuteWorkspaceSaveChat(
    const std::vector<ChatMessage>& messages,
    const std::string& workspacePath,
    const std::string& customName,
    const std::string& primaryModel)
{
    WorkspaceSaveChatResult result;
    result.success = false;
    result.messageCount = 0;

    // ── Validate input ───────────────────────────────────────────
    if (messages.empty()) {
        result.errorMessage = "No messages to save.";
        return result;
    }

    // ── Ensure workspace folder exists (create if needed) ────────
    std::error_code ec;
    fs::path wsRoot(workspacePath);

    if (!fs::exists(wsRoot, ec)) {
        fs::create_directories(wsRoot, ec);
        if (ec) {
            result.errorMessage = "Could not create workspace folder: " + ec.message();
            return result;
        }
    }

    if (!fs::is_directory(wsRoot, ec)) {
        result.errorMessage = "Workspace path is not a directory: " + workspacePath;
        return result;
    }

    // ── Build filename ───────────────────────────────────────────
    std::string baseName;
    if (!customName.empty()) {
        baseName = SanitizeFilename(customName);
        // If sanitization removed everything, fall back to timestamp
        if (baseName.empty())
            baseName = "chat_" + TimestampNow();
    } else {
        baseName = "chat_" + TimestampNow();
    }

    // Strip .md if the user included it (we add it ourselves)
    if (baseName.size() > 3 && baseName.substr(baseName.size() - 3) == ".md")
        baseName = baseName.substr(0, baseName.size() - 3);

    std::string fileName = baseName + ".md";
    fs::path filePath = wsRoot / fileName;

    // ── If file already exists, append a number ──────────────────
    int suffix = 1;
    while (fs::exists(filePath, ec)) {
        fileName = baseName + "_" + std::to_string(suffix) + ".md";
        filePath = wsRoot / fileName;
        ++suffix;
        if (suffix > 999) {
            result.errorMessage = "Too many files with similar names.";
            return result;
        }
    }

    // ── Format the conversation as markdown ──────────────────────
    std::ostringstream md;

    md << "# Chat Export\n\n";
    md << "**Date:** " << DateTimeNow() << "\n";
    if (!primaryModel.empty())
        md << "**Model:** " << primaryModel << "\n";
    md << "**Messages:** " << messages.size() << "\n";
    md << "\n---\n\n";

    int written = 0;
    for (const auto& msg : messages) {
        if (msg.content.empty())
            continue;

        if (msg.role == "user") {
            md << "**You:**\n\n";
        } else if (msg.role == "assistant") {
            if (!msg.model.empty())
                md << "**" << msg.model << ":**\n\n";
            else
                md << "**Assistant:**\n\n";
        } else if (msg.role == "system") {
            md << "**[System]:**\n\n";
        } else {
            md << "**" << msg.role << ":**\n\n";
        }

        md << msg.content << "\n\n";
        md << "---\n\n";
        ++written;
    }

    // ── Write the file ───────────────────────────────────────────
    try {
        std::ofstream outFile(filePath, std::ios::out | std::ios::binary);
        if (!outFile.is_open()) {
            result.errorMessage = "Could not open file for writing: " + filePath.string();
            return result;
        }

        std::string content = md.str();
        outFile.write(content.data(), content.size());
        outFile.close();

        if (outFile.fail()) {
            result.errorMessage = "Write failed: " + filePath.string();
            return result;
        }
    }
    catch (const std::exception& ex) {
        result.errorMessage = std::string("Error writing file: ") + ex.what();
        return result;
    }

    // ── Success ──────────────────────────────────────────────────
    result.success = true;
    result.savedFilePath = filePath.string();
    result.savedFileName = fileName;
    result.messageCount = written;
    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  FormatSaveChatResult
// ═══════════════════════════════════════════════════════════════════

std::string FormatSaveChatResult(const WorkspaceSaveChatResult& result)
{
    std::ostringstream oss;

    if (!result.success) {
        oss << "Save chat failed.\n"
            << "  " << result.errorMessage;
        return oss.str();
    }

    oss << "Chat saved successfully.\n"
        << "  File: " << result.savedFileName << "\n"
        << "  Path: " << result.savedFilePath << "\n"
        << "  Messages: " << result.messageCount;

    return oss.str();
}
