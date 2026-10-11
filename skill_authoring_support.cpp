// ─── skill_authoring_support.cpp ───────────────────────────────────
// Implementation of the skill authoring helpers extracted from
// LlamaBoss.cpp.  See skill_authoring_support.h for the public API
// contract and notes on the underlying protocol.

#include "skill_authoring_support.h"
#include "lb_string_utils.h"

// ─── Public marker definitions ─────────────────────────────────────
const char* const kSkillPythonHelperBeginMarker =
    "<<<LLAMABOSS_SKILL_PYTHON_HELPER_BEGIN>>>";
const char* const kSkillPythonHelperEndMarker =
    "<<<LLAMABOSS_SKILL_PYTHON_HELPER_END>>>";

namespace {

// Skill creation is a real design conversation.  While that design
// session is active, only explicit control messages should be
// intercepted here.  Ordinary messages must reach the model.
std::string LbNormalizeSkillAuthoringControl(std::string text)
{
    text = LbLowerAscii(LbTrimAscii(std::move(text)));

    while (!text.empty()) {
        const char ch = text.back();
        if (ch != '.' && ch != '?' && ch != '!' && ch != ',' &&
            ch != ';' && ch != ':') {
            break;
        }
        text.pop_back();
        text = LbTrimAscii(std::move(text));
    }

    static const char* kOptionalPrefixes[] = {
        "please ",
        "can you ",
        "could you ",
        "would you "
    };

    for (const char* prefix : kOptionalPrefixes) {
        const std::string prefixText(prefix);
        if (text.rfind(prefixText, 0) == 0) {
            text = LbTrimAscii(text.substr(prefixText.size()));
            break;
        }
    }

    return text;
}

std::string LbStripOptionalMarkdownFence(std::string text)
{
    text = LbTrimAscii(std::move(text));
    if (text.rfind("```", 0) != 0) return text;

    const size_t firstNl = text.find('\n');
    if (firstNl == std::string::npos) return text;

    std::string body = text.substr(firstNl + 1);
    body = LbTrimAscii(std::move(body));

    const size_t lastFence = body.rfind("\n```");
    if (lastFence != std::string::npos) {
        const std::string trailing = LbTrimAscii(body.substr(lastFence + 4));
        if (trailing.empty()) {
            body = body.substr(0, lastFence);
            return LbTrimAscii(std::move(body));
        }
    }

    if (body.size() >= 3 && body.compare(body.size() - 3, 3, "```") == 0) {
        body.erase(body.size() - 3);
        return LbTrimAscii(std::move(body));
    }

    return text;
}

} // anonymous namespace

// ─── Control-message recognizers ───────────────────────────────────

bool LbSkillAuthoringInputCancelsSetup(const std::string& input)
{
    const std::string normalized =
        LbNormalizeSkillAuthoringControl(input);

    return normalized == "cancel" ||
           normalized == "cancel skill setup" ||
           normalized == "cancel this skill" ||
           normalized == "cancel the skill" ||
           normalized == "skip skill setup" ||
           normalized == "never mind" ||
           normalized == "nevermind";
}

bool LbSkillAuthoringInputRequestsDraft(const std::string& input)
{
    const std::string normalized =
        LbNormalizeSkillAuthoringControl(input);
    if (normalized.empty()) return false;

    static const char* kDraftPhrases[] = {
        "draft this skill",
        "draft the skill",
        "write this skill",
        "write the skill",
        "create this skill",
        "create the skill",
        "build this skill",
        "build the skill",
        "save this skill",
        "save the skill",
        "go ahead and draft this skill",
        "go ahead and draft the skill",
        "go ahead and create this skill",
        "go ahead and create the skill",
        "go ahead and build this skill",
        "go ahead and build the skill",
        "yes draft this skill",
        "yes draft the skill",
        "yes create this skill",
        "yes create the skill",
        "yes build this skill",
        "yes build the skill",
        "lets draft this skill",
        "let's draft this skill",
        "lets build this skill",
        "let's build this skill"
    };

    for (const char* phrase : kDraftPhrases) {
        if (normalized == phrase) {
            return true;
        }
    }

    return false;
}

