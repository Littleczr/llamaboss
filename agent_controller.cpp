// agent_controller.cpp
//
// Phase 9: typed AgentEvent envelope.  Keeps the Phase 5 sink
// architecture and Phase 6 approval state machine intact while routing
// controller emissions through a small structured event object.

// MSVC: silence wx's transitive use of strcpy/wcscpy/_wopen in
// wxcrt.h and filefn.h.  Phase 4 got this for free because the file
// included chat_display.h → <wx/wx.h> (the umbrella header has its
// own CRT-secure handling).  Phase 5 dropped that include — the
// controller is wx-free at the source level — so we now match the
// project's per-file convention used by LlamaBoss.cpp,
// chat_history.cpp, tool_dispatcher.cpp, etc.  Must come before any
// other include so the deprecation tagging is suppressed before
// wx headers get to <wxcrt.h>.
#define _CRT_SECURE_NO_WARNINGS

#include "agent_controller.h"
#include "python_runner.h"
#include "python_arg_policy.h"
#include "path_safety.h"

#include "app_state.h"
#include "chat_history.h"
#include "cmd_executor.h"     // CmdResult definition
#include "server_manager.h"    // ModelDisplayName
#include "tool_call_parser.h"
#include "tool_grep.h"         // GrepResult definition
#include "tool_web_fetch.h"    // WebFetchResult definition
#include "tool_router.h"       // BuildToolsArrayJson, GetGlobalRouter
#include "tool_approval.h"     // Phase 6 approval cards

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>


namespace {

// Phase 3 bugfix #1 (now the Phase 10 fallback):
// Native function-calling responses may contain several tool_calls in
// one assistant turn.  Phase 10 executes the whole batch sequentially
// and persists the full sidecar via KeepExecutableToolCallsJson — but
// only when every call carries a usable unique id.  When ids are
// missing or ambiguous, the controller falls back to this conservative
// single-call helper: execute one tool, persist only the matching
// tool_call entry.  Storing entries we will not answer would make the
// next OpenAI-style request invalid: assistant.tool_calls contains
// A+B, but only role:"tool" for A exists.
std::string KeepOnlySelectedToolCallJson(const std::string& toolCallsJson,
                                         const std::string& selectedCallId)
{
    if (toolCallsJson.empty()) return std::string();

    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(toolCallsJson);
        Poco::JSON::Array::Ptr arr = var.extract<Poco::JSON::Array::Ptr>();
        if (!arr || arr->empty()) return std::string();

        Poco::JSON::Object::Ptr selected;

        // Prefer the exact id of the invocation we are about to dispatch.
        // This keeps the assistant.tool_calls id aligned with the later
        // role:"tool" message's tool_call_id.
        if (!selectedCallId.empty()) {
            for (size_t i = 0; i < arr->size(); ++i) {
                Poco::JSON::Object::Ptr obj;
                try { obj = arr->getObject(i); } catch (...) { continue; }
                if (!obj) continue;

                try {
                    if (obj->getValue<std::string>("id") == selectedCallId) {
                        selected = obj;
                        break;
                    }
                } catch (...) {
                    // Missing/non-string id. Keep scanning.
                }
            }
        }

        // Fallback: preserve the first object.  This should only matter for
        // malformed provider output where the id is missing, but it is safer
        // than re-attaching the full multi-call array.
        if (!selected) {
            try { selected = arr->getObject(0); } catch (...) { selected = nullptr; }
        }
        if (!selected) return std::string();

        Poco::JSON::Array one;
        one.add(selected);

        std::ostringstream oss;
        one.stringify(oss);
        return oss.str();
    } catch (...) {
        // Fail closed: returning empty avoids storing a full multi-call array
        // that would not match the single tool result we can currently append.
        return std::string();
    }
}

// Phase 10: multi-call sidecar builder.  Returns a tool_calls array
// containing exactly the entries (in invocation order) whose ids match
// the invocations LlamaBoss is about to execute sequentially.  The
// controller guarantees one role:"tool" reply per executed invocation
// (real result, error result, denied, or skipped), so persisting all of
// them keeps the OpenAI transcript valid:
//   assistant.tool_calls = [A, B, C]
//   role:"tool" tool_call_id=A, then B, then C — consecutive, in order.
//
// Fails closed (returns empty) when any selected invocation has a
// missing or duplicate id, or when an id cannot be found in the source
// array.  The caller falls back to the conservative single-call path in
// that case, which tolerates id-less providers exactly as before.
std::string KeepExecutableToolCallsJson(
    const std::string&                 toolCallsJson,
    const std::vector<ToolInvocation>& selected)
{
    if (toolCallsJson.empty() || selected.empty()) return std::string();

    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(toolCallsJson);
        Poco::JSON::Array::Ptr arr = var.extract<Poco::JSON::Array::Ptr>();
        if (!arr || arr->empty()) return std::string();

        // Index source entries by id.  Reject duplicate source ids —
        // pairing would be ambiguous.
        std::map<std::string, Poco::JSON::Object::Ptr> byId;
        for (size_t i = 0; i < arr->size(); ++i) {
            Poco::JSON::Object::Ptr obj;
            try { obj = arr->getObject(i); } catch (...) { continue; }
            if (!obj) continue;

            std::string id;
            try { id = obj->getValue<std::string>("id"); }
            catch (...) { continue; }
            if (id.empty()) continue;

            if (byId.count(id)) return std::string();   // duplicate id
            byId[id] = obj;
        }

        Poco::JSON::Array out;
        std::map<std::string, bool> used;
        for (const ToolInvocation& inv : selected) {
            if (inv.toolCallId.empty()) return std::string();
            if (used.count(inv.toolCallId)) return std::string();
            auto it = byId.find(inv.toolCallId);
            if (it == byId.end()) return std::string();
            used[inv.toolCallId] = true;
            out.add(it->second);
        }
        if (out.size() == 0) return std::string();

        std::ostringstream oss;
        out.stringify(oss);
        return oss.str();
    } catch (...) {
        return std::string();
    }
}




std::string AgentLowerAscii(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// List the top-level keys of a JSON object payload ("path, contents,
// mode").  Used to enrich native tool-call validation errors: when a
// model invents a parameter name ({"file": "x.txt"} instead of
// {"args": ...}), the generic "requires a path" error gives it nothing
// to correct against, and it burns the malformed budget re-guessing.
// Echoing the keys it actually sent lets it self-correct in one retry.
// Empty string when the payload is not a JSON object or has no keys.
std::string AgentListJsonObjectKeys(const std::string& json)
{
    if (json.empty()) return std::string();
    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(json);
        Poco::JSON::Object::Ptr obj = var.extract<Poco::JSON::Object::Ptr>();
        if (!obj) return std::string();

        std::vector<std::string> names;
        obj->getNames(names);

        std::string out;
        for (const std::string& n : names) {
            if (!out.empty()) out += ", ";
            out += n;
        }
        return out;
    } catch (...) {
        return std::string();
    }
}

std::string AgentPresentedFileExtLower(const PresentedFile& f)
{
    std::string name = !f.displayName.empty() ? f.displayName : f.diskPath;
    std::replace(name.begin(), name.end(), '\\', '/');
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return std::string();
    return AgentLowerAscii(name.substr(dot + 1));
}

struct AgentArtifactPresentation {
    std::string iconUtf8;
    std::string toolName;
};

AgentArtifactPresentation BuildAgentArtifactPresentation(const std::vector<PresentedFile>& files)
{
    AgentArtifactPresentation p;
    if (files.empty()) return p;

    bool hasDocx = false;
    bool hasSheet = false;
    bool hasPdf = false;
    bool hasMarkdown = false;
    bool hasText = false;
    bool hasImage = false;
    bool hasOther = false;

    for (const auto& f : files) {
        const std::string ext = AgentPresentedFileExtLower(f);
        const std::string lang = AgentLowerAscii(f.language);

        if (ext == "docx") {
            hasDocx = true;
        } else if (ext == "xlsx" || ext == "csv") {
            hasSheet = true;
        } else if (ext == "pdf") {
            hasPdf = true;
        } else if (ext == "md" || ext == "markdown" || lang == "markdown" || lang == "md") {
            hasMarkdown = true;
        } else if (ext == "txt" || lang == "text") {
            hasText = true;
        } else if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") {
            hasImage = true;
        } else {
            hasOther = true;
        }
    }

    const int kinds = (hasDocx ? 1 : 0) + (hasSheet ? 1 : 0) +
                      (hasPdf ? 1 : 0) + (hasMarkdown ? 1 : 0) +
                      (hasText ? 1 : 0) + (hasImage ? 1 : 0) +
                      (hasOther ? 1 : 0);

    if (files.size() == 1 && kinds == 1) {
        if (hasDocx)     return { "\xF0\x9F\x93\x84", "Create Word Document" };      // 📄
        if (hasSheet)    return { "\xF0\x9F\x93\x8A", "Create Spreadsheet" };        // 📊
        if (hasPdf)      return { "\xF0\x9F\x93\x84", "Create PDF" };                 // 📄
        if (hasMarkdown) return { "\xF0\x9F\x93\x9D", "Create Markdown Document" };  // 📝
        if (hasText)     return { "\xF0\x9F\x93\x84", "Create Text Document" };       // 📄
        if (hasImage)    return { "\xF0\x9F\x96\xBC", "Create Image" };               // 🖼
        return { "\xF0\x9F\x93\x8E", "Create File" };                                 // 📎
    }

    if (hasDocx && kinds == 1)     return { "\xF0\x9F\x93\x84", "Create Word Documents" };
    if (hasSheet && kinds == 1)    return { "\xF0\x9F\x93\x8A", "Create Spreadsheets" };
    if (hasPdf && kinds == 1)      return { "\xF0\x9F\x93\x84", "Create PDFs" };
    if (hasMarkdown && kinds == 1) return { "\xF0\x9F\x93\x9D", "Create Markdown Documents" };

    return { "\xF0\x9F\x93\xA6", "Create Files" };                                    // 📦
}

void ApplyAgentArtifactPresentation(ToolInvocationResult& r)
{
    if (r.presentedFiles.empty()) return;
    if (!r.errorBody.empty()) return;

    AgentArtifactPresentation p = BuildAgentArtifactPresentation(r.presentedFiles);
    if (p.toolName.empty()) return;

    r.iconUtf8 = p.iconUtf8;
    r.toolName = p.toolName;
}


std::string AgentTrimPackageToken(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n\"'`.,:;()[]{}");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n\"'`.,:;()[]{}");
    return s.substr(a, b - a + 1);
}

std::string AgentNormalizePackageTokenForDisplay(const std::string& raw)
{
    std::string p = AgentLowerAscii(AgentTrimPackageToken(raw));
    std::replace(p.begin(), p.end(), '_', '-');
    return p;
}

bool AgentExtractAfterToken(const std::string& text,
                         const std::string& token,
                         std::string& out)
{
    size_t pos = text.find(token);
    if (pos == std::string::npos) return false;
    pos += token.size();

    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
            text[pos] == '\n' || text[pos] == '\'' || text[pos] == '"' ||
            text[pos] == '`')) {
        ++pos;
    }

    size_t end = pos;
    while (end < text.size()) {
        const char c = text[end];
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        if (!ok) break;
        ++end;
    }

    if (end <= pos) return false;
    out = text.substr(pos, end - pos);
    return !out.empty();
}

