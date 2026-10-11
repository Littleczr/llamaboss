// answer_receipts.h
//
// Receipt check for long hex values in a final agent reply.
//
// Hashes, tree hashes and commit ids are what release work runs on, and a
// model that copies one from a tool result into prose can garble it: in a
// 2026-10-09 website deploy, GPT-6 Luna reported a 58-character "SHA-256"
// (one character swapped, six dropped) while the tool output it was quoting
// had all 64.  Nothing in the harness noticed; the user did, by counting.
//
// After an agent turn ends, every hex run of 32+ characters in the final
// reply is looked up, case-insensitively and as a complete token (so a
// truncated digest does not match the longer original), in what the model could legitimately
// have copied it from: tool results and the user's own messages in this chat,
// plus the chat's spilled tool output files (Vars\, ToolOutputs\, Web Pages\),
// since large results reach history only as a handle card.  Values found
// nowhere are reported to the USER as a notice under the reply.  The notice is
// display-only; it is not sent to the model and changes nothing on its own --
// the user decides what to do with it.
//
// Deliberately narrow: only hex runs, only 32+ characters (MD5 length and
// up, so short commit ids and colour codes never trigger), and a run must mix
// digits and letters so long decimal numbers and words are ignored.
//
// Header-only and free of wx/Poco so it is unit-testable off Windows.
//
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace answer_receipts {

constexpr std::size_t kMinHexRun = 32;

inline bool IsHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool IsWordChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

inline std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Drops <think>...</think> spans (prior reasoning is not the answer).
inline std::string StripThinkBlocks(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t open = text.find("<think>", pos);
        if (open == std::string::npos) { out.append(text, pos, std::string::npos); break; }
        out.append(text, pos, open - pos);
        const size_t close = text.find("</think>", open);
        if (close == std::string::npos) break;
        pos = close + 8;
    }
    return out;
}

// Maximal hex runs of kMinHexRun+ characters that are whole tokens (not part
// of a longer identifier or base64 blob) and contain both a digit and a
// letter.  Lowercased, de-duplicated, in order of first appearance.
inline std::vector<std::string> ExtractHexTokens(const std::string& text)
{
    std::vector<std::string> out;
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        if (!IsHex(text[i])) { ++i; continue; }
        // "0x" prefix: the digest starts after it.
        bool prefixed = false;
        if (text[i] == '0' && i + 2 < n && (text[i + 1] == 'x' || text[i + 1] == 'X') &&
            IsHex(text[i + 2]) && (i == 0 || !IsWordChar(text[i - 1]))) {
            i += 2;
            prefixed = true;
        }
        size_t j = i;
        while (j < n && IsHex(text[j])) ++j;
        const bool leftOk  = prefixed || (i == 0) || !IsWordChar(text[i - 1]);
        const bool rightOk = (j == n) || !IsWordChar(text[j]);
        if (leftOk && rightOk && j - i >= kMinHexRun) {
            bool digit = false, letter = false;
            for (size_t k = i; k < j; ++k) {
                if (text[k] >= '0' && text[k] <= '9') digit = true;
                else letter = true;
            }
            if (digit && letter) {
                std::string tok = Lower(text.substr(i, j - i));
                if (std::find(out.begin(), out.end(), tok) == out.end())
                    out.push_back(std::move(tok));
            }
        }
        // Skip the rest of this word so "0x...g" style tails cannot restart
        // a run mid-identifier.
        while (j < n && IsWordChar(text[j])) ++j;
        i = j;
    }
    return out;
}

// Evidence text for one stored message.  Tool results ("[tool: name]\n> echo")
// lose their echo line, which repeats the model's own arguments; hash results
// also lose "Expected..." lines for the same reason.  Lowercased.
inline std::string EvidenceFromMessage(const std::string& role,
                                       const std::string& content)
{
    if (role == "assistant" || role == "system") return std::string();
    std::string text = content;
    if (text.rfind("[tool: ", 0) == 0) {
        const size_t l1 = text.find('\n');
        if (l1 != std::string::npos) {
            const size_t l2 = text.find('\n', l1 + 1);
            if (text.compare(l1 + 1, 2, "> ") == 0)
                text.erase(l1 + 1, l2 == std::string::npos ? std::string::npos : l2 - l1);
        }
        if (text.rfind("[tool: hash]", 0) == 0) {
            std::string kept;
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                const size_t end = nl == std::string::npos ? text.size() : nl + 1;
                if (text.compare(pos, 8, "Expected") != 0) kept.append(text, pos, end - pos);
                pos = end;
            }
            text.swap(kept);
        }
    }
    return Lower(text);
}

