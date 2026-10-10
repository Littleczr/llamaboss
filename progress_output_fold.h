#pragma once
// ═══════════════════════════════════════════════════════════════════
//  progress_output_fold.h — tame progress bars and PS 5.1 stderr noise
// ═══════════════════════════════════════════════════════════════════
//
// A single curl download can produce ~130 red "####   37.8%" lines in the
// PowerShell card on an exit-0 call.  Three separate problems, three
// passes:
//
//   1. CollapseCarriageReturns()
//      Progress bars (curl --progress-bar, tqdm, pip, git) redraw one
//      line with bare '\r'.  A terminal shows only the final redraw; we
//      capture every frame and the rich-text control renders each '\r'
//      as a line break.  Keep the LAST non-empty '\r' segment of each
//      line, like a terminal would.  CRLF terminators are untouched.
//
//   2. UnwrapNativeCommandErrors()
//      Windows PowerShell 5.1 wraps a native program's redirected
//      stderr (2>&1) in an ErrorRecord and prints it as
//          llama-server.exe : <the actual line>
//          At line:2 char:289
//          + ... <script excerpt> ...
//          +     ~~~~~~~~~~~~~~
//              + CategoryInfo          : NotSpecified: (...) [], RemoteException
//              + FullyQualifiedErrorId : NativeCommandError
//      Only the first line's text is real output.  Rewritten to just
//      "<the actual line>".  Requires the exact FullyQualifiedErrorId
//      NativeCommandError, so genuine PowerShell errors are never touched.
//
//   3. FoldProgressOnlyStderr()
//      curl, git, pip, and tqdm write progress to stderr by convention.
//      When EVERY non-blank stderr line is a progress line, stderr
//      carries no error at all: move a one-line summary to stdout and
//      clear stderr, so the card is not red, not auto-expanded, and the
//      agent loop does not record the call as a failure.  If any other
//      line is present (e.g. "curl: (22) The requested URL returned
//      error: 404"), stderr is left in place, collapsed by pass 1.
//
// Shape-matched, never command-matched.  No dependencies: tested in
// progress_output_fold_tests.cpp.

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace lb_progressfold {

namespace detail {

// Split keeping terminators ('\n' included in each piece).
inline std::vector<std::string> SplitKeepNl(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { lines.push_back(text.substr(pos)); break; }
        lines.push_back(text.substr(pos, nl + 1 - pos));
        pos = nl + 1;
    }
    return lines;
}

// Separate a line into content and its terminator ("", "\n", "\r\n").
// Any run of '\r' immediately before '\n' (or at end of text) counts as
// part of the terminator, not as a redraw.
inline void SplitTerminator(const std::string& line, std::string& content, std::string& term)
{
    std::size_t end = line.size();
    bool nl = end > 0 && line[end - 1] == '\n';
    if (nl) --end;
    std::size_t crEnd = end;
    while (end > 0 && line[end - 1] == '\r') --end;
    const bool hadCr = crEnd != end;
    content = line.substr(0, end);
    term = nl ? (hadCr ? "\r\n" : "\n") : std::string();
}

inline std::string Trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline bool StartsWith(const std::string& s, const char* p)
{
    const std::string pp(p);
    return s.size() >= pp.size() && s.compare(0, pp.size(), pp) == 0;
}

inline bool IsBlank(const std::string& s) { return Trim(s).empty(); }

// "At line:2 char:289"
inline bool IsAtLinePosition(const std::string& s)
{
    const std::string t = Trim(s);
    if (!StartsWith(t, "At line:")) return false;
    return t.find(" char:") != std::string::npos;
}

// "<program> : <text>" — the head of a NativeCommandError record.
// Program token must be non-empty and contain no spaces before " : "
// unless it is a quoted/rooted path; we accept anything up to the
// first " : " as long as it does not start with whitespace.
inline bool SplitRecordHead(const std::string& s, std::string& text)
{
    if (s.empty() || std::isspace((unsigned char)s[0])) return false;
    const std::size_t sep = s.find(" : ");
    if (sep == std::string::npos || sep == 0) return false;
    text = s.substr(sep + 3);
    return true;
}

} // namespace detail