bool AgentFindMissingPythonPackage(const std::string& stdoutText,
                                const std::string& stderrText,
                                std::string&       importNameOut,
                                std::string&       packageNameOut,
                                bool&              installableOut)
{
    importNameOut.clear();
    packageNameOut.clear();
    installableOut = false;

    const std::string text = stderrText + "\n" + stdoutText;
    const std::string lower = AgentLowerAscii(text);

    std::string candidate;
    if (!AgentExtractAfterToken(text, "No module named", candidate) &&
        !AgentExtractAfterToken(text, "no module named", candidate)) {
        if (!AgentExtractAfterToken(lower, "pip install --user --disable-pip-version-check", candidate) &&
            !AgentExtractAfterToken(lower, "pip install --user", candidate)) {
            if (lower.find("openpyxl python package is required") != std::string::npos) {
                candidate = "openpyxl";
            } else if (lower.find("missing pdf form dependency") != std::string::npos ||
                       lower.find("install pymupdf") != std::string::npos) {
                candidate = "pymupdf";
            } else if (lower.find("missing pdf text extraction dependency") != std::string::npos ||
                       lower.find("install pypdf") != std::string::npos) {
                candidate = "pypdf";
            }
        }
    }

    candidate = AgentTrimPackageToken(candidate);
    if (candidate.empty()) return false;

    importNameOut = candidate;

    std::string normalizedPackage;
    std::string normalizeError;
    installableOut = python_arg_policy::NormalizeAllowedPythonPackage(
        candidate, normalizedPackage, normalizeError);

    // For installable packages, use the exact shared normalized name that
    // python_install_package will accept.  For unsupported tokens, keep a
    // simple lowercase display value so the recovery message remains useful
    // without duplicating the install policy.
    packageNameOut = installableOut
        ? normalizedPackage
        : AgentNormalizePackageTokenForDisplay(candidate);
    return true;
}

void ApplyAgentMissingPythonPackageRecovery(ToolInvocationResult& r,
                                       const PythonRunResult& py)
{
    if (py.exitCode == 0 || py.cancelled || py.timedOut) return;

    std::string importName;
    std::string packageName;
    bool installable = false;
    if (!AgentFindMissingPythonPackage(py.stdoutText,
                                    py.stderrText,
                                    importName,
                                    packageName,
                                    installable)) {
        return;
    }

    r.iconUtf8 = "\xF0\x9F\x93\xA6"; // 📦
    r.toolName = installable ? std::string("Missing Python Package")
                             : std::string("Unsupported Python Package");

    std::ostringstream body;
    if (installable) {
        body << "Python needs the package `" << packageName
             << "` before this step can continue.\n\n"
             << "Suggested next step for LlamaBoss: use `python_install_package "
             << packageName << "`, then retry the failed step once.\n\n"
             << "No package was installed yet. The user will see an approval "
                "card with the exact package name before pip runs.";
    } else {
        body << "Python tried to import `" << importName
             << "`, but the inferred package name `" << packageName
             << "` is not a simple PyPI name LlamaBoss can install through "
                "python_install_package.\n\n"
             << "The script may need to be rewritten using the standard "
                "library, or the user can install the dependency manually.";
    }

    if (!r.body.empty()) {
        body << "\n\nOriginal stdout:\n" << r.body;
    }

    r.body = body.str();
    r.bodyLang = "markdown";
}

bool TryReadSmallTextFile(const std::string& path,
                          size_t             maxBytes,
                          std::string&       out,
                          size_t&            sizeOut)
{
    out.clear();
    sizeOut = 0;

#ifdef _WIN32
    std::ifstream file(path_safety::Utf8ToWide(path),
                       std::ios::binary | std::ios::ate);
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
#endif
    if (!file) return false;

    std::streampos end = file.tellg();
    if (end == std::streampos(-1)) return false;

    sizeOut = static_cast<size_t>(end);
    if (sizeOut > maxBytes) return false;

    file.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

void InlineSmallPdfExtractedMarkdown(ToolInvocationResult& r)
{
    if (r.toolTag != tool_names::kPdfExtractText &&
        r.toolTag != tool_names::kDocxExtractText) return;
    if (!r.errorBody.empty()) return;
    if (r.presentedFiles.empty()) return;

    constexpr size_t kMaxInlinePdfMarkdownBytes = 32 * 1024;

    const PresentedFile& f = r.presentedFiles.front();
    if (f.diskPath.empty()) return;

    std::string inlineMarkdown;
    size_t inlineBytes = 0;
    if (TryReadSmallTextFile(f.diskPath,
                             kMaxInlinePdfMarkdownBytes,
                             inlineMarkdown,
                             inlineBytes)) {
        r.body = inlineMarkdown;
        r.errorBody.clear();
        r.bodyLang = "markdown";
    }
}




std::string AgentJoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char sep = '\\';
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

std::string AgentConversationScriptsDirForCwd(const std::string& cwd)
{
    // Shared lane resolver.  This used to be a hand-mirrored copy of
    // python_runner's conversation-lane recognizer; because this
    // function guards the one-shot python_run_script approval bypass
    // against cross-lane shadowing, a silent divergence between the
    // copies would weaken exactly the safety property it protects.
    // Both sides now resolve through ServerManager.
    return ServerManager::ConversationScriptsDirForCwd(cwd);
}

bool AgentRegularFileExists(const std::string& path)
{
    std::wstring wide = path_safety::Utf8ToWide(path);
    if (wide.empty()) return false;

    DWORD attrs = ::GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ProjectScriptRunBypassWouldBeShadowedByConversationScript(
    const ToolContext& ctx,
    const std::string& createdDisplayName)
{
    // Project-scoped python_create_script writes to the project's Workflows
    // folder, but python_run_script resolves bare filenames by checking the
    // conversation Scripts lane first.  If a stale same-named script already
    // exists there, a filename-only one-shot bypass would execute that older
    // higher-priority file instead of the just-reviewed project workflow file.
    if (ctx.activeProjectRoot.empty() || createdDisplayName.empty()) return false;

    std::string scriptsDir = AgentConversationScriptsDirForCwd(ctx.cwd);
    if (scriptsDir.empty()) return false;

    return AgentRegularFileExists(AgentJoinPath(scriptsDir, createdDisplayName));
}

std::string NormalizeScriptNameForOneShotApproval(const std::string& input)
{
    // python_run_script's preferred contract is multiline args:
    //   script.py
    //   arg1
    //   arg2
    // The one-shot bypass must compare only the script request line, not the
    // argv payload.  Otherwise a legitimate immediate run such as
    // "report.py\nC:\\input.csv" would miss the carry-forward approval.
    std::string name = input;
    size_t lineEnd = name.find_first_of("\r\n");
    if (lineEnd != std::string::npos)
        name = name.substr(0, lineEnd);

    size_t a = name.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = name.find_last_not_of(" \t\r\n");
    name = name.substr(a, b - a + 1);

    // The carry-forward approval is intentionally narrow: it applies only to
    // the exact filename created by python_create_script.  python_run_script
    // also accepts path-shaped in-lane requests, but those may resolve to a
    // different older script with the same basename.  Do not collapse paths to
    // basenames here; path-shaped requests must show the normal approval card.
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos) {
        return std::string();
    }

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) name += ".py";

    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string NormalizeForToolSignature(const std::string& input)
{
    std::string out;
    out.reserve(input.size());

    bool lastWasSpace = false;
    for (size_t i = 0; i < input.size(); ++i) {
        char ch = input[i];
        unsigned char c = static_cast<unsigned char>(ch);

        // Normalize CRLF and CR to LF so the same edit/write request
        // does not evade the guard just because line endings differ.
        if (ch == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n')
                ++i;
            ch = '\n';
            c = '\n';
        }

        if (ch == '\n') {
            while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
                out.pop_back();
            if (out.empty() || out.back() != '\n')
                out.push_back('\n');
            lastWasSpace = false;
            continue;
        }

        if (std::isspace(c)) {
            if (!lastWasSpace) {
                out.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }

        out.push_back(ch);
        lastWasSpace = false;
    }

    // Trim outer whitespace/newlines after normalization.
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = out.find_last_not_of(" \t\r\n");
    out = out.substr(a, b - a + 1);

    // Keep the ring buffer lightweight even if the model repeats a
    // long write/edit body.  The length suffix still distinguishes
    // differently-sized large calls.
    constexpr size_t kMaxSignatureArgChars = 4096;
    if (out.size() > kMaxSignatureArgChars) {
        out = out.substr(0, kMaxSignatureArgChars) +
              "\n...[signature truncated, original bytes=" +
              std::to_string(input.size()) + "]";
    }
    return out;
}


AgentEventType ClassifyToolOutputEvent(const ToolInvocationResult& r)
{
    auto hasChip = [&](const std::string& needle) -> bool {
        return std::find(r.chips.begin(), r.chips.end(), needle) != r.chips.end();
    };

    if (r.toolTag == "agent_status")
        return AgentEventType::AgentStatus;

    if (r.toolTag == tool_names::kWrite && hasChip("created"))
        return AgentEventType::FileCreated;

    if (r.toolTag == tool_names::kEdit && hasChip("edited"))
        return AgentEventType::EditApplied;

    if (r.toolTag == tool_names::kMkdir && hasChip("created"))
        return AgentEventType::DirectoryCreated;

    if (r.toolTag == tool_names::kDelete && hasChip("deleted"))
        return AgentEventType::FileDeleted;

    if (!r.errorBody.empty() && r.body.empty())
        return AgentEventType::Error;

    return AgentEventType::ToolOutput;
}

bool HasChip(const ToolInvocationResult& r, const std::string& chip)
{
    return std::find(r.chips.begin(), r.chips.end(), chip) != r.chips.end();
}

bool IsAgentPythonAsyncToolName(const std::string& name)
{
    if (name == tool_names::kGrep ||
        name == tool_names::kPowerShell ||
        name == tool_names::kWebFetchUrl) {
        return false;
    }

    const ToolSpec* spec = GetGlobalRouter().Find(name);
    return spec && spec->safety.isAsync;
}

bool IsAgentWebFetchAsyncToolName(const std::string& name)
{
    return name == tool_names::kWebFetchUrl;
}

ToolInvocationResult MakeWebFetchToolResult(const ToolInvocation& inv,
                                            const WebFetchResult& webResult)
{
    ToolInvocationResult r;
    r.toolTag       = tool_names::kWebFetchUrl;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = tool_approval::ToolIcon(tool_names::kWebFetchUrl);
    r.toolName      = tool_approval::ToolDisplayName(tool_names::kWebFetchUrl);
    r.commandEcho   = webResult.commandEcho.empty()
                        ? std::string("/web_fetch_url ") + inv.args
                        : webResult.commandEcho;
    r.chips         = webResult.chips;
    r.body          = webResult.body;
    r.errorBody     = webResult.errorBody;
    r.bodyLang      = webResult.bodyLang;

    if (!webResult.textPath.empty()) {
        PresentedFile textFile;
        textFile.displayName = webResult.textDisplayName.empty()
            ? std::string("webpage_text.md")
            : webResult.textDisplayName;
        textFile.language  = "markdown";
        textFile.diskPath  = webResult.textPath;
        textFile.sizeBytes = webResult.textBytes;
        textFile.lineCount = webResult.textLineCount;
        r.presentedFiles.push_back(std::move(textFile));
    }

    if (!webResult.rawHtmlPath.empty()) {
        PresentedFile htmlFile;
        htmlFile.displayName = webResult.rawHtmlDisplayName.empty()
            ? std::string("webpage_raw.html")
            : webResult.rawHtmlDisplayName;
        htmlFile.language  = "html";
        htmlFile.diskPath  = webResult.rawHtmlPath;
        htmlFile.sizeBytes = webResult.htmlBytes;
        htmlFile.lineCount = 0;
        r.presentedFiles.push_back(std::move(htmlFile));
    }

    return r;
}

ToolInvocationResult MakeAsyncLaunchErrorResult(const ToolInvocation& inv,
                                                const std::string&    errorText)
{
    ToolInvocationResult r;
    r.toolTag       = inv.name.empty() ? std::string("tool") : inv.name;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = tool_approval::ToolIcon(inv.name);
    r.toolName      = tool_approval::ToolDisplayName(inv.name);
    r.commandEcho   = tool_approval::CommandEcho(inv);
    r.chips         = { "error", "launch failed" };
    r.errorBody     = errorText.empty()
        ? std::string("Tool failed to start before producing a result.")
        : errorText;
    return r;
}

// Standalone file/artifact requests usually end immediately after write or
// overwrite_file succeeds. Without this deterministic stop, small local models
// often ask for the same write again; write then reports "exists", the model
// switches to overwrite_file, and the loop keeps replacing the same artifact.
//
// Keep this scoped to conversations with no active project attached so project
// workflows can still do multi-step flows such as overwriting Inputs\*.txt and
// then running a workflow script. Project chats can still terminate normally
// when the model emits final prose after the write result.
bool ShouldStopAfterStandaloneWriteArtifact(const ToolInvocation& inv,
                                            const ToolInvocationResult& r,
                                            const ToolContext& ctx)
{
    if (inv.name != tool_names::kWrite &&
        inv.name != tool_names::kOverwriteFile) {
        return false;
    }

    if (!ctx.activeProjectRoot.empty()) return false;
    if (!r.errorBody.empty()) return false;
    if (r.presentedFiles.empty()) return false;

    return HasChip(r, "created") || HasChip(r, "overwritten");
}

std::string StandaloneWriteCompletionMessage(const ToolInvocation& inv,
                                             const ToolInvocationResult& r)
{
    std::string name = r.presentedFiles.empty()
        ? std::string("the file")
        : r.presentedFiles.front().displayName;

    if (name.empty()) name = "the file";

    if (inv.name == tool_names::kOverwriteFile)
        return "I have updated " + name + ".";

    return "I have created " + name + ".";
}

} // namespace

// ═══════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════

AgentController::AgentController(std::unique_ptr<ChatHistory>& history,
                                 AgentEventSink* sink,
                                 AppState*       appState,
                                 GrepExecutor*   grepExec,
                                 CmdExecutor*    cmdExec,
                                 PythonRunner*   pythonRunner,
                                 WebFetchExecutor* webFetchExec,
                                 ToolWorkerExecutor* toolWorker)
    : m_history(history)
    , m_sink(sink)
    , m_appState(appState)
    , m_grepExec(grepExec)
    , m_cmdExec(cmdExec)
    , m_pythonRunner(pythonRunner)
    , m_webFetchExec(webFetchExec)
    , m_toolWorker(toolWorker)
{}

// ═══════════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════════

void AgentController::Begin()
{
    m_active               = true;
    m_cancelled            = false;
    m_iterationsUsed       = 0;   // incremented when a counted tool result is fed back
    m_consecutiveMalformed = 0;
    m_awaitingAsyncResult    = false;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext    = ToolContext{};
    m_awaitingApproval       = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};
    m_currentToolCallId.clear();
    m_recentToolSignatures.clear();
    m_pendingSoftHint.clear();
    m_queuedInvocations.clear();
    // The one-shot python_create_script -> python_run_script bypass may
    // survive one follow-up user turn so a natural "yes, run it" can work.
    // If it was not consumed in that next turn, clear it on the following
    // Begin() so an old approval cannot surprise-run an unrelated script.
    if (!m_oneShotApprovedScriptRun.empty()) {
        if (m_oneShotApprovedScriptRunBeginCredits <= 0) {
            m_oneShotApprovedScriptRun.clear();
        } else {
            --m_oneShotApprovedScriptRunBeginCredits;
        }
    }

    // Phase 5: signal loop start so the frame (or future P9 parent)
    // can install loop-scoped UI state.  Currently a no-op in
    // MyFrame's implementation — the user message and first request
    // are already on screen by the time Begin() runs — but the seam
    // exists for future hooks.
    if (m_sink) m_sink->OnAgentEvent(AgentEvent::LoopBegin());
}

