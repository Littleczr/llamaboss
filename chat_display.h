// chat_display.h
#pragma once

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/timer.h>
#include <chrono>
#include <string>
#include <vector>
#include <memory>
#include <functional>

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
    // target: if non-empty, shows "YOU → target" above the bubble text
    // inlineImages: absolute file paths of images to show as thumbnails
    void DisplayUserMessage(const std::string& text,
                            const std::string& target = "",
                            const std::vector<std::string>& inlineImages = {});
    void DisplaySystemMessage(const std::string& text);

    // Informational notice that must NOT disturb live work (e.g. "Can't
    // switch models while a response is streaming").  DisplaySystemMessage
    // tears down the live pending-tool card and thinking dots because it
    // doubles as the terminal message on error paths; that also ended the
    // ActivityStrip mid-run, so a PowerShell command looked stopped when it
    // was still running.  This variant writes the notice ABOVE any live
    // indicator (the pending tool card, or the assistant prefix + thinking
    // dots) and leaves them running.  With no live indicator, or while
    // assistant text is actively streaming, it behaves exactly like
    // DisplaySystemMessage.
    void DisplaySystemNotice(const std::string& text);

    // Render image thumbnails at the chat tail, outside any message
    // prefix.  Used for assistant-generated images (image-output
    // models): after DisplayAssistantComplete on the live path, and
    // after DisplayAssistantMessage on conversation replay.  Absolute
    // file paths; unloadable/corrupt entries are skipped silently —
    // a deleted chat folder must not break conversation replay.
    // Scaling matches DisplayUserMessage's thumbnail rules.
    void DisplayInlineImages(const std::vector<std::string>& imagePaths);

private:
    // ── Image thumbnail context menu ─────────────────────────────
    // WriteImage embeds a bitmap with no memory of its source file,
    // so every thumbnail we write is tagged with its absolute source
    // path via wxRichTextProperties on the wxRichTextImage object
    // itself.  Properties travel with the object, so later
    // insertions shifting buffer positions can't orphan the mapping
    // — no positional registry to maintain.  The context-menu
    // handler hit-tests the click, reads the tag, and offers
    // "Save image as..." / "Show in folder".  Untagged positions
    // Skip() through to the control's default menu.
    void TagLastWrittenImage(const std::string& absPath);
    void OnImageContextMenu(wxContextMenuEvent& event);
    void SaveImageAs(const wxString& srcPath);
    void ShowInFolder(const wxString& path);

    // Resolves the tagged source path of the thumbnail at a buffer
    // position ("" when the position is not one of our thumbnails).
    // Shared by the context menu and the left-click viewer so both
    // use the same off-by-one-tolerant leaf lookup.
    wxString ImageSrcAtPosition(long pos) const;

    // Left-click lightbox: shows the full-resolution image scaled to
    // fit the frame, over the modal scrim.  Dismissed by click,
    // Escape, Enter, or Space.
    void ShowImageViewer(const wxString& srcPath);

