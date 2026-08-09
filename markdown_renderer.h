// markdown_renderer.h
// Streaming markdown renderer for wxRichTextCtrl.
// Parses incoming deltas line-by-line, rendering complete lines with full
// markdown formatting (bold, italic, code, headings, lists, code blocks)
// while showing partial lines as plain text until they complete.

#pragma once

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <string>
#include <functional>

#include "presented_file.h"

class MarkdownRenderer
{
public:
    MarkdownRenderer(wxRichTextCtrl* ctrl);
    ~MarkdownRenderer() = default;

    // ── Streaming interface ───────────────────────────────────────
    // Call ProcessDelta() for each chunk of text as it arrives.
    // Call Flush() once when the message is complete.
    // Call Reset() before starting a new message.

    void ProcessDelta(const std::string& delta, const wxColour& baseColor);
    void Flush(const wxColour& baseColor);
    void Reset();

    // ── Bulk / replay mode ────────────────────────────────────────
    // When enabled, ProcessDelta() skips two things meant for live
    // streaming: the partial-line preview (the caller is feeding whole
    // messages followed by Flush(), so the preview is created and
    // immediately consumed) and the per-call ShowPosition() scroll.
    //
    // The ShowPosition() suppression is the important one: it forces a
    // wxRichTextCtrl layout pass whose cost scales with the current
    // document size, so calling it once per message while replaying a
    // saved conversation turns an n-message replay into O(n^2) work.
    // ChatDisplay enables bulk mode for the duration of a replay batch
    // (BeginReplayBatch/EndReplayBatch) and performs a single
    // scroll-to-end once the whole transcript is rebuilt.
    void SetBulkMode(bool enabled) { m_bulkMode = enabled; }

    // ── Color configuration ──────────────────────────────────────
    void SetCodeColor(const wxColour& color)           { m_codeColor = color; }
    void SetHeadingColor(const wxColour& color)         { m_headingColor = color; }
    void SetCodeLabelColor(const wxColour& color)       { m_codeLabelColor = color; }
    void SetHorizontalRuleColor(const wxColour& color)  { m_horizontalRuleColor = color; }

    // ── Code block copy support ───────────────────────────────────
    struct CopyLink {
        long startPos;       // Character position where "📋 Copy" starts
        long endPos;         // Character position where it ends
        size_t blockIndex;   // Index into m_codeBlocks
    };
    size_t GetCodeBlockCount() const { return m_codeBlocks.size(); }
    const std::string& GetCodeBlock(size_t index) const;
    void ClearCodeBlocks();
    // Returns block index if pos is inside a [Copy] link, or -1
    int HitTestCopyLink(long pos) const;
    // True when any [Copy] links exist at all.  Lets ChatDisplay's
    // mouse-motion handler skip its hit-test work entirely for
    // transcripts with no interactive ranges (see the fast path there).
    bool HasCopyLinks() const { return !m_copyLinks.empty(); }
    // Changes the clicked code-block affordance from "Copy" to "Copied".
    // The rendered affordance reserves six monospace cells ("Copy  "), so
    // this replacement does not shift any later rich-text ranges.
    bool MarkCopyLinkCopied(size_t blockIndex);

    // ── Position maintenance ──────────────────────────────────────
    // m_copyLinks holds ABSOLUTE offsets into the shared
    // wxRichTextCtrl document.  Any mutation elsewhere in that
    // document (tool-block expand/collapse, approval-row removal,
    // cancelled-turn pruning) invalidates them, so ChatDisplay must
    // drive these two the same way it drives ShiftOtherRegions.
    //
    // ShiftCopyLinks: every link at or past |pivot| moves by |delta|.
    // PruneCopyLinksFrom: drop links starting at or past |pivot|,
    // for ranges about to be removed from the document entirely.
    // Their m_codeBlocks slots are deliberately left orphaned rather
    // than reindexed -- same policy as Reset().
    void ShiftCopyLinks(long pivot, long delta);
    void PruneCopyLinksFrom(long pivot);

    // ── File chip callback ────────────────────────────────────────
    // When a fenced code block closes, the renderer packages it into a
    // PresentedFile and invokes this callback.  ChatDisplay wires it to
    // its own PresentFile() method so the same chip-rendering path is
    // shared with future producers (PowerShell tool, etc.).
    using FileCallback = std::function<void(const PresentedFile&)>;
    void SetFileCallback(FileCallback cb) { m_fileCallback = std::move(cb); }

    // ── Sticky autoscroll ─────────────────────────────────────────
    // Optional predicate consulted before every live-streaming
    // ShowPosition() scroll.  ChatDisplay wires this to its follow-mode
    // flag so a user who has scrolled up to read is not yanked back to
    // the bottom on every delta.  Unset (default) preserves the old
    // always-scroll behavior.  Bulk/replay mode skips these scrolls
    // entirely and never consults the predicate.
    void SetAutoScrollPredicate(std::function<bool()> pred)
    { m_autoScroll = std::move(pred); }

private:
    wxRichTextCtrl* m_ctrl;