void AgentController::Cancel()
{
    if (!m_active) return;
    m_cancelled = true;

    // Agent-owned async tools run while MyFrame's coarse state may still
    // look like Streaming.  Cancel them here so the Stop button reaches
    // the actual worker instead of only stopping a chat stream.
    if (m_awaitingAsyncResult) {
        if (m_pendingAsyncInvocation.name == tool_names::kGrep && m_grepExec) {
            m_grepExec->Cancel();
        }
        else if (m_pendingAsyncInvocation.name == tool_names::kPowerShell && m_cmdExec) {
            m_cmdExec->Cancel();
        }
        else if (IsAgentPythonAsyncToolName(m_pendingAsyncInvocation.name) &&
                 m_pythonRunner) {
            m_pythonRunner->Cancel();
        }
        else if (IsAgentWebFetchAsyncToolName(m_pendingAsyncInvocation.name) &&
                 m_webFetchExec) {
            m_webFetchExec->Cancel();
        }
        else if (ShouldDispatchToolOnWorker(m_pendingAsyncInvocation.name) &&
                 m_toolWorker) {
            m_toolWorker->Cancel();
        }
    }
}

void AgentController::EndLoop(AgentEndReason     reason,
                              const std::string& userFacingMessage)
{
    m_active                 = false;
    m_awaitingAsyncResult    = false;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext    = ToolContext{};
    m_awaitingApproval       = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};
    m_currentToolCallId.clear();
    m_recentToolSignatures.clear();
    m_pendingSoftHint.clear();
    // Safety net: call sites that abandon a partially-executed batch
    // must drain it (with skipped results) BEFORE calling EndLoop so
    // native transcript pairing survives.  Clearing here guards any
    // future path that forgets, at the cost of the sanitizer stripping
    // the orphaned sidecar on the next request.
    m_queuedInvocations.clear();
    // Bundled-approval bypass survives a Normal exit (model paused with
    // "Want me to run it?" prose).  Abnormal exits — user cancelled,
    // loop guard tripped, malformed cap, iteration cap, send/stream
    // errors — wipe it because the user's intent context is gone.
    if (reason != AgentEndReason::Normal) {
        m_oneShotApprovedScriptRun.clear();
        m_oneShotApprovedScriptRunBeginCredits = 0;
    }

    // Phase 5: single sink call replaces the Phase-4 pair of
    // (DisplaySystemMessage + onLoopEnd callback).  The frame's
    // implementation surfaces the message via DisplaySystemMessage
    // when non-empty and runs its standard finalization sequence.
    if (m_sink) m_sink->OnAgentEvent(AgentEvent::LoopEnd(reason, userFacingMessage));
}

void AgentController::SetMaxToolSteps(int steps)
{
    if (steps < kMinConfigurableToolSteps) steps = kMinConfigurableToolSteps;
    if (steps > kMaxConfigurableToolSteps) steps = kMaxConfigurableToolSteps;
    m_maxToolSteps = steps;
}

// ═══════════════════════════════════════════════════════════════════
//  Phase 10: native multi-call batch queue
// ═══════════════════════════════════════════════════════════════════

bool AgentController::DispatchNextQueuedInvocation()
{
    if (m_queuedInvocations.empty()) return false;

    ToolInvocation next = m_queuedInvocations.front();
    m_queuedInvocations.erase(m_queuedInvocations.begin());
    return DispatchAndContinue(next);
}

void AgentController::DrainQueuedInvocationsWithSkippedResults(
    const std::string& reason)
{
    if (m_queuedInvocations.empty()) return;

    std::vector<ToolInvocation> drained;
    drained.swap(m_queuedInvocations);

    for (const ToolInvocation& inv : drained) {
        ToolInvocationResult r;
        r.toolTag       = inv.name.empty() ? std::string("tool") : inv.name;
        r.invocationRaw = inv.rawBlock;
        r.iconUtf8      = tool_approval::ToolIcon(inv.name);
        r.toolName      = tool_approval::ToolDisplayName(inv.name);
        r.commandEcho   = tool_approval::CommandEcho(inv);
        r.chips         = { "skipped" };
        r.errorBody     = "Not executed: " + reason +
                          " Re-issue this tool call in your next turn if it "
                          "is still needed.";

        // Thread each skipped result with its own call id so the
        // assistant.tool_calls sidecar persisted for this batch keeps a
        // complete, consecutive set of role:"tool" replies.
        m_currentToolCallId = inv.toolCallId;
        EmitAndStoreTerminalToolResult(r, /*startExpanded=*/false);
    }
}

// Phase 5: pack a ToolInvocationResult into a ToolBlock and send it
// to the sink.  Replaces four near-duplicate inline blocks that
// constructed a ChatDisplay::ToolBlock and called DisplayToolBlock
// directly — sync dispatch (FeedResultAndIterate), grep cancel
// rendering, cmd cancel rendering, and malformed-cap error
// rendering.
void AgentController::EmitToolBlock(const ToolInvocationResult& r,
                                    bool startExpanded)
{
    if (!m_sink) return;
    ToolBlock tb;
    tb.iconUtf8    = r.iconUtf8;
    tb.toolName    = r.toolName;
    tb.statusChips = r.chips;
    tb.commandEcho = r.commandEcho;
    tb.body        = r.body;
    tb.errorBody   = r.errorBody;
    tb.bodyLang    = r.bodyLang;
    tb.presentedFiles = r.presentedFiles;
    m_sink->OnAgentEvent(AgentEvent::ToolOutput(tb, startExpanded, ClassifyToolOutputEvent(r)));
}


void AgentController::EmitAndStoreTerminalToolResult(const ToolInvocationResult& r,
                                                     bool startExpanded)
{
    EmitToolBlock(r, startExpanded);

    std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
        r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang, r.presentedFiles);
    m_history->AddToolResultMessage(m_currentToolCallId, formatted);
    m_currentToolCallId.clear();
}

void AgentController::EmitPendingToolBlock(const ToolInvocation& inv)
{
    if (!m_sink) return;

    ToolBlock tb;
    tb.iconUtf8    = tool_approval::ToolIcon(inv.name);
    tb.toolName    = tool_approval::ToolDisplayName(inv.name);
    tb.statusChips = { "pending" };
    tb.commandEcho = tool_approval::CommandEcho(inv);

    // Keep this intentionally body-less. The completed ToolBlock will carry
    // stdout/stderr/artifacts. This card is just a lightweight acknowledgement
    // that the approved async tool has started, so the user is not left staring
    // at a blank assistant turn. It is UI-only and is not added to chat history.
    m_sink->OnAgentEvent(AgentEvent::ToolOutput(
        tb, /*expanded=*/false, AgentEventType::AgentStatus));
}

void AgentController::EmitAndStoreAgentStatusCard(
    const std::string& title,
    const std::vector<std::string>& chips,
    const std::string& message,
    bool startExpanded)
{
    ToolInvocationResult r;
    r.toolTag     = "agent_status";
    r.iconUtf8    = "\xE2\x9A\xA0";  // ⚠
    r.toolName    = title.empty() ? "Agent Status" : title;
    r.commandEcho = "agent loop";
    r.chips       = chips;
    r.errorBody   = message;

    EmitAndStoreTerminalToolResult(r, startExpanded);
}

std::string AgentController::BuildToolSignature(const ToolInvocation& inv) const
{
    std::string name = inv.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });

    return name + "|" + NormalizeForToolSignature(inv.args);
}

bool AgentController::WouldTripLoopGuard(const ToolInvocation& inv,
                                         std::string&          signatureOut,
                                         int&                  repeatCountOut) const
{
    signatureOut = BuildToolSignature(inv);
    repeatCountOut = 1;  // include the candidate invocation

    if (signatureOut.empty()) return false;

    const size_t keepBeforeCandidate =
        (kLoopGuardWindow > 0) ? static_cast<size_t>(kLoopGuardWindow - 1) : 0;
    const size_t n = m_recentToolSignatures.size();
    const size_t start = (n > keepBeforeCandidate) ? (n - keepBeforeCandidate) : 0;

    for (size_t i = start; i < n; ++i) {
        if (m_recentToolSignatures[i].signature == signatureOut)
            ++repeatCountOut;
    }

    return repeatCountOut >= kLoopGuardRepeatThreshold;
}

