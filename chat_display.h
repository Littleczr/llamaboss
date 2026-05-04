// chat_display.h
#pragma once

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/timer.h>
#include <string>
#include <vector>
#include <memory>

#include "presented_file.h"
#include "tool_block.h"

// Forward declarations
class MarkdownRenderer;
struct ThemeData;
struct ColoredChar;

// Manages the display of chat messages in a wxRichTextCtrl,
// handling different roles (user, assistant, system) and formats,
// including distinguishing between an AI's "thought" process and its final answer.
// Assistant responses are rendered with markdown formatting via MarkdownRenderer.
class ChatDisplay
{
public:
    ChatDisplay(wxRichTextCtrl* displayCtrl);
    ~ChatDisplay();

    // Display different types of messages
    // target: if non-empty, shows "You → target:" instead of "You:"
    // inlineImages: absolute file paths of images to show as thumbnails
    void DisplayUserMessage(const std::string& text,
                            const std::string& target = "",
                            const std::vector<std::string>& inlineImages = {});
    void DisplaySystemMessage(const std::string& text);

    // ── Tool-result rendering ────────────────────────────────────
    // A ToolBlock is the generic payload for any slash-command or
    // harness-driven tool invocation.  Rendering is four-part:
    //
    //   "<icon> <toolName>  ·  <chip>  ·  <chip>  ..."   <- header (bold)
    //   "> <commandEcho>"                                <- echo (muted)
    //   <body>                                           <- primary output
    //   <errorBody>                                      <- optional, red
    //
    // All text is monospace.  Callers are responsible for packing
    // tool-specific metadata into statusChips (e.g. "exit 0", "42 KB",
    // "3 matches").  `bodyLang` is reserved for future syntax
    // highlighting; empty means "plain".
    //
    // Phase 5: the struct itself moved to tool_block.h so non-UI
    // components (AgentController, tool dispatchers, future P6/P9
    // pieces) can build payloads without dragging in wx.  The alias
    // here keeps existing `ChatDisplay::ToolBlock` callers compiling.
    using ToolBlock = ::ToolBlock;

    void DisplayToolBlock(const ToolBlock& block, bool startExpanded = false);

    // Temporarily disables [details] toggles while the assistant/tool loop
    // is still appending text.  This prevents wxRichTextCtrl range mutation
    // from corrupting live tool output. File actions stay clickable.
    void SetToolBlockInteractionEnabled(bool enabled);

    void DisplayAssistantPrefix(const std::string& modelName);
    void DisplayAssistantPrefix(const std::string& modelName, const wxColour& accentColor);
    void DisplayAssistantDelta(const std::string& delta);
    void DisplayAssistantComplete();
    void CancelPendingAssistantDisplay();
    void DisplayAssistantMessage(const std::string& modelName,
        const std::string& content,
        const wxColour& accentColor);

    // ── File presentation ────────────────────────────────────────
    // Drops a clickable file chip into the chat at the current insertion
    // point.  Any producer can call this — MarkdownRenderer wires its
    // code-block callback to us automatically; future tool handlers
    // (e.g. a PowerShell tool) can call it directly after writing a
    // file to disk.  Click opens Save As.
    void PresentFile(const PresentedFile& file);

    // Persistence context: while set, any PresentFile() call with
    // inlineContent also writes the bytes to
    //   {absDir}/{msgIdx}_{chipIdx}_{displayName}
    // and records the resulting path on the chip.  Set by the frame
    // just before streaming begins; cleared on complete/error/stop.
    // When unset, chips render in-memory only.
    void SetFilePersistenceContext(const std::string& absDir, size_t msgIdx);
    void ClearFilePersistenceContext();

    // ── ASCII Animation support ──────────────────────────────────
    void BeginAnimationFrame();
    void WriteAnimationLine(const std::vector<ColoredChar>& line);
    void EndAnimationFrame();
    void ClearAnimation();
    bool IsAnimating() const { return m_animActive; }

    // Utility methods
    void Clear();
    void ScrollToBottom();

    // Configuration methods for customizing appearance
    void SetUserColor(const wxColour& color);
    void SetAssistantColor(const wxColour& color);
    void SetSystemColor(const wxColour& color);
    void SetThoughtColor(const wxColour& color);
    void SetFont(const wxFont& font);

    // Apply all colors from a ThemeData
    void ApplyTheme(const ThemeData& theme);

private:
    wxRichTextCtrl* m_displayCtrl;
    std::unique_ptr<MarkdownRenderer> m_markdownRenderer;

    // Colors for different message types
    wxColour m_userColor;
    wxColour m_assistantColor;
    wxColour m_systemColor;
    wxColour m_thoughtColor;
    wxColour m_stdoutColor;             // Body text for /cmd stdout and future tool blocks

    // State tracking for assistant messages
    bool m_isInThoughtBlock;            // True if we are currently printing thought text
    bool m_isFirstAssistantDelta;       // True while probing for <think> at message start
    bool m_hasRenderedAssistantContent; // True once visible assistant content has been rendered
    wxColour m_activeAssistantColor;    // Color used for the current streaming response
    std::string m_thinkProbeBuffer;     // Accumulates first few bytes to detect <think> across deltas
    std::string m_thinkEndProbeBuffer;  // Holds last 7 chars of thought text to detect </think> across deltas
    long m_currentAssistantStartPos = -1;  // Start of currently streaming assistant prefix