// ── Pass 1 ────────────────────────────────────────────────────────
// Returns the number of overwritten redraw segments removed.
inline std::size_t CollapseCarriageReturns(std::string& text)
{
    // Fast path: no '\r' that is not part of a CRLF / trailing run.
    bool any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\r') continue;
        std::size_t j = i;
        while (j < text.size() && text[j] == '\r') ++j;
        if (j < text.size() && text[j] != '\n') { any = true; break; }
        i = j;
    }
    if (!any) return 0;

    std::string out;
    out.reserve(text.size());
    std::size_t removed = 0;
    for (const std::string& line : detail::SplitKeepNl(text)) {
        std::string content, term;
        detail::SplitTerminator(line, content, term);
        if (content.find('\r') == std::string::npos) { out += line; continue; }

        // Last non-empty segment wins.
        std::vector<std::string> segs;
        std::size_t p = 0;
        while (true) {
            std::size_t cr = content.find('\r', p);
            segs.push_back(content.substr(p, cr == std::string::npos ? std::string::npos : cr - p));
            if (cr == std::string::npos) break;
            p = cr + 1;
        }
        std::string keep;
        for (auto it = segs.rbegin(); it != segs.rend(); ++it)
            if (!detail::IsBlank(*it)) { keep = *it; break; }
        removed += segs.size() - 1;
        out += keep + term;
    }
    text.swap(out);
    return removed;
}

// ── Pass 2 ────────────────────────────────────────────────────────
// Returns the number of records unwrapped.
inline std::size_t UnwrapNativeCommandErrors(std::string& text)
{
    if (text.find("NativeCommandError") == std::string::npos) return 0;

    const std::vector<std::string> lines = detail::SplitKeepNl(text);
    std::string out;
    out.reserve(text.size());
    std::size_t unwrapped = 0;
    std::size_t i = 0;
    while (i < lines.size()) {
        std::string c0, t0, msg;
        detail::SplitTerminator(lines[i], c0, t0);
        if (!detail::SplitRecordHead(c0, msg) || i + 1 >= lines.size()) {
            out += lines[i]; ++i; continue;
        }
        std::string c1, t1;
        detail::SplitTerminator(lines[i + 1], c1, t1);
        if (!detail::IsAtLinePosition(c1)) { out += lines[i]; ++i; continue; }

        // "+ ..." excerpt lines, then the two "    + Key : Value" lines.
        std::size_t j = i + 2;
        while (j < lines.size()) {
            std::string c, t;
            detail::SplitTerminator(lines[j], c, t);
            if (detail::StartsWith(c, "+ ") || c == "+") { ++j; continue; }
            break;
        }
        bool sawCategory = false, sawNative = false;
        while (j < lines.size()) {
            std::string c, t;
            detail::SplitTerminator(lines[j], c, t);
            const std::string tc = detail::Trim(c);
            if (detail::StartsWith(tc, "+ CategoryInfo")) { sawCategory = true; ++j; continue; }
            if (detail::StartsWith(tc, "+ FullyQualifiedErrorId")) {
                sawNative = tc.find(": NativeCommandError") != std::string::npos;
                ++j;
                break;
            }
            break;
        }
        if (!sawCategory || !sawNative) { out += lines[i]; ++i; continue; }

        // Swallow the single blank spacer line PowerShell prints after a record.
        if (j < lines.size()) {
            std::string c, t;
            detail::SplitTerminator(lines[j], c, t);
            if (detail::IsBlank(c)) ++j;
        }
        out += msg + (t0.empty() ? std::string("\r\n") : t0);
        ++unwrapped;
        i = j;
    }
    if (unwrapped) text.swap(out);
    return unwrapped;
}

// ── Pass 3 ────────────────────────────────────────────────────────

