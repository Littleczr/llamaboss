// tool_block.h
//
// Phase 5: Lifted out of ChatDisplay so non-UI components — the agent
// loop, the tool dispatcher, future P6 approval cards, future P9
// sub-agent forwarders — can construct and pass tool-block payloads
// without depending on chat_display.h (and transitively on wx).
//
// ChatDisplay still defines `using ToolBlock = ::ToolBlock;` for
// backward compatibility, so existing call sites that say
// `ChatDisplay::ToolBlock` keep compiling.  New code should use the
// global `ToolBlock` directly.
//
// Rendering is four-part (header, echo, body, optional errorBody);
// see chat_display.h's DisplayToolBlock comment for the semantic
// contract this struct fulfills.
//
#pragma once

#include <string>
#include <vector>

#include "presented_file.h"

struct ToolBlock {
    std::string              iconUtf8;      // e.g. "\xE2\x9A\x99" (⚙)
    std::string              toolName;      // e.g. "PowerShell", "Read"
    std::vector<std::string> statusChips;   // e.g. {"0.82s","exit 0"}
    std::string              commandEcho;   // shown after "> " prefix
    std::string              body;          // stdout / file contents / listing
    std::string              errorBody;     // stderr / failure detail
    std::string              bodyLang;      // reserved — Phase 3.x highlighting

    // Optional clickable file chips to render with the tool result.
    // Used for files that already exist on disk after a tool succeeds
    // (e.g. /write hello.cpp). ChatDisplay presents these via the
    // same PresentFile() path used for model-generated code blocks.
    std::vector<PresentedFile> presentedFiles;
};
