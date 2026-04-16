// markdown_renderer.cpp
// Streaming markdown renderer for wxRichTextCtrl.
//
// Architecture: line-buffered rendering.
//   - Incoming deltas accumulate in m_lineBuffer.
//   - When a newline (\n) is found, the complete line is extracted and
//     rendered with full markdown formatting.
//   - The remaining partial line is rendered as plain text and tracked
//     so it can be erased and re-rendered when the next delta arrives.
//   - Freeze/Thaw wraps each ProcessDelta to prevent flicker.

#include "markdown_renderer.h"
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════
//  Construction / Reset
// ═══════════════════════════════════════════════════════════════════

MarkdownRenderer::MarkdownRenderer(wxRichTextCtrl* ctrl)
    : m_ctrl(ctrl)
    , m_inCodeBlock(false)
    , m_partialLineStart(-1)
    , m_codeColor(232, 184, 77)
    , m_headingColor(232, 232, 232)         // Near-white (#E8E8E8)
    , m_codeLabelColor(120, 120, 120)       // Gray
    , m_horizontalRuleColor(80, 80, 80)     // Dark gray
{
}

void MarkdownRenderer::Reset()
{
    m_lineBuffer.clear();
    m_inCodeBlock = false;
    m_codeBlockLang.clear();
    m_partialLineStart = -1;
    // Note: m_codeBlocks is intentionally NOT cleared here —
    // old code blocks from previous messages must remain accessible.
    // Call ClearCodeBlocks() explicitly when the entire chat is cleared.
    m_currentCodeContent.clear();
}

const std::string& MarkdownRenderer::GetCodeBlock(size_t index) const
{
    static const std::string empty;
    return index < m_codeBlocks.size() ? m_codeBlocks[index] : empty;
}

void MarkdownRenderer::ClearCodeBlocks()
{
    m_codeBlocks.clear();
    m_copyLinks.clear();
}

