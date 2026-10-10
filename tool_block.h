// tool_block.h
//
// Tool-block payload, kept out of ChatDisplay so non-UI components —
// the agent loop, the tool dispatcher, approval cards — can construct
// and pass tool-block payloads without depending on chat_display.h
// (and transitively on wx).
//
// ChatDisplay defines `using ToolBlock = ::ToolBlock;` so call sites
// that say `ChatDisplay::ToolBlock` keep compiling.  New code should
// use the global `ToolBlock` directly.
//
// Rendering is four-part (header, echo, body, optional errorBody);
// see chat_display.h's DisplayToolBlock comment for the semantic
// contract this struct fulfills.
//
#pragma once

#include <string>
#include <vector>

#include "presented_file.h"

enum class ToolApprovalPresentation {
    ToolAction,
    WriteRootGrant,
};

struct ToolBlock {
    std::string              iconUtf8;      // e.g. "\xE2\x9A\x99" (⚙)
    std::string              toolName;      // e.g. "PowerShell", "Read"
    std::vector<std::string> statusChips;   // e.g. {"0.82s","exit 0"}
    std::string              commandEcho;   // shown after "> " prefix
    std::string              body;          // stdout / file contents / listing
    std::string              errorBody;     // stderr / failure detail
    std::string              bodyLang;      // reserved for syntax highlighting

    // Optional clickable file chips to render with the tool result.
    // Used for files that already exist on disk after a tool succeeds
    // (e.g. /write hello.cpp). ChatDisplay presents these via the
    // same PresentFile() path used for model-generated code blocks.
    std::vector<PresentedFile> presentedFiles;

    // When true, ChatDisplay renders an inline button row
    // "[ Allow Once ]   [ Allow Always ]   [ Deny ]" beneath the
    // [show details] affordance.  Clicks dispatch through
    // ChatDisplay::SetApprovalCallback back to the frame, which routes
    // to HandleApprovalCommand with the appropriate (approve,
    // rememberForChat) pair.  Set by OnAgentApprovalRequired and the
    // slash-command approval gate; left false for normal tool result
    // rendering.  The typed fallback in TryHandlePendingApprovalInput
    // remains as a keyboard safety net.
    bool requiresApproval = false;

    // Folder grants use a purpose-built two-button row instead of the
    // generic Allow Once / Allow Always choices.  The first choice still
    // maps to ApprovalChoice::Always internally, but AgentController/MyFrame
    // interpret it as "grant this exact root for the current chat" rather
    // than enabling general tool trust.
    ToolApprovalPresentation approvalPresentation =
        ToolApprovalPresentation::ToolAction;

    // ── Live progress metadata ───────────────────────────────────
    // Set ONLY by AgentController::EmitPendingToolBlock.  isPending
    // marks this card as the start-of-async acknowledgement (worker
    // dispatch or wait), telling ChatDisplay to arm its live progress
    // line right after rendering it. A terminal
    // result block leaves these at their defaults, so rendering it
    // both removes the line (DisplayToolBlock clears defensively)
    // and does not restart it.
    //
    // pendingWaitTotalSec > 0 selects countdown mode for the wait tool;
    // 0 selects generic elapsed mode. pendingTimeoutSec is an optional
    // hard deadline shown for executors such as PowerShell.
    // pendingWaitReason is the model's optional human-readable wait label.
    // UI-only — none of this is serialized into chat history.
    bool        isPending          = false;
    int         pendingWaitTotalSec = 0;
    int         pendingTimeoutSec   = 0;
    std::string pendingWaitReason;
};