bool AgentController::WouldTripCycleGuard(const ToolInvocation& inv,
                                          std::string&          signatureOut,
                                          int&                  distinctCountOut) const
{
    signatureOut = BuildToolSignature(inv);
    distinctCountOut = 0;
    if (signatureOut.empty()) return false;

    std::vector<std::string> window;
    window.reserve(static_cast<size_t>(kCycleGuardWindow));

    const size_t keepBeforeCandidate =
        (kCycleGuardWindow > 0) ? static_cast<size_t>(kCycleGuardWindow - 1) : 0;
    const size_t n = m_recentToolSignatures.size();
    const size_t start = (n > keepBeforeCandidate) ? (n - keepBeforeCandidate) : 0;

    for (size_t i = start; i < n; ++i) {
        if (!m_recentToolSignatures[i].signature.empty())
            window.push_back(m_recentToolSignatures[i].signature);
    }
    window.push_back(signatureOut);

    if (window.size() < static_cast<size_t>(kCycleGuardWindow))
        return false;

    std::map<std::string, int> counts;
    for (const auto& sig : window)
        ++counts[sig];

    distinctCountOut = static_cast<int>(counts.size());
    if (distinctCountOut > kCycleGuardMaxDistinct)
        return false;

    for (const auto& kv : counts) {
        if (kv.second < kCycleGuardMinRepeats)
            return false;
    }

    return true;
}

void AgentController::RecordToolSignature(const std::string& signature)
{
    if (signature.empty()) return;

    m_recentToolSignatures.push_back(ToolSignatureRecord{signature, false});
    const size_t maxKeep = static_cast<size_t>(
        std::max(kLoopGuardWindow,
                 std::max(kLoopGuardRepeatThreshold,
                          kCycleGuardWindow - 1)));
    while (m_recentToolSignatures.size() > maxKeep)
        m_recentToolSignatures.erase(m_recentToolSignatures.begin());
}

