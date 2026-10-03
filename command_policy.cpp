// command_policy.cpp

#include "command_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace {

// ─── Auto-run allowlist: verb prefixes ──────────────────────────
// Any cmdlet whose name begins (case-insensitively) with one of
// these may remain on the silent read-only path, provided the full
// command also avoids broader shell syntax that now routes to approval.
constexpr std::array<const char*, 13> kVerbPrefixes = {
    "Get-",        "Test-",        "Measure-",
    "Select-",     "Where-",       "Sort-",
    "Group-",      "Compare-",     "ConvertTo-",
    "ConvertFrom-","Format-",      "Find-",
    "Resolve-"
};

// ─── Auto-run allowlist: exact names ────────────────────────────
// Read-only commands that don't follow the verb-prefix shape.
// Out-File and Tee-Object are deliberately NOT here — they may still
// be used, but only through the approval-gated PowerShell path.
//
// ForEach-Object is deliberately NOT here either.  Its positional
// -MemberName form invokes a METHOD by name with no braces, parens, or
// '$' for the scanner to catch:
//     Get-ChildItem C:\Docs -Recurse -File | ForEach-Object Delete
//     Get-Process | ForEach-Object Kill
// The classifier cannot tell a property read from a method call
// without type information, so ForEach-Object always routes to
// approval.  Select-Object -ExpandProperty covers the read-only case.
constexpr std::array<const char*, 8> kExactNames = {
    "Out-String", "Out-Default", "Out-Host", "Out-Null",
    "date", "whoami", "hostname", "echo"
};

struct ReviewChar {
    char        ch;
    const char* humanName;
};

// Characters that historically triggered hard rejection because they
// make a simplistic allowlist checker unsafe.  They are still not
// eligible for silent auto-run, but they now route to approval instead
// of being blocked outright.
//
// NOTE: ';' used to live in this list (flagged unconditionally as a
// "statement separator").  It has been promoted to a real statement
// boundary — see ScanAndSplitStages — because reflexively gating every
// semicolon punished the extremely common pattern of chaining several
// already-read-only diagnostic commands (e.g. a preference assignment
// followed by two Get- calls). Each ';'-separated statement is still
// independently required to be read-only-safe; nothing about that
// change widens what a single statement is allowed to do.
constexpr std::array<ReviewChar, 7> kReviewOutsideQuotes = {{
    { '&', "call/background operator '&'" },
    { '>', "redirection '>'" },
    { '<', "redirection '<'" },
    { '{', "script block '{'" },
    { '}', "script block '}'" },
    { '(', "grouping expression '('" },
    { ')', "grouping expression ')'" },
}};

constexpr std::array<const char*, 3> kReviewDigraphs = {
    "$(",   // subexpression
    "@(",   // array subexpression
    "@{"    // hashtable / expression-property literal
};

// ─── Narrow calculated-property allowance ───────────────────────
// A Select-Object / Format-Table "calculated property" is a hashtable
// literal shaped like:
//     @{ N='Label' ; E={ <expression> } }
// (N/Name/L/Label may be used interchangeably, as may E/Expression).
// This is one of the most common PowerShell idioms for producing
// readable diagnostic output (disk space in GB, file size in MB,
// etc.) and models reach for it constantly on ordinary read-only
// inspection tasks. TryConsumeCalculatedProperty recognizes ONLY this
// exact narrow shape, with the expression body restricted to a small
// safe grammar (property access off $_, numeric literals with PS size
// suffixes, arithmetic, and a short allowlist of static math methods).
// Anything that doesn't match this shape falls straight back to the
// original behavior of gating on '@{' and the individual braces —
// this is purely additive, never a relaxation for anything else.
constexpr std::array<const char*, 4> kCalcPropNameKeys = { "N", "Name", "L", "Label" };
constexpr std::array<const char*, 2> kCalcPropExprKeys = { "E", "Expression" };
constexpr std::array<const char*, 1> kSafeStaticTypes   = { "math" };
constexpr std::array<const char*, 4> kSafeStaticMethods = { "Round", "Floor", "Ceiling", "Truncate" };
constexpr std::array<const char*, 5> kSizeSuffixes      = { "KB", "MB", "GB", "TB", "PB" };

