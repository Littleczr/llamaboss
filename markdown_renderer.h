// markdown_renderer.h
// Streaming markdown renderer for wxRichTextCtrl.
// Parses incoming deltas line-by-line, rendering complete lines with full
// markdown formatting (bold, italic, code, headings, lists, code blocks)
// while showing partial lines as plain text until they complete.

#pragma once

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <string>

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

private:
    wxRichTextCtrl* m_ctrl;

    // ── Streaming state ──────────────────────────────────────────
    std::string m_lineBuffer;          // Accumulated text not yet rendered
    bool        m_inCodeBlock;         // Currently inside a ``` fenced block
    std::string m_codeBlockLang;       // Language tag from opening fence (if any)
    long        m_partialLineStart;    // Character position where partial line begins (-1 = none)

    // ── Colors ───────────────────────────────────────────────────
    wxColour m_codeColor;
    wxColour m_headingColor;
    wxColour m_codeLabelColor;
    wxColour m_horizontalRuleColor;

    // ── Code block copy tracking ──────────────────────────────────
    std::vector<std::string>  m_codeBlocks;         // Completed code block contents
    std::vector<CopyLink>     m_copyLinks;          // Clickable [Copy] link positions
    std::string               m_currentCodeContent;  // Accumulating during current block

    // ── Block-level rendering ────────────────────────────────────
    void RenderCompleteLine(const std::string& line, const wxColour& baseColor);
    void RenderCodeBlockLine(const std::string& line);
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
};