// A progress line, after pass 1 (one line per bar):
//   curl bar:     "########################   37.8%"  /  "#=#=#"  /  "##O#-#"
//   bare percent: "  12.0%"
//   tqdm:         "model.bin:  45%|████▌     | 1.2G/2.6G [00:31<00:36, 38MB/s]"
inline bool IsProgressLine(const std::string& raw)
{
    const std::string s = detail::Trim(raw);
    if (s.empty()) return false;

    // tqdm-style: "NN%|...|" plus a bracketed rate/eta tail.
    {
        const std::size_t bar = s.find("%|");
        if (bar != std::string::npos && bar > 0 &&
            std::isdigit((unsigned char)s[bar - 1]) &&
            s.find('|', bar + 2) != std::string::npos)
            return true;
    }

    // curl-style: only bar glyphs, digits, '.', '%', spaces.
    bool hasHash = false, hasPct = false;
    for (char ch : s) {
        const unsigned char c = (unsigned char)ch;
        if (c == '#') { hasHash = true; continue; }
        if (c == '%') { hasPct = true; continue; }
        if (std::isdigit(c) || c == '.' || c == ' ' || c == '\t' ||
            c == '=' || c == 'O' || c == '-')
            continue;
        return false;
    }
    if (hasPct) return true;
    // Spinner frames ("#=#=#", "##O#-#", "##O=#  #") must contain '#'.
    return hasHash && s.size() >= 3;
}

// Final percentage shown on a progress line, e.g. "100.0%" ("" if none).
inline std::string LastPercent(const std::string& line)
{
    const std::size_t pct = line.rfind('%');
    if (pct == std::string::npos) return std::string();
    std::size_t b = pct;
    while (b > 0 && (std::isdigit((unsigned char)line[b - 1]) || line[b - 1] == '.')) --b;
    if (b == pct) return std::string();
    return line.substr(b, pct + 1 - b);
}

// If stderr is nothing but progress lines (and blanks), replace it with
// one summary line appended to stdout and clear stderr.  Returns true
// when folded.
inline bool FoldProgressOnlyStderr(std::string& stdoutText, std::string& stderrText)
{
    if (detail::IsBlank(stderrText)) return false;

    std::vector<std::string> finals;
    std::size_t progressLines = 0;
    for (const std::string& line : detail::SplitKeepNl(stderrText)) {
        std::string c, t;
        detail::SplitTerminator(line, c, t);
        if (detail::IsBlank(c)) continue;
        if (!IsProgressLine(c)) return false;
        ++progressLines;
        const std::string p = LastPercent(c);
        if (!p.empty()) finals.push_back(p);
    }
    if (progressLines == 0) return false;

    std::string summary = "[progress output on stderr folded: ";
    if (finals.empty()) {
        summary += std::to_string(progressLines) + " progress line(s)";
    } else {
        summary += std::to_string(finals.size()) +
                   (finals.size() == 1 ? " progress bar, final " : " progress bars, final ");
        for (std::size_t k = 0; k < finals.size(); ++k) {
            if (k) summary += ", ";
            summary += finals[k];
        }
    }
    summary += "]";

    const bool crlf = stdoutText.find("\r\n") != std::string::npos ||
                      stderrText.find("\r\n") != std::string::npos;
    if (!stdoutText.empty() && stdoutText.back() != '\n')
        stdoutText += crlf ? "\r\n" : "\n";
    stdoutText += summary + (crlf ? "\r\n" : "\n");
    stderrText.clear();
    return true;
}

// All three passes, in order.  Call on captured output before
// large-output handling and before any LlamaBoss breadcrumbs.
inline void TidyCapturedStreams(std::string& stdoutText, std::string& stderrText)
{
    CollapseCarriageReturns(stdoutText);
    CollapseCarriageReturns(stderrText);
    UnwrapNativeCommandErrors(stderrText);
    UnwrapNativeCommandErrors(stdoutText);   // 2>&1 can land records in stdout too
    FoldProgressOnlyStderr(stdoutText, stderrText);
}

} // namespace lb_progressfold
