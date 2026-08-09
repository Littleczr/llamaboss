// ─── skill_authoring_support.cpp ───────────────────────────────────
// Implementation of the skill authoring helpers extracted from
// LlamaBoss.cpp.  See skill_authoring_support.h for the public API
// contract and notes on the underlying protocol.

#include "skill_authoring_support.h"
#include "lb_string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

// ─── Public marker definitions ─────────────────────────────────────
const char* const kSkillPythonHelperBeginMarker =
    "<<<LLAMABOSS_SKILL_PYTHON_HELPER_BEGIN>>>";
const char* const kSkillPythonHelperEndMarker =
    "<<<LLAMABOSS_SKILL_PYTHON_HELPER_END>>>";

namespace {

// Skills Phase 2I: Skill creation is now a real design conversation.
// While that design session is active, only explicit control messages
// should be intercepted here.  Ordinary messages must reach the model.
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
std::string LbReadFrontmatterFieldFromText(const std::string& text,
                                           const std::string& field,
                                           std::size_t maxChars)
{
    size_t bodyStart = 0, bodyEnd = 0;
    if (!LbFindFrontmatterBlock(text, bodyStart, bodyEnd))
        return std::string();

    const std::string prefix = field + ":";
    std::istringstream block(text.substr(bodyStart, bodyEnd - bodyStart));
    std::string line;
    while (std::getline(block, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = LbTrimAscii(line);
        const std::string lower = LbLowerAscii(trimmed);
        if (lower.rfind(prefix, 0) != 0) continue;

        std::string value = LbTrimAscii(trimmed.substr(prefix.size()));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        value = LbCollapseWhitespaceOneLine(value);
        if (maxChars > 0 && value.size() > maxChars) {
            value.resize(maxChars);
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

std::string LbEnsureSkillFrontmatter(std::string markdown,
                                     const std::string& skillFolderName)
{
    const size_t start = LbSkipUtf8Bom(markdown);

    // Builder already emitted a frontmatter block: keep it verbatim.
    if (LbIsFrontmatterFenceAt(markdown, start)) return markdown;

    // Synthesize a description from the first non-heading, non-blank
    // body line (the intro paragraph in the required section order).
    std::string description;
    {
        std::istringstream body(markdown);
        std::string line;
        while (std::getline(body, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string trimmed = LbTrimAscii(line);
            if (trimmed.empty()) continue;
            if (trimmed[0] == '#') continue;   // headings
            if (trimmed[0] == '-' || trimmed[0] == '*') continue; // list items
            description = LbCollapseWhitespaceOneLine(trimmed);
            break;
        }
    }
    if (description.empty()) {
        description = "Reusable LlamaBoss Skill. Read this SKILL.md and "
                      "follow its steps when the user asks to run it.";
    }
    if (description.size() > 400) {
        description.resize(400);
        description += "...";
    }
    // YAML double-quoted scalar escaping.
    std::string escaped;
    escaped.reserve(description.size());
    for (char ch : description) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        escaped.push_back(ch);
    }

    std::ostringstream fm;
    fm << "---\n"
       << "name: "
       << (skillFolderName.empty() ? std::string("skill") : skillFolderName)
       << "\n"
       << "description: \"" << escaped << "\"\n"
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
