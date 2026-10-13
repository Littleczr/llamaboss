#pragma once
#include "lb_ascii.h"
// skill_connections.h
//
// Which Connections (SecretsStore providers) a script is allowed to see.
//
// python_run_script used to inject EVERY stored secret -- including the AI
// endpoint keys -- into every script it ran.  That was fine while every
// Skill was hand-written, but Import a Skill means third-party code runs
// through the same path.  The rule now: a script receives only the
// Connections declared by the instruction file of the Skill or project
// workflow folder that owns it.  Scripts with no owning SKILL.md /
// WORKFLOW.md (the conversation Scripts lane, flat Workflows\*.py) get none.
//
// Accepted declarations in SKILL.md / WORKFLOW.md (either or both):
//
//   frontmatter:   connections: runpod, smartsheet
//                  connections: [runpod, smartsheet]
//                  connections:
//                    - runpod
//
//   body section:  ## Connections
//                  - runpod                 (text after the first word is ignored)
//                  - `SMARTSHEET_TOKEN`     (an injected env var name works too)
//
// A declared token matches a stored secret when it equals the provider
// name or the injected env var name, compared case-insensitively with
// non-alphanumerics folded to '_' (the same rule BuildInjectedEnvName uses).
//
// Pure C++17, no wx, so LlamaBossTests can compile it directly.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace lb_skill_connections {

inline std::string NormalizeToken(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(lb_ascii::IsAlnum(c) ? static_cast<char>(lb_ascii::ToLower(c)) : '_');
    }
    return out;
}

namespace detail {

inline std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string Lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(lb_ascii::ToLower(static_cast<unsigned char>(c)));
    return s;
}

inline std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (nl == text.size()) break;
        start = nl + 1;
    }
    return lines;
}

// First identifier-ish word of a list item: strips markdown emphasis,
// backticks and quotes, then keeps [A-Za-z0-9_.-].
inline std::string FirstWord(const std::string& raw)
{
    std::string s = Trim(raw);
    size_t i = 0;
    while (i < s.size() && (s[i] == '`' || s[i] == '*' || s[i] == '_' ||
                            s[i] == '"' || s[i] == '\'')) {
        ++i;
    }
    std::string word;
    for (; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (lb_ascii::IsAlnum(c) || c == '_' || c == '-' || c == '.') {
            word.push_back(static_cast<char>(c));
        } else {
            break;
        }
    }
    while (!word.empty() && (word.back() == '.' || word.back() == '-' || word.back() == '_'))
        word.pop_back();
    return word;
}

// "- x", "* x", "+ x", "1. x", "1) x" -> "x"; anything else -> false.
inline bool StripListMarker(const std::string& line, std::string& rest)
{
    std::string s = Trim(line);
    if (s.size() >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') &&
        (s[1] == ' ' || s[1] == '\t')) {
        rest = s.substr(2);
        return true;
    }
    size_t i = 0;
    while (i < s.size() && lb_ascii::IsDigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i > 0 && i + 1 < s.size() && (s[i] == '.' || s[i] == ')') &&
        (s[i + 1] == ' ' || s[i + 1] == '\t')) {
        rest = s.substr(i + 2);
        return true;
    }
    return false;
}

inline void AddUnique(std::vector<std::string>& out, const std::string& word)
{
    if (word.empty()) return;
    const std::string norm = NormalizeToken(word);
    if (std::find(out.begin(), out.end(), norm) == out.end()) out.push_back(norm);
}

} // namespace detail