bool LbSkillAuthoringInputConfirmsDraftPrompt(const std::string& input,
                                              const std::string& lastAssistantMessage)
{
    const std::string normalized =
        LbNormalizeSkillAuthoringControl(input);
    if (normalized.empty()) return false;

    static const char* kConfirmationPhrases[] = {
        "yes",
        "yep",
        "yeah",
        "sure",
        "ok",
        "okay",
        "go ahead",
        "do it",
        "lets do it",
        "let's do it",
        "sounds good",
        "that sounds good"
    };

    bool isConfirmation = false;
    for (const char* phrase : kConfirmationPhrases) {
        if (normalized == phrase) {
            isConfirmation = true;
            break;
        }
    }
    if (!isConfirmation) return false;

    const std::string lowerLast =
        LbLowerAscii(lastAssistantMessage);
    return lowerLast.find("draft this skill") != std::string::npos &&
           lowerLast.find("write the skill files") != std::string::npos;
}

// ─── Skill markdown sniffers ───────────────────────────────────────

bool LbLooksLikeSkillMarkdown(const std::string& markdown)
{
    // Substring checks only, so an Agent Skills YAML frontmatter block
    // at the top passes through untouched.
    const std::string lower = LbLowerAscii(markdown);
    return lower.find("# ") != std::string::npos &&
           lower.find("## trigger phrases") != std::string::npos &&
           lower.find("## inputs to ask for") != std::string::npos &&
           lower.find("## steps") != std::string::npos &&
           lower.find("## output expectations") != std::string::npos;
}

bool LbSkillMarkdownRequestsPythonHelper(const std::string& markdown)
{
    const std::string lower = LbLowerAscii(markdown);
    return lower.find("## optional python script") != std::string::npos;
}

// ─── Builder payload parser ────────────────────────────────────────

LbSkillDraftBuilderPayload LbParseSkillDraftBuilderPayload(const std::string& rawResponse)
{
    LbSkillDraftBuilderPayload payload;
    std::string text = LbStripOptionalMarkdownFence(rawResponse);

    const size_t begin = text.find(kSkillPythonHelperBeginMarker);
    if (begin == std::string::npos) {
        payload.markdown = LbTrimAscii(std::move(text));
        return payload;
    }

    payload.markdown = LbStripOptionalMarkdownFence(
        LbTrimAscii(text.substr(0, begin)));

    const size_t helperStart =
        begin + std::char_traits<char>::length(kSkillPythonHelperBeginMarker);
    const size_t end = text.find(kSkillPythonHelperEndMarker, helperStart);
    if (end == std::string::npos) {
        payload.malformedHelperBlock = true;
        return payload;
    }

    std::string helper = LbTrimAscii(text.substr(helperStart, end - helperStart));
    payload.pythonHelperSource = LbStripOptionalMarkdownFence(std::move(helper));
    return payload;
}

// ─── Path derivation ───────────────────────────────────────────────

// Shared: split a SKILL.md contract path into (skillFolder, folderName).
// Returns false on malformed input.
namespace {
bool LbSplitSkillContractPath(const std::string& skillContractPath,
                              std::string& outSkillFolder,
                              std::string& outFolderName,
                              char& outSeparator)
{
    outSkillFolder.clear();
    outFolderName.clear();
    outSeparator = '\\';
    if (skillContractPath.empty()) return false;

    std::string normalized = skillContractPath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    const size_t contractSlash = normalized.find_last_of('/');
    if (contractSlash == std::string::npos || contractSlash == 0)
        return false;

    const size_t folderSlash = normalized.find_last_of('/', contractSlash - 1);
    const size_t folderStart = folderSlash == std::string::npos ? 0 : folderSlash + 1;
    outFolderName = normalized.substr(folderStart, contractSlash - folderStart);
    if (outFolderName.empty()) return false;

    const size_t originalSlash = skillContractPath.find_last_of("\\/");
    if (originalSlash == std::string::npos) return false;

    outSkillFolder = skillContractPath.substr(0, originalSlash);
    if (outSkillFolder.empty()) return false;

    outSeparator =
        outSkillFolder.find('\\') != std::string::npos ? '\\' : '/';
    return true;
}
} // anonymous namespace

std::string LbSkillFolderNameFromContractPath(const std::string& skillContractPath)
{
    std::string folder, name;
    char sep = '\\';
    if (!LbSplitSkillContractPath(skillContractPath, folder, name, sep))
        return std::string();
    return name;
}

