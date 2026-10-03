#pragma once

// model_url.h
//
// Pure logic for "paste a link, download a model" in Manage Models.
// No wx, no Poco, no I/O -- tested standalone by model_url_tests.cpp.
//
// What it does with the text the user pasted:
//   - trims it, requires https://, drops any #fragment;
//   - on Hugging Face (huggingface.co / hf.co), rewrites the page link
//     people actually copy (.../blob/<rev>/file.gguf) into the direct
//     download link (.../resolve/<rev>/file.gguf);
//   - explains, instead of failing later, when the link is a repo page,
//     not a .gguf file, or one part of a split model;
//   - derives a safe local filename from the last path segment;
//   - names the bundle folder a casual-mode download is saved into
//     (BundleFolderName), and tells companion files (mmproj / draft)
//     apart from model weights (IsCompanionFilename).

#include <cctype>
#include <string>

namespace model_url {

struct ParseResult {
    bool        ok = false;
    std::string url;        // direct download URL (when ok)
    std::string filename;   // local file name, e.g. "Qwen3.8-27B-Q4_K_M.gguf"
    std::string error;      // user-facing reason (when !ok)
};

namespace detail {

inline std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n\"'<>");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n\"'<>");
    return s.substr(a, b - a + 1);
}

inline int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string PercentDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = HexVal(s[i + 1]), lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

inline bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "-00001-of-00003.gguf" (llama.cpp gguf-split naming).
inline bool IsSplitPart(const std::string& lowerName)
{
    const std::string tail = ".gguf";
    if (!EndsWith(lowerName, tail)) return false;
    const std::string stem = lowerName.substr(0, lowerName.size() - tail.size());
    // needs at least "-NNNNN-of-NNNNN"
    if (stem.size() < 15) return false;
    const std::string t = stem.substr(stem.size() - 15);
    if (t[0] != '-' || t.compare(6, 4, "-of-") != 0) return false;
    for (int i : {1, 2, 3, 4, 5, 10, 11, 12, 13, 14})
        if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
    return true;
}

inline bool IsSafeFilename(const std::string& name)
{
    if (name.empty() || name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (c < 32) return false;
        switch (c) {
            case '\\': case '/': case ':': case '*': case '?':
            case '"':  case '<': case '>': case '|':
                return false;
            default: break;
        }
    }
    return name.back() != '.' && name.back() != ' ';
}

inline bool IsHuggingFaceHost(const std::string& lowerHost)
{
    return lowerHost == "huggingface.co" || lowerHost == "www.huggingface.co" ||
           lowerHost == "hf.co";
}

} // namespace detail