    // ── Animation state ──────────────────────────────────────────
    long m_animStartPos = -1;           // Char position where animation frame begins
    bool m_animActive   = false;

    // ── Thinking indicator state ─────────────────────────────────
    // Shows animated dots (. -> .. -> ...) in the thought color after the
    // assistant prefix while waiting for visible tokens to arrive.  Covers
    // the time-to-first-token gap on MoE/reasoning models and the probe
    // window while we're buffering bytes to detect a <think> tag.
    class ThinkingTimer : public wxTimer {
    public:
        explicit ThinkingTimer(ChatDisplay* owner) : m_owner(owner) {}
        void Notify() override;
    private:
        ChatDisplay* m_owner;
    };
    std::unique_ptr<ThinkingTimer> m_thinkingTimer;
    long m_thinkingDotsStartPos = -1;   // Char pos where dots begin (right after prefix)
    long m_thinkingDotsEndPos   = -1;   // Char pos where dots end (for Remove range)
    int  m_thinkingDotsFrame    = 0;    // Current animation frame: 0,1,2 -> 1,2,3 dots
    bool m_thinkingActive       = false;

    void StartThinkingIndicator();
    void ClearThinkingIndicator();
    void OnThinkingTick();

    // ── File card action registry ────────────────────────────────
    // Each clickable action inside an artifact:file card registers a
    // character range plus the PresentedFile it acts on.
    enum class FileAction {
        SaveAs,
        Open,
        OpenFolder
    };

    struct FileChipRegion {
        long          startPos;
        long          endPos;
        PresentedFile file;   // full copy — survives even if the source is freed
        FileAction    action = FileAction::SaveAs;
    };
    std::vector<FileChipRegion> m_fileChips;
    wxColour                    m_fileChipColor = wxColour(170, 190, 230);  // soft blue

    // Persistence context — set by the frame around streaming.
    std::string m_filePersistenceDir;        // Empty = persistence off
    size_t      m_filePersistenceMsgIdx = 0;
    size_t      m_filePersistenceChipSeq = 0; // Per-message chip counter

    int  HitTestFileChip(long pos) const;     // returns action index or -1
    void HandleFileChipClick(size_t chipIdx);

    // ── Tool block registry ──────────────────────────────────────
    // Each tool block emitted by DisplayToolBlock registers two text
    // ranges: the body (stdout + stderr) and the "[hide details]" /
    // "[show details]" affordance.  Click on the affordance toggles
    // the body's visibility by Remove()ing or re-inserting the body
    // text and swapping the affordance label.  Stashed body/errorBody
    // strings let us re-render on expand without re-running the tool.
    //
    // Lives parallel to m_fileChips; same hit-test priority chain in
    // OnLeftUp dispatches to whichever region is hit.
    struct ToolBlockRegion {
        long affordanceStart;   // inclusive — first char of bracketed label
        long affordanceEnd;     // exclusive — one past last char
        long bodyStart;         // inclusive — start of body+errorBody region
        long bodyEnd;           // exclusive — end of region (== bodyStart when collapsed)
        std::string body;       // stashed for re-render on expand
        std::string errorBody;  // stashed for re-render on expand
        bool        expanded;   // current visibility state
    };
    std::vector<ToolBlockRegion> m_toolBlocks;
    bool m_toolBlockInteractionEnabled = true;

    int  HitTestToolBlockAffordance(long pos) const;  // -1 if no hit
    void HandleToolBlockAffordanceClick(size_t idx);

    // Auto-expand classifier for Phase C.  Returns true if the block
    // looks like a failure (non-empty errorBody, or any chip indicating
    // policy denial / cancellation / timeout / non-zero exit / generic
    // "error").  Used by DisplayToolBlock to decide whether to start
    // expanded when the caller didn't explicitly request it.
    static bool IsToolBlockFailure(const ToolBlock& block);

    // Writes body + errorBody at the current insertion point with the
    // standard tool-block styling (Consolas, stdout color for body,
    // red for errorBody, trailing \n if missing).  Returns the number
    // of chars written so callers can derive the (start, end) range.
    long WriteToolBodyAtCursor(const std::string& body,
                               const std::string& errorBody);

    // Swaps the details affordance between "[show details]" and
    // "[hide details]". Those labels intentionally have the same length so
    // the replacement does not shift any registered rich-text ranges.
    void SetAffordanceText(ToolBlockRegion& r, const wxString& newText);

    // Shifts all region positions >= pivot by delta, EXCEPT positions
    // belonging to *skip (which the caller updates manually).  Used
    // around toggle so the current region's own position update isn't
    // double-counted.  Pass nullptr to shift everything.
    void ShiftOtherRegions(const ToolBlockRegion* skip,
                           long pivot, long delta);

    // Helper methods for formatting
    void AppendFormattedText(const std::string& text, const wxColour& color,
        bool bold = false, bool italic = false);
    void SetInsertionPointToEnd();
    void EnsureVisibleAtEnd();

    // Image thumbnail limits for inline display
    static constexpr int kImageMaxWidth  = 300;
    static constexpr int kImageMaxHeight = 300;
};