struct ScanResult {
    bool                                   ok = false;
    bool                                   requiresApproval = false;
    std::string                            reason;
    // Top-level ';'-separated statements; each is itself a list of
    // '|'-separated pipeline stages.
    std::vector<std::vector<std::string>>  statements;
};

// ─── Helpers ────────────────────────────────────────────────────

bool StartsWithCi(const std::string& s, const char* prefix) {
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (i >= s.size()) return false;
        if (std::tolower((unsigned char)s[i]) !=
            std::tolower((unsigned char)prefix[i])) return false;
        ++i;
    }
    return true;
}

bool EqualsCi(const std::string& s, const char* other) {
    size_t i = 0;
    while (other[i] != '\0' && i < s.size()) {
        if (std::tolower((unsigned char)s[i]) !=
            std::tolower((unsigned char)other[i])) return false;
        ++i;
    }
    return other[i] == '\0' && i == s.size();
}

template <size_t N>
bool CiInArray(const std::string& v, const std::array<const char*, N>& arr) {
    for (const char* item : arr) if (EqualsCi(v, item)) return true;
    return false;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string FirstToken(const std::string& stage) {
    std::string t = Trim(stage);
    if (t.empty()) return {};
    size_t end = t.find_first_of(" \t\r\n");
    return (end == std::string::npos) ? t : t.substr(0, end);
}

bool IsIdentifierShape(const std::string& head) {
    if (head.empty()) return false;
    for (char c : head) {
        const bool ok = std::isalnum((unsigned char)c) ||
                        c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool HeadVerbAutoAllowed(const std::string& head, std::string& reasonOut) {
    if (head.empty()) {
        reasonOut = "empty pipeline stage";
        return false;
    }
    if (!IsIdentifierShape(head)) {
        reasonOut = "command head '" + head +
                    "' is outside the simple read-only cmdlet shape";
        return false;
    }

    for (const char* name : kExactNames) {
        if (EqualsCi(head, name)) return true;
    }

    for (const char* pfx : kVerbPrefixes) {
        if (StartsWithCi(head, pfx)) return true;
    }

    reasonOut = "command '" + head +
                "' is outside the automatic read-only allowlist";
    return false;
}

bool IsReviewOutsideQuoteChar(char c, const char*& humanNameOut) {
    for (const auto& rc : kReviewOutsideQuotes) {
        if (c == rc.ch) {
            humanNameOut = rc.humanName;
            return true;
        }
    }
    return false;
}

bool HasReviewDigraphAt(const std::string& cmd, size_t i, const char*& digraphOut) {
    for (const char* d : kReviewDigraphs) {
        const size_t n = std::char_traits<char>::length(d);
        if (i + n <= cmd.size() && cmd.compare(i, n, d) == 0) {
            digraphOut = d;
            return true;
        }
    }
    return false;
}

void MarkApproval(ScanResult& out, const std::string& reason) {
    if (!out.requiresApproval) {
        out.requiresApproval = true;
        out.reason = reason;
    }
}

// ─── Safe-expression micro-grammar (calculated-property bodies) ──

bool IsIdentStartChar(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool IsIdentChar(char c)      { return std::isalnum((unsigned char)c) || c == '_'; }

void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

std::string ReadIdent(const std::string& s, size_t& i) {
    size_t start = i;
    if (i < s.size() && IsIdentStartChar(s[i])) {
        ++i;
        while (i < s.size() && IsIdentChar(s[i])) ++i;
    }
    return s.substr(start, i - start);
}

// A single- or double-quoted string literal. Double-quoted strings
// containing '$' are rejected outright (interpolation risk) rather
// than partially matched.
bool TryMatchSafeStringLiteral(const std::string& s, size_t& i) {
    size_t j = i;
    if (j >= s.size()) return false;
    if (s[j] == '\'') {
        ++j;
        while (j < s.size()) {
            if (s[j] == '\'') {
                if (j + 1 < s.size() && s[j + 1] == '\'') { j += 2; continue; }
                ++j; i = j; return true;
            }
            ++j;
        }
        return false; // unterminated
    }
    if (s[j] == '"') {
        ++j;
        while (j < s.size()) {
            if (s[j] == '"') { ++j; i = j; return true; }
            if (s[j] == '$') return false; // interpolation risk
            ++j;
        }
        return false; // unterminated
    }
    return false;
}

// $_.Prop.Prop... property-access chain.
bool TryMatchDollarUnderscoreChain(const std::string& s, size_t& i) {
    if (i + 1 >= s.size() || s[i] != '$' || s[i + 1] != '_') return false;
    size_t j = i + 2;
    while (j < s.size() && s[j] == '.') {
        size_t k = j + 1;
        std::string ident = ReadIdent(s, k);
        if (ident.empty()) return false;
        j = k;
    }
    i = j;
    return true;
}

// Numeric literal with an optional PowerShell size suffix (1GB, 2.5MB).
bool TryMatchNumberLiteral(const std::string& s, size_t& i) {
    size_t j = i;
    bool any = false;
    while (j < s.size() && std::isdigit((unsigned char)s[j])) { ++j; any = true; }
    if (j < s.size() && s[j] == '.') {
        size_t k = j + 1;
        bool anyFrac = false;
        while (k < s.size() && std::isdigit((unsigned char)s[k])) { ++k; anyFrac = true; }
        if (anyFrac) { j = k; any = true; }
    }
    if (!any) return false;

    for (const char* suf : kSizeSuffixes) {
        const size_t n = std::char_traits<char>::length(suf);
        if (j + n <= s.size()) {
            bool matched = true;
            for (size_t t = 0; t < n; ++t) {
                if (std::tolower((unsigned char)s[j + t]) != std::tolower((unsigned char)suf[t])) {
                    matched = false;
                    break;
                }
            }
            if (matched && !(j + n < s.size() && IsIdentChar(s[j + n]))) {
                i = j + n;
                return true;
            }
        }
    }
    i = j;
    return true;
}

bool TryMatchSafeExpr(const std::string& s, size_t& i);

// [math]::Round( <safe expr>, ... )  — allowlisted type + method only.
bool TryMatchStaticCall(const std::string& s, size_t& i) {
    size_t j = i;
    if (j >= s.size() || s[j] != '[') return false;
    ++j;
    std::string type = ReadIdent(s, j);
    if (type.empty() || !CiInArray(type, kSafeStaticTypes)) return false;
    if (j >= s.size() || s[j] != ']') return false;
    ++j;
    if (j + 1 >= s.size() || s[j] != ':' || s[j + 1] != ':') return false;
    j += 2;
    std::string method = ReadIdent(s, j);
    if (method.empty() || !CiInArray(method, kSafeStaticMethods)) return false;
    SkipWs(s, j);
    if (j >= s.size() || s[j] != '(') return false;
    ++j;
    SkipWs(s, j);
    if (j < s.size() && s[j] != ')') {
        for (;;) {
            if (!TryMatchSafeExpr(s, j)) return false;
            SkipWs(s, j);
            if (j < s.size() && s[j] == ',') { ++j; SkipWs(s, j); continue; }
            break;
        }
    }
    if (j >= s.size() || s[j] != ')') return false;
    ++j;
    i = j;
    return true;
}

bool TryMatchSafeTerm(const std::string& s, size_t& i) {
    SkipWs(s, i);
    size_t j = i;
    if (j < s.size() && s[j] == '(') {
        ++j;
        if (!TryMatchSafeExpr(s, j)) return false;
        SkipWs(s, j);
        if (j >= s.size() || s[j] != ')') return false;
        ++j;
        i = j;
        return true;
    }
    if (TryMatchStaticCall(s, j))            { i = j; return true; }
    if (TryMatchDollarUnderscoreChain(s, j)) { i = j; return true; }
    if (TryMatchNumberLiteral(s, j))         { i = j; return true; }
    return false;
}

// term ( (+|-|*|/|%) term )*
bool TryMatchSafeExpr(const std::string& s, size_t& i) {
    size_t j = i;
    if (!TryMatchSafeTerm(s, j)) return false;
    for (;;) {
        SkipWs(s, j);
        if (j < s.size() && (s[j] == '+' || s[j] == '-' || s[j] == '*' || s[j] == '/' || s[j] == '%')) {
            ++j;
            if (!TryMatchSafeTerm(s, j)) return false;
            continue;
        }
        break;
    }
    i = j;
    return true;
}

// @{ N='Label' ; E={ <safe expr> } }  (N/E in either order, either key alias).
// Requires exactly one name-like key and one expression-like key; anything
// else (unknown keys, duplicates, missing piece) fails closed.
bool TryConsumeCalculatedProperty(const std::string& cmd, size_t& i) {
    size_t j = i;
    if (j + 1 >= cmd.size() || cmd[j] != '@' || cmd[j + 1] != '{') return false;
    j += 2;

    bool sawName = false, sawExpr = false;
    int pairs = 0;
    for (;;) {
        SkipWs(cmd, j);
        std::string key = ReadIdent(cmd, j);
        if (key.empty()) return false;
        SkipWs(cmd, j);
        if (j >= cmd.size() || cmd[j] != '=') return false;
        ++j;
        SkipWs(cmd, j);

        const bool isNameKey = CiInArray(key, kCalcPropNameKeys);
        const bool isExprKey = CiInArray(key, kCalcPropExprKeys);

        if (isNameKey && !sawName) {
            if (!TryMatchSafeStringLiteral(cmd, j)) return false;
            sawName = true;
        } else if (isExprKey && !sawExpr) {
            if (j >= cmd.size() || cmd[j] != '{') return false;
            ++j;
            if (!TryMatchSafeExpr(cmd, j)) return false;
            SkipWs(cmd, j);
            if (j >= cmd.size() || cmd[j] != '}') return false;
            ++j;
            sawExpr = true;
        } else {
            return false; // unknown key, or this key already used once
        }

        ++pairs;
        SkipWs(cmd, j);
        if (j < cmd.size() && cmd[j] == ';') {
            if (pairs >= 4) return false; // defensive bound, shouldn't trigger
            ++j;
            continue;
        }
        break;
    }

    SkipWs(cmd, j);
    if (j >= cmd.size() || cmd[j] != '}') return false;
    ++j;

    if (!sawName || !sawExpr) return false;

    i = j;
    return true;
}

// $Identifier = <literal>  — the whole statement, nothing else.  Covers
// the extremely common `$ErrorActionPreference='SilentlyContinue'` /
// `$ProgressPreference='SilentlyContinue'` idiom models chain in front
// of read-only diagnostic calls. RHS is restricted to $true/$false, a
// plain integer, or a quoted literal (no interpolation, no
// subexpressions) — assigning a local session variable has no side
// effect beyond that assignment itself.
bool IsSafeSimpleAssignment(const std::string& stmt) {
    size_t i = 0;
    SkipWs(stmt, i);
    if (i >= stmt.size() || stmt[i] != '$') return false;
    ++i;
    std::string name = ReadIdent(stmt, i);
    if (name.empty()) return false;
    SkipWs(stmt, i);
    if (i >= stmt.size() || stmt[i] != '=') return false;
    if (i + 1 < stmt.size() && stmt[i + 1] == '=') return false; // not '=='
    ++i;
    SkipWs(stmt, i);

    if (stmt.compare(i, 5, "$true") == 0 && i + 5 == stmt.size()) {
        i += 5;
    } else if (stmt.compare(i, 6, "$false") == 0 && i + 6 == stmt.size()) {
        i += 6;
    } else if (i < stmt.size() && (stmt[i] == '\'' || stmt[i] == '"')) {
        if (!TryMatchSafeStringLiteral(stmt, i)) return false;
    } else {
        size_t start = i;
        while (i < stmt.size() && std::isdigit((unsigned char)stmt[i])) ++i;
        if (i == start) return false;
    }

    SkipWs(stmt, i);
    return i == stmt.size();
}

// Scan once, respecting quotes:
//   * recognize the narrow calculated-property shape and skip over it
//     wholesale, without flagging its internal braces/parens
//   * split top-level statements on ';' and pipeline stages on '|',
//     both outside quotes and outside a consumed calculated property
//   * mark broader shell syntax as approval-required instead of rejecting it
//   * reject only quote drift that makes the scanner unable to classify command text
//
// Single quotes are treated as literal PowerShell strings.  A doubled
// single quote inside a single-quoted string is accepted and skipped.
// PowerShell here-strings:
//
//   @'<line break>          @"<line break>
//   ...literal body...      ...expandable body...
//   <line break>'@          <line break>"@
//
// The opener is @' or @" followed only by spaces/tabs and a line break;
// the closer is '@ or "@ at the very start of a line.  Inside the body,
// quotes of either kind are ordinary characters, so a C++ literal such
// as L'"' in a single-quoted here-string must NOT flip the scanner's
// quote state.  Before this rule existed the scanner did exactly that,
// drifted out of phase, and rejected valid scripts as "unterminated
// double-quoted string".
//
// On entry `i` indexes the '@'.  Returns:
//   kNotHereString  - not an opener; caller scans '@' normally
//   kHereString     - consumed; `i` is the last char of the closer and
//                     `quote`, `bodyStart`, `bodyEnd` describe the body
//   kUnterminated   - opener with no closer
enum class HereStringScan { kNotHereString, kHereString, kUnterminated };

HereStringScan TryScanHereString(const std::string& cmd, size_t& i,
                                 char& quote, size_t& bodyStart,
                                 size_t& bodyEnd) {
    if (cmd[i] != '@' || i + 1 >= cmd.size()) return HereStringScan::kNotHereString;
    const char q = cmd[i + 1];
    if (q != '\'' && q != '"') return HereStringScan::kNotHereString;

    size_t j = i + 2;
    while (j < cmd.size() && (cmd[j] == ' ' || cmd[j] == '\t')) ++j;
    if (j >= cmd.size() || (cmd[j] != '\r' && cmd[j] != '\n'))
        return HereStringScan::kNotHereString;

    // Body begins after the opener's line break (CRLF or LF).
    size_t body = j;
    if (cmd[body] == '\r' && body + 1 < cmd.size() && cmd[body + 1] == '\n') ++body;
    ++body;

    // Closer: q '@' at the start of a line.  Searching from j (the
    // opener's own line break) also accepts an empty body.
    const std::string closer = std::string("\n") + q + "@";
    const size_t close = cmd.find(closer, j);
    if (close == std::string::npos) return HereStringScan::kUnterminated;

    quote     = q;
    bodyStart = body;
    bodyEnd   = close;                    // body excludes the final line break
    i         = close + closer.size() - 1;
    return HereStringScan::kHereString;
}

ScanResult ScanAndSplitStages(const std::string& cmd) {
    ScanResult out;

    bool inSingle = false;
    bool inDouble = false;

    size_t stageStart = 0;
    std::vector<std::string> curStatementStages;

    for (size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];

        // Backtick is PowerShell's escape/line-continuation character.
        // It remains valid for an approved command, but it is too rich
        // for the silent read-only classifier.  Outside single quotes it
        // escapes the NEXT character, which must be skipped: otherwise
        // `" inside "…" closes the string early and a valid command like
        //   Write-Output "hello`""
        // is rejected as unterminated.  Inside single quotes a backtick
        // is literal (no escape), so nothing is skipped there.
        if (c == '`') {
            MarkApproval(out, "backtick escape syntax requires approval");
            if (!inSingle && i + 1 < cmd.size()) ++i;
            continue;
        }

        if (inSingle) {
            if (c == '\'') {
                // PowerShell literal quote escape: 'can''t'
                if (i + 1 < cmd.size() && cmd[i + 1] == '\'') {
                    ++i;
                    continue;
                }
                inSingle = false;
            }
            continue;
        }

        if (inDouble) {
            if (c == '"') {
                inDouble = false;
                continue;
            }
            if (c == '$') {
                MarkApproval(out, "double-quoted variable/interpolation syntax requires approval");
            }
            continue;
        }

        // Outside quotes from here down.

        // Comments.  Their text is inert, so quotes, pipes and ';' inside
        // them must not be scanned: "Get-Item . # user's folder" used to
        // be rejected as an unterminated single-quoted string.
        //
        //   * '#' at the start or after whitespace opens a line comment
        //     that PowerShell ignores to end of line.  Skip to (not past)
        //     the newline, so multi-line commands still require approval.
        //     A '#' glued to other text (a#b, .#) is an ordinary character
        //     in PowerShell and is left alone, so classification never
        //     skips text PowerShell would run.
        //   * '<#' opens a block comment: approval required, content
        //     skipped, and an unterminated one stays a hard reject.
        //     Belt and braces: a comment that contains command
        //     separators still requires approval (as it did before this
        //     rule existed), so skipping comment text can never widen
        //     what auto-runs even if this rule and PowerShell disagreed.
        if (c == '#' &&
            (i == 0 || cmd[i - 1] == ' ' || cmd[i - 1] == '\t')) {
            const size_t commentStart = i;
            while (i + 1 < cmd.size() && cmd[i + 1] != '\n' && cmd[i + 1] != '\r')
                ++i;
            const std::string text = cmd.substr(commentStart, i + 1 - commentStart);
            if (text.find_first_of(";|&`") != std::string::npos) {
                MarkApproval(out, "comment containing command separators requires approval");
            }
            continue;
        }
        if (c == '<' && i + 1 < cmd.size() && cmd[i + 1] == '#') {
            const size_t close = cmd.find("#>", i + 2);
            if (close == std::string::npos) {
                out.reason = "unterminated block comment";
                return out;
            }
            MarkApproval(out, "block comment syntax requires approval");
            i = close + 1;   // for-loop's ++i lands just past "#>"
            continue;
        }

        // Here-strings span lines, so they always require approval (as
        // any multi-line command already does).  Their bodies are skipped
        // whole: no quote, separator or digraph inside one is scanned.
        if (c == '@') {
            char   q = 0;
            size_t bodyStart = 0, bodyEnd = 0;
            const HereStringScan hs = TryScanHereString(cmd, i, q, bodyStart, bodyEnd);
            if (hs == HereStringScan::kUnterminated) {
                // i still indexes the '@', so cmd[i + 1] is the quote.
                out.reason = (cmd[i + 1] == '"')
                    ? "unterminated double-quoted here-string (closing \"@ must start a line)"
                    : "unterminated single-quoted here-string (closing '@ must start a line)";
                return out;
            }
            if (hs == HereStringScan::kHereString) {
                MarkApproval(out, q == '"'
                    ? "double-quoted here-string (expandable) requires approval"
                    : "here-string syntax requires approval");
                continue;   // for-loop's ++i lands just past the closer
            }
        }

        if (c == '\'') {
            inSingle = true;
            continue;
        }
        if (c == '"') {
            inDouble = true;
            continue;
        }

        if (c == '\r' || c == '\n') {
            MarkApproval(out, "multi-line PowerShell syntax requires approval");
            continue;
        }

        // Narrow calculated-property allowance: try to consume the
        // whole "@{ N=...; E={...} }" span before it can trip the
        // generic '@{' digraph / brace checks below. Falls through
        // unchanged (still flagged) if it doesn't match that exact shape.
        if (c == '@' && i + 1 < cmd.size() && cmd[i + 1] == '{') {
            size_t newI = i;
            if (TryConsumeCalculatedProperty(cmd, newI)) {
                i = newI - 1; // for-loop's ++i lands exactly past the span
                continue;
            }
        }

        const char* digraph = nullptr;
        if (HasReviewDigraphAt(cmd, i, digraph)) {
            MarkApproval(out, std::string("PowerShell expression syntax '") + digraph + "' requires approval");
        }

        const char* humanName = nullptr;
        if (IsReviewOutsideQuoteChar(c, humanName)) {
            MarkApproval(out, std::string(humanName) + " requires approval");
        }

        if (c == '|') {
            curStatementStages.push_back(cmd.substr(stageStart, i - stageStart));
            stageStart = i + 1;
            continue;
        }

        if (c == ';') {
            curStatementStages.push_back(cmd.substr(stageStart, i - stageStart));
            out.statements.push_back(curStatementStages);
            curStatementStages.clear();
            stageStart = i + 1;
            continue;
        }
    }

    if (inSingle) {
        out.reason = "unterminated single-quoted string";
        return out;
    }
    if (inDouble) {
        out.reason = "unterminated double-quoted string";
        return out;
    }

    curStatementStages.push_back(cmd.substr(stageStart));
    out.statements.push_back(curStatementStages);
    out.ok = true;
    return out;
}

} // namespace

// ─── Public entry point ─────────────────────────────────────────

PolicyDecision EvaluatePowerShellCommand(const std::string& cmdIn) {
    PolicyDecision out;

    const std::string cmd = Trim(cmdIn);
    if (cmd.empty()) {
        out.reason = "empty command";
        return out;
    }

    ScanResult scan = ScanAndSplitStages(cmd);
    if (!scan.ok) {
        out.reason = scan.reason;
        return out;
    }

    if (scan.requiresApproval) {
        out.requiresApproval = true;
        out.reason = scan.reason;
        return out;
    }

    for (size_t s = 0; s < scan.statements.size(); ++s) {
        const std::vector<std::string>& stages = scan.statements[s];

        // A whole statement that is exactly a safe simple assignment
        // (e.g. $ErrorActionPreference='SilentlyContinue') is allowed
        // without going through the cmdlet-head check.
        if (stages.size() == 1 && IsSafeSimpleAssignment(Trim(stages[0]))) {
            continue;
        }

        for (size_t i = 0; i < stages.size(); ++i) {
            std::string head = FirstToken(stages[i]);
            std::string reason;
            if (!HeadVerbAutoAllowed(head, reason)) {
                out.requiresApproval = true;
                out.reason = "statement " + std::to_string(s + 1) +
                             ", stage " + std::to_string(i + 1) + ": " + reason;
                return out;
            }
        }
    }

    out.allowed = true;
    return out;
}


// ─── Advisory lint: hazardous-but-legal string constructs ───────
//
// See command_policy.h for the contract.  Kept deliberately narrow:
// two checks with low false-positive rates.  Warnings ride on the
// tool result so the model can self-correct on the next call; they
// never influence the allow/approve/reject decision above.

namespace {

// Return true when `command` contains an assignment to $name
// ("$name =" / "$name=" / "${name} ="), scanning case-insensitively
// the way PowerShell resolves variables.
bool HasAssignmentTo(const std::string& command, const std::string& name)
{
    auto ciFind = [](const std::string& hay, const std::string& needle,
                     size_t from) -> size_t {
        if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
        for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
            size_t j = 0;
            while (j < needle.size() &&
                   std::tolower((unsigned char)hay[i + j]) ==
                   std::tolower((unsigned char)needle[j])) {
                ++j;
            }
            if (j == needle.size()) return i;
        }
        return std::string::npos;
    };

    const std::string bare   = "$"  + name;
    const std::string braced = "${" + name + "}";

    for (const std::string& form : { bare, braced }) {
        size_t pos = 0;
        while ((pos = ciFind(command, form, pos)) != std::string::npos) {
            size_t after = pos + form.size();
            // Bare form must not be a prefix of a longer identifier.
            if (form[1] != '{' && after < command.size() &&
                IsIdentChar(command[after])) {
                ++pos;
                continue;
            }
            size_t k = after;
            while (k < command.size() &&
                   (command[k] == ' ' || command[k] == '\t')) {
                ++k;
            }
            if (k < command.size() && command[k] == '=' &&
                (k + 1 >= command.size() || command[k + 1] != '=')) {
                return true;
            }
            ++pos;
        }
    }
    return false;
}

} // namespace

std::vector<std::string> LintPowerShellHazards(const std::string& command)
{
    std::vector<std::string> warnings;

    // ── 1. Backtick-escaped double quote anywhere in the command ──
    // Legal PowerShell, and -EncodedCommand delivers it intact here,
    // but it is the construct most often mangled when the model
    // regenerates or relays the text through any other quoting layer,
    // and it is almost never necessary: single quotes inside a
    // double-quoted string interpolate-safely without escaping.
    if (command.find("`\"") != std::string::npos) {
        warnings.push_back(
            "backtick-escaped double quote (`\") present. Prefer single "
            "quotes inside a double-quoted string when interpolation is "
            "needed, or the -f format operator when it is not; escaped "
            "double quotes are the construct most often corrupted when "
            "code is regenerated or relayed.");
    }

    // ── 2. $Name_With_Underscores inside a double-quoted string ───
    // In a double-quoted string PowerShell greedily parses the longest
    // identifier after $, so "$Version_DRAFT" is ONE variable, not
    // $Version + "_DRAFT".  Flag underscore-bearing variables used in
    // a double-quoted context when the command never assigns them —
    // strong signal the author meant ${Shorter}_literal.
    std::vector<std::string> flagged;

    // Shared $Name_With_Underscores check for one '$' inside an
    // expandable context.  `i` indexes the '$'; on return it indexes the
    // last character consumed.
    auto checkDollar = [&](size_t& i) {
        if (i + 1 >= command.size()) return;
        const char n = command[i + 1];
        if (n == '{' || n == '(') return;          // braced / subexpr: fine
        if (!IsIdentStartChar(n)) return;
        size_t j = i + 1;
        std::string name;
        while (j < command.size() && IsIdentChar(command[j])) {
            name += command[j];
            ++j;
        }
        i = j - 1;
        if (name.size() < 2) return;               // skip $_ and 1-char
        if (name.find('_') == std::string::npos) return;
        for (const std::string& f : flagged) {
            if (EqualsCi(f, name.c_str())) return;
        }
        if (!HasAssignmentTo(command, name)) flagged.push_back(name);
    };

    bool inSingle = false, inDouble = false;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];

        // Here-strings: skip literal bodies, scan expandable bodies for
        // $ names, and never let quotes inside either flip the state.
        if (!inSingle && !inDouble && c == '@') {
            char   q = 0;
            size_t bodyStart = 0, bodyEnd = 0;
            size_t k = i;
            if (TryScanHereString(command, k, q, bodyStart, bodyEnd) ==
                HereStringScan::kHereString) {
                if (q == '"') {
                    for (size_t b = bodyStart; b < bodyEnd; ++b) {
                        if (command[b] == '`') { ++b; continue; }
                        if (command[b] == '$') checkDollar(b);
                    }
                }
                i = k;
                continue;
            }
        }

        if (inSingle) {
            if (c == '\'') {
                if (i + 1 < command.size() && command[i + 1] == '\'') ++i;
                else inSingle = false;
            }
            continue;
        }
        if (inDouble) {
            if (c == '`') { ++i; continue; }          // escape next char
            if (c == '"') {
                if (i + 1 < command.size() && command[i + 1] == '"') ++i;
                else inDouble = false;
                continue;
            }
            if (c == '$') checkDollar(i);
            continue;
        }
        if (c == '\'')      inSingle = true;
        else if (c == '"')  inDouble = true;
        else if (c == '`')  ++i;                       // escape outside strings
    }
    for (const std::string& name : flagged) {
        warnings.push_back(
            "\"$" + name + "\" inside a double-quoted string parses as ONE "
            "variable name (underscores are identifier characters), and no "
            "assignment to $" + name + " appears in this command. If a "
            "shorter variable plus literal text was intended, brace it: "
            "${Var}_suffix.");
    }

    return warnings;
}
