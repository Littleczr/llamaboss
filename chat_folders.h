// chat_folders.h
//
// Single source of truth for the per-conversation ("chat") folder layout:
//
//   %USERPROFILE%\LlamaBoss\Chats\<chat folder>\Workspace
//   %USERPROFILE%\LlamaBoss\Chats\<chat folder>\Scripts
//   %USERPROFILE%\LlamaBoss\Chats\<chat folder>\attachments   ...etc.
//
// Chat folder names are human-readable and sort chronologically:
//
//   2026-09-26_invoice-parser-fix_1ed3d3c2
//   └ created ┘ └── title slug ──┘ └ id ─┘
//
// The trailing id is the stable key.  It is the part after "chat_" in the
// conversation JSON name (conversations\chat_1ed3d3c2.json), so the folder is
// always found by id, never by title.  The folder is named once, when first
// created, and is never renamed afterwards (renaming a chat in the sidebar
// does not touch the folder; see the _title.txt marker for the live title).
//
// Legacy layout, still recognized so that folders the one-time migration
// could not move keep working:
//
//   %USERPROFILE%\LlamaBoss\Workflows\chat_1ed3d3c2\Workspace
//
// "Workflows" means ONLY the project Workflows lane
// (<project>\Workflows, see ProjectManager::ProjectWorkflowsPath).  Nothing
// chat-related should use that word.
//
// Everything in this header is pure string logic with no wx / Win32
// dependency, so any translation unit (tools, path safety, tests) can use it.
// Filesystem work (resolve / create / migrate) lives on ChatHistory.

#pragma once

#include <cstdint>
#include <string>