void AgentController::ResolveToolSignatureOutcome(const ToolInvocation& inv,
                                                  bool                  failed)
{
    const std::string sig = BuildToolSignature(inv);
    if (sig.empty() || m_recentToolSignatures.empty()) return;

    // The matching record is normally the newest one (recorded at this
    // call's dispatch).  An async completion can only resolve after its
    // own dispatch, and the batch queue is sequential, so back() is the
    // right slot whenever the signatures agree.
    if (m_recentToolSignatures.back().signature == sig) {
        m_recentToolSignatures.back().failed = failed;
    }

    if (!failed) {
        // Phase 7d: a success after earlier failures of the SAME call
        // proves the blocking state changed (run(missing) ->
        // python_create_script -> run(ok) is the canonical transcript).
        // Purge those failed records so a later legitimate identical
        // call is not hard-blocked or soft-hinted as a "stuck loop".
        // Successful records stay, keeping the identical-successful-call
        // doom-loop guard intact.
        for (size_t i = 0; i + 1 < m_recentToolSignatures.size(); ) {
            if (m_recentToolSignatures[i].failed &&
                m_recentToolSignatures[i].signature == sig) {
                m_recentToolSignatures.erase(
                    m_recentToolSignatures.begin() +
                    static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
}

void AgentController::EmitToolCallEvent(const ToolInvocation& inv,
                                        const std::string&   signature)
{
    if (!m_sink) return;

    m_sink->OnAgentEvent(AgentEvent::ToolCall(
        inv.name,
        tool_approval::CommandEcho(inv),
        inv.toolCallId,
        signature));
}


// ═══════════════════════════════════════════════════════════════════
//  Request building
// ═══════════════════════════════════════════════════════════════════

std::string AgentController::BuildRequestBody()
{
    // ChatHistory::BuildChatRequestJson accepts an optional system
    // prompt as its third arg.  Poco handles proper JSON escaping
    // (including UTF-8 multi-byte sequences) — don't reinvent it.
    //
    // Fourth arg is the context-size hint in tokens; when provided,
    // the builder elides old tool-result bodies if the request would
    // exceed ~70% of the budget.  We grab the live ctx size from
    // AppState so Settings changes mid-loop take effect on the next
    // iteration without needing to restart anything.
    //
    // Phase 3c-i: fifth arg is an optional tools-array JSON string.
    // We attach it only when the active model has been confirmed
    // (via Phase 3b detection) to support native function calling.
    // On Xml/Unknown protocol we leave the field empty and the
    // builder produces the historical XML-only request shape.
    std::string model     = m_appState->GetModel();
    std::string sysPrompt = m_cb.buildSystemPrompt
                              ? m_cb.buildSystemPrompt()
                              : std::string();
    int ctxTokens = m_appState->GetCtxSize();
    if (ctxTokens <= 0) ctxTokens = 8192;  // defensive fallback

    std::string tools;
    bool native = false;
    if (m_cb.getActiveProtocol &&
        m_cb.getActiveProtocol() == ToolProtocol::Native) {
        tools = GetCachedToolsArrayJson();
        native = true;
    }

    return m_history->BuildChatRequestJson(model, /*stream*/ true,
                                           sysPrompt, ctxTokens,
                                           tools, native,
                                           true);
}

// ═══════════════════════════════════════════════════════════════════
//  Dispatch + iteration
// ═══════════════════════════════════════════════════════════════════

bool AgentController::ApprovePendingTool(bool rememberForChat)
{
    if (!m_active || !m_awaitingApproval) return false;

    ToolInvocation inv = m_pendingApprovalInvocation;
    ToolContext    ctx = m_pendingApprovalContext;

    m_awaitingApproval = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};

    // Mark approval memory BEFORE dispatch so an immediate follow-up
    // tool call in the same loop can skip another card.  "Approve
    // always" now means one-approval mode for this conversation, not
    // only this individual tool name.
    if (rememberForChat && m_history) {
        m_history->RememberAllToolApprovalsForChat();
    }

    return DispatchApprovedAndContinue(inv, ctx);
}

bool AgentController::DenyPendingTool()
{
    if (!m_active || !m_awaitingApproval) return false;

    ToolInvocation inv = m_pendingApprovalInvocation;
    m_awaitingApproval = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};
    m_pendingSoftHint.clear();

    ToolInvocationResult r = tool_approval::DeniedResult(
        inv, "Denied by user. Tool was not executed.");

    // Phase 10: a deny aborts the rest of the current batch.  Later
    // calls in the same assistant turn may depend on the denied one
    // (e.g. write file -> run script), and executing them would do
    // work the user just declined.  Feed the denial, emit skipped
    // results for the remainder (keeping native transcript pairing),
    // then continue to the next model request so the model can
    // re-plan with the full picture.
    FeedResultOnly(r, /*countTowardIterationCap=*/false);
    DrainQueuedInvocationsWithSkippedResults(
        "an earlier tool call in this batch was denied by the user.");
    ContinueLoop();
    return true;
}

bool AgentController::CancelPendingApproval()
{
    if (!m_active || !m_awaitingApproval) return false;

    ToolInvocation inv = m_pendingApprovalInvocation;
    m_awaitingApproval = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};
    m_pendingSoftHint.clear();

    ToolInvocationResult r = tool_approval::DeniedResult(
        inv, "Cancelled by user before approval. Tool was not executed.");
    EmitAndStoreTerminalToolResult(r, true);

    DrainQueuedInvocationsWithSkippedResults(
        "the agent loop was stopped by the user before this call ran.");

    EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
    return true;
}

bool AgentController::DispatchAndContinue(const ToolInvocation& inv)
{
    // Phase 3c-ii: thread the call id through to FeedResultAndIterate
    // so the eventual tool-result message can carry the right
    // tool_call_id.  Empty for XML-protocol invocations and for
    // malformed/missing invocations — both are stored as plain user
    // messages exactly as before.
    m_currentToolCallId = inv.toolCallId;

    if (!inv.valid) {
        // Malformed / unknown tool.  Feed the reason back to the
        // model as an error result — it may self-correct.  Counter
        // ticks up; if we exceed the cap, EndLoop.
        ++m_consecutiveMalformed;

        // Echo a head+tail PREVIEW of the broken block, never the full
        // block. The batch parser already previews its malformed
        // rawText, but invocations that arrive here via the recovery
        // shims (parsed-but-invalid: unknown tool, bad args) carry the
        // full rawBlock. Echoing a multi-KB copy of the model's own
        // mistake back into context makes the broken pattern the
        // strongest signal the model sees, and small models then repeat
        // it verbatim until the cap trips (observed 2026-06-11).
        const std::string rawPreview =
            MakeToolCallDiagnosticPreview(inv.rawBlock);

        ToolInvocationResult r;
        r.toolTag       = inv.name.empty() ? "tool" : inv.name;
        r.invocationRaw = rawPreview;
        r.toolName      = inv.name.empty() ? "Tool" : inv.name;
        r.commandEcho   = rawPreview.empty()
            ? std::string("<malformed tool_call>") : rawPreview;
        r.errorBody     = inv.invalidReason.empty()
            ? "Invalid tool invocation."
            : inv.invalidReason;
        r.iconUtf8      = "\xE2\x9A\xA0";  // ⚠
        r.chips         = { "error" };

        // Second strike and beyond: the first corrective message did
        // not land, so escalate. Name the repetition explicitly and
        // demand a bare corrected block — small models that ignore the
        // format reminder often still respond to a direct prohibition
        // on repeating the previous output.
        if (m_consecutiveMalformed >= 2 &&
            m_consecutiveMalformed < kMaxMalformedPerTurn) {
            r.errorBody +=
                "\nThis is malformed tool call " +
                std::to_string(m_consecutiveMalformed) + " of " +
                std::to_string(kMaxMalformedPerTurn) +
                " allowed. Do NOT repeat the previous block. Reply with"
                " ONLY one corrected tool call block and nothing else,"
                " ending with the literal closing tag </tool_call>.";
        }

        if (m_consecutiveMalformed >= kMaxMalformedPerTurn) {
            // Render and store the terminal error so native tool-call
            // sidecars are not left orphaned on disk.  No iteration
            // follow-up, but the final error still round-trips through
            // history with the current tool_call_id when one exists.
            EmitAndStoreTerminalToolResult(r, true);

            DrainQueuedInvocationsWithSkippedResults(
                "the agent stopped on repeated malformed tool calls "
                "before this call ran.");

            EndLoop(AgentEndReason::MalformedCap,
                    "Agent stopped: " +
                    std::to_string(kMaxMalformedPerTurn) +
                    " malformed tool calls in a row.");
            return false;
        }

        // Feed the error back and let the model try again.
        FeedResultAndIterate(r, /*countTowardIterationCap=*/false);
        return true;
    }

    // Valid invocation — reset malformed counter (we saw progress).
    m_consecutiveMalformed = 0;

    // Phase 7: controlled multi-step loop guard.  Stop before
    // dispatch if the model is about to repeat the same exact tool
    // call too many times in a small rolling window.
    std::string signature;
    int repeatCount = 0;
    if (WouldTripLoopGuard(inv, signature, repeatCount)) {
        ToolInvocationResult r;
        r.toolTag       = inv.name.empty() ? "tool" : inv.name;
        r.invocationRaw = inv.rawBlock;
        r.iconUtf8      = "\xE2\x9A\xA0";  // ⚠
        r.toolName      = "Loop Guard";
        r.commandEcho   = tool_approval::CommandEcho(inv);
        r.chips         = { "blocked", "loop guard" };
        r.errorBody     =
            "Agent stopped before dispatch: the same tool call was requested " +
            std::to_string(repeatCount) +
            " times within the recent loop window. Tool was not executed.";

        EmitAndStoreTerminalToolResult(r, true);
        DrainQueuedInvocationsWithSkippedResults(
            "the loop guard stopped the agent before this call ran.");
        EndLoop(AgentEndReason::LoopGuard,
                "Agent stopped: repeated the same tool call.");
        return false;
    }

    int cycleDistinctCount = 0;
    if (WouldTripCycleGuard(inv, signature, cycleDistinctCount)) {
        ToolInvocationResult r;
        r.toolTag       = inv.name.empty() ? "tool" : inv.name;
        r.invocationRaw = inv.rawBlock;
        r.iconUtf8      = "\xE2\x9A\xA0";  // ⚠
        r.toolName      = "Loop Guard";
        r.commandEcho   = tool_approval::CommandEcho(inv);
        r.chips         = { "blocked", "loop guard", "cycle" };
        r.errorBody     =
            "Agent stopped before dispatch: the recent tool window appears "
            "to be cycling across " + std::to_string(cycleDistinctCount) +
            " repeated tool calls. Tool was not executed.";

        EmitAndStoreTerminalToolResult(r, true);
        DrainQueuedInvocationsWithSkippedResults(
            "the cycle guard stopped the agent before this call ran.");
        EndLoop(AgentEndReason::LoopGuard,
                "Agent stopped: repeated a cycle of tool calls.");
        return false;
    }
    // NOTE: the signature is recorded in DispatchApprovedAndContinue,
    // i.e. only when the tool actually dispatches.  Recording here —
    // before the approval gate — meant a DENIED call still counted as
    // a repeat, so a legitimate deny → model re-asks → user approves
    // sequence could trip the guard on a tool that only ever ran once.

    ToolContext ctx = m_cb.buildToolContext ? m_cb.buildToolContext()
                                              : ToolContext{};

    // Phase 6 follow-up: per-chat remembered approvals.  If the user
    // has previously approved this tool in this conversation with the
    // "always" variant, skip the approval card entirely and dispatch
    // directly.  Read-only tools never enter the gate to begin with.
    const bool alreadyApproved =
        m_history && m_history->IsToolChatApproved(inv.name);

    bool oneShotScriptRunApproved = false;
    if (inv.name == tool_names::kPythonRunScript &&
        !m_oneShotApprovedScriptRun.empty()) {
        std::string requested = NormalizeScriptNameForOneShotApproval(inv.args);
        if (!requested.empty()) {
            for (const auto& candidate : m_oneShotApprovedScriptRun) {
                if (requested == candidate) {
                    oneShotScriptRunApproved = true;
                    break;
                }
            }
        }

        // The carry-forward approval is single-use. If the model asks to run
        // a different script, fall through to the normal approval card and
        // do not keep a stale bypass around for later.
        m_oneShotApprovedScriptRun.clear();
        m_oneShotApprovedScriptRunBeginCredits = 0;
    }

    tool_approval::ApprovalDecision approval;
    if (!alreadyApproved &&
        !oneShotScriptRunApproved &&
        tool_approval::RequiresApproval(inv, ctx, approval)) {
        m_awaitingApproval = true;
        m_pendingApprovalInvocation = inv;
        m_pendingApprovalContext    = ctx;
        if (m_sink) m_sink->OnAgentEvent(AgentEvent::ApprovalRequired(approval.block));
        return true;
    }

    return DispatchApprovedAndContinue(inv, ctx);
}

bool AgentController::DispatchApprovedAndContinue(const ToolInvocation& inv,
                                                  const ToolContext&    ctx)
{
    // Phase 7b: soft-hint nudge.  Set this only at the approved dispatch
    // point so a denied approval cannot receive a factually-wrong repeat
    // warning.  The exact-repeat hard guard already ran before any approval
    // card was shown; this is only the one-step-early model-facing hint.
    std::string signature;
    int repeatCount = 0;
    (void)WouldTripLoopGuard(inv, signature, repeatCount);
    const bool consecutiveRepeat =
        !signature.empty() &&
        !m_recentToolSignatures.empty() &&
        m_recentToolSignatures.back().signature == signature;

    if (consecutiveRepeat &&
        repeatCount == kLoopGuardRepeatThreshold - 1 &&
        repeatCount >= 2) {
        m_pendingSoftHint +=
            "\n\n[notice] You just repeated this exact tool call. One more "
            "identical call will trip the loop guard and stop the agent. "
            "Try a different approach: call notes_read if a saved path may "
            "exist, ask the user where the file lives, or use a different "
            "tool. Do NOT repeat this exact call.";
    }

    // Phase 7 loop guard bookkeeping happens HERE — at the moment the
    // tool actually dispatches — not at the pre-approval check in
    // DispatchAndContinue.  Denied or cancelled approvals therefore
    // never count as "repeats" against a later, genuinely-approved
    // attempt at the same call.
    const std::string dispatchSignature =
        signature.empty() ? BuildToolSignature(inv) : signature;
    RecordToolSignature(dispatchSignature);

    // Phase 9/Phase 3 trace: typed, non-rendered event for
    // observers/tests/loggers.  Emit at the approved-dispatch point so
    // trace durations do not include approval wait time or denied calls.
    EmitToolCallEvent(inv, dispatchSignature);

    // Filesystem tools are synchronous at the router layer, but reads,
    // listings, writes, and Python syntax checks can all block on
    // antivirus or slow/network-backed paths.  Run registered candidates on
    // the frame-owned serialized worker rather than in this wx event handler.
    if (ShouldDispatchToolOnWorker(inv.name)) {
        if (!m_toolWorker || !m_toolWorker->Start(inv, ctx)) {
            ResolveToolSignatureOutcome(inv, /*failed=*/true);
            ToolInvocationResult r = MakeAsyncLaunchErrorResult(
                inv, "The background tool worker is already busy or could not start.");
            FeedResultAndIterate(r, /*countTowardIterationCap=*/false);
            return true;
        }

        m_awaitingAsyncResult    = true;
        m_pendingAsyncInvocation = inv;
        m_pendingAsyncContext    = ctx;
        EmitPendingToolBlock(inv);
        return true;
    }

    const DispatchOutcome out = DispatchInvocation(
        inv, ctx, m_grepExec, m_cmdExec, m_pythonRunner, m_webFetchExec);
    return FinishDispatchedInvocation(inv, ctx, out);
}

bool AgentController::FinishDispatchedInvocation(
    const ToolInvocation& inv,
    const ToolContext&    ctx,
    const DispatchOutcome& out)
{
    switch (out.status) {
        case DispatchStatus::Completed:
            // Phase 7d: resolve the just-recorded signature's outcome so
            // the loop guards can distinguish failure loops from
            // legitimate successful repeats.
            ResolveToolSignatureOutcome(inv, !out.result.errorBody.empty());

            if (inv.name == tool_names::kPythonCreateScript &&
                out.result.errorBody.empty() &&
                !out.result.presentedFiles.empty()) {
                m_oneShotApprovedScriptRun.clear();
                m_oneShotApprovedScriptRunBeginCredits = 0;

                // Grant the one-shot run bypass ONLY for the final on-disk
                // artifact filename returned by python_create_script.  The
                // bypass is filename-only by design; path-shaped run requests
                // are forced through the normal approval card.
                //
                // Important safety edge case:
                //   requested: report.py
                //   existing : report.py
                //   created  : report_2.py
                //
                // If we also approved the originally-requested name, the next
                // python_run_script report.py could bypass approval and run the
                // older script instead of the just-reviewed artifact.  Collision
                // renamed scripts therefore require the model to run the final
                // displayed artifact name, or the normal approval card appears.
                const PresentedFile& createdScript = out.result.presentedFiles.front();
                std::string finalName = NormalizeScriptNameForOneShotApproval(
                    createdScript.displayName);

                // Project scripts are created in the project Workflows lane,
                // but bare python_run_script filenames resolve conversation
                // Scripts first.  If a stale same-named conversation script
                // exists, skip the bypass so the normal approval card appears
                // instead of silently running the higher-priority older file.
                if (!finalName.empty() &&
                    !ProjectScriptRunBypassWouldBeShadowedByConversationScript(
                        ctx, createdScript.displayName)) {
                    m_oneShotApprovedScriptRun.push_back(finalName);
                    m_oneShotApprovedScriptRunBeginCredits = 1;
                }
            }

            // Phase 10: the deterministic write-and-stop heuristic exists
            // to keep small models from re-writing the same artifact in a
            // solo-call loop.  When the model explicitly batched more
            // calls behind this write, it has a plan — let the batch run.
            if (m_queuedInvocations.empty() &&
                ShouldStopAfterStandaloneWriteArtifact(inv, out.result, ctx)) {
                EmitAndStoreTerminalToolResult(out.result, false);

                if (m_sink) {
                    m_sink->OnAgentEvent(AgentEvent::TurnComplete(
                        StandaloneWriteCompletionMessage(inv, out.result)));
                }

                EndLoop(AgentEndReason::Normal, "");
                return true;
            }

            FeedResultAndIterate(out.result);
            return true;

        case DispatchStatus::Invalid:
            // Dispatch-level failure (e.g. path resolution).  Render, feed
            // back, continue loop — the invocation itself was well-formed.
            ResolveToolSignatureOutcome(inv, /*failed=*/true);
            FeedResultAndIterate(out.result);
            return true;

        case DispatchStatus::Async:
            // Specialized worker running.  Stash both the invocation and its
            // original context so all async completion paths have one state
            // shape and future result policy remains deterministic.
            m_awaitingAsyncResult    = true;
            m_pendingAsyncInvocation = inv;
            m_pendingAsyncContext    = ctx;
            EmitPendingToolBlock(inv);
            return true;
    }
    return false;
}

bool AgentController::HandleToolWorkerComplete(
    const ToolWorkerResult& workerResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;
    if (!ShouldDispatchToolOnWorker(m_pendingAsyncInvocation.name)) return false;
    if (workerResult.toolName != m_pendingAsyncInvocation.name) return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    ToolContext ctx = m_pendingAsyncContext;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    if (m_cancelled || workerResult.cancelled) {
        // A filesystem operation may have crossed its non-cancellable commit
        // point before Stop was pressed.  Preserve the real result on screen
        // and in history, then terminate the loop rather than asking the model
        // for another step.
        ResolveToolSignatureOutcome(
            inv, workerResult.outcome.status != DispatchStatus::Completed ||
                 !workerResult.outcome.result.errorBody.empty());
        return FinishAsyncToolResult(
            inv, workerResult.outcome.result, /*cancelled=*/true);
    }

    return FinishDispatchedInvocation(inv, ctx, workerResult.outcome);
}

void AgentController::FeedResultAndIterate(const ToolInvocationResult& rIn,
                                           bool countTowardIterationCap)
{
    FeedResultOnly(rIn, countTowardIterationCap);
    ContinueLoop();
}

void AgentController::FeedResultOnly(const ToolInvocationResult& rIn,
                                     bool countTowardIterationCap)
{
    ToolInvocationResult r = rIn;
    InlineSmallPdfExtractedMarkdown(r);

    // Phase 7b: consume pending soft-hint set at dispatch.  Appended
    // to body so the notice surfaces in both the model-facing tool
    // message and the on-screen tool block, then cleared (one-shot).
    if (!m_pendingSoftHint.empty()) {
        r.body += m_pendingSoftHint;
        m_pendingSoftHint.clear();
    }

    // Phase 5: emit a ToolBlock event instead of pushing directly
    // to ChatDisplay.  MyFrame's sink implementation forwards to
    // DisplayToolBlock unchanged; future P6 approval cards will
    // intercept this seam to gate dangerous tool results.
    EmitToolBlock(r);

    // Round-trip to history.  Uses the exact same format as a
    // user-typed invocation — so from the model's POV, its own
    // calls look identical to user calls on the next turn.  That's
    // deliberate: uniform history means uniform behavior.
    //
    // Phase 3c-ii: AddToolResultMessage attaches the tool_call_id
    // sidecar (if any) so the next request — under native protocol
    // — can emit a proper role:"tool" reply threaded to the
    // assistant call.  XML-protocol invocations have empty ids,
    // and AddToolResultMessage degrades to AddUserMessage in that
    // case, preserving Phase 1/2 behaviour.
    std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
        r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang, r.presentedFiles);

    // Step-budget trailer (model-facing only).  Only counted, actually
    // dispatched tool results consume the hard cap; approval denials and
    // malformed-call coaching are recovery/control messages, not tool work.
    if (countTowardIterationCap) {
        const int stepsUsed = m_iterationsUsed + 1;   // this result inclusive
        const int remaining = m_maxToolSteps - stepsUsed;
        formatted += "\n\n[agent tool step " + std::to_string(stepsUsed) +
                     " of " + std::to_string(m_maxToolSteps) + "]";
        if (remaining <= 2 && remaining > 0) {
            formatted += " Budget nearly exhausted: answer from the evidence "
                         "you already have unless one final tool call is "
                         "essential to complete the user's request.";
        }
    }

    m_history->AddToolResultMessage(m_currentToolCallId, formatted);

    // Consumed — clear so the next iteration's malformed-error
    // path or sync dispatch doesn't accidentally re-use it.
    m_currentToolCallId.clear();

    if (countTowardIterationCap)
        ++m_iterationsUsed;
}