// Returns normalized declared tokens (see NormalizeToken), in order.
inline std::vector<std::string> ParseDeclaredConnections(const std::string& doc)
{
    using namespace detail;
    std::vector<std::string> out;
    const std::vector<std::string> lines = SplitLines(doc);

    size_t bodyStart = 0;

    // ── Frontmatter ────────────────────────────────────────────────
    size_t first = 0;
    // Tolerate a UTF-8 BOM on the first line.
    std::string line0 = lines.empty() ? std::string() : lines[0];
    if (line0.size() >= 3 && static_cast<unsigned char>(line0[0]) == 0xEF &&
        static_cast<unsigned char>(line0[1]) == 0xBB &&
        static_cast<unsigned char>(line0[2]) == 0xBF) {
        line0 = line0.substr(3);
    }
    if (!lines.empty() && Trim(line0) == "---") {
        size_t end = first + 1;
        while (end < lines.size() && Trim(lines[end]) != "---") ++end;
        if (end < lines.size()) {
            bodyStart = end + 1;
            for (size_t i = 1; i < end; ++i) {
                const std::string& l = lines[i];
                if (l.empty() || l[0] == ' ' || l[0] == '\t') continue;  // only top-level keys
                const size_t colon = l.find(':');
                if (colon == std::string::npos) continue;
                if (Lower(Trim(l.substr(0, colon))) != "connections") continue;

                std::string value = Trim(l.substr(colon + 1));
                if (!value.empty()) {
                    if (value.front() == '[') value.erase(0, 1);
                    if (!value.empty() && value.back() == ']') value.pop_back();
                    size_t pos = 0;
                    while (pos <= value.size()) {
                        size_t comma = value.find(',', pos);
                        if (comma == std::string::npos) comma = value.size();
                        AddUnique(out, FirstWord(value.substr(pos, comma - pos)));
                        pos = comma + 1;
                    }
                } else {
                    // YAML block list on the following indented lines.
                    for (size_t j = i + 1; j < end; ++j) {
                        const std::string& item = lines[j];
                        if (item.empty() || (item[0] != ' ' && item[0] != '\t')) break;
                        std::string rest;
                        if (!StripListMarker(item, rest)) break;
                        AddUnique(out, FirstWord(rest));
                    }
                }
            }
        }
    }

    // ── "## Connections" section in the body ─────────────────────────
    bool inSection = false;
    bool inFence = false;
    for (size_t i = bodyStart; i < lines.size(); ++i) {
        const std::string t = Trim(lines[i]);
        if (t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0) {
            inFence = !inFence;
            continue;
        }
        if (inFence) continue;
        if (!t.empty() && t[0] == '#') {
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            const std::string title = Lower(Trim(t.substr(h)));
            inSection = (title == "connections");
            continue;
        }
        if (!inSection) continue;
        std::string rest;
        if (StripListMarker(lines[i], rest)) AddUnique(out, FirstWord(rest));
    }

    return out;
}

inline bool IsDeclared(const std::vector<std::string>& declared,
                       const std::string& provider,
                       const std::string& envName)
{
    const std::string p = NormalizeToken(provider);
    const std::string e = NormalizeToken(envName);
    for (const auto& d : declared) {
        if (d == p || d == e) return true;
    }
    return false;
}

namespace detail {

inline std::string CompareKey(const std::filesystem::path& p)
{
    std::string s = Lower(p.lexically_normal().generic_u8string());
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

} // namespace detail

// Finds the SKILL.md / WORKFLOW.md that owns scriptPath: the nearest one in
// the script's folder or an ancestor, strictly below whichever lane root
// contains the script.  Returns "" when the script is under no lane root
// or no instruction file is found before reaching the root.
inline std::string FindOwningInstructionDoc(const std::string& scriptPathUtf8,
                                            const std::vector<std::string>& laneRootsUtf8)
{
    namespace fs = std::filesystem;
    const fs::path script = fs::u8path(scriptPathUtf8).lexically_normal();
    const std::string scriptKey = detail::CompareKey(script);

    for (const auto& rootUtf8 : laneRootsUtf8) {
        if (rootUtf8.empty()) continue;
        const fs::path root = fs::u8path(rootUtf8).lexically_normal();
        const std::string rootKey = detail::CompareKey(root);
        if (scriptKey.size() <= rootKey.size() + 1 ||
            scriptKey.compare(0, rootKey.size(), rootKey) != 0 ||
            scriptKey[rootKey.size()] != '/') {
            continue;
        }

        fs::path dir = script.parent_path();
        while (true) {
            const std::string dirKey = detail::CompareKey(dir);
            if (dirKey.size() <= rootKey.size()) break;   // reached the lane root
            for (const char* name : { "SKILL.md", "WORKFLOW.md" }) {
                const fs::path doc = dir / name;
                std::error_code ec;
                if (fs::is_regular_file(doc, ec)) return doc.u8string();
            }
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return std::string();
    }
    return std::string();
}

} // namespace lb_skill_connections
