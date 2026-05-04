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
#include <cctype>
#include <cstring>
#include <vector>

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
    m_codeBlockFilename.clear();
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
            // Closing fence — finalize the block and draw the bottom border.
            //
            // Copy link was already written into the header row at the opening
            // fence; the index recorded there expects m_codeBlocks.size() to be
            // its slot, so we push now with no intervening modifications.
            //
            // File chip (saved-to-disk PresentedFile) is intentionally not written
            // in v1 — the aesthetic spec calls for no footer, filename, or
            // line count.  The SetFileCallback infrastructure remains intact and
            // can be re-invoked here in a future step (e.g., a right-click Save
            // As menu on the block) without re-wiring anything.
            if (!m_currentCodeContent.empty() && m_currentCodeContent.back() == '\n')
                m_currentCodeContent.pop_back();
            m_codeBlocks.push_back(m_currentCodeContent);
            m_currentCodeContent.clear();

            // Bottom border:  ╰──…──╯
            // Only drawn when a header was drawn (i.e. a language was present).
            // For fences without a language, we just emit a newline so the code
            // body flows back into regular prose without decorative chrome.
            if (!m_codeBlockLang.empty()) {
                constexpr int kBoxWidth   = 72;
                constexpr int kInnerWidth = kBoxWidth - 2;

                std::string hfill;
                hfill.reserve(kInnerWidth * 3);
                for (int i = 0; i < kInnerWidth; ++i) hfill += "\xE2\x94\x80";  // ─

                std::string bottomBorder;
                bottomBorder += "\xE2\x95\xB0";   // ╰  (U+2570)
                bottomBorder += hfill;
                bottomBorder += "\xE2\x95\xAF";   // ╯  (U+256F)
                bottomBorder += "\n";
                WriteStyled(bottomBorder, m_codeLabelColor, false, false, true);
            } else {
                WriteStyled("\n", m_codeLabelColor);
            }

            m_inCodeBlock = false;
            m_codeBlockLang.clear();
            m_codeBlockFilename.clear();
        }
        else {
            // Opening fence — parse language + optional filename from the
            // tail of the fence line (everything after the leading ```).
            m_inCodeBlock = true;
            m_currentCodeContent.clear();
            m_codeBlockLang.clear();
            m_codeBlockFilename.clear();

            std::string trimmed = line;
            size_t start = trimmed.find_first_not_of(" \t");
            if (start != std::string::npos) trimmed = trimmed.substr(start);

            if (trimmed.size() > 3) {
                std::string rawAfter = trimmed.substr(3);
                // Strip trailing whitespace/CR from the tail
                size_t end = rawAfter.find_last_not_of(" \t\r");
                if (end != std::string::npos) rawAfter = rawAfter.substr(0, end + 1);
                else                          rawAfter.clear();

                FenceInfo info = ParseFenceInfo(rawAfter);
                m_codeBlockLang     = info.language;
                m_codeBlockFilename = info.filename;
            }

            // Render the code-block header: top border, language row, and
            // separator rule. Drawn with Unicode box-drawing characters in
            // the theme's muted color. The enclosure is deliberately three-
            // sided — the right edge of code lines has no border, which
            // sidesteps per-line padding for ragged-right content (matches
            // how GitHub, GitLab, and most polished code viewers render).
            //
            // v1 Step 1: header only.  Left-side walls on code lines and
            // the bottom border arrive in later steps.
            if (!m_codeBlockLang.empty()) {
                const std::string displayName = LanguageDisplayName(m_codeBlockLang);

                // Box width is measured in monospace display cells.  All
                // box-drawing glyphs below are 1 cell wide; `</>` is 3
                // ASCII cells; language display names are ASCII-only.
                constexpr int kBoxWidth   = 72;
                constexpr int kInnerWidth = kBoxWidth - 2;  // minus both corner cells

                // Reusable 70-cell horizontal fill.  "─" (U+2500) is 3 UTF-8 bytes.
                std::string hfill;
                hfill.reserve(kInnerWidth * 3);
                for (int i = 0; i < kInnerWidth; ++i) hfill += "\xE2\x94\x80";  // ─

                // Top border:  ╭──…──╮
                std::string topBorder;
                topBorder += "\xE2\x95\xAD";  // ╭  (U+256D)
                topBorder += hfill;
                topBorder += "\xE2\x95\xAE";  // ╮  (U+256E)
                topBorder += "\n";
                WriteStyled(topBorder, m_codeLabelColor, false, false, true);

                // Header row:  │ </> <n>  <padding>  Copy │
                //
                // Cell budget:
                //   "│ " (2) + "</> " (4) + nameCells + padding + copyCells (4) + " │" (2) = kBoxWidth
                //
                // Plain "Copy" — no emoji.  Emoji rendering width varies across
                // fonts (1 cell in some, 2 in others, proportional in emoji fonts),
                // which made header alignment fragile.  Plain ASCII sidesteps the
                // whole problem and the position itself (top-right of a framed
                // container) already signals clickability.
                const int prefixCells = 2 + 4;    // "│ " + "</> "
                const int copyCells   = 4;        // "Copy" (4)
                const int suffixCells = 2;        // " │"
                const int nameCells   = static_cast<int>(displayName.size());
                int padding = kBoxWidth - prefixCells - nameCells - copyCells - suffixCells;
                if (padding < 1) padding = 1;     // defensive — overflows rightward if name is huge

                // Header prefix — everything up to the Copy action.
                std::string headerPrefix;
                headerPrefix += "\xE2\x94\x82 ";  // │ + space
                headerPrefix += "</> ";
                headerPrefix += displayName;
                headerPrefix.append(padding, ' ');
                WriteStyled(headerPrefix, m_codeLabelColor, false, false, true);

                // Copy action — record character range for click/hover hit-testing.
                // Block index is m_codeBlocks.size() at write time; the block gets
                // pushed with exactly that index when the closing fence arrives.
                // No one else pushes to m_codeBlocks between open and close, so the
                // mapping is stable. If the stream is cut off before close, clicks
                // on this Copy return an empty string (GetCodeBlock is bounds-checked).
                long linkStart = m_ctrl->GetLastPosition();
                WriteStyled("Copy",
                    m_codeLabelColor, false, false, true);
                long linkEnd = m_ctrl->GetLastPosition();
                m_copyLinks.push_back({ linkStart, linkEnd, m_codeBlocks.size() });

                // Header suffix — close the right side of the header row.
                WriteStyled(" \xE2\x94\x82\n",     // space + │ + newline
                    m_codeLabelColor, false, false, true);

                // Separator rule:  ├──…──┤
                std::string separator;
                separator += "\xE2\x94\x9C";  // ├  (U+251C)
                separator += hfill;
                separator += "\xE2\x94\xA4";  // ┤  (U+2524)
                separator += "\n";
                WriteStyled(separator, m_codeLabelColor, false, false, true);
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
    // Preserve the raw code line for the saved block — the left/right walls
    // below are chrome, not part of the block's copyable payload.
    m_currentCodeContent += line + "\n";

    // No-language fences render plain, no frame.
    if (m_codeBlockLang.empty()) {
        WriteStyled(line + "\n", m_codeColor, false, false, true);
        return;
    }

    // Frame geometry:
    //   │ <content> <pad> │
    //   ^---^-----------^--^
    //   2   contentCells pad  2   cells = kBoxWidth (72)
    //
    // contentCells is approximated by byte count — exact for ASCII, a little
    // off for multi-byte characters (e.g. unicode identifiers or comments in
    // non-ASCII languages).  Acceptable for v1; LLMs overwhelmingly emit ASCII.
    constexpr int kBoxWidth = 72;
    constexpr int kInterior = kBoxWidth - 4;   // minus "│ " + " │"
    const int contentCells = static_cast<int>(line.size());
    const int pad = kInterior - contentCells;

    // Left wall always drawn.
    WriteStyled("\xE2\x94\x82 ", m_codeLabelColor, false, false, true);  // │ + space

    // Code content in syntax-highlight color.
    WriteStyled(line, m_codeColor, false, false, true);

    // Right wall — only if the line fits the box.  Lines that exceed the
    // interior skip the right wall entirely (the frame "opens" on that line
    // and reconnects on the next).  Better than letting the wall drift off
    // the box's right edge and breaking the bottom-border alignment visually.
    if (pad >= 1) {
        std::string rightSide(pad, ' ');
        rightSide += " \xE2\x94\x82\n";   // space + │ + newline
        WriteStyled(rightSide, m_codeLabelColor, false, false, true);
    } else {
        WriteStyled("\n", m_codeLabelColor, false, false, true);
    }
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

// Markdown-style underscores are useful for _italic_, but they are very common
// in filenames such as artifact_label_word_test.docx. Treat underscores inside
// ordinary identifier/filename words as literal characters so assistant summary
// text does not mangle generated filenames. Asterisks keep the old behavior.
static bool IsWordCharForUnderscore(char c)
{
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '.' || c == '-';
}

static bool IsIntraWordUnderscore(const std::string& text, size_t pos)
{
    if (pos >= text.size() || text[pos] != '_') return false;
    if (pos == 0 || pos + 1 >= text.size()) return false;
    return IsWordCharForUnderscore(text[pos - 1]) &&
           IsWordCharForUnderscore(text[pos + 1]);
}

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
            // Keep filename / identifier underscores literal:
            // artifact_label_word_test.docx should not render as
            // artifactlabelword_test.docx with italicized chunks.
            if (IsIntraWordUnderscore(text, i)) {
                buffer += text[i];
                i++;
                continue;
            }

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

// ── Filename detection helpers ──────────────────────────────────
// Loose rule: looks like "name.ext" — alphanumerics, dots, dashes,
// underscores, and exactly one trailing extension of 1–10 chars.
// Rejects anything with whitespace, slashes, or suspicious punctuation
// so prose like "file called foo" or URLs never slip through.
bool MarkdownRenderer::IsLikelyFilename(const std::string& s) const
{
    if (s.empty() || s.size() > 120) return false;
    size_t dot = s.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot == s.size() - 1) return false;

    std::string stem = s.substr(0, dot);
    std::string ext  = s.substr(dot + 1);

    if (ext.size() < 1 || ext.size() > 10) return false;
    for (char c : ext)
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;

    for (char c : stem) {
        // Allow alphanumerics plus a small punctuation set.  Disallow
        // whitespace, slashes, colons, quotes, parens, angles, etc.
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        if (c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
}

std::string MarkdownRenderer::LanguageToExtension(const std::string& lang) const
{
    std::string l = lang;
    for (auto& c : l) c = static_cast<char>(::tolower((unsigned char)c));
    if (l == "cpp" || l == "c++" || l == "cxx" || l == "cc") return "cpp";
    if (l == "c")                                               return "c";
    if (l == "h")                                               return "h";
    if (l == "hpp")                                             return "hpp";
    if (l == "py" || l == "python")                             return "py";
    if (l == "js" || l == "javascript")                         return "js";
    if (l == "ts" || l == "typescript")                         return "ts";
    if (l == "cs" || l == "csharp" || l == "c#")                return "cs";
    if (l == "java")                                            return "java";
    if (l == "kt" || l == "kotlin")                             return "kt";
    if (l == "swift")                                           return "swift";
    if (l == "go")                                              return "go";
    if (l == "rs" || l == "rust")                               return "rs";
    if (l == "rb" || l == "ruby")                               return "rb";
    if (l == "php")                                             return "php";
    if (l == "pl" || l == "perl")                               return "pl";
    if (l == "sh" || l == "bash" || l == "shell" || l == "zsh") return "sh";
    if (l == "ps" || l == "ps1" || l == "powershell")           return "ps1";
    if (l == "bat" || l == "batch" || l == "cmd")               return "bat";
    if (l == "sql")                                             return "sql";
    if (l == "html")                                            return "html";
    if (l == "xml")                                             return "xml";
    if (l == "css")                                             return "css";
    if (l == "json")                                            return "json";
    if (l == "yaml" || l == "yml")                              return "yml";
    if (l == "toml")                                            return "toml";
    if (l == "md" || l == "markdown")                           return "md";
    if (l == "lua")                                             return "lua";
    if (l == "r")                                               return "r";
    if (l == "dart")                                            return "dart";
    if (l == "cmake")                                           return "cmake";
    if (l == "makefile" || l == "make")                         return "mk";
    if (l == "dockerfile")                                      return "dockerfile";
    if (l == "tex" || l == "latex")                             return "tex";
    return "txt";
}

// Pretty display name for a fence language tag. Used in the code-block
// header to replace technical slugs ("cpp") with recognizable names
// ("C++"). Unknown tags fall through to a title-case transformation so
// weird fences still render something reasonable.
std::string MarkdownRenderer::LanguageDisplayName(const std::string& lang) const
{
    std::string l = lang;
    for (auto& c : l) c = static_cast<char>(::tolower((unsigned char)c));

    if (l == "cpp" || l == "c++" || l == "cxx" || l == "cc")    return "C++";
    if (l == "c")                                               return "C";
    if (l == "h" || l == "hpp")                                 return "C/C++ Header";
    if (l == "cs" || l == "csharp" || l == "c#")                return "C#";
    if (l == "py" || l == "python")                             return "Python";
    if (l == "js" || l == "javascript")                         return "JavaScript";
    if (l == "ts" || l == "typescript")                         return "TypeScript";
    if (l == "rs" || l == "rust")                               return "Rust";
    if (l == "go" || l == "golang")                             return "Go";
    if (l == "rb" || l == "ruby")                               return "Ruby";
    if (l == "java")                                            return "Java";
    if (l == "kt" || l == "kotlin")                             return "Kotlin";
    if (l == "swift")                                           return "Swift";
    if (l == "php")                                             return "PHP";
    if (l == "pl" || l == "perl")                               return "Perl";
    if (l == "sh" || l == "bash" || l == "shell" || l == "zsh") return "Shell";
    if (l == "ps" || l == "ps1" || l == "powershell" ||
        l == "pwsh")                                            return "PowerShell";
    if (l == "bat" || l == "batch" || l == "cmd")               return "Batch";
    if (l == "sql")                                             return "SQL";
    if (l == "html")                                            return "HTML";
    if (l == "css")                                             return "CSS";
    if (l == "xml")                                             return "XML";
    if (l == "json")                                            return "JSON";
    if (l == "yaml" || l == "yml")                              return "YAML";
    if (l == "toml")                                            return "TOML";
    if (l == "md" || l == "markdown")                           return "Markdown";
    if (l == "lua")                                             return "Lua";
    if (l == "r")                                               return "R";
    if (l == "dart")                                            return "Dart";
    if (l == "cmake")                                           return "CMake";
    if (l == "makefile" || l == "make")                         return "Makefile";
    if (l == "dockerfile" || l == "docker")                     return "Dockerfile";
    if (l == "tex" || l == "latex")                             return "LaTeX";
    if (l == "ini")                                             return "INI";
    if (l == "diff")                                            return "Diff";
    if (l == "hs" || l == "haskell")                            return "Haskell";

    // Unknown tag — title-case the lowercased input as a polite fallback.
    if (l.empty()) return l;
    l[0] = static_cast<char>(::toupper((unsigned char)l[0]));
    return l;
}

// Parses the string after the leading ``` of a code fence.  Handles:
//   ""                         -> {lang: "",   filename: ""}
//   "cpp"                      -> {lang: "cpp", filename: ""}
//   "cpp:hello.cpp"            -> {lang: "cpp", filename: "hello.cpp"}
//   "cpp hello.cpp"            -> {lang: "cpp", filename: "hello.cpp"}
//   "cpp filename=hello.cpp"   -> {lang: "cpp", filename: "hello.cpp"}
//   'cpp title="hello.cpp"'    -> {lang: "cpp", filename: "hello.cpp"}
//   "cpp title=\"some title\"" -> {lang: "cpp", filename: ""}   // title isn't a filename
MarkdownRenderer::FenceInfo
MarkdownRenderer::ParseFenceInfo(const std::string& rawAfterTicks) const
{
    FenceInfo out;
    if (rawAfterTicks.empty()) return out;

    // Strip an optional {...} Pandoc-style attribute wrapper.
    std::string s = rawAfterTicks;
    if (!s.empty() && s.front() == '{' && s.back() == '}')
        s = s.substr(1, s.size() - 2);

    // Pattern 1: language:filename
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        std::string left  = s.substr(0, colon);
        std::string right = s.substr(colon + 1);
        // Trim
        size_t l0 = left.find_first_not_of(" \t");
        size_t l1 = left.find_last_not_of(" \t");
        size_t r0 = right.find_first_not_of(" \t");
        size_t r1 = right.find_last_not_of(" \t");
        if (l0 != std::string::npos) left  = left.substr(l0, l1 - l0 + 1);
        else                         left.clear();
        if (r0 != std::string::npos) right = right.substr(r0, r1 - r0 + 1);
        else                         right.clear();

        // Strip surrounding quotes on filename candidate
        if (right.size() >= 2 &&
            ((right.front() == '"' && right.back() == '"') ||
             (right.front() == '\'' && right.back() == '\''))) {
            right = right.substr(1, right.size() - 2);
        }

        if (IsLikelyFilename(right)) {
            out.language = left;
            out.filename = right;
            return out;
        }
        // Colon present but right side isn't a filename — fall through
        // and treat the whole thing as language + extras.
    }

    // Tokenize on whitespace.  First token is always the language.
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : s) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
    }
    if (tokens.empty()) return out;
    out.language = tokens[0];

    // Scan remaining tokens for filename= / title= / a bare filename-ish token.
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];

        // key=value form
        size_t eq = t.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = t.substr(0, eq);
            std::string val = t.substr(eq + 1);
            for (auto& c : key) c = static_cast<char>(::tolower((unsigned char)c));
            // Strip surrounding quotes
            if (val.size() >= 2 &&
                ((val.front() == '"' && val.back() == '"') ||
                 (val.front() == '\'' && val.back() == '\''))) {
                val = val.substr(1, val.size() - 2);
            }
            if ((key == "filename" || key == "file" ||
                 key == "title"    || key == "name") &&
                IsLikelyFilename(val)) {
                out.filename = val;
                return out;
            }
            continue;  // not filename-ish; move on
        }

        // Bare token — accept if it looks like a filename
        std::string bare = t;
        if (bare.size() >= 2 &&
            ((bare.front() == '"' && bare.back() == '"') ||
             (bare.front() == '\'' && bare.back() == '\''))) {
            bare = bare.substr(1, bare.size() - 2);
        }
        if (IsLikelyFilename(bare)) {
            out.filename = bare;
            return out;
        }
    }

    return out;
}

