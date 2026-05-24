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
constexpr std::array<const char*, 9> kExactNames = {
    "ForEach-Object",
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
constexpr std::array<ReviewChar, 8> kReviewOutsideQuotes = {{
    { ';', "statement separator ';'" },
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

struct ScanResult {
    bool                     ok = false;
    bool                     requiresApproval = false;
    std::string              reason;
    std::vector<std::string> stages;
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

// Scan once, respecting quotes:
//   * split pipeline stages only on | outside quotes
//   * mark broader shell syntax as approval-required instead of rejecting it
//   * reject only quote drift that makes the scanner unable to classify command text
//
// Single quotes are treated as literal PowerShell strings.  A doubled
// single quote inside a single-quoted string is accepted and skipped.
ScanResult ScanAndSplitStages(const std::string& cmd) {
    ScanResult out;

    bool inSingle = false;
    bool inDouble = false;

    size_t stageStart = 0;

    for (size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];

        // Backtick is PowerShell's escape/line-continuation character.
        // It remains valid for an approved command, but it is too rich
        // for the silent read-only classifier.
        if (c == '`') {
            MarkApproval(out, "backtick escape syntax requires approval");
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

        const char* digraph = nullptr;
        if (HasReviewDigraphAt(cmd, i, digraph)) {
            MarkApproval(out, std::string("PowerShell expression syntax '") + digraph + "' requires approval");
        }

        const char* humanName = nullptr;
        if (IsReviewOutsideQuoteChar(c, humanName)) {
            MarkApproval(out, std::string(humanName) + " requires approval");
        }

        if (c == '|') {
            out.stages.push_back(cmd.substr(stageStart, i - stageStart));
            stageStart = i + 1;
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

    out.stages.push_back(cmd.substr(stageStart));
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

    for (size_t i = 0; i < scan.stages.size(); ++i) {
        std::string head = FirstToken(scan.stages[i]);
        std::string reason;
        if (!HeadVerbAutoAllowed(head, reason)) {
            out.requiresApproval = true;
            out.reason = "stage " + std::to_string(i + 1) + ": " + reason;
            return out;
        }
    }

    out.allowed = true;
    return out;
}
