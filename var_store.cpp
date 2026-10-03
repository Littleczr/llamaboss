// var_store.cpp
// See var_store.h for the design contract.  Conventions mirror
// tool_read.cpp: UTF-8 in the interface, path_safety::Utf8ToWide at
// every fstream/Win32 boundary, no wx dependency.

#include "var_store.h"

#include "path_safety.h"   // Utf8ToWide, SanitizeFilename

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace varstore {

const char* const kCardSentinel = "[LARGE OUTPUT -> stored as variable]";
const char* const kVarsLaneName = "Vars";

namespace {

// ── small utilities ────────────────────────────────────────────────

std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Extension (lowercase, with dot) of the LAST path-ish token in a
// hint string.  For a read echo this is the file's extension; for a
// command line it is usually "" — which is the correct answer (shape
// dispatch falls through to generic).
std::string ExtensionOfHint(const std::string& hint)
{
    // Take the last whitespace-delimited token, then its extension.
    size_t end = hint.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return "";
    size_t start = hint.find_last_of(" \t", end);
    std::string tok = hint.substr(start == std::string::npos ? 0 : start + 1,
                                  end - (start == std::string::npos ? 0 : start + 1) + 1);
    size_t dot = tok.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= tok.size()) return "";
    // Reject "extensions" containing separators ("a.b\c") or absurd length.
    std::string ext = tok.substr(dot);
    if (ext.size() > 8) return "";
    if (ext.find_first_of("\\/") != std::string::npos) return "";
    return ToLowerAscii(ext);
}

bool IsCodeExt(const std::string& ext)
{
    static const char* kExts[] = { ".h", ".hpp", ".c", ".cc", ".cpp", ".cs",
                                   ".py", ".js", ".ts", ".ps1", ".rs",
                                   ".java", ".go" };
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

// Human-readable byte size, matching the chip style used elsewhere
// ("54.3 KB", "1.2 MB").
std::string HumanBytes(size_t n)
{
    char buf[32];
    if (n >= 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f MB", (double)n / (1024.0 * 1024.0));
    else if (n >= 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f KB", (double)n / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%zu B", n);
    return std::string(buf);
}

size_t CountLines(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = (size_t)std::count(s.begin(), s.end(), '\n');
    if (s.back() != '\n') ++n;   // final unterminated line counts
    return n;
}

// Split into lines WITHOUT copying the whole body again: returns
// (offset,length) pairs.  Length excludes the terminator; \r\n and \n
// both handled.
struct LineSpan { size_t off; size_t len; };

std::vector<LineSpan> SplitLineSpans(const std::string& s, size_t maxLinesToScan)
{
    std::vector<LineSpan> out;
    size_t pos = 0;
    while (pos < s.size() && out.size() < maxLinesToScan) {
        size_t nl = s.find('\n', pos);
        size_t end = (nl == std::string::npos) ? s.size() : nl;
        size_t len = end - pos;
        if (len > 0 && s[pos + len - 1] == '\r') --len;
        out.push_back({ pos, len });
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// Last N line spans of the body (scans backwards; cheap even on MB
// bodies because it touches only the tail).
std::vector<LineSpan> TailLineSpans(const std::string& s, size_t n)
{
    std::vector<LineSpan> out;
    if (s.empty() || n == 0) return out;
    size_t end = s.size();
    if (s[end - 1] == '\n') --end;               // ignore trailing terminator
    while (end > 0 && out.size() < n) {
        size_t nl  = s.rfind('\n', end - 1);
        size_t off = (nl == std::string::npos) ? 0 : nl + 1;
        size_t len = end - off;
        if (len > 0 && s[off + len - 1] == '\r') --len;
        out.push_back({ off, len });
        if (nl == std::string::npos) break;
        end = nl;
        if (end > 0 && s[end - 1] == '\r') { /* \r consumed via len math above */ }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// Shrink a byte-length clamp back to a UTF-8 character boundary so a
// preview or shape line can never end mid-codepoint.
//
// This matters well beyond cosmetics.  The card built here is stored in
// ChatHistory and serialized by Poco's JSON stringifier, which is called
// with default options and therefore emits non-ASCII bytes verbatim.
// llama-server's JSON parser rejects malformed UTF-8 inside a string
// outright, so ONE truncated multi-byte sequence anywhere in the card
// fails the entire request — the model never sees the tool result at
// all.  A 200-byte clamp lands mid-character roughly 2/3 of the time on
// CJK text and a percent or two of the time on accented Latin text.
//
// Never grows the clamp, so every existing byte budget still holds.
// Malformed input (a stray continuation byte, an invalid lead) is left
// exactly as-is: this function's job is to avoid CREATING breakage, not
// to sanitize a body that arrived broken.
size_t ClampToUtf8Boundary(const char* data, size_t len)
{
    if (len == 0) return 0;

    // Walk back over continuation bytes (10xxxxxx) to the lead byte of
    // the last character that the clamp touches.
    size_t j = len;
    while (j > 0 && ((unsigned char)data[j - 1] & 0xC0) == 0x80) --j;
    if (j == 0) return len;                    // no lead byte in range

    const size_t leadIdx  = j - 1;
    const unsigned char c = (unsigned char)data[leadIdx];

    size_t seqLen;
    if      ((c & 0x80) == 0x00) seqLen = 1;
    else if ((c & 0xE0) == 0xC0) seqLen = 2;
    else if ((c & 0xF0) == 0xE0) seqLen = 3;
    else if ((c & 0xF8) == 0xF0) seqLen = 4;
    else return len;                           // invalid lead byte

    // Keep the character only when the whole sequence fits.
    return (leadIdx + seqLen <= len) ? len : leadIdx;
}

// Append one preview line, clamped so a single pathological line can't
// blow the preview budget.  Marks clamping visibly.
//
// Both branches run the boundary check: callers in BuildShapeReport
// hand in a span that is ALREADY shortened (min(len, 120) and friends),
// which takes the first branch, so clamping only the second branch
// would leave the shape report able to emit a partial character.
void AppendClampedLine(std::ostringstream& os, const std::string& body,
                       const LineSpan& sp, size_t maxLineBytes)
{
    const char* p = body.data() + sp.off;
    if (sp.len <= maxLineBytes) {
        os.write(p, (std::streamsize)ClampToUtf8Boundary(p, sp.len));
    } else {
        os.write(p, (std::streamsize)ClampToUtf8Boundary(p, maxLineBytes));
        os << " ...[line clamped]";
    }
    os << '\n';
}

// True when a whole-file `read` echo names an EXISTING file that
// resolves inside the workspace (Vars\ spools and imported text
// attachments alike).  Used to reuse that source file in the handle
// card instead of spooling a duplicate.  Callers must never apply this
// to grep/inspect/extract/command output: those results describe the
// source file but are not the source file's contents.
// Conservative: a miss just means one extra copy is written, never
// anything unsafe.  Returns the workspace-relative form via
// relOut (backslash-joined, as the model should quote it).
bool EchoPointsInsideWorkspace(const std::string& echo,
                               const std::string& workspaceDirUtf8,
                               std::string* relOut)
{
    std::string tok = echo;
    // Trim to last whitespace token (read echoes are a bare path; keep
    // it simple — multi-word command lines will simply not match).
    size_t end = tok.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return false;
    size_t start = tok.find_last_of(" \t", end);
    tok = tok.substr(start == std::string::npos ? 0 : start + 1,
                     end - (start == std::string::npos ? 0 : start + 1) + 1);
    if (tok.empty()) return false;

    std::error_code ec;
    std::filesystem::path p = std::filesystem::path(path_safety::Utf8ToWide(tok));
    if (p.is_relative())
        p = std::filesystem::path(path_safety::Utf8ToWide(workspaceDirUtf8)) / p;
    std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;

    std::filesystem::path wsCanon = std::filesystem::weakly_canonical(
        std::filesystem::path(path_safety::Utf8ToWide(workspaceDirUtf8)), ec);
    if (ec) return false;

    std::wstring canonW = canon.wstring();
    std::wstring varsW  = wsCanon.wstring();
    if (canonW.size() <= varsW.size()) return false;
    // Case-insensitive prefix match + separator (Windows filesystems).
    auto ieq = [](wchar_t a, wchar_t b) {
        return ::towlower((wint_t)a) == ::towlower((wint_t)b);
    };
    if (!std::equal(varsW.begin(), varsW.end(), canonW.begin(), ieq)) return false;
    if (canonW[varsW.size()] != L'\\' && canonW[varsW.size()] != L'/') return false;

    if (!std::filesystem::is_regular_file(canon, ec) || ec) return false;
    if (relOut) {
        // Workspace-relative, backslash-joined — the exact form the
        // model should pass back to read/grep.
        std::wstring relW = canonW.substr(varsW.size() + 1);
        for (auto& ch : relW)
            if (ch == L'/') ch = L'\\';
        *relOut = path_safety::WideToUtf8(relW);
    }
    return true;
}

}  // namespace

// ── public: EnsureVarsDir ──────────────────────────────────────────

std::string EnsureVarsDir(const std::string& workspaceDirUtf8)
{
    if (workspaceDirUtf8.empty()) return "";
    std::wstring wWs = path_safety::Utf8ToWide(workspaceDirUtf8);
    if (wWs.empty()) return "";

    std::filesystem::path vars =
        std::filesystem::path(wWs) / path_safety::Utf8ToWide(std::string(kVarsLaneName));
    std::error_code ec;
    std::filesystem::create_directories(vars, ec);   // idempotent; also creates workspace
    if (ec) return "";
    return path_safety::WideToUtf8(vars.wstring());
}

// ── public: BuildShapeReport ───────────────────────────────────────

std::string BuildShapeReport(const std::string& text,
                             const std::string& nameHint,
                             const DemotionConfig& cfg)
{
    const std::string ext = ExtensionOfHint(nameHint);
    std::ostringstream os;
    size_t entries = 0;

    auto budgetLeft = [&]() {
        return entries < cfg.maxShapeLines &&
               (size_t)os.tellp() < cfg.maxShapeBytes;
    };

    if (ext == ".md" || ext == ".markdown") {
        // Heading lines with 1-based line numbers.  Skip fenced code
        // blocks so a quoted "# comment" inside ``` doesn't pollute.
        bool inFence = false;
        size_t lineNo = 0, pos = 0;
        while (pos <= text.size() && budgetLeft()) {
            size_t nl = text.find('\n', pos);
            size_t end = (nl == std::string::npos) ? text.size() : nl;
            ++lineNo;
            size_t len = end - pos;
            if (len > 0 && text[pos + len - 1] == '\r') --len;
            if (len >= 3 && text.compare(pos, 3, "```") == 0) {
                inFence = !inFence;
            } else if (!inFence && len > 0 && text[pos] == '#') {
                os << "L" << lineNo << ": ";
                AppendClampedLine(os, text, { pos, std::min(len, (size_t)120) }, 120);
                ++entries;
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    } else if (IsCodeExt(ext)) {
        // Declaration-looking lines.  Heuristic on purpose — this is a
        // navigation aid, not a parser.  Column-0-ish lines that start
        // with a known declaration keyword, or look like a C/C++
        // function signature (identifier '(' ... at low indent, no ';').
        static const char* kKw[] = { "class ", "struct ", "enum ", "namespace ",
                                     "def ", "function ", "template", "void ",
                                     "bool ", "int ", "std::", "static ",
                                     "public:", "private:", "protected:",
                                     "impl ", "fn ", "func " };
        size_t lineNo = 0, pos = 0;
        while (pos <= text.size() && budgetLeft()) {
            size_t nl = text.find('\n', pos);
            size_t end = (nl == std::string::npos) ? text.size() : nl;
            ++lineNo;
            size_t len = end - pos;
            if (len > 0 && text[pos + len - 1] == '\r') --len;
            // At most one level of indent — member functions in headers
            // sit at 4 spaces; deep bodies don't.
            size_t indent = 0;
            while (indent < len && (text[pos + indent] == ' ' || text[pos + indent] == '\t'))
                ++indent;
            if (len > 0 && indent <= 4) {
                bool hit = false;
                for (const char* kw : kKw) {
                    size_t kwLen = std::char_traits<char>::length(kw);
                    if (len - indent >= kwLen &&
                        text.compare(pos + indent, kwLen, kw) == 0) { hit = true; break; }
                }
                if (hit) {
                    os << "L" << lineNo << ": ";
                    AppendClampedLine(os, text, { pos + indent,
                                                  std::min(len - indent, (size_t)110) }, 110);
                    ++entries;
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    } else if (ext == ".csv" || ext == ".tsv") {
        auto spans = SplitLineSpans(text, 1);
        if (!spans.empty()) {
            os << "header: ";
            AppendClampedLine(os, text, { spans[0].off,
                                          std::min(spans[0].len, (size_t)300) }, 300);
            os << "records: ~" << (CountLines(text) > 0 ? CountLines(text) - 1 : 0) << '\n';
        }
    } else if (ext == ".json") {
        // Depth-1 key scan: keys that appear when brace depth is 1 and
        // we're not inside a string.  Robust enough for config/exports.
        int depth = 0; bool inStr = false; bool esc = false;
        size_t i = 0;
        while (i < text.size() && budgetLeft()) {
            char c = text[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
            } else if (c == '"') {
                if (depth == 1) {
                    size_t j = i + 1; bool e2 = false;
                    while (j < text.size()) {
                        char d = text[j];
                        if (e2) e2 = false;
                        else if (d == '\\') e2 = true;
                        else if (d == '"') break;
                        ++j;
                    }
                    // Only report as a key when a ':' follows the close quote.
                    size_t k = j + 1;
                    while (k < text.size() &&
                           (text[k] == ' ' || text[k] == '\t' ||
                            text[k] == '\r' || text[k] == '\n')) ++k;
                    if (k < text.size() && text[k] == ':') {
                        // Same UTF-8 rule as the preview: an 80-byte key
                        // clamp must not split a multi-byte character.
                        const size_t keyLen = ClampToUtf8Boundary(
                            text.data() + i + 1,
                            std::min(j - i - 1, (size_t)80));
                        os << "key: " << text.substr(i + 1, keyLen) << '\n';
                        ++entries;
                    }
                    i = j;   // resume after the closing quote (loop ++i below)
                } else {
                    inStr = true;
                }
            } else if (c == '{' || c == '[') { ++depth; }
            else if (c == '}' || c == ']') { --depth; }
            ++i;
        }
    }
    // else: generic — no shape section; head/tail preview suffices.

    return os.str();
}

// ── public: MaybeDemoteToolBody ────────────────────────────────────

DemoteOutcome MaybeDemoteToolBody(const std::string& toolTag,
                                  const std::string& commandEcho,
                                  const std::string& body,
                                  const std::string& workspaceDirUtf8,
                                  const DemotionConfig& cfg)
{
    DemoteOutcome out;

    if (workspaceDirUtf8.empty()) return out;              // unsaved conversation
    if (body.size() <= cfg.thresholdBytes) return out;     // small enough
    // Re-demotion guard: our own card, or an earlier build's elision
    // marker, must never be spooled as if it were payload.
    if (body.compare(0, std::char_traits<char>::length(kCardSentinel),
                     kCardSentinel) == 0) return out;

    const std::string varsAbs = EnsureVarsDir(workspaceDirUtf8);
    if (varsAbs.empty()) return out;                       // fall back: inject whole

    // ── reuse guard: a WHOLE-FILE READ already lives in workspace ───
    // Only `read` returns the contents of the path named by its echo.
    // Other tools (grep, *_inspect, *_extract, cmd, py, etc.) merely
    // mention an input path while returning a derived result.  Reusing
    // that path for those tools would point the handle card at the wrong
    // bytes -- and can even point it at a binary XLSX/PDF/DOCX source.
    //
    // Vars\ spool whole-reads (the read→demote→read loop) and whole-file
    // reads of imported TEXT attachments land here: the card points at
    // the existing text file, nothing new is written, no duplicates
    // accumulate, and the doom guard sees a stable true repeat.
    std::string reuseRel;
    const bool isWholeFileRead = (ToLowerAscii(toolTag) == "read");
    const bool reused = isWholeFileRead &&
        EchoPointsInsideWorkspace(commandEcho, workspaceDirUtf8, &reuseRel);

    std::string relPath;
    std::string absPath;

    if (reused) {
        relPath = reuseRel;
        absPath.clear();   // signals "nothing written" to the caller
    } else {
        // ── spool filename: <tag>_<NNNN>[_<hint>].txt ──────────────
        std::string tag = path_safety::SanitizeFilename(toolTag, "tool");
        // Optional human hint from the echo's last token's stem.
        std::string hint;
        {
            std::string ext = ExtensionOfHint(commandEcho);
            size_t end = commandEcho.find_last_not_of(" \t\r\n");
            if (!ext.empty() && end != std::string::npos) {
                size_t start = commandEcho.find_last_of(" \t\\/", end);
                std::string base = commandEcho.substr(
                    start == std::string::npos ? 0 : start + 1,
                    end - (start == std::string::npos ? 0 : start + 1) + 1);
                hint = path_safety::SanitizeFilename(base, "");
                // Fold the dot so the spool stays a .txt ("chat_history.cpp"
                // → "chat_history-cpp"); avoids double-extension confusion.
                std::replace(hint.begin(), hint.end(), '.', '-');
                // Same boundary rule — a spool name is converted through
                // Utf8ToWide, and half a character there yields a garbled
                // (or unopenable) filename.
                if (hint.size() > 40)
                    hint.resize(ClampToUtf8Boundary(hint.data(), 40));
            }
        }

        // First free index, race-safe via CREATE_NEW.
        for (int idx = 1; idx <= 9999; ++idx) {
            char num[8];
            std::snprintf(num, sizeof(num), "%04d", idx);
            std::string fname = tag + "_" + num + (hint.empty() ? "" : "_" + hint) + ".txt";
            std::string cand  = path_safety::WideToUtf8(
                (std::filesystem::path(path_safety::Utf8ToWide(varsAbs)) /
                 path_safety::Utf8ToWide(fname)).wstring());
            HANDLE h = ::CreateFileW(path_safety::Utf8ToWide(cand).c_str(),
                                     GENERIC_WRITE, 0, nullptr,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                if (::GetLastError() == ERROR_FILE_EXISTS) continue;   // next index
                return out;                                            // I/O trouble → no demote
            }
            // Byte-exact write of the body.
            const char* p = body.data();
            size_t left = body.size();
            bool ok = true;
            while (left > 0) {
                DWORD chunk = (DWORD)std::min<size_t>(left, 1u << 20);
                DWORD wrote = 0;
                if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) {
                    ok = false; break;
                }
                p += wrote; left -= wrote;
            }
            ::CloseHandle(h);
            if (!ok) {
                ::DeleteFileW(path_safety::Utf8ToWide(cand).c_str());
                return out;                                            // fall back whole
            }
            relPath = std::string(kVarsLaneName) + "\\" + fname;
            absPath = cand;
            break;
        }
        if (relPath.empty()) return out;   // 9999 spools in one chat — give up gracefully
    }

    // ── build the handle card ──────────────────────────────────────
    out.demoted  = true;
    out.relPath  = relPath;
    out.absPath  = absPath;
    out.cardBody = BuildHandleCard(relPath, body, commandEcho, cfg,
                                   /*reusedSameFile=*/reused,
                                   /*utf16Original=*/false);
    return out;
}

// ── public: BuildHandleCard ────────────────────────────────────────

std::string BuildHandleCard(const std::string& relPath,
                            const std::string& body,
                            const std::string& shapeHint,
                            const DemotionConfig& cfg,
                            bool reusedSameFile,
                            bool utf16Original)
{
    const size_t lines = CountLines(body);

    std::ostringstream card;
    card << kCardSentinel << '\n'
         << "file: " << relPath << "  (" << HumanBytes(body.size())
         << ", " << lines << " lines)\n";
    if (reusedSameFile)
        card << "note: this is the SAME variable file you just read whole — "
                "re-reading it returns this card again and makes no progress.\n";
    if (utf16Original)
        card << "note: the on-disk file is UTF-16; this preview was converted "
                "to UTF-8. grep may miss — prefer python_run_script for exact "
                "reads of this file.\n";
    card << "This is a PREVIEW ONLY. The complete content is stored in the file above.\n"
         << "A whole-file read of it returns this card again, not the contents. "
            "Query it instead:\n"
         << "  grep \"<pattern>\" " << relPath << "\n"
         << "  read_range <start>:<end> " << relPath
         << "   (1-based inclusive, up to 1000 lines per call)\n"
         << "If the user asks for the full text, tell them where the file is and "
            "offer specific sections -- do not refuse the request.\n";

    std::string shape = BuildShapeReport(body, shapeHint, cfg);
    if (!shape.empty())
        card << "\n-- shape --\n" << shape;

    // Head / tail preview under a shared byte budget.
    {
        std::ostringstream prev;
        auto head = SplitLineSpans(body, cfg.headLines);
        prev << "\n-- head (first " << head.size() << " lines) --\n";
        for (const auto& sp : head) {
            AppendClampedLine(prev, body, sp, 200);
            if ((size_t)prev.tellp() > cfg.maxPreviewBytes) break;
        }
        if (lines > cfg.headLines + cfg.tailLines &&
            (size_t)prev.tellp() < cfg.maxPreviewBytes) {
            auto tail = TailLineSpans(body, cfg.tailLines);
            prev << "-- tail (last " << tail.size() << " lines) --\n";
            for (const auto& sp : tail) {
                AppendClampedLine(prev, body, sp, 200);
                if ((size_t)prev.tellp() > cfg.maxPreviewBytes) break;
            }
        }
        card << prev.str();
    }

    return card.str();
}

}  // namespace varstore