// Looks at the first few non-blank lines of code for a filename hint
// embedded in a leading comment.  Handles the common comment syntaxes.
// Returns "" when no confident match is found.
std::string MarkdownRenderer::ExtractFilenameFromContent(
    const std::string& content) const
{
    // Grab up to the first ~5 non-blank lines.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : content) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
            if (lines.size() >= 6) break;
        }
        if (!cur.empty() && lines.size() < 6) lines.push_back(cur);
    }

    for (const std::string& raw : lines) {
        // Trim leading whitespace
        size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        std::string line = raw.substr(a);

        // Skip shebangs — language concern, not filename
        if (line.size() >= 2 && line[0] == '#' && line[1] == '!') continue;

        // Strip each known comment opener/closer.  Order matters for
        // the multi-char ones (<!-- before --, /* before *).
        auto stripPrefix = [](std::string& t, const char* p) {
            size_t n = std::strlen(p);
            if (t.size() >= n && t.compare(0, n, p) == 0) { t.erase(0, n); return true; }
            return false;
        };
        auto stripSuffix = [](std::string& t, const char* p) {
            size_t n = std::strlen(p);
            if (t.size() >= n && t.compare(t.size() - n, n, p) == 0) {
                t.erase(t.size() - n); return true;
            }
            return false;
        };

        bool stripped =
            stripPrefix(line, "<!--") ||
            stripPrefix(line, "///")  ||
            stripPrefix(line, "//")   ||
            stripPrefix(line, "/*")   ||
            stripPrefix(line, "--")   ||
            stripPrefix(line, "#")    ||
            stripPrefix(line, ";")    ||   // Lisp, ini
            stripPrefix(line, "%");        // MATLAB/LaTeX

        if (!stripped) continue;  // not a comment — bail, don't dig into code

        stripSuffix(line, "-->");
        stripSuffix(line, "*/");

        // Trim whitespace on both sides
        size_t s0 = line.find_first_not_of(" \t");
        size_t s1 = line.find_last_not_of(" \t");
        if (s0 == std::string::npos) continue;
        line = line.substr(s0, s1 - s0 + 1);

        // Sometimes the comment is a full phrase: "filename: hello.cpp".
        // Take the last whitespace-separated token if it looks like a file.
        size_t sp = line.find_last_of(" \t");
        if (sp != std::string::npos) {
            std::string tail = line.substr(sp + 1);
            if (IsLikelyFilename(tail)) return tail;
        }
        if (IsLikelyFilename(line)) return line;
    }

    return "";
}