namespace chat_folders {

// Folder names directly under %USERPROFILE%\LlamaBoss.
inline constexpr const char* kChatsRootName       = "Chats";
inline constexpr const char* kLegacyChatsRootName = "Workflows";

// Legacy per-chat folder prefix and conversation JSON prefix.
inline constexpr const char* kLegacyChatPrefix = "chat_";

// Maximum characters of title slug placed in a folder name.  Keeps the full
// path comfortably short for MAX_PATH-limited tools (Python, zip, Office).
inline constexpr std::size_t kMaxSlugChars = 40;

namespace detail {

inline std::string TrimTrailingSeparators(std::string s)
{
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

inline std::string ParentDir(const std::string& path)
{
    std::string s = TrimTrailingSeparators(path);
    std::size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? std::string() : s.substr(0, pos);
}

inline std::string BaseName(const std::string& path)
{
    std::string s = TrimTrailingSeparators(path);
    std::size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

inline std::string LowerAscii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

inline bool IsHex(char c)
{
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool StartsWithNoCase(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() &&
           LowerAscii(s.substr(0, prefix.size())) == LowerAscii(prefix);
}

} // namespace detail

// True for a chat id as produced by ChatHistory::GenerateFilePath (8 hex
// chars) or its full-UUID fallback (36 chars, hex + hyphens).
inline bool IsChatId(const std::string& id)
{
    if (id.size() == 8) {
        for (char c : id) if (!detail::IsHex(c)) return false;
        return true;
    }
    if (id.size() == 36) {
        for (std::size_t i = 0; i < id.size(); ++i) {
            const bool dashSlot = (i == 8 || i == 13 || i == 18 || i == 23);
            if (dashSlot ? id[i] != '-' : !detail::IsHex(id[i])) return false;
        }
        return true;
    }
    return false;
}

// Stable 8-hex id for a conversation whose JSON stem is not "chat_<id>"
// (Save-As to a custom filename).  FNV-1a over the lowercased stem so the
// same file always maps to the same chat folder.
inline std::string HashId(const std::string& stem)
{
    std::uint32_t h = 2166136261u;
    for (char c : detail::LowerAscii(stem)) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    static const char* kHex = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) { out[i] = kHex[h & 0xF]; h >>= 4; }
    return out;
}

// Conversation JSON stem ("chat_1ed3d3c2") -> chat id ("1ed3d3c2").
inline std::string ChatIdFromConversationStem(const std::string& stem)
{
    if (detail::StartsWithNoCase(stem, kLegacyChatPrefix)) {
        std::string id = stem.substr(std::char_traits<char>::length(kLegacyChatPrefix));
        if (IsChatId(id)) return detail::LowerAscii(id);
    }
    return HashId(stem);
}

// True when `name` starts with a YYYY-MM-DD date followed by '_'.
inline bool HasDatePrefix(const std::string& name)
{
    if (name.size() < 11) return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!detail::IsDigit(name[i])) return false;
    return name[4] == '-' && name[7] == '-' && name[10] == '_';
}

// Chat folder name -> chat id, or "" when `name` is not a chat folder.
// Accepts the current "<date>_<slug>_<id>" / "<date>_<id>" form and the
// legacy "chat_<id>" form.
inline std::string ChatIdFromFolderName(const std::string& name)
{
    if (HasDatePrefix(name)) {
        const std::size_t us = name.find_last_of('_');
        if (us == std::string::npos || us < 10) return std::string();
        std::string id = name.substr(us + 1);
        return IsChatId(id) ? detail::LowerAscii(id) : std::string();
    }
    if (detail::StartsWithNoCase(name, kLegacyChatPrefix)) {
        std::string id = name.substr(std::char_traits<char>::length(kLegacyChatPrefix));
        return id.empty() ? std::string() : detail::LowerAscii(id);
    }
    return std::string();
}

inline bool IsChatFolderName(const std::string& name)
{
    return !ChatIdFromFolderName(name).empty();
}

// True for the folder that holds chat folders: "Chats", or the legacy
// "Workflows" root.
inline bool IsChatsRootName(const std::string& name)
{
    const std::string lower = detail::LowerAscii(name);
    return lower == detail::LowerAscii(kChatsRootName) ||
           lower == detail::LowerAscii(kLegacyChatsRootName);
}

// Title -> short, filesystem-safe, lowercase ASCII slug.
// "Fix the invoice parser (again!)" -> "fix-the-invoice-parser-again"
// Returns "" when the title has no usable ASCII letters/digits or is the
// untitled placeholder.
inline std::string MakeTitleSlug(const std::string& title,
                                 std::size_t maxChars = kMaxSlugChars)
{
    if (detail::LowerAscii(title) == "untitled conversation") return std::string();

    // Fold common accented Latin letters (UTF-8 U+00C0..U+00FF, two bytes
    // starting 0xC3) to ASCII so "Qué pasa" becomes "que-pasa", not "qu-pasa".
    // Index = second byte - 0x80.  '-' means "treat as a separator".
    static const char kLatin1Fold[65] =
        "AAAAAAACEEEEIIII" "DNOOOOO-OUUUUYTs"
        "aaaaaaaceeeeiiii" "dnooooo-ouuuuyty";
    std::string folded;
    folded.reserve(title.size());
    for (std::size_t i = 0; i < title.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(title[i]);
        if (b == 0xC3 && i + 1 < title.size()) {
            const unsigned char n = static_cast<unsigned char>(title[i + 1]);
            if (n >= 0x80 && n <= 0xBF) {
                folded.push_back(kLatin1Fold[n - 0x80]);
                ++i;
                continue;
            }
        }
        folded.push_back(title[i]);
    }

    std::string slug;
    bool pendingDash = false;
    for (char raw : folded) {
        char c = raw;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        const bool keep = (c >= 'a' && c <= 'z') || detail::IsDigit(c);
        if (keep) {
            if (pendingDash && !slug.empty()) slug.push_back('-');
            pendingDash = false;
            slug.push_back(c);
        } else {
            pendingDash = true;   // any run of other chars -> one '-'
        }
    }
    if (slug.size() > maxChars) {
        // Cut at the last word boundary that fits, if there is a sensible one.
        std::size_t cut = slug.rfind('-', maxChars);
        slug = slug.substr(0, (cut != std::string::npos && cut >= maxChars / 2)
                                  ? cut : maxChars);
        while (!slug.empty() && slug.back() == '-') slug.pop_back();
    }
    return slug;
}

// "2026-09-26", "Fix invoice parser", "1ed3d3c2"
//   -> "2026-09-26_fix-invoice-parser_1ed3d3c2"
// Empty/placeholder title -> "2026-09-26_1ed3d3c2".
inline std::string BuildChatFolderName(const std::string& dateYmd,
                                       const std::string& title,
                                       const std::string& chatId)
{
    const std::string slug = MakeTitleSlug(title);
    return slug.empty() ? dateYmd + "_" + chatId
                        : dateYmd + "_" + slug + "_" + chatId;
}

// Recognizes <Chats root>\<chat folder>\Workspace (or the legacy
// <Workflows>\chat_<id>\Workspace) and returns the chat folder path, or ""
// when `cwd` is anything else (custom /cd, project folder, default
// %USERPROFILE%\LlamaBoss\Shared\Workspace).
//
// Every "which chat does this cwd belong to" check in the codebase must go
// through here: a private copy that drifts silently changes which lanes
// tools and approvals trust.
inline std::string ChatFolderFromWorkspaceCwd(const std::string& cwd)
{
    const std::string clean = detail::TrimTrailingSeparators(cwd);
    if (clean.empty()) return std::string();
    if (detail::LowerAscii(detail::BaseName(clean)) != "workspace") return std::string();

    const std::string chatFolder = detail::ParentDir(clean);
    const std::string chatsRoot  = detail::ParentDir(chatFolder);
    if (chatFolder.empty() || chatsRoot.empty()) return std::string();
    if (!IsChatFolderName(detail::BaseName(chatFolder))) return std::string();
    if (!IsChatsRootName(detail::BaseName(chatsRoot))) return std::string();
    return chatFolder;
}

} // namespace chat_folders