int MarkdownRenderer::HitTestCopyLink(long pos) const
{
    for (const auto& link : m_copyLinks) {
        if (pos >= link.startPos && pos < link.endPos)
            return static_cast<int>(link.blockIndex);
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════
//  Streaming Interface
// ═══════════════════════════════════════════════════════════════════

void MarkdownRenderer::ProcessDelta(const std::string& delta, const wxColour& baseColor)
{
    if (delta.empty()) return;

    m_ctrl->Freeze();

    // Erase the previous partial-line preview (if any) before modifying the buffer,
    // so it doesn't get baked in as permanent text.
    RemovePartialLine();

    // Accumulate new text
    m_lineBuffer += delta;

    // Render all complete lines (terminated by \n)
    size_t pos = 0;
    size_t newlinePos;
    while ((newlinePos = m_lineBuffer.find('\n', pos)) != std::string::npos) {
        std::string line = m_lineBuffer.substr(pos, newlinePos - pos);
        RenderCompleteLine(line, baseColor);
        pos = newlinePos + 1;
    }

    // Remove the consumed complete lines from the buffer
    if (pos > 0) {
        m_lineBuffer.erase(0, pos);
    }

    // Show the remaining incomplete text as a temporary partial-line preview.
    // This gives the user immediate visual feedback for every token.
    if (!m_lineBuffer.empty()) {
        RenderPartialLine(m_lineBuffer, baseColor);
    }

    m_ctrl->Thaw();
    m_ctrl->ShowPosition(m_ctrl->GetLastPosition());
}

void MarkdownRenderer::Flush(const wxColour& baseColor)
{
    if (m_lineBuffer.empty()) return;

    m_ctrl->Freeze();

    // Remove the partial-line preview — we're about to render this text
    // permanently with full markdown formatting.
    RemovePartialLine();

    // Render the final buffered remainder as a complete line
    RenderCompleteLine(m_lineBuffer, baseColor);
    m_lineBuffer.clear();

    m_ctrl->Thaw();
    m_ctrl->ShowPosition(m_ctrl->GetLastPosition());
}

// ═══════════════════════════════════════════════════════════════════
//  Partial Line Management
// ═══════════════════════════════════════════════════════════════════

void MarkdownRenderer::RemovePartialLine()
{
    if (m_partialLineStart >= 0) {
        long endPos = m_ctrl->GetLastPosition();
        if (m_partialLineStart < endPos) {
            m_ctrl->Remove(m_partialLineStart, endPos);
        }
        m_partialLineStart = -1;
    }
}

void MarkdownRenderer::RenderPartialLine(const std::string& text, const wxColour& baseColor)
{
    m_ctrl->SetInsertionPointEnd();
    m_partialLineStart = m_ctrl->GetLastPosition();

    // Partial line: render as plain text in the appropriate color
    if (m_inCodeBlock) {
        WriteStyled(text, m_codeColor, false, false, true);
    }
    else {
        WriteStyled(text, baseColor);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Block-Level Rendering
// ═══════════════════════════════════════════════════════════════════

void MarkdownRenderer::RenderCompleteLine(const std::string& line, const wxColour& baseColor)
{
    m_ctrl->SetInsertionPointEnd();

    // ── Code fence? ──────────────────────────────────────────────
    if (IsCodeFence(line)) {
        if (m_inCodeBlock) {
            // Closing fence — store code block and render [Copy] link
            // Trim trailing newline from accumulated content
            if (!m_currentCodeContent.empty() && m_currentCodeContent.back() == '\n')
                m_currentCodeContent.pop_back();
            size_t idx = m_codeBlocks.size();
            m_codeBlocks.push_back(m_currentCodeContent);
            m_currentCodeContent.clear();

            // Render [Copy] link and record its character range
            long linkStart = m_ctrl->GetLastPosition();
            WriteStyled("\xF0\x9F\x93\x8B Copy", m_codeLabelColor, false, true, true);
            long linkEnd = m_ctrl->GetLastPosition();
            m_copyLinks.push_back({ linkStart, linkEnd, idx });
            WriteStyled("\n", m_codeLabelColor);

            m_inCodeBlock = false;
            m_codeBlockLang.clear();
        }
        else {
            // Opening fence — start code block, extract language tag
            m_inCodeBlock = true;
            m_currentCodeContent.clear();
            std::string trimmed = line;
            size_t start = trimmed.find_first_not_of(" \t");
            if (start != std::string::npos) trimmed = trimmed.substr(start);
            if (trimmed.size() > 3) {
                m_codeBlockLang = trimmed.substr(3);
                size_t end = m_codeBlockLang.find_last_not_of(" \t\r");
                if (end != std::string::npos)
                    m_codeBlockLang = m_codeBlockLang.substr(0, end + 1);
            }

            // Render a subtle language label if present
            if (!m_codeBlockLang.empty()) {
                WriteStyled("[" + m_codeBlockLang + "]\n",
                    m_codeLabelColor, false, true, true);
            }
        }
        return;
    }

    // ── Inside a code block? ─────────────────────────────────────
    if (m_inCodeBlock) {
        RenderCodeBlockLine(line);
        return;
    }

    // ── Horizontal rule? ─────────────────────────────────────────
    if (IsHorizontalRule(line)) {
        RenderHorizontalRule(baseColor);
        return;
    }

    // ── Heading? ─────────────────────────────────────────────────
    int headingLevel = GetHeadingLevel(line);
    if (headingLevel > 0) {
        size_t textStart = headingLevel;
        if (textStart < line.size() && line[textStart] == ' ')
            textStart++;
        RenderHeading(line.substr(textStart), headingLevel, baseColor);
        return;
    }

    // ── Bullet list item? ────────────────────────────────────────
    if (IsBulletItem(line)) {
        // Find the bullet marker after any leading whitespace
        size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) ++indent;
        std::string text = line.substr(indent + 2);  // skip "- " or "* "
        int depth = static_cast<int>(indent / 2);     // 2 spaces per indent level
        RenderBulletItem(text, baseColor, depth);
        return;
    }

    // ── Numbered list item? ──────────────────────────────────────
    std::string numPrefix;
    if (IsNumberedItem(line, numPrefix)) {
        size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) ++indent;
        std::string text = line.substr(indent + numPrefix.size());
        int depth = static_cast<int>(indent / 2);
        RenderNumberedItem(numPrefix, text, baseColor, depth);
        return;
    }

    // ── Regular paragraph line ───────────────────────────────────
    RenderInlineMarkdown(line, baseColor);
    WriteStyled("\n", baseColor);
}

void MarkdownRenderer::RenderCodeBlockLine(const std::string& line)
{
    m_currentCodeContent += line + "\n";
    WriteStyled(line + "\n", m_codeColor, false, false, true);
}

void MarkdownRenderer::RenderHeading(const std::string& text, int level,
    const wxColour& baseColor)
{
    // Increased deltas for more visible differentiation
    int sizeDelta = 0;
    switch (level) {
    case 1: sizeDelta = 6; break;
    case 2: sizeDelta = 4; break;
    case 3: sizeDelta = 2; break;
    default: break;
    }

    WriteStyled(text + "\n", m_headingColor, true, false, false, sizeDelta);
}

void MarkdownRenderer::RenderBulletItem(const std::string& text,
    const wxColour& baseColor, int depth)
{
    std::string indent(2 + depth * 3, ' ');  // base indent + 3 spaces per nesting level
    WriteStyled(indent + "\xE2\x80\xA2 ", baseColor);  // UTF-8 bullet •
    RenderInlineMarkdown(text, baseColor);
    WriteStyled("\n", baseColor);
}

void MarkdownRenderer::RenderNumberedItem(const std::string& prefix,
    const std::string& text,
    const wxColour& baseColor, int depth)
{
    std::string indent(2 + depth * 3, ' ');
    WriteStyled(indent + prefix, baseColor);
    RenderInlineMarkdown(text, baseColor);
    WriteStyled("\n", baseColor);
}

void MarkdownRenderer::RenderHorizontalRule(const wxColour& baseColor)
{
    WriteStyled("\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n",
        m_horizontalRuleColor);
}

// ═══════════════════════════════════════════════════════════════════
//  Inline Markdown Parser
// ═══════════════════════════════════════════════════════════════════

// Helper: check if character is a markdown emphasis marker (* or _)
static bool IsEmphasisChar(char c) { return c == '*' || c == '_'; }

void MarkdownRenderer::RenderInlineMarkdown(const std::string& text,
    const wxColour& baseColor)
{
    // Inline formatting parser supporting both * and _ markers:
    //   `code`               →  monospace + code color
    //   **bold** / __bold__  →  bold
    //   *italic* / _italic_  →  italic
    //   ***bi*** / ___bi___  →  bold + italic
    //
    // Markers must match: ** closes with **, __ closes with __.
    // If a closing marker is not found, the marker chars render as literal text.

    size_t len = text.size();
    size_t i = 0;
    std::string buffer;

    auto flushBuffer = [&]() {
        if (!buffer.empty()) {
            WriteStyled(buffer, baseColor);
            buffer.clear();
        }
        };

    while (i < len) {
        // ── Inline code: `...` ───────────────────────────────────
        if (text[i] == '`') {
            size_t closePos = text.find('`', i + 1);
            if (closePos != std::string::npos) {
                flushBuffer();
                std::string code = text.substr(i + 1, closePos - i - 1);
                WriteStyled(code, m_codeColor, false, false, true);
                i = closePos + 1;
                continue;
            }
            buffer += '`';
            i++;
            continue;
        }

        // ── Emphasis: * or _ ─────────────────────────────────────
        if (IsEmphasisChar(text[i])) {
            char marker = text[i];

            // Count consecutive identical marker chars
            size_t markerCount = 0;
            size_t j = i;
            while (j < len && text[j] == marker) {
                markerCount++;
                j++;
            }

            bool matched = false;

            // Try triple first (bold italic)
            if (!matched && markerCount >= 3) {
                std::string m(3, marker);
                size_t closePos = text.find(m, i + 3);
                if (closePos != std::string::npos) {
                    flushBuffer();
                    WriteStyled(text.substr(i + 3, closePos - i - 3), baseColor, true, true);
                    i = closePos + 3;
                    matched = true;
                }
            }

            // Try double (bold)
            if (!matched && markerCount >= 2) {
                std::string m(2, marker);
                size_t closePos = text.find(m, i + 2);
                if (closePos != std::string::npos) {
                    flushBuffer();
                    WriteStyled(text.substr(i + 2, closePos - i - 2), baseColor, true, false);
                    i = closePos + 2;
                    matched = true;
                }
            }

            // Try single (italic)
            if (!matched && markerCount >= 1) {
                size_t closePos = text.find(marker, i + 1);
                if (closePos != std::string::npos) {
                    flushBuffer();
                    WriteStyled(text.substr(i + 1, closePos - i - 1), baseColor, false, true);
                    i = closePos + 1;
                    matched = true;
                }
            }

            // No closing marker — literal
            if (!matched) {
                for (size_t k = 0; k < markerCount; k++)
                    buffer += marker;
                i += markerCount;
            }

            continue;
        }

        // ── Regular character ────────────────────────────────────
        buffer += text[i];
        i++;
    }

    flushBuffer();
}

// ═══════════════════════════════════════════════════════════════════
//  Low-Level Text Output
// ═══════════════════════════════════════════════════════════════════

void MarkdownRenderer::WriteStyled(const std::string& text, const wxColour& color,
    bool bold, bool italic, bool monospace,
    int fontSizeDelta)
{
    if (text.empty()) return;

    m_ctrl->SetInsertionPointEnd();

    wxRichTextAttr attr;
    attr.SetTextColour(color);
    attr.SetFontWeight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
    attr.SetFontStyle(italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);

    if (monospace) {
        attr.SetFontFaceName("Cascadia Code");
    }

    // Always explicitly set font size on every style attr.
    // wxRichTextCtrl can lose track of size if not set each time,
    // which was causing headings to render at body size.
    wxFont currentFont = m_ctrl->GetFont();
    int baseSize = currentFont.GetPointSize();
    if (baseSize <= 0) baseSize = 14;  // Fallback if font reports 0
    attr.SetFontSize(baseSize + fontSizeDelta);

    m_ctrl->BeginStyle(attr);
    m_ctrl->WriteText(wxString::FromUTF8(text));
    m_ctrl->EndStyle();
}

// ═══════════════════════════════════════════════════════════════════
//  Line Classification Helpers
// ═══════════════════════════════════════════════════════════════════

bool MarkdownRenderer::IsCodeFence(const std::string& line) const
{
    std::string trimmed = line;
    size_t start = trimmed.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    trimmed = trimmed.substr(start);
    return trimmed.size() >= 3
        && trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`';
}

bool MarkdownRenderer::IsHorizontalRule(const std::string& line) const
{
    std::string stripped;
    for (char c : line) {
        if (c != ' ' && c != '\t') stripped += c;
    }
    if (stripped.size() < 3) return false;

    char first = stripped[0];
    if (first != '-' && first != '*' && first != '_') return false;

    return std::all_of(stripped.begin(), stripped.end(),
        [first](char c) { return c == first; });
}

int MarkdownRenderer::GetHeadingLevel(const std::string& line) const
{
    int level = 0;
    for (size_t i = 0; i < line.size() && i < 6; ++i) {
        if (line[i] == '#')
            level++;
        else
            break;
    }

    if (level > 0 && level < (int)line.size() && line[level] == ' ')
        return level;

    return 0;
}

bool MarkdownRenderer::IsBulletItem(const std::string& line) const
{
    // Skip leading whitespace to support indented sub-lists
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

    if (i + 1 < line.size() &&
        (line[i] == '-' || line[i] == '*') && line[i + 1] == ' ')
        return true;
    return false;
}

bool MarkdownRenderer::IsNumberedItem(const std::string& line,
    std::string& prefix) const
{
    // Skip leading whitespace to support indented sub-lists
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

    size_t numStart = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        i++;
    }

    if (i > numStart && i + 1 < line.size() && line[i] == '.' && line[i + 1] == ' ') {
        prefix = line.substr(numStart, i - numStart + 2);  // "1. "
        return true;
    }

    return false;
}

std::string MarkdownRenderer::TrimLeading(const std::string& s, char c) const
{
    size_t start = s.find_first_not_of(c);
    if (start == std::string::npos) return "";
    return s.substr(start);
}
