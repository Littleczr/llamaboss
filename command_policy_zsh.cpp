// command_policy_zsh.cpp
//
// See command_policy_zsh.h.  Mirrors the shape of the PowerShell policy:
// a quote-aware scan first (reject malformed, escalate risky syntax), then
// a per-stage head allowlist with argument checks for the few read-only
// programs that also have a writing mode.
#include "command_policy_zsh.h"

#include <set>
#include <vector>

namespace {

std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool IsIdentStart(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool IsIdentChar(char c)
{
    return IsIdentStart(c) || (c >= '0' && c <= '9');
}

PolicyDecision Approval(const std::string& reason)
{
    PolicyDecision d;
    d.requiresApproval = true;
    d.reason = reason;
    return d;
}

PolicyDecision Reject(const std::string& reason)
{
    PolicyDecision d;
    d.reason = reason;
    return d;
}

// One pipeline stage, split into words with quotes removed.
using Words = std::vector<std::string>;

// Quote-aware scan of the whole command.  Fills `stages` (split on single
// '|') and returns a decision only when the command must be rejected or
// escalated; otherwise returns a default decision with allowed=false and
// requiresApproval=false and sets ok=true.
bool ScanCommand(const std::string& cmd, std::vector<Words>& stages,
                 PolicyDecision& early)
{
    Words words;
    std::string word;
    bool inWord = false;

    auto endWord = [&]() {
        if (inWord) words.push_back(word);
        word.clear();
        inWord = false;
    };
    auto endStage = [&]() {
        endWord();
        stages.push_back(words);
        words.clear();
    };

    // Accepts "$NAME" only; anything else after '$' needs review.
    auto checkDollar = [&](size_t i) -> bool {
        return i + 1 < cmd.size() && IsIdentStart(cmd[i + 1]);
    };

    const size_t n = cmd.size();
    for (size_t i = 0; i < n; ++i) {
        const char c = cmd[i];
        const unsigned char uc = static_cast<unsigned char>(c);

        if (uc >= 0x80) {
            early = Approval("non-ASCII characters are not eligible for automatic run");
            return false;
        }
        if (c == '\n' || c == '\r') {
            early = Approval("multi-line commands need review");
            return false;
        }

        if (c == '\'') {
            const size_t close = cmd.find('\'', i + 1);
            if (close == std::string::npos) {
                early = Reject("unterminated single-quoted string");
                return false;
            }
            word.append(cmd, i + 1, close - i - 1);
            inWord = true;
            i = close;
            continue;
        }

        if (c == '"') {
            size_t j = i + 1;
            for (; j < n; ++j) {
                const char d = cmd[j];
                if (static_cast<unsigned char>(d) >= 0x80) {
                    early = Approval("non-ASCII characters are not eligible for automatic run");
                    return false;
                }
                if (d == '\\' && j + 1 < n) { word.push_back(cmd[++j]); continue; }
                if (d == '"') break;
                if (d == '`') {
                    early = Approval("command substitution needs review");
                    return false;
                }
                if (d == '$' && !checkDollar(j)) {
                    early = Approval("shell expansion other than $NAME needs review");
                    return false;
                }
                word.push_back(d);
            }
            if (j >= n) {
                early = Reject("unterminated double-quoted string");
                return false;
            }
            inWord = true;
            i = j;
            continue;
        }

        if (c == '\\') {
            if (i + 1 < n) { word.push_back(cmd[++i]); inWord = true; }
            continue;
        }

        if (c == ' ' || c == '\t') { endWord(); continue; }

        if (c == '|') {
            if (i + 1 < n && (cmd[i + 1] == '|' || cmd[i + 1] == '&')) {
                early = Approval("command separators (||, |&) need review");
                return false;
            }
            endStage();
            continue;
        }

        if (c == '>' || c == '<') {
            // Discarding or merging stderr/stdout is harmless and common
            // in read-only inspection: 2>/dev/null, >/dev/null, 2>&1.
            const bool fdTwo = inWord && word == "2" && c == '>';
            const std::string rest = cmd.substr(i + 1);
            if (c == '>' && (rest.rfind("/dev/null", 0) == 0 ||
                             rest.rfind(" /dev/null", 0) == 0)) {
                const size_t skip = rest[0] == ' ' ? 10 : 9;
                const size_t after = i + 1 + skip;
                if (after >= n || cmd[after] == ' ' || cmd[after] == '\t' || cmd[after] == '|') {
                    if (fdTwo) { word.clear(); inWord = false; }
                    i = after - 1;
                    continue;
                }
            }
            if (fdTwo && rest.rfind("&1", 0) == 0) {
                word.clear();
                inWord = false;
                i += 2;
                continue;
            }
            early = Approval("redirection other than 2>/dev/null or 2>&1 needs review");
            return false;
        }

        if (c == ';' || c == '&') {
            early = Approval("command separators or background (& ;) need review");
            return false;
        }
        if (c == '(' || c == ')' || c == '{' || c == '}') {
            early = Approval("grouping, subshells and glob qualifiers need review");
            return false;
        }
        if (c == '`') {
            early = Approval("command substitution needs review");
            return false;
        }
        if (c == '$' && !checkDollar(i)) {
            early = Approval("shell expansion other than $NAME needs review");
            return false;
        }

        word.push_back(c);
        inWord = true;
    }
    endStage();
    return true;
}

bool HasArg(const Words& w, const std::set<std::string>& flags)
{
    for (size_t i = 1; i < w.size(); ++i)
        if (flags.count(w[i])) return true;
    return false;
}

bool HasArgPrefix(const Words& w, const std::string& prefix)
{
    for (size_t i = 1; i < w.size(); ++i)
        if (w[i].rfind(prefix, 0) == 0) return true;
    return false;
}

size_t CountOperands(const Words& w)
{
    size_t n = 0;
    for (size_t i = 1; i < w.size(); ++i)
        if (w[i].empty() || w[i][0] != '-') ++n;
    return n;
}

// Returns true when `w` (one stage) is clearly read-only.
bool StageIsReadOnly(const Words& w, std::string& reason)
{
    if (w.empty()) {
        reason = "empty pipeline stage";
        return false;
    }
    const std::string& head = w[0];

    if (head.find('=') != std::string::npos) {
        reason = "environment assignments before a command need review";
        return false;
    }
    if (head.find('/') != std::string::npos) {
        reason = "running a program by path needs review";
        return false;
    }

    static const std::set<std::string> kPlain = {
        "ls", "cat", "head", "tail", "wc", "grep", "egrep", "fgrep",
        "stat", "du", "df", "pwd", "echo", "printf", "cal", "uname",
        "sw_vers", "arch", "which", "whereis", "type", "whoami", "id",
        "groups", "printenv", "cut", "tr", "nl", "column", "diff", "cmp",
        "comm", "basename", "dirname", "realpath", "readlink", "md5",
        "shasum", "cksum", "hexdump", "od", "strings", "uptime", "ps",
        "vm_stat", "mdfind", "mdls", "system_profiler", "otool",
    };
    if (kPlain.count(head)) return true;

    if (head == "find") {
        if (HasArg(w, {"-exec", "-execdir", "-ok", "-okdir", "-delete",
                       "-fprint", "-fprint0", "-fprintf", "-fls"})) {
            reason = "find with -exec/-delete/-fprint needs review";
            return false;
        }
        return true;
    }
    if (head == "sort") {
        if (HasArg(w, {"-o"}) || HasArgPrefix(w, "--output")) {
            reason = "sort -o writes a file";
            return false;
        }
        return true;
    }
    if (head == "uniq") {
        if (CountOperands(w) > 1) {
            reason = "uniq with an output file writes a file";
            return false;
        }
        return true;
    }
    if (head == "rg") {
        if (HasArgPrefix(w, "--pre")) {
            reason = "rg --pre runs a program";
            return false;
        }
        return true;
    }
    if (head == "file") {
        if (HasArg(w, {"-C"})) {
            reason = "file -C writes a magic file";
            return false;
        }
        return true;
    }
    if (head == "tree") {
        if (HasArg(w, {"-o"})) {
            reason = "tree -o writes a file";
            return false;
        }
        return true;
    }
    if (head == "xxd") {
        if (HasArg(w, {"-r", "-revert"}) || CountOperands(w) > 1) {
            reason = "xxd -r or an output file writes data";
            return false;
        }
        return true;
    }
    if (head == "date") {
        for (size_t i = 1; i < w.size(); ++i) {
            const std::string& a = w[i];
            if (a.empty() || (a[0] != '+' && a != "-u" && a != "-R")) {
                reason = "date with arguments other than +format, -u or -R needs review";
                return false;
            }
        }
        return true;
    }
    if (head == "hostname") {
        if (w.size() > 1 && CountOperands(w) > 0) {
            reason = "hostname with an argument sets the host name";
            return false;
        }
        return true;
    }
    if (head == "sysctl") {
        for (size_t i = 1; i < w.size(); ++i) {
            if (w[i] == "-w" || w[i].find('=') != std::string::npos) {
                reason = "sysctl that sets a value needs review";
                return false;
            }
        }
        return true;
    }
    if (head == "defaults") {
        static const std::set<std::string> kReadVerbs = {
            "read", "read-type", "domains", "find"};
        for (size_t i = 1; i < w.size(); ++i) {
            if (w[i].empty() || w[i][0] == '-') continue;
            if (kReadVerbs.count(w[i])) return true;
            break;
        }
        reason = "defaults other than read/read-type/domains/find changes settings";
        return false;
    }
    if (head == "plutil") {
        if (w.size() > 1 && (w[1] == "-p" || w[1] == "-lint")) return true;
        reason = "plutil other than -p or -lint can rewrite files";
        return false;
    }

    reason = "'" + head + "' is outside the automatic read-only profile";
    return false;
}

} // namespace

PolicyDecision EvaluateZshCommand(const std::string& commandIn)
{
    const std::string cmd = Trim(commandIn);
    if (cmd.empty()) return Reject("empty command");

    std::vector<Words> stages;
    PolicyDecision early;
    if (!ScanCommand(cmd, stages, early)) return early;

    for (const Words& w : stages) {
        std::string reason;
        if (!StageIsReadOnly(w, reason)) return Approval(reason);
    }

    PolicyDecision ok;
    ok.allowed = true;
    return ok;
}