public:

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

    // ── Approval buttons (Phase 6 UX) ────────────────────────────
    // When a ToolBlock with requiresApproval=true is rendered,
    // DisplayToolBlock writes a clickable button row beneath
    // [show details]:
    //
    //   [ Allow Once ]   [ Allow Always ]   [ Deny ]
    //
    // Folder-access cards instead render:
    //   [ Grant Folder for Chat ]   [ Cancel ]
    //
    // Each label is a separately-registered click region styled the
    // same as the [show details] affordance (italic Consolas, soft
    // blue).  Clicks dispatch through the registered callback to the
    // frame, which routes to HandleApprovalCommand using the existing
    // chat-scoped approval semantics:
    //
    //   Once   -> HandleApprovalCommand(true,  /*rememberForChat=*/false)
    //   Always -> HandleApprovalCommand(true,  /*rememberForChat=*/true)
    //   Deny   -> HandleApprovalCommand(false)
    //
    // ClearApprovalButtons removes the visual row.  The frame calls it
    // when an approval is resolved via the typed-command fallback in
    // TryHandlePendingApprovalInput, so the row vanishes regardless of
    // which path resolved it.  HandleApprovalButtonClick also calls it
    // before invoking the callback, so a click resolves itself
    // visually before the new tool output (or denial system message)
    // starts streaming.
    enum class ApprovalChoice { Once, Always, Deny };
    void SetApprovalCallback(std::function<void(ApprovalChoice)> callback);

    // Live progress for a pending async tool (see private section).
    void SetPendingToolCallbacks(std::function<void(const ToolBlock&)> onStarted,
                                 std::function<void()>                 onEnded);
    void ClearApprovalButtons();

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

    // Long saved conversations can be expensive to replay into wxRichTextCtrl
    // because each rendered message normally scrolls/repaints the control.
    // Batch replay freezes the transcript and defers the final scroll until the
    // whole conversation has been rebuilt.
    void BeginReplayBatch();
    void EndReplayBatch();
    bool IsReplayBatchActive() const { return m_replayBatchDepth > 0; }

    // Configuration methods for customizing appearance
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

    // Replay batching suppresses repeated scroll/repaint work while a saved
    // conversation is being rebuilt into the transcript control.
    int  m_replayBatchDepth = 0;
    bool m_replayBatchFrozen = false;
    bool m_replayBatchNeedsScroll = false;

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
    // Shows a braille spinner (U+280B..U+280F) in the thought color after the
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
    int  m_thinkingDotsFrame    = 0;    // Current spinner frame index (0..9)
    bool m_thinkingActive       = false;

    void StartThinkingIndicator();
    void ClearThinkingIndicator();
    void OnThinkingTick();

    // ── Live async-tool progress ────────────────────────────────
    // The pending card in the transcript is static and UI-only; it is
    // removed before the terminal tool card is rendered.  The *live*
    // part (elapsed clock, output tail, gauge) is no longer drawn inside
    // the rich text control — it is delegated to the owner via the
    // callbacks below (MyFrame wires them to ActivityStrip).  Drawing it
    // here re-laid-out the tail paragraph every second and fought the
    // user's scroll position for the whole duration of a long command.
    long m_pendingCardStartPos     = -1;
    long m_pendingCardEndPos       = -1;
    bool m_pendingProgressActive   = false;

    std::function<void(const ToolBlock&)> m_onPendingToolStarted;
    std::function<void()>                 m_onPendingToolEnded;

    void StartPendingToolProgress(const ToolBlock& block,
                                  long pendingCardStart,
                                  long pendingCardEnd);
    void ClearPendingToolProgress();

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

    // True once any tagged image thumbnail exists in the transcript.
    // Consulted by the wxEVT_MOTION fast path: a transcript with no
    // interactive ranges skips HitTest entirely, and thumbnails are
    // an interactive range too (hover shows the hand cursor, click
    // opens the lightbox viewer).  Set by TagLastWrittenImage, reset
    // by Clear().  Never set back to false while the document lives —
    // images aren't individually removed, so no per-image tracking.
    bool m_hasImageThumbnails = false;
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
        long chevronStart = -1; // optional command-echo chevron toggle
        long chevronEnd   = -1; // exclusive — one past chevron char
        long bodyStart;         // inclusive — start of body+errorBody region
        long bodyEnd;           // exclusive — end of region (== bodyStart when collapsed)
        std::string body;       // stashed for re-render on expand
        std::string errorBody;  // stashed for re-render on expand
        bool        expanded;   // current visibility state
        bool        errorIsFailure = true; // red stderr only when the call failed
    };
    std::vector<ToolBlockRegion> m_toolBlocks;
    bool m_toolBlockInteractionEnabled = true;

    int  HitTestToolBlockAffordance(long pos) const;  // -1 if no hit
    void HandleToolBlockAffordanceClick(size_t idx);

    // ── Approval button registry ─────────────────────────────────
    // Parallel to m_fileChips / m_toolBlocks.  At most three entries
    // (Once/Always/Deny) populated by DisplayToolBlock when the block
    // has requiresApproval=true; cleared by ClearApprovalButtons (called
    // either directly by HandleApprovalButtonClick or by the frame from
    // the typed-command fallback path).  m_approvalRowStart/End track
    // the full row bounds — including the trailing newline — so the
    // entire row can be removed as one Remove() call.
    struct ApprovalButtonRegion {
        long startPos;
        long endPos;
        ApprovalChoice choice;
    };
    std::vector<ApprovalButtonRegion> m_approvalButtons;
    long m_approvalRowStart = -1;
    long m_approvalRowEnd   = -1;
    std::function<void(ApprovalChoice)> m_approvalCallback;

    int  HitTestApprovalButton(long pos) const;       // -1 if no hit
    void HandleApprovalButtonClick(size_t idx);

    // Auto-expand classifier for Phase C.  Returns true if the block
    // looks like a failure (non-empty errorBody, or any chip indicating
    // policy denial / cancellation / timeout / non-zero exit / generic
    // "error").  Used by DisplayToolBlock to decide whether to start
    // expanded when the caller didn't explicitly request it.
    static bool IsToolBlockFailure(const ToolBlock& block);

    // Writes body + errorBody at the current insertion point with the
    // standard tool-block styling (Consolas, stdout color for body,
    // red errorBody on failure / muted on exit 0, trailing \n if missing).  Returns the number
    // of chars written so callers can derive the (start, end) range.
    long WriteToolBodyAtCursor(const std::string& body,
                               const std::string& errorBody,
                               bool errorIsFailure = true);
    // stderr from a call that otherwise succeeded ("exit 0" chip, no
    // blocked/cancelled/timeout/error chip) is informational: progress
    // remnants, native-tool log lines.  Rendered muted, not red.
    static bool IsStderrFailure(const std::vector<std::string>& chips);

    // Swaps the details affordance between "[show details]" and
    // "[hide details]". Those labels intentionally have the same length so
    // the replacement does not shift any registered rich-text ranges.
    void SetAffordanceText(ToolBlockRegion& r, const wxString& newText);

    // Swaps the optional command-echo chevron between collapsed ">" and
    // expanded "▾". Both are one displayed character, so ranges stay stable.
    void SetChevronText(ToolBlockRegion& r, const wxString& newText);

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

    // ── Sticky autoscroll (follow mode) ───────────────────────────
    // While the user is at (or near) the bottom, streamed content
    // auto-scrolls to stay visible. Wherever the user's navigation landed
    // (wheel, scrollbar, Page Up/Down, Home/End, arrows, middle-click pan,
    // or drag-selection autoscroll) re-evaluates follow: moving up
    // disengages it so the user can read while the model keeps typing
    // below; returning to the bottom — or any deliberate jump (sending a
    // message, replay end, Clear) — re-engages it. Mirrors
    // Claude/ChatGPT streaming behavior.
    //
    // EnsureVisibleAtEnd() stays the FORCED jump (and re-engages
    // follow); EnsureVisibleAtEndIfFollowing() is the gated variant
    // used by streaming-driven call sites (deltas, tool blocks,
    // thinking indicator, stream completion, animation frames).
    static constexpr int kFollowSlackPx = 4;
    bool IsNearBottom() const;
    void UpdateFollowFromScrollPosition();
    void EnsureVisibleAtEndIfFollowing();
    bool m_followStream = true;

    // Expiry token for the deferred (CallAfter) scroll-position
    // checks: destroyed with this object, so a check that fires after
    // this ChatDisplay is gone no-ops instead of touching freed state.
    std::shared_ptr<int> m_followCheckAlive = std::make_shared<int>(0);

    // Image thumbnail limits for inline display.
    // Bumped from 300 — at modern resolutions (1440p ultrawide and up)
    // 300px thumbnails read as postage stamps.
    static constexpr int kImageMaxWidth  = 440;
    static constexpr int kImageMaxHeight = 440;
};
