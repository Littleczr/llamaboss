// ─── skill_authoring_support.cpp ───────────────────────────────────
// Implementation of the skill authoring helpers extracted from
// LlamaBoss.cpp.  See skill_authoring_support.h for the public API
// contract and notes on the underlying protocol.

#include "skill_authoring_support.h"
#include "lb_string_utils.h"

#include <algorithm>
#include <fstream>
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

std::string LbSkillPythonHelperPathFromContractPath(const std::string& skillContractPath)
{
    if (skillContractPath.empty()) return std::string();

    std::string normalized = skillContractPath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    const size_t contractSlash = normalized.find_last_of('/');
    if (contractSlash == std::string::npos || contractSlash == 0)
        return std::string();

    const size_t folderSlash = normalized.find_last_of('/', contractSlash - 1);
    const size_t folderStart = folderSlash == std::string::npos ? 0 : folderSlash + 1;
    const std::string folderName = normalized.substr(folderStart, contractSlash - folderStart);
    if (folderName.empty()) return std::string();

    const size_t originalSlash = skillContractPath.find_last_of("\\/");
    if (originalSlash == std::string::npos)
        return std::string();

    const std::string skillFolder = skillContractPath.substr(0, originalSlash);
    if (skillFolder.empty()) return std::string();

    const char separator = skillFolder.find('\\') != std::string::npos ? '\\' : '/';
    return skillFolder + separator + folderName + ".py";
}

// ─── File I/O ──────────────────────────────────────────────────────

bool LbWriteUtf8TextFile(const std::string& path, const std::string& content)
{
    if (path.empty()) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}