void AgentController::ContinueLoop()
{
    // Iteration-cap check BEFORE kicking off the next request.  A
    // partially-executed batch is drained with skipped results first so
    // the persisted assistant.tool_calls sidecar keeps one reply per
    // call and the next native request stays valid.
    if (m_iterationsUsed >= m_maxToolSteps) {
        DrainQueuedInvocationsWithSkippedResults(
            "the agent reached its tool step cap before this call ran.");

        const std::string msg =
            "Agent stopped after " +
            std::to_string(m_maxToolSteps) +
            " tool step(s). This is a safety cap to prevent runaway loops. "
            "Ask the model to continue if you want more inspection, or "
            "raise the cap with /agent_steps <n>.";

        EmitAndStoreAgentStatusCard(
            "Agent Status",
            { "stopped", "tool cap", std::to_string(m_maxToolSteps) + " steps" },
            msg,
            true);

        EndLoop(AgentEndReason::IterationCap, msg);
        return;
    }
    if (m_cancelled) {
        DrainQueuedInvocationsWithSkippedResults(
            "the agent loop was stopped by the user.");
        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return;
    }

    // Phase 10: still inside a multi-call batch — execute the next
    // queued invocation from this assistant turn before asking the
    // model for anything new.
    if (!m_queuedInvocations.empty()) {
        DispatchNextQueuedInvocation();
        return;
    }

    // Fire next iteration: add a fresh assistant placeholder for
    // the upcoming streamed reply, prepare the UI, send request.
    //
    // Phase 5: OnAgentIterationBegin replaces the old
    // beginNextIteration callback.  The frame uses it to reset
    // streaming state, render the assistant prefix, and re-arm the
    // streaming flag.
    std::string model = m_appState->GetModel();
    m_history->AddAssistantPlaceholder(model);

    unsigned long genId = m_cb.bumpGenerationId ? m_cb.bumpGenerationId() : 0;
    if (m_sink) m_sink->OnAgentEvent(AgentEvent::IterationBegin());

    std::string body = BuildRequestBody();
    if (!m_cb.sendRequest || !m_cb.sendRequest(model, body, genId)) {
        m_history->RemoveLastAssistantMessage();
        EndLoop(AgentEndReason::SendFailed,
                "Agent stopped: failed to send next request.");
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Event handlers
// ═══════════════════════════════════════════════════════════════════

bool AgentController::HandleAssistantComplete(const std::string& fullResponse,
                                                const std::string& toolCallsJson)
{
    if (!m_active) return false;

    if (m_cancelled) {
        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return true;
    }

    // ── Phase 3c-ii: native protocol path ───────────────────────
    // When the active model is on the native tool-calling protocol
    // AND the streaming layer extracted at least one structured
    // tool_call, we bypass the XML parser entirely.  The model's
    // call(s) arrive as a JSON array of {id, type, function:
    // {name, arguments}} objects; we synthesize ToolInvocations
    // (carrying the call ids), store the assistant message with
    // its tool_calls sidecar so the next request can thread
    // role:"tool" replies, and dispatch through the router exactly
    // the same way XML invocations dispatch.
    //
    // Phase 10: multi-call batch dispatch.  When every parsed call has
    // a usable unique id, the full tool_calls sidecar is persisted and
    // the calls execute sequentially (first now, the rest queued); the
    // controller guarantees one role:"tool" reply per persisted call so
    // the transcript stays paired.  Id-less or ambiguous batches fall
    // back to the conservative Phase 3 rule: execute only the first
    // invocation and persist only its matching sidecar entry.
    const bool nativeActive =
        m_cb.getActiveProtocol &&
        m_cb.getActiveProtocol() == ToolProtocol::Native;

    if (nativeActive && !toolCallsJson.empty()) {
        std::vector<ToolInvocation> invocations =
            ParseStructuredToolCalls(toolCallsJson);

        if (!invocations.empty()) {
            // Phase 10: multi-call batch dispatch.  Persist the
            // tool_calls sidecar for every invocation we will execute,
            // queue the rest of the batch, and dispatch sequentially.
            // The controller guarantees one role:"tool" reply per
            // persisted call (result, error, denied, or skipped), so
            // the next native request keeps a valid paired transcript:
            //   assistant.tool_calls[k].id == role:"tool"[k].tool_call_id
            //
            // Ceiling: anything past kMaxNativeCallsPerTurn is dropped
            // from the sidecar entirely (so no orphaned ids) and the
            // model is told to re-issue the remainder next turn.
            std::string overflowNotice;
            if (invocations.size() >
                static_cast<size_t>(kMaxNativeCallsPerTurn)) {
                const size_t dropped =
                    invocations.size() -
                    static_cast<size_t>(kMaxNativeCallsPerTurn);
                invocations.resize(
                    static_cast<size_t>(kMaxNativeCallsPerTurn));
                overflowNotice =
                    "\n\n[notice] You emitted more tool calls than the per-"
                    "turn batch ceiling (" +
                    std::to_string(kMaxNativeCallsPerTurn) + "). The last " +
                    std::to_string(dropped) + " call(s) were dropped. "
                    "Re-issue them after reading these results if they are "
                    "still needed.";
            }

            bool batchMode = invocations.size() > 1;
            std::string sidecarJson;
            if (batchMode) {
                sidecarJson =
                    KeepExecutableToolCallsJson(toolCallsJson, invocations);
                // Fail closed to the conservative single-call path when
                // ids are missing, duplicated, or unmatched — pairing
                // would be ambiguous otherwise.
                if (sidecarJson.empty()) batchMode = false;
            }

            if (batchMode) {
                m_history->SetLastAssistantToolCalls(sidecarJson);

                m_queuedInvocations.assign(invocations.begin() + 1,
                                           invocations.end());
                if (!overflowNotice.empty()) {
                    m_pendingSoftHint += overflowNotice;
                }

                bool cont = DispatchAndContinue(invocations.front());
                return cont;
            }

            // ── Single-call path (also the id-less fallback) ─────
            // Execute exactly one native tool call and persist only the
            // matching sidecar entry, exactly as Phase 3 did.
            ToolInvocation first = invocations.front();
            if (invocations.size() > 1) {
                m_pendingSoftHint +=
                    "\n\n[notice] You emitted " + std::to_string(invocations.size()) +
                    " tool calls in one assistant turn, but this provider did "
                    "not supply usable tool call ids, so LlamaBoss executed only "
                    "the first one to keep the native transcript valid. Re-issue "
                    "any remaining tool calls one at a time after reading this result.";
            }
            std::string oneToolCallJson =
                KeepOnlySelectedToolCallJson(toolCallsJson, first.toolCallId);

            if (!oneToolCallJson.empty()) {
                m_history->SetLastAssistantToolCalls(oneToolCallJson);
            } else {
                // Defensive fallback: if we could not safely attach the
                // assistant.tool_calls sidecar, do not add the upcoming
                // result as role:"tool".  Let AddToolResultMessage degrade
                // to the legacy user-message format instead; that is valid
                // for both XML and native requests, while an orphan tool
                // message is not.
                first.toolCallId.clear();
            }

            bool cont = DispatchAndContinue(first);
            return cont;
        }

        // Model emitted structured toolCallsJson but none of the entries
        // could be projected into a ToolInvocation.  Treat this like a
        // malformed tool call and feed the error back once; silently ending
        // the loop here makes native-tool regressions look like the assistant
        // simply ignored its own call.
        ToolInvocation bad;
        bad.valid         = false;
        bad.rawBlock      = "[native tool_call] " + toolCallsJson;
        bad.invalidReason = "Native tool call payload could not be parsed.";
        bool cont = DispatchAndContinue(bad);
        return cont;
    }

    // Phase 7c: when native protocol is active and the model emitted
    // NO toolCallsJson at all, the response is the model's final
    // prose answer with no tool call.  Do NOT fall through to XML
    // parsing of fullResponse -- prose can legitimately contain
    // <tool_call> as plain text (e.g. when the model is explaining
    // its own tool-call protocol, citing docs, or quoting source
    // code that mentions <tool_call> blocks), and the XML parser
    // would synthesize a spurious malformed-invocation error.
    // Observed with Qwen3.6 (~every turn) and Gemma 26B (code-review
    // answers that describe the XML protocol).  Native function-call
    // is authoritative when active; empty == final answer, end loop.
    if (nativeActive && toolCallsJson.empty()) {
        EndLoop(AgentEndReason::Normal, "");
        return false;
    }

    // ── XML protocol path (Phase 1/2/3a/3b/3c-i unchanged) ──────
    ParsedAssistantResponse parsed = ParseAssistantResponse(fullResponse);

    // Malformed-only reply (has blocks but all unparseable).
    // Treat this as if we received one invalid invocation — the
    // counter-based stop rule kicks in.
    if (!parsed.hasInvocation && !parsed.malformed.empty()) {
        // Strip the unparseable block from the stored assistant
        // message, mirroring the valid-invocation strip below.
        // MyFrame's OnAssistantComplete already finalized the last
        // assistant message with the FULL response — broken block
        // included — and replaying a multi-KB verbatim copy of the
        // model's own mistake on every remaining iteration re-anchors
        // exactly the pattern the error result is coaching it away
        // from (see the 2026-06-11 note in DispatchAndContinue).  The
        // compact head+tail preview still reaches the model once, via
        // the error result's command echo, so it can self-correct
        // without the full block dominating its context.
        if (m_history->HasAssistantPlaceholder() ||
            m_history->GetMessageCount() > 0) {
            m_history->UpdateLastAssistantMessage(parsed.prose);
        }
        if (parsed.prose.empty()) {
            m_history->RemoveLastAssistantMessage();
        }

        ToolInvocation bad;
        bad.valid         = false;
        bad.rawBlock      = parsed.malformed.front().rawText;
        bad.invalidReason = parsed.malformed.front().reason;
        bool cont = DispatchAndContinue(bad);
        return cont;  // true if loop continuing; false if we bailed
    }

    // No tool call at all — this is the model's final answer.
    // MyFrame already rendered the streamed prose; we just exit
    // the loop and let normal completion flow through.
    if (!parsed.hasInvocation) {
        // Normal end of loop — no user message needed.
        EndLoop(AgentEndReason::Normal, "");
        return false;   // let MyFrame do normal completion handling
    }

    // Has a tool call.  The model's reply may have included prose
    // before the <tool_call>.  That prose was already streamed to
    // the display via DisplayAssistantDelta; we only need to clean
    // up history — strip the <tool_call> block from the stored
    // assistant message so it reads as prose-only.
    if (m_history->HasAssistantPlaceholder() ||
        m_history->GetMessageCount() > 0) {
        // Replace the last assistant message (which was just
        // finalized by MyFrame's OnAssistantComplete before this
        // call) with the prose portion only.  If prose is empty,
        // the message effectively disappears — that's fine,
        // reduces noise.
        m_history->UpdateLastAssistantMessage(parsed.prose);
    }

    // If prose is empty AND the model emitted only a tool call,
    // remove the now-empty assistant message so history stays tidy.
    // (An empty assistant message before a tool-result user message
    // confuses chat templates on some models.)
    if (parsed.prose.empty()) {
        m_history->RemoveLastAssistantMessage();
    }

    bool cont = DispatchAndContinue(parsed.invocation);
    return cont;
}

// Phase 3c-ii: parse the structured tool_calls JSON from the
// streaming response into ToolInvocations.  Each entry's
// `function.arguments` is a JSON-encoded string per OpenAI spec;
// we project that into our existing args-string contract by
// extracting the *single argument* shape that LlamaBoss's tool
// schemas use (each tool exposes one or two parameters that we
// flatten to the conventional space-separated form expected by
// the existing dispatchers).
//
// On any malformed entry we skip it rather than abort the whole
// batch — partial dispatch is better than total failure.
std::vector<ToolInvocation> AgentController::ParseStructuredToolCalls(
    const std::string& toolCallsJson)
{
    std::vector<ToolInvocation> out;
    if (toolCallsJson.empty()) return out;

    Poco::JSON::Array::Ptr arr;
    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(toolCallsJson);
        arr = var.extract<Poco::JSON::Array::Ptr>();
    } catch (...) {
        return out;
    }
    if (!arr) return out;

    for (size_t i = 0; i < arr->size(); ++i) {
        Poco::JSON::Object::Ptr entry;
        try { entry = arr->getObject(i); } catch (...) { continue; }
        if (!entry) continue;

        ToolInvocation inv;
        try { inv.toolCallId = entry->getValue<std::string>("id"); }
        catch (...) { /* leave empty */ }

        Poco::JSON::Object::Ptr fn;
        try { fn = entry->getObject("function"); } catch (...) { continue; }
        if (!fn) continue;

        try { inv.name = fn->getValue<std::string>("name"); }
        catch (...) { /* leave empty */ }

        // arguments is a JSON-encoded string.  We parse it as a
        // small JSON object and project to the legacy args-string
        // shape that ValidateToolArgs and the dispatchers expect.
        std::string argsRaw;
        try { argsRaw = fn->getValue<std::string>("arguments"); }
        catch (...) { argsRaw.clear(); }

        inv.args     = ProjectStructuredArgs(inv.name, argsRaw);
        inv.rawBlock = "[native tool_call] " + inv.name + "(" + argsRaw + ")";

        // Validate now so DispatchAndContinue can route the same
        // way malformed XML invocations route.
        std::string reason;
        if (inv.name.empty()) {
            inv.valid         = false;
            inv.invalidReason = "tool call missing function name";
        } else if (!ValidateToolArgs(inv.name, inv.args, reason)) {
            inv.valid         = false;
            inv.invalidReason = reason;

            // The most common native-path validation failure is a
            // hallucinated parameter name: ProjectStructuredArgs found
            // none of the schema fields, projected an empty args
            // string, and the validator reported a generic "requires
            // a path"-style error.  Name the keys the model actually
            // sent so it can fix the parameter name on the next try
            // instead of re-guessing blind.
            if (inv.args.empty() && !argsRaw.empty()) {
                std::string keys = AgentListJsonObjectKeys(argsRaw);
                if (!keys.empty()) {
                    inv.invalidReason +=
                        " (your tool call sent JSON argument keys: " + keys +
                        " — none match this tool's parameter schema; use the"
                        " parameter names from the tool catalog)";
                }
            }
        } else {
            inv.valid = true;
        }

        out.push_back(std::move(inv));
    }

    return out;
}

// Project the OpenAI structured arguments (a JSON object inside a
// stringified payload) onto LlamaBoss's legacy args-string form.
//
// Most tools' wire schemas declare a single string parameter named
// "args" carrying the same legacy args body the XML protocol
// produced, and the dispatchers reuse their existing per-tool
// parsers without a structured-args branch.  pwd is the lone
// no-args exception: empty string always.
//
// Phase 3c-iii/iv: several native tools moved to structured shapes.
//   write             : {path, content}              → "path\ncontent"
//   overwrite_file    : {path, content}              → "path\ncontent"
//   read/open/ls/mkdir/delete: {path} or aliases     → "path"
//   edit              : {path, old_str, new_str}     → sentinel form
//   grep              : {pattern, path?}             → "pattern\npath"
//   python_run_script : {script, argv?[]}            → "script\narg1\narg2"
// The flattened result feeds the existing dispatcher/parser code
// verbatim, so tool internals stay unchanged.  Backward-compat:
// if a model emits the old {args} shape, the trailing fallback at
// the bottom picks it up and the dispatchers run as before.
//
// On parse failure we fall through to the raw string; most
// dispatchers tolerate that for single-arg tools and the
// validator catches anything truly malformed.
std::string AgentController::ProjectStructuredArgs(
    const std::string& toolName,
    const std::string& argsJson)
{
    if (toolName == tool_names::kPwd) return std::string();

    Poco::JSON::Object::Ptr obj;
    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(argsJson.empty() ? std::string("{}") : argsJson);
        obj = var.extract<Poco::JSON::Object::Ptr>();
    } catch (...) {
        return argsJson;   // best-effort fallback
    }
    if (!obj) return argsJson;

    // Small helper: pull a string field, return empty on absence
    // or type mismatch (rather than throw) so the projection
    // stays best-effort.
    auto getStr = [&](const std::string& key) -> std::string {
        if (!obj->has(key)) return std::string();
        try { return obj->getValue<std::string>(key); }
        catch (...) { return std::string(); }
    };

    // Small helper: pull the most common path-ish native field.
    // A few providers/models keep choosing {path} even for legacy
    // single-string tools that historically declared {args}; accept
    // those aliases so harmless read/open/ls/mkdir calls do not burn
    // the malformed-call budget.
    auto getPathLike = [&]() -> std::string {
        static const char* kKeys[] = {
            "path", "file_path", "filepath", "file", "filename", "directory", "dir"
        };
        for (const char* key : kKeys) {
            std::string v = getStr(key);
            if (!v.empty()) return v;
        }
        return std::string();
    };

    // Small helper: pull a JSON array field as text argv tokens.
    // String entries pass through unchanged; simple scalar entries are
    // converted by Poco so native callers cannot accidentally drop
    // a numeric option value such as 1.
    auto getStringArray = [&](const std::string& key) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (!obj->has(key)) return out;
        try {
            Poco::JSON::Array::Ptr arr = obj->getArray(key);
            if (!arr) return out;
            out.reserve(arr->size());
            for (size_t i = 0; i < arr->size(); ++i) {
                try {
                    std::string item = arr->get(i).convert<std::string>();
                    out.push_back(std::move(item));
                } catch (...) {
                    // Skip malformed argv entries; validator/runtime
                    // will still reject an empty script name below.
                }
            }
        } catch (...) {
            // Not an array.  Leave empty so legacy {args} fallback
            // can still run if present.
        }
        return out;
    };

    // ── python_create_script: {filename, content} → "filename\n" + content
    // This creates a reviewable .py artifact in the Scripts lane.
    // It does not execute the script.
    if (toolName == tool_names::kPythonCreateScript && obj->has("filename")) {
        std::string filename = getStr("filename");
        std::string content  = getStr("content");
        if (!filename.empty()) {
            return filename + "\n" + content;
        }
    }

    // ── write: {path, content} → "path\n" + content ─────────────
    // SplitPathAndContent reads the first line as the path and
    // everything after the first '\n' as the content body, so the
    // simple concatenation reproduces the legacy shape exactly.
    // Missing content → empty string → empty-file write.  We only
    // engage this branch if `path` is present; otherwise we fall
    // through so a model still emitting the old {args} shape gets
    // handled as a backward-compat case.
    if ((toolName == tool_names::kWrite ||
         toolName == tool_names::kOverwriteFile ||
         toolName == tool_names::kWritePowerShellScript) && obj->has("path")) {
        std::string path    = getStr("path");
        std::string content = getStr("content");
        if (!path.empty()) {
            return path + "\n" + content;
        }
    }

    // ── edit: {path, old_str, new_str} → sentinel form ──────────
    // ParseEditArgs requires <<<OLD>>> and <<<NEW>>> each on their
    // own line.  We surround them with '\n' on both sides; a missing
    // trailing newline on new_str is fine — ParseEditArgs accepts
    // either shape.  Empty new_str produces a delete-OLD operation
    // (allowed).  Empty old_str makes ParseEditArgs reject with a
    // useful "<<<OLD>>> block is empty" message — the model gets
    // clear feedback instead of silent misbehavior.
    if (toolName == tool_names::kEdit && obj->has("path")) {
        std::string path = getStr("path");
        std::string oldS = getStr("old_str");
        std::string newS = getStr("new_str");
        if (!path.empty()) {
            return path + "\n<<<OLD>>>\n" + oldS +
                   "\n<<<NEW>>>\n" + newS;
        }
    }

    // ── simple path tools: {path} / {file_path} → "path" ─────────
    // Some native callers prefer structured path keys even when the
    // tool catalog still exposes a legacy {args} string.  Keep the
    // legacy {args} fallback below, but accept path-like aliases for
    // tools whose dispatcher consumes a single path argument.
    if (toolName == tool_names::kRead ||
        toolName == tool_names::kOpen ||
        toolName == tool_names::kLs ||
        toolName == tool_names::kMkdir ||
        toolName == tool_names::kDelete) {
        std::string path = getPathLike();
        if (!path.empty()) return path;
    }

    // ── read_head: {path, lines} → "<lines>\n<path>" ─────────
    if (toolName == tool_names::kReadHead) {
        std::string path = getPathLike();
        std::string lines;
        if (obj->has("lines")) {
            try { lines = std::to_string(obj->getValue<int>("lines")); }
            catch (...) { lines.clear(); }
        }
        if (!path.empty()) {
            return (lines.empty() ? std::string("40") : lines) + "\n" + path;
        }
    }

    // ── grep: {pattern, path?} → "pattern\npath" ───────────────
    // The newline shape lets native callers search for literal patterns
    // containing spaces without triggering the legacy "first whitespace
    // token is pattern" parser.  DoGrep keeps the one-line legacy parser
    // for XML and old native {args} calls.
    if (toolName == tool_names::kGrep && obj->has("pattern")) {
        std::string pattern = getStr("pattern");
        std::string path    = getStr("path");
        if (!pattern.empty()) {
            return path.empty() ? pattern : (pattern + "\n" + path);
        }
    }

    // ── python_run_script: {script, argv?[]} → script + arg lines ──
    // The runner already treats every later line as exactly one argv
    // token.  Native structured argv removes the fragile model-facing
    // "one token per line" prose while preserving runner behavior.
    if (toolName == tool_names::kPythonRunScript && obj->has("script")) {
        std::string script = getStr("script");
        if (!script.empty()) {
            std::ostringstream flat;
            flat << script;
            for (const std::string& arg : getStringArray("argv")) {
                // The legacy runner's transport is one argv token per
                // line, so keep each native argv element on exactly one
                // line.  If a model tries to put embedded newlines in an
                // argument, collapse them to spaces instead of letting one
                // JSON array element become several argv tokens.
                std::string oneLineArg = arg;
                std::replace(oneLineArg.begin(), oneLineArg.end(), '\r', ' ');
                std::replace(oneLineArg.begin(), oneLineArg.end(), '\n', ' ');
                flat << "\n" << oneLineArg;
            }
            return flat.str();
        }
    }

    // ── Single-string tools, plus backward-compat fallback ──────
    // Remaining single-string tools (read, ls, open, mkdir, delete,
    // powershell, etc.) declare one "args" string property and the
    // dispatcher's per-tool parser consumes it. This branch is also
    // the backward-compat fallback for tools migrated to structured
    // schemas when a model or saved replay still emits {args}.
    if (obj->has("args")) {
        try { return obj->getValue<std::string>("args"); }
        catch (...) { /* fall through */ }
    }
    return std::string();
}