std::string LbSkillPythonHelperPathFromContractPath(const std::string& skillContractPath)
{
    std::string skillFolder, folderName;
    char separator = '\\';
    if (!LbSplitSkillContractPath(skillContractPath, skillFolder,
                                  folderName, separator)) {
        return std::string();
    }

    // Agent Skills layout: helpers live under the skill's scripts
    // subfolder.  ListWorkflowScriptsInDir already enumerates that
    // subfolder (runPod-style skills), so bare-name resolution works.
    return skillFolder + separator + "scripts" + separator +
           folderName + ".py";
}

// ─── Frontmatter ───────────────────────────────────────────────────

namespace {

// True when `text` (already positioned at a candidate start) begins a
// YAML frontmatter fence line: "---" followed by end-of-line.
bool LbIsFrontmatterFenceAt(const std::string& text, size_t pos)
{
    if (text.compare(pos, 3, "---") != 0) return false;
    const size_t after = pos + 3;
    if (after >= text.size()) return true;
    return text[after] == '\n' ||
           (text[after] == '\r' && after + 1 < text.size() &&
            text[after + 1] == '\n');
}

// Offset of the first content byte, skipping a UTF-8 BOM if present.
size_t LbSkipUtf8Bom(const std::string& text)
{
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        return 3;
    }
    return 0;
}