    // ── Streaming state ──────────────────────────────────────────
    std::string m_lineBuffer;          // Accumulated text not yet rendered
    bool        m_inCodeBlock;         // Currently inside a ``` fenced block
    std::string m_codeBlockLang;       // Language tag from opening fence (just "cpp", etc.)
    std::string m_codeBlockFilename;   // Filename parsed from opening fence (may be empty)
    long        m_partialLineStart;    // Character position where partial line begins (-1 = none)
    // How many bytes of m_lineBuffer are currently drawn as the preview.
    // Invariant: when m_partialLineStart >= 0, the on-screen preview is
    // exactly m_lineBuffer.substr(0, m_partialLineRenderedLen).  Lets
    // ProcessDelta append just the new suffix instead of deleting and
    // rewriting the whole preview on every token.
    size_t      m_partialLineRenderedLen;
    bool        m_bulkMode;            // Replay mode: skip partial-line preview + per-call scroll

    // ── Colors ───────────────────────────────────────────────────
    wxColour m_codeColor;
    wxColour m_headingColor;
    wxColour m_codeLabelColor;
    wxColour m_horizontalRuleColor;

    // ── Code block copy tracking ──────────────────────────────────
    std::vector<std::string>  m_codeBlocks;         // Completed code block contents
    std::vector<CopyLink>     m_copyLinks;          // Clickable [Copy] link positions
    std::string               m_currentCodeContent;  // Accumulating during current block

    // ── File chip callback ────────────────────────────────────────
    FileCallback              m_fileCallback;        // Invoked when a fenced block closes
    std::function<bool()>     m_autoScroll;          // Empty = always scroll (legacy)

    bool ShouldAutoScroll() const
    { return !m_autoScroll || m_autoScroll(); }

    // ── Block-level rendering ────────────────────────────────────
    void RenderCompleteLine(const std::string& line, const wxColour& baseColor);
    void RenderCodeBlockLine(const std::string& line);

    // Finalize the currently-open code block (if any).  Pushes the
    // accumulated content into m_codeBlocks so any Copy link recorded
    // at fence-open time resolves to its block, then resets the block
    // state members.  When drawBottomBorder is true, also writes the
    // closing border (or a trailing newline for no-language fences) —
    // matches the visuals of a normally-closed fence.
    //
    // Called from three places:
    //   • RenderCompleteLine when a closing ``` arrives (drawBottomBorder=true)
    //   • Flush()  when a stream ends without a closing fence (true)
    //   • Reset()  for defense against cancel/error paths        (false)
    void FinalizeOpenCodeBlock(bool drawBottomBorder);

    void RenderHeading(const std::string& text, int level, const wxColour& baseColor);
    void RenderBulletItem(const std::string& text, const wxColour& baseColor,
                          int depth = 0);
    void RenderNumberedItem(const std::string& prefix, const std::string& text,
                            const wxColour& baseColor, int depth = 0);
    void RenderHorizontalRule(const wxColour& baseColor);

    // ── Inline markdown parsing ──────────────────────────────────
    void RenderInlineMarkdown(const std::string& text, const wxColour& baseColor);

    // ── Low-level text output ────────────────────────────────────
    void WriteStyled(const std::string& text, const wxColour& color,
                     bool bold = false, bool italic = false, bool monospace = false,
                     int fontSizeDelta = 0);

    // ── Partial line management ──────────────────────────────────
    void RemovePartialLine();
    void RenderPartialLine(const std::string& text, const wxColour& baseColor);

    // ── Helpers ──────────────────────────────────────────────────
    bool IsCodeFence(const std::string& line) const;
    bool IsHorizontalRule(const std::string& line) const;
    int  GetHeadingLevel(const std::string& line) const;
    bool IsBulletItem(const std::string& line) const;
    bool IsNumberedItem(const std::string& line, std::string& prefix) const;
    std::string TrimLeading(const std::string& s, char c) const;

    // ── Filename detection helpers (used by the file-chip callback) ─
    struct FenceInfo {
        std::string language;   // Just "cpp", "python", ... stripped of any extras
        std::string filename;   // Parsed from fence if present, else empty
    };
    FenceInfo    ParseFenceInfo(const std::string& rawAfterTicks) const;
    std::string  LanguageToExtension(const std::string& lang) const;
    std::string  LanguageDisplayName(const std::string& lang) const;
    std::string  ExtractFilenameFromContent(const std::string& content) const;
    bool         IsLikelyFilename(const std::string& s) const;
};