bool AgentController::FinishAsyncToolResult(const ToolInvocation&       inv,
                                            const ToolInvocationResult& r,
                                            bool                        cancelled)
{
    if (cancelled) {
        // Still render the (likely-partial) result so the user sees
        // what came back before the cancel landed, and round-trip it
        // to history so the model's transcript reflects what actually
        // happened.
        EmitToolBlock(r);

        std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
            r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang, r.presentedFiles);
        m_history->AddToolResultMessage(inv.toolCallId, formatted);

        DrainQueuedInvocationsWithSkippedResults(
            "the agent loop was stopped by the user before this call ran.");

        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return true;
    }

    // Phase 7d: async outcome lands here; resolve before continuing so
    // a successful retry-after-fix purges the earlier failed records of
    // the same call (e.g. python_run_script after python_create_script).
    ResolveToolSignatureOutcome(inv, !r.errorBody.empty());

    FeedResultAndIterate(r);
    return true;
}

bool AgentController::HandleGrepComplete(const GrepResult& grepResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;

    // Defensive: only consume the event if THIS controller fired
    // the grep call.  Without this check, a stale /grep result
    // could be mis-attributed while another async tool is pending.
    if (m_pendingAsyncInvocation.name != tool_names::kGrep)
        return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    ToolInvocationResult r;
    r.toolTag       = tool_names::kGrep;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = tool_approval::ToolIcon(inv.name);
    r.toolName      = tool_approval::ToolDisplayName(inv.name);
    r.commandEcho   = grepResult.commandEcho;
    r.chips         = grepResult.chips;
    r.body          = grepResult.body;
    r.errorBody     = grepResult.errorBody;
    r.bodyLang      = grepResult.bodyLang;

    return FinishAsyncToolResult(inv, r, m_cancelled || grepResult.cancelled);
}