std::string LbCollapseWhitespaceOneLine(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    bool lastWasSpace = false;
    for (char ch : in) {
        const bool isSpace = ch == ' ' || ch == '\t' ||
                             ch == '\r' || ch == '\n';
        if (isSpace) {
            if (!out.empty() && !lastWasSpace) out.push_back(' ');
            lastWasSpace = true;
        } else {
            out.push_back(ch);
            lastWasSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // anonymous namespace

namespace {

// Read up to maxBytes from the head of a file (frontmatter is small by
// design; Agent Skills caps description at 1024 chars).
std::string LbReadFileHead(const std::string& path, size_t maxBytes)
{
    if (path.empty()) return std::string();
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return std::string();
    std::string head(maxBytes, '\0');
    f.read(&head[0], static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(f.gcount()));
    return head;
}

// Locate the frontmatter block body inside `text`.  On success,
// [outBodyStart, outBodyEnd) covers the lines between the opening and
// closing `---` fences (closing fence excluded).
bool LbFindFrontmatterBlock(const std::string& text,
                            size_t& outBodyStart,
                            size_t& outBodyEnd)
{
    outBodyStart = outBodyEnd = std::string::npos;

    const size_t start = LbSkipUtf8Bom(text);
    if (!LbIsFrontmatterFenceAt(text, start)) return false;

    const size_t openNl = text.find('\n', start);
    if (openNl == std::string::npos) return false;

    size_t lineStart = openNl + 1;
    while (lineStart < text.size()) {
        if (LbIsFrontmatterFenceAt(text, lineStart)) {
            outBodyStart = openNl + 1;
            outBodyEnd = lineStart;
            return true;
        }
        const size_t nl = text.find('\n', lineStart);
        if (nl == std::string::npos) break;
        lineStart = nl + 1;
    }
    return false;
}

// Extract a top-level `field:` value from a frontmatter body slice.
// Strips one layer of surrounding quotes and collapses whitespace.
// Multi-line values are joined into one line: YAML block scalars
// (`description: >-` / `|` followed by indented lines) and plain scalars
// that continue on more-indented lines.  Without this, a valid imported
// SKILL.md using `description: >-` surfaced as the literal text ">-".
std::string LbReadFrontmatterFieldFromText(const std::string& text,
                                           const std::string& field,
                                           std::size_t maxChars)
{
    size_t bodyStart = 0, bodyEnd = 0;
    if (!LbFindFrontmatterBlock(text, bodyStart, bodyEnd))
        return std::string();

    std::vector<std::string> lines;
    {
        std::istringstream block(text.substr(bodyStart, bodyEnd - bodyStart));
        std::string line;
        while (std::getline(block, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    auto indentOf = [](const std::string& l) {
        size_t n = 0;
        while (n < l.size() && (l[n] == ' ' || l[n] == '\t')) ++n;
        return n;
    };

    const std::string prefix = field + ":";
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string trimmed = LbTrimAscii(lines[li]);
        const std::string lower = LbLowerAscii(trimmed);
        if (lower.rfind(prefix, 0) != 0) continue;

        const size_t keyIndent = indentOf(lines[li]);
        std::string value = LbTrimAscii(trimmed.substr(prefix.size()));

        // Block scalar header: '|' or '>' plus optional chomping/indent
        // indicators (e.g. ">-", "|+", ">2").  The content is on the
        // following lines.
        bool blockScalar = false;
        if (!value.empty() && (value[0] == '|' || value[0] == '>')) {
            blockScalar = true;
            for (size_t k = 1; k < value.size(); ++k) {
                const char c = value[k];
                if (!(c == '-' || c == '+' || (c >= '0' && c <= '9'))) {
                    // Only a trailing comment may follow the indicators.
                    if (c == ' ' || c == '\t') {
                        const std::string rest = LbTrimAscii(value.substr(k));
                        if (!rest.empty() && rest[0] != '#') blockScalar = false;
                    } else {
                        blockScalar = false;
                    }
                    break;
                }
            }
            if (blockScalar) value.clear();
        }

        const bool quoted = !value.empty() && (value[0] == '"' || value[0] == '\'');
        if (blockScalar || !quoted) {
            // Gather continuation lines: more indented than the key, or
            // blank lines inside the scalar.
            for (size_t nx = li + 1; nx < lines.size(); ++nx) {
                const std::string t = LbTrimAscii(lines[nx]);
                if (t.empty()) continue;
                if (indentOf(lines[nx]) <= keyIndent) break;
                if (!value.empty()) value += " ";
                value += t;
            }
        }

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        value = LbCollapseWhitespaceOneLine(value);
        if (maxChars > 0 && value.size() > maxChars) {
            // UTF-8-safe cut: descriptions land in the system prompt, and a
            // split multi-byte character makes llama-server reject requests.
            value = LbUtf8SafeTruncate(value, maxChars);
            value += "...";
        }
        return value;
    }
    return std::string();
}

} // anonymous namespace

std::string LbReadSkillFrontmatterDescription(const std::string& skillContractPath,
                                              std::size_t maxChars)
{
    return LbReadFrontmatterFieldFromText(
        LbReadFileHead(skillContractPath, 8192), "description", maxChars);
}

std::string LbReadSkillFrontmatterName(const std::string& skillContractPath)
{
    return LbReadFrontmatterFieldFromText(
        LbReadFileHead(skillContractPath, 8192), "name", 128);
}

std::string LbRewriteSkillFrontmatterName(std::string markdown,
                                          const std::string& name)
{
    if (name.empty()) return markdown;

    size_t bodyStart = 0, bodyEnd = 0;
    if (!LbFindFrontmatterBlock(markdown, bodyStart, bodyEnd))
        return markdown;

    // Find an existing top-level `name:` line inside the block.
    size_t lineStart = bodyStart;
    while (lineStart < bodyEnd) {
        size_t lineEnd = markdown.find('\n', lineStart);
        if (lineEnd == std::string::npos || lineEnd > bodyEnd)
            lineEnd = bodyEnd;

        std::string line = markdown.substr(lineStart, lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = LbTrimAscii(line);
        const std::string lower = LbLowerAscii(trimmed);
        if (lower.rfind("name:", 0) == 0) {
            std::string value = LbTrimAscii(trimmed.substr(5));
            if (value.size() >= 2 &&
                ((value.front() == '"' && value.back() == '"') ||
                 (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }
            if (value == name) return markdown;  // already aligned

            // Replace the line's content, preserving its CR if present.
            const bool hadCr =
                lineEnd > lineStart &&
                markdown[lineEnd - 1] == '\r' &&
                lineEnd <= bodyEnd;
            std::string newLine = "name: " + name;
            if (hadCr) newLine += '\r';
            markdown.replace(lineStart,
                             (lineEnd - lineStart),
                             newLine);
            return markdown;
        }

        if (lineEnd >= bodyEnd) break;
        lineStart = lineEnd + 1;
    }

    // No name line inside the block: insert one right after the
    // opening fence.
    markdown.insert(bodyStart, "name: " + name + "\n");
    return markdown;
}

namespace {

// Description synthesized from the first non-heading, non-list body line
// (the intro paragraph in the required section order).
std::string LbSynthesizeSkillDescription(const std::string& body)
{
    std::string description;
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = LbTrimAscii(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;                       // headings
        if (trimmed[0] == '-' || trimmed[0] == '*') continue;  // list items
        description = LbCollapseWhitespaceOneLine(trimmed);
        break;
    }
    if (description.empty()) {
        description = "Reusable LlamaBoss Skill. Read this SKILL.md and "
                      "follow its steps when the user asks to run it.";
    }
    if (description.size() > 400) {
        // UTF-8-safe cut: this text is saved into SKILL.md and later
        // injected into the system prompt; a split multi-byte character
        // makes llama-server reject every request.
        description = LbUtf8SafeTruncate(description, 400);
        description += "...";
    }
    return description;
}

// YAML double-quoted scalar.
std::string LbYamlQuote(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char ch : value) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

// True for a `key:` line (YAML-style key at column 0).
bool LbLooksLikeYamlKeyLine(const std::string& line)
{
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t k = 0; k < colon; ++k) {
        const char c = line[k];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace

std::string LbEnsureSkillFrontmatter(std::string markdown,
                                     const std::string& skillFolderName)
{
    const size_t start = LbSkipUtf8Bom(markdown);
    const std::string name =
        skillFolderName.empty() ? std::string("skill") : skillFolderName;

    if (LbIsFrontmatterFenceAt(markdown, start)) {
        size_t bodyStart = 0, bodyEnd = 0;
        if (!LbFindFrontmatterBlock(markdown, bodyStart, bodyEnd)) {
            // Unterminated block (no closing fence): close it after the
            // leading run of `key:` lines and their indented continuations,
            // so the rest of the draft stays body text instead of
            // silently swallowing the whole file.
            const size_t openNl = markdown.find('\n', start);
            size_t insertAt = (openNl == std::string::npos) ? markdown.size() : openNl + 1;
            while (insertAt < markdown.size()) {
                const size_t nl = markdown.find('\n', insertAt);
                const size_t lineEnd = (nl == std::string::npos) ? markdown.size() : nl;
                std::string line = markdown.substr(insertAt, lineEnd - insertAt);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const bool continuation = !line.empty() &&
                                          (line[0] == ' ' || line[0] == '\t') &&
                                          !LbTrimAscii(line).empty();
                if (!LbLooksLikeYamlKeyLine(line) && !continuation) break;
                insertAt = (nl == std::string::npos) ? markdown.size() : nl + 1;
            }
            if (insertAt == markdown.size() &&
                (markdown.empty() || markdown.back() != '\n')) {
                markdown += "\n";
                insertAt = markdown.size();
            }
            markdown.insert(insertAt, "---\n");
            if (!LbFindFrontmatterBlock(markdown, bodyStart, bodyEnd))
                return markdown;
        }

        // Valid block: fill in required fields the builder left out.
        // Insert description first, then name, both right after the
        // opening fence (name ends up first).
        if (LbReadFrontmatterFieldFromText(markdown, "description", 0).empty()) {
            const size_t closeNl = markdown.find('\n', bodyEnd);
            const std::string body = (closeNl == std::string::npos)
                ? std::string() : markdown.substr(closeNl + 1);
            markdown.insert(bodyStart, "description: " +
                LbYamlQuote(LbSynthesizeSkillDescription(body)) + "\n");
        }
        if (LbReadFrontmatterFieldFromText(markdown, "name", 0).empty()) {
            markdown.insert(bodyStart, "name: " + name + "\n");
        }
        return markdown;
    }

    std::ostringstream fm;
    fm << "---\n"
       << "name: " << name << "\n"
       << "description: " << LbYamlQuote(LbSynthesizeSkillDescription(markdown)) << "\n"
       << "---\n\n";
    return fm.str() + markdown;
}

// ─── File I/O ──────────────────────────────────────────────────────

bool LbWriteUtf8TextFile(const std::string& path, const std::string& content)
{
    if (path.empty()) return false;

    // Create missing parent directories (the scripts subfolder on a
    // first helper save).  Narrow-path std::filesystem matches the
    // narrow-path std::ofstream below, so both agree on encoding.
    {
        std::error_code ec;
        const std::filesystem::path parent =
            std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            // A failed create_directories on an already-existing dir is
            // not an error; the ofstream below is the real gate.
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}