// True when `tok` occurs in `hay` as a COMPLETE hex token: the match must
// not be extended by more word characters on either side.  A plain substring
// test let a truncated digest pass -- a 58-character prefix of a real
// 64-character SHA-256 is "found" inside it -- which is exactly the failure
// this check exists to catch.  A "0x" prefix in the evidence is allowed.
inline bool ContainsWholeHexToken(const std::string& hay, const std::string& tok)
{
    if (tok.empty()) return false;
    size_t pos = hay.find(tok);
    while (pos != std::string::npos) {
        const size_t end = pos + tok.size();
        bool leftOk = (pos == 0) || !IsWordChar(hay[pos - 1]);
        if (!leftOk && pos >= 2 && (hay[pos - 1] == 'x' || hay[pos - 1] == 'X') &&
            hay[pos - 2] == '0' && (pos == 2 || !IsWordChar(hay[pos - 3])))
            leftOk = true;
        const bool rightOk = (end == hay.size()) || !IsWordChar(hay[end]);
        if (leftOk && rightOk) return true;
        pos = hay.find(tok, pos + 1);
    }
    return false;
}

// Tokens absent, as complete tokens, from every evidence string.
inline std::vector<std::string> FindUnbacked(const std::vector<std::string>& tokens,
                                             const std::vector<std::string>& lowerEvidence)
{
    std::vector<std::string> missing;
    for (const std::string& tok : tokens) {
        bool found = false;
        for (const std::string& ev : lowerEvidence) {
            if (ContainsWholeHexToken(ev, tok)) { found = true; break; }
        }
        if (!found) missing.push_back(tok);
    }
    return missing;
}

// Second chance for values that live only in spilled tool output: scan text
// files under `dirs` (recursively) and drop every token found.  Bounded so a
// huge workspace cannot stall the UI: at most kMaxFiles files, files over
// kMaxFileBytes skipped, kMaxTotalBytes read in total.
inline void RemoveTokensFoundInFiles(std::vector<std::string>& tokens,
                                     const std::vector<std::filesystem::path>& dirs)
{
    constexpr std::size_t   kMaxFiles      = 400;
    constexpr std::uintmax_t kMaxFileBytes = 8ull * 1024 * 1024;
    constexpr std::uintmax_t kMaxTotalBytes = 48ull * 1024 * 1024;

    std::size_t files = 0;
    std::uintmax_t total = 0;
    for (const auto& dir : dirs) {
        if (tokens.empty()) return;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        std::filesystem::recursive_directory_iterator it(
            dir, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (tokens.empty() || files >= kMaxFiles || total >= kMaxTotalBytes) return;
            std::error_code fe;
            if (!it->is_regular_file(fe)) continue;
            const std::uintmax_t size = it->file_size(fe);
            if (fe || size == 0 || size > kMaxFileBytes) continue;
            std::ifstream f(it->path(), std::ios::binary);
            if (!f) continue;
            std::string data(static_cast<std::size_t>(size), '\0');
            f.read(&data[0], static_cast<std::streamsize>(size));
            data.resize(static_cast<std::size_t>(f.gcount()));
            ++files;
            total += data.size();
            data = Lower(data);
            tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                             [&](const std::string& t) { return ContainsWholeHexToken(data, t); }),
                         tokens.end());
        }
    }
}

// User-facing notice; empty when nothing is unbacked.
inline std::string FormatNotice(const std::vector<std::string>& unbacked)
{
    if (unbacked.empty()) return std::string();
    std::string s = unbacked.size() == 1
        ? "Receipt check: this reply contains a hex value that does not appear in any tool output or message in this chat:\n"
        : "Receipt check: this reply contains hex values that do not appear in any tool output or message in this chat:\n";
    const size_t shown = std::min<size_t>(unbacked.size(), 5);
    for (size_t i = 0; i < shown; ++i)
        s += "  " + unbacked[i] + "  (" + std::to_string(unbacked[i].size()) + " chars)\n";
    if (unbacked.size() > shown)
        s += "  ... and " + std::to_string(unbacked.size() - shown) + " more\n";
    s += unbacked.size() == 1
        ? "It may have been mistyped. Check it against the source before relying on it."
        : "They may have been mistyped. Check them against the source before relying on them.";
    return s;
}

} // namespace answer_receipts