// ─── powershell completion ──────────────────────────────────────
//
// Mirrors HandleGrepComplete.  The chip composition matches
// the slash /cmd renderer so the agent-rendered ToolBlock and the
// history round-trip carry the same status / elapsed / truncated
// markers a user-typed /cmd would.
//
// Cancel handling: if either the loop was cancelled OR the
// executor reports cancelled, render whatever output we have, feed
// it back to history (so the model's transcript reflects what
// actually happened), and end the loop.
bool AgentController::HandleCmdComplete(const CmdResult& cmdResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;

    // Defensive: only consume the event if THIS controller fired
    // the call.  Without this check, a /cmd run started before the
    // loop began (and still in flight) could be mis-attributed.
    if (m_pendingAsyncInvocation.name != tool_names::kPowerShell)
        return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    // Build chips identical to the slash /cmd result layout.
    std::vector<std::string> chips;
    if (cmdResult.cancelled) {
        chips.push_back("cancelled");
    } else if (cmdResult.timedOut) {
        chips.push_back("timed out");
    } else {
        std::ostringstream ec;
        ec << "exit " << cmdResult.exitCode;
        chips.push_back(ec.str());
    }
    {
        std::ostringstream ts;
        ts << std::fixed;
        ts.precision(cmdResult.elapsedSec < 10.0 ? 2 : 1);
        ts << cmdResult.elapsedSec << "s";
        chips.push_back(ts.str());
    }
    if (cmdResult.truncated) chips.push_back("truncated");

    ToolInvocationResult r;
    r.toolTag       = tool_names::kPowerShell;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = tool_approval::ToolIcon(inv.name);
    r.toolName      = tool_approval::ToolDisplayName(inv.name);
    r.commandEcho   = cmdResult.command;
    r.chips         = chips;
    r.body          = cmdResult.stdoutText;
    r.errorBody     = cmdResult.stderrText;
    r.bodyLang      = "powershell";
    r.presentedFiles = cmdResult.presentedFiles;

    return FinishAsyncToolResult(inv, r, m_cancelled || cmdResult.cancelled);
}

// ─── controlled Python helper completion ─────────────────────────
// Mirrors HandleCmdComplete, but for fixed Python helper tools.
bool AgentController::HandlePythonComplete(const PythonRunResult& pythonResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;

    if (!IsAgentPythonAsyncToolName(m_pendingAsyncInvocation.name))
        return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    std::vector<std::string> chips;
    if (pythonResult.cancelled) {
        chips.push_back("cancelled");
    } else if (pythonResult.timedOut) {
        chips.push_back("timed out");
    } else {
        std::ostringstream ec;
        ec << "exit " << pythonResult.exitCode;
        chips.push_back(ec.str());
    }
    {
        std::ostringstream ts;
        ts << std::fixed;
        ts.precision(pythonResult.elapsedSec < 10.0 ? 2 : 1);
        ts << pythonResult.elapsedSec << "s";
        chips.push_back(ts.str());
    }
    if (!pythonResult.pythonCommand.empty())
        chips.push_back(pythonResult.pythonCommand);
    if (pythonResult.truncated) chips.push_back("truncated");

    // Presentation comes from the spec's kPresentation row via the
    // router-backed tool_approval helpers.  This replaces the
    // 13-flag ternary ladder that hand-duplicated icon, display
    // name, and echo fallback per Python helper — the third copy of
    // that mapping in the codebase, and the one that drifted first.
    const bool isXlsxCreate = (inv.name == tool_names::kXlsxCreateWorkbook);
    const bool isRun        = (inv.name == tool_names::kPythonRunScript);
    const bool isInstall    = (inv.name == tool_names::kPythonInstallPackage);

    ToolInvocationResult r;
    r.toolTag       = inv.name;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = tool_approval::ToolIcon(inv.name);
    r.toolName      = tool_approval::ToolDisplayName(inv.name);
    // The helper's own echo wins; the fallback is the wire name,
    // which is exactly what the old per-tool fallback ladder spelled
    // out ("python_run_script", "csv_inspect", ...).
    r.commandEcho   = pythonResult.commandEcho.empty()
                        ? inv.name
                        : pythonResult.commandEcho;
    r.chips         = chips;
    r.body          = pythonResult.stdoutText;
    r.errorBody     = pythonResult.stderrText;
    r.bodyLang      = (isRun || isInstall) ? std::string() : std::string("json");
    r.presentedFiles = pythonResult.presentedFiles;
    if (isRun && pythonResult.exitCode == 0 && !pythonResult.cancelled && !pythonResult.timedOut) {
        ApplyAgentArtifactPresentation(r);
    }
    ApplyAgentMissingPythonPackageRecovery(r, pythonResult);

    if (m_cancelled || pythonResult.cancelled) {
        return FinishAsyncToolResult(inv, r, true);
    }

    // If workbook creation failed after the fixed helper had a chance to
    // validate/repair the spec, stop the turn instead of feeding the same
    // failing tool result back into the model. Local models commonly retry
    // the identical bad JSON, which looks like a loop to the user.
    if (isXlsxCreate &&
        m_queuedInvocations.empty() &&
        pythonResult.exitCode != 0 &&
        !pythonResult.cancelled &&
        !pythonResult.timedOut) {
        EmitAndStoreTerminalToolResult(r, true);
        // ToolFailedStop, not Normal: the loop is stopping BECAUSE a
        // tool failed terminally.  The agent-trace JSONL records the
        // end reason verbatim, and labeling failure stops "normal"
        // silently skews any analysis run over the traces.  Goal mode
        // also keys off this: a non-Normal reason skips the pointless
        // verification pass and keeps the goal active instead.
        EndLoop(AgentEndReason::ToolFailedStop,
                "I couldn't create the Excel workbook. Check the tool details above for the exact error.");
        return true;
    }

    // Artifact-producing create helpers are complete once the file card is
    // rendered. Do not feed a successful workbook creation back into the
    // model for another iteration, because smaller/local models may simply
    // repeat the same create call and generate duplicate files until the
    // loop guard stops them.
    //
    // Still end with a short deterministic completion message so the turn
    // feels finished even though we intentionally skip the model's final
    // prose pass.
    if (isXlsxCreate &&
        m_queuedInvocations.empty() &&
        pythonResult.exitCode == 0 &&
        !pythonResult.timedOut &&
        !pythonResult.presentedFiles.empty()) {
        EmitAndStoreTerminalToolResult(r, false);

        std::string workbookName = pythonResult.presentedFiles.front().displayName;

        std::string completionMessage;
        if (workbookName.empty()) {
            completionMessage = "I have created the Excel workbook.";
        } else {
            completionMessage = "I have created the " + workbookName + " Excel workbook.";
        }

        // Render the deterministic completion as a normal assistant reply,
        // not as a gray/italic system status line.  This keeps successful
        // xlsx_create_workbook turns visually consistent with CSV/write
        // completions while still avoiding another model pass that could
        // repeat the same create call.
        if (m_sink)
            m_sink->OnAgentEvent(AgentEvent::TurnComplete(completionMessage));

        EndLoop(AgentEndReason::Normal, "");
        return true;
    }

    return FinishAsyncToolResult(inv, r, false);
}


bool AgentController::HandleWebFetchComplete(const WebFetchResult& webResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;

    if (!IsAgentWebFetchAsyncToolName(m_pendingAsyncInvocation.name))
        return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    ToolInvocationResult r = MakeWebFetchToolResult(inv, webResult);

    return FinishAsyncToolResult(inv, r, m_cancelled || webResult.cancelled);
}

bool AgentController::HandleCmdError(const std::string& errorText)
{
    if (!m_active || !m_awaitingAsyncResult) return false;
    if (m_pendingAsyncInvocation.name != tool_names::kPowerShell) return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    // Phase 7d: a launch failure is a FAILED outcome for the loop-guard
    // records.  Without this, launch-failed attempts stay marked as
    // non-failed, a later success of the same call cannot purge them,
    // and a legitimate retry after the fix can trip the exact-repeat
    // guard (run(launch-fail) ×2 → fix → run(ok) → run again blocked).
    ResolveToolSignatureOutcome(inv, /*failed=*/true);

    ToolInvocationResult r = MakeAsyncLaunchErrorResult(inv, errorText);
    FeedResultAndIterate(r, /*countTowardIterationCap=*/false);
    return true;
}

bool AgentController::HandlePythonError(const std::string& errorText)
{
    if (!m_active || !m_awaitingAsyncResult) return false;
    if (!IsAgentPythonAsyncToolName(m_pendingAsyncInvocation.name)) return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    // Phase 7d: see HandleCmdError — launch failures must be marked
    // failed so a later success of the same call purges them.
    ResolveToolSignatureOutcome(inv, /*failed=*/true);

    ToolInvocationResult r = MakeAsyncLaunchErrorResult(inv, errorText);
    FeedResultAndIterate(r, /*countTowardIterationCap=*/false);
    return true;
}


bool AgentController::HandleWebFetchError(const std::string& errorText)
{
    if (!m_active || !m_awaitingAsyncResult) return false;
    if (!IsAgentWebFetchAsyncToolName(m_pendingAsyncInvocation.name)) return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_pendingAsyncContext = ToolContext{};

    // Phase 7d: see HandleCmdError — launch failures must be marked
    // failed so a later success of the same call purges them.
    ResolveToolSignatureOutcome(inv, /*failed=*/true);

    ToolInvocationResult r = MakeAsyncLaunchErrorResult(inv, errorText);
    FeedResultAndIterate(r, /*countTowardIterationCap=*/false);
    return true;
}

bool AgentController::HandleAssistantError(const std::string& /*errorText*/)
{
    if (!m_active) return false;
    // Errors always end the loop.  Let MyFrame's existing error
    // handler render the friendly message; we just reset state.
    EndLoop(AgentEndReason::StreamError, "");  // no extra message
    return false;
}
