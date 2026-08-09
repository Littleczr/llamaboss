#include "lb_input_parsers.h"

#include <string_view>

#include "tool_invocation.h"

namespace lb_input_parsers {
namespace {

bool IsAsciiWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string TrimAsciiWhitespace(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();

    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string NormalizeApprovalForMatch(const std::string& s)
{
    const std::string trimmed = TrimAsciiWhitespace(s);

    std::string out;
    out.reserve(trimmed.size());

    bool inWs = false;
    for (char c : trimmed) {
        char lc = (c >= 'A' && c <= 'Z')
                  ? static_cast<char>(c - 'A' + 'a')
                  : c;

        if (IsAsciiWhitespace(lc)) {
            if (!out.empty() && !inWs) out.push_back(' ');
            inWs = true;
        } else {
            out.push_back(lc);
            inWs = false;
        }
    }

    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

struct SlashEntry {
    std::string_view prefix;
    const char*      toolName;
};

// Table order matters for commands where one verb is a prefix of
// another, e.g. /read_head before /read.
static const SlashEntry kToolSlashTable[] = {
    { "/read_head", tool_names::kReadHead   },
    { "/read",      tool_names::kRead       },
    { "/ls",        tool_names::kLs         },
    { "/grep",      tool_names::kGrep       },
    { "/pwd",       tool_names::kPwd        },
    { "/open",      tool_names::kOpen       },
    { "/cmd",       tool_names::kPowerShell },
    { "/python_health",          tool_names::kPythonHealth },
    { "/csv_inspect",            tool_names::kCsvInspect   },
    { "/csv_report",             tool_names::kCsvReport    },
    { "/csv_to_xlsx",            tool_names::kCsvToXlsx    },
    { "/xlsx_inspect",           tool_names::kXlsxInspect  },
    { "/xlsx_report",            tool_names::kXlsxReport   },
    { "/xlsx_create_workbook",   tool_names::kXlsxCreateWorkbook },
    { "/pdf_extract_text",       tool_names::kPdfExtractText },
    { "/pdf_inspect_form",       tool_names::kPdfInspectForm },
    { "/pdf_fill_form",          tool_names::kPdfFillForm    },
    { "/python_create_script",   tool_names::kPythonCreateScript },
    { "/python_run_script",      tool_names::kPythonRunScript    },
    { "/python_install_package", tool_names::kPythonInstallPackage },
    { "/web_fetch_url",          tool_names::kWebFetchUrl },
    { "/notes_read",             tool_names::kNotesRead },
    { "/notes_append",           tool_names::kNotesAppend },
    { "/reminder_create",        tool_names::kReminderCreate },
    { "/reminder_list",          tool_names::kReminderList },
    { "/reminder_cancel",        tool_names::kReminderCancel },
    { "/project_notes_read",     tool_names::kProjectNotesRead },
    { "/project_notes_append",   tool_names::kProjectNotesAppend },

    // Mutating tools.  These stay in the slash parser so MyFrame only
    // has to ask whether an input is a recognized tool command.  The
    // actual sandbox and approval checks remain in the normal tool path.
    { "/overwrite_file", tool_names::kOverwriteFile },
    { "/write",          tool_names::kWrite      },
    { "/mkdir",          tool_names::kMkdir      },
    { "/edit",           tool_names::kEdit       },
    { "/delete",         tool_names::kDelete     },
};

} // namespace

ApprovalInputAction ParseApprovalInput(const std::string& userInput)
{
    const std::string normalized = NormalizeApprovalForMatch(userInput);

    if (normalized == "/approve once" ||
        normalized == "approve once" ||
        normalized == "allow once" ||
        normalized == "just once") {
        return ApprovalInputAction::ApproveOnce;
    }

    if (normalized == "/approve" ||
        normalized == "approve" ||
        normalized == "allow" ||
        normalized == "run it" ||
        normalized == "go ahead" ||
        normalized == "/approve always" ||
        normalized == "/approve all" ||
        normalized == "/approve chat" ||
        normalized == "/trust chat" ||
        normalized == "approve always" ||
        normalized == "approve all" ||
        normalized == "approve chat" ||
        normalized == "approve conversation" ||
        normalized == "allow always" ||
        normalized == "trust chat") {
        return ApprovalInputAction::ApproveAlways;
    }

    if (normalized == "/deny" ||
        normalized == "deny" ||
        normalized == "cancel" ||
        normalized == "no") {
        return ApprovalInputAction::Deny;
    }

    return ApprovalInputAction::Unrecognized;
}

SlashCommandParseResult TryParseToolSlashCommand(const std::string& userInput)
{
    SlashCommandParseResult result;

    if (userInput.empty() || userInput[0] != '/') return result;

    for (const SlashEntry& e : kToolSlashTable) {
        const size_t plen = e.prefix.size();

        if (userInput.size() < plen) continue;
        if (userInput.compare(0, plen, e.prefix.data(), plen) != 0) continue;

        // Require whitespace or EOS after the verb so "/lsfoo" falls
        // through as normal chat instead of being parsed as "/ls foo".
        if (userInput.size() != plen && !IsAsciiWhitespace(userInput[plen])) {
            continue;
        }

        result.matched = true;
        result.toolName = e.toolName;

        if (userInput.size() > plen) {
            // Preserve existing behavior: the caller already proved the
            // next byte is a whitespace delimiter, and the old parser
            // skipped exactly one delimiter before trimming both ends.
            result.args = userInput.substr(plen + 1);
            result.args = TrimAsciiWhitespace(result.args);
        }

        return result;
    }

    return result;
}

} // namespace lb_input_parsers