inline ParseResult Parse(const std::string& input)
{
    using namespace detail;
    ParseResult r;

    std::string s = Trim(input);
    if (s.empty()) { r.error = "Paste a link to a .gguf file first."; return r; }

    const size_t hash = s.find('#');
    if (hash != std::string::npos) s.erase(hash);

    const std::string lower = Lower(s);
    if (lower.rfind("http://", 0) == 0) {
        r.error = "Use an https:// link. Plain http downloads are not allowed.";
        return r;
    }
    if (lower.rfind("https://", 0) != 0) {
        r.error = "That doesn't look like a web link. It should start with https://";
        return r;
    }

    // Split https://host[:port]/path?query
    const size_t hostStart = 8;
    size_t pathStart = s.find('/', hostStart);
    const size_t qMark = s.find('?', hostStart);
    if (qMark != std::string::npos && (pathStart == std::string::npos || qMark < pathStart))
        pathStart = std::string::npos;   // "https://host?x" has no path
    std::string host = s.substr(hostStart,
        (pathStart == std::string::npos ? (qMark == std::string::npos ? s.size() : qMark)
                                        : pathStart) - hostStart);
    const size_t colon = host.find(':');
    const std::string hostOnly = Lower(colon == std::string::npos ? host : host.substr(0, colon));
    if (hostOnly.empty() || host.find('@') != std::string::npos) {
        r.error = "That link has no valid website name in it.";
        return r;
    }

    std::string path, query;
    if (pathStart != std::string::npos) {
        const size_t q = s.find('?', pathStart);
        path  = s.substr(pathStart, q == std::string::npos ? std::string::npos : q - pathStart);
        query = q == std::string::npos ? std::string() : s.substr(q);
    }

    if (IsHuggingFaceHost(hostOnly)) {
        // /<owner>/<repo>/(blob|resolve)/<rev>/<file...>
        std::string segs[5];
        size_t pos = 1;
        int found = 0;
        for (; found < 4; ++found) {
            const size_t slash = path.find('/', pos);
            if (slash == std::string::npos) break;
            segs[found] = path.substr(pos, slash - pos);
            pos = slash + 1;
        }
        const std::string rest = pos <= path.size() ? path.substr(pos) : std::string();
        const bool hasFile = found == 4 && !rest.empty() &&
                             (segs[2] == "blob" || segs[2] == "resolve");
        if (!hasFile) {
            r.error = "That's a model page, not a file. On Hugging Face open "
                      "\"Files and versions\", click the .gguf file you want, "
                      "and copy that link.";
            return r;
        }
        // Rebuild from the parsed segments so only the 3rd segment is
        // touched (a repo or owner literally named "blob" stays intact).
        path = "/" + segs[0] + "/" + segs[1] + "/resolve/" + segs[3] + "/" + rest;
    }

    const size_t lastSlash = path.find_last_of('/');
    const std::string rawName = lastSlash == std::string::npos
        ? std::string() : path.substr(lastSlash + 1);
    const std::string name = PercentDecode(rawName);
    const std::string lowerName = Lower(name);

    if (!EndsWith(lowerName, ".gguf")) {
        r.error = "That link isn't a .gguf file. LlamaBoss can only download "
                  "GGUF model files.";
        return r;
    }
    if (IsSplitPart(lowerName)) {
        r.error = "This model is split into several files (" + name +
                  "). Split models aren't supported here yet. Pick a "
                  "single-file .gguf, or download every part in your browser "
                  "and put them in the models folder.";
        return r;
    }
    if (!IsSafeFilename(name)) {
        r.error = "The file name in that link can't be used on Windows.";
        return r;
    }

    r.ok       = true;
    r.url      = "https://" + host + path + query;
    r.filename = name;
    return r;
}

// ── Where a link download lands (casual mode) ───────────────────
//
// Casual mode (default models folder) keeps each model in its own
// bundle folder, models\<stem>\<file>.gguf, the same layout the
// catalog downloader (BuildDestPath) produces.  Same-folder is what
// pairs a model with its mmproj / draft companions.

// Companion files ride along with a model rather than being one:
// vision projectors (mmproj) and speculative draft models.  Mirrors
// IsMmprojFilename / IsDraftFilename in server_manager.cpp.  A
// companion pasted on its own is NOT given a folder of its own: a
// bundle with no weights is invisible, and generic names such as
// "mmproj-F16.gguf" belong to many different models.
inline bool IsCompanionFilename(const std::string& filename)
{
    const std::string lower = detail::Lower(filename);
    return lower.find("mmproj") != std::string::npos ||
           lower.find("draft")  != std::string::npos;
}

// Folder name for a casual-mode link download.  attempt 1 is the file
// stem ("Qwen3.8-27B-Q4_K_M"); attempt n >= 2 is "<stem> (n)", used
// when a folder of that name already holds a different model.
// Windows rejects folder names ending in '.' or ' ', so those are
// trimmed; an empty result falls back to "model".
inline std::string BundleFolderName(const std::string& filename, int attempt = 1)
{
    std::string stem = filename;
    if (detail::EndsWith(detail::Lower(stem), ".gguf"))
        stem.resize(stem.size() - 5);
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();
    if (stem.empty()) stem = "model";
    if (attempt >= 2) stem += " (" + std::to_string(attempt) + ")";
    return stem;
}

} // namespace model_url
