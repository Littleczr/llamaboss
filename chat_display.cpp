// chat_display.cpp
#include "chat_display.h"
#include "markdown_renderer.h"
#include "ascii_animation.h"
#include "theme.h"
#include <wx/clipbrd.h>
#include <algorithm>

ChatDisplay::ChatDisplay(wxRichTextCtrl* displayCtrl)
    : m_displayCtrl(displayCtrl)
    , m_markdownRenderer(std::make_unique<MarkdownRenderer>(displayCtrl))
    , m_userColor(108, 180, 238)       // Soft blue (#6CB4EE)
    , m_assistantColor(125, 212, 160)  // Mint green (#7DD4A0)
    , m_systemColor(136, 136, 136)     // Medium gray (#888888)
    , m_thoughtColor(154, 154, 154)    // Light gray (#9A9A9A)
    , m_isInThoughtBlock(false)
    , m_isFirstAssistantDelta(true)
    , m_hasRenderedAssistantContent(false)
    , m_activeAssistantColor(125, 212, 160)
{
    // Configure markdown renderer colors to match theme
    m_markdownRenderer->SetCodeColor(wxColour(232, 184, 77));      // Warm amber (#E8B84D)
    m_markdownRenderer->SetHeadingColor(wxColour(232, 232, 232));  // Near-white (#E8E8E8)

    // ── Code block copy: click handler ────────────────────────────
    m_displayCtrl->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        event.Skip();  // Let normal text selection proceed

        long pos = 0;
        auto hit = m_displayCtrl->HitTest(event.GetPosition(), &pos);
        if (hit == wxTE_HT_ON_TEXT || hit == wxTE_HT_BEFORE) {
            int blockIdx = m_markdownRenderer->HitTestCopyLink(pos);
            if (blockIdx >= 0) {
                const std::string& code = m_markdownRenderer->GetCodeBlock(static_cast<size_t>(blockIdx));
                if (wxTheClipboard->Open()) {
                    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(code)));
                    wxTheClipboard->Close();
                }
            }
        }
    });

    // ── Code block copy: hand cursor on hover ─────────────────────
    m_displayCtrl->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        long pos = 0;
        auto hit = m_displayCtrl->HitTest(event.GetPosition(), &pos);
        bool overLink = false;
        if (hit == wxTE_HT_ON_TEXT || hit == wxTE_HT_BEFORE) {
            overLink = (m_markdownRenderer->HitTestCopyLink(pos) >= 0);
        }
        if (overLink) {
            m_displayCtrl->SetCursor(wxCursor(wxCURSOR_HAND));
            // Don't Skip — prevents wxRichTextCtrl from resetting cursor
        } else {
            m_displayCtrl->SetCursor(wxCursor(wxCURSOR_IBEAM));
            event.Skip();
        }
    });
}

// Destructor defined here (not in header) because unique_ptr<MarkdownRenderer>
// needs the complete type, and the header only forward-declares it.
ChatDisplay::~ChatDisplay() = default;

void ChatDisplay::DisplayUserMessage(const std::string& text,
                                     const std::string& target,
                                     const std::vector<std::string>& inlineImages)
{
    SetInsertionPointToEnd();

    wxRichTextAttr prefixAttr;
    prefixAttr.SetTextColour(m_userColor);
    prefixAttr.SetFontWeight(wxFONTWEIGHT_BOLD);
    m_displayCtrl->BeginStyle(prefixAttr);

    if (!target.empty()) {
        // Shorten the target name for display:
        // "pidrilkin/gemma3:Q4_K_M" → "gemma3:Q4_K_M"
        std::string shortTarget = target;
        size_t slash = shortTarget.rfind('/');
        if (slash != std::string::npos && slash + 1 < shortTarget.size())
            shortTarget = shortTarget.substr(slash + 1);

        m_displayCtrl->WriteText(wxString::FromUTF8(
            "You \xe2\x86\x92 " + shortTarget + ": "));  // → arrow
    }
    else {
        m_displayCtrl->WriteText("You: ");
    }
    m_displayCtrl->EndStyle();

    wxRichTextAttr textAttr;
    textAttr.SetTextColour(m_userColor);
    textAttr.SetFontWeight(wxFONTWEIGHT_NORMAL);
    m_displayCtrl->BeginStyle(textAttr);
    m_displayCtrl->WriteText(wxString::FromUTF8(text + "\n"));
    m_displayCtrl->EndStyle();

    // ── Inline image thumbnails ──────────────────────────────────
    for (const auto& imgPath : inlineImages) {
        wxImage img;
        if (img.LoadFile(wxString::FromUTF8(imgPath))) {
            int w = img.GetWidth(), h = img.GetHeight();
            if (w <= 0 || h <= 0) continue;  // Skip corrupt/empty images

            // Scale to thumbnail preserving aspect ratio
            if (w > kImageMaxWidth || h > kImageMaxHeight) {
                double scaleW = static_cast<double>(kImageMaxWidth) / w;
                double scaleH = static_cast<double>(kImageMaxHeight) / h;
                double scale = (scaleW < scaleH) ? scaleW : scaleH;
                int newW = std::max(1, static_cast<int>(w * scale));
                int newH = std::max(1, static_cast<int>(h * scale));
                img.Rescale(newW, newH, wxIMAGE_QUALITY_HIGH);
            }
            m_displayCtrl->WriteImage(img);
            m_displayCtrl->WriteText("\n");
        }
    }

    // Trailing spacing
    m_displayCtrl->WriteText("\n");

    EnsureVisibleAtEnd();
}

void ChatDisplay::DisplaySystemMessage(const std::string& text)
{
    SetInsertionPointToEnd();

    wxRichTextAttr attr;
    attr.SetTextColour(m_systemColor);
    attr.SetFontStyle(wxFONTSTYLE_ITALIC);
    m_displayCtrl->BeginStyle(attr);
    m_displayCtrl->WriteText(wxString::FromUTF8(text + "\n\n"));
    m_displayCtrl->EndStyle();

    EnsureVisibleAtEnd();
}

void ChatDisplay::DisplayAssistantPrefix(const std::string& modelName)
{
    DisplayAssistantPrefix(modelName, m_assistantColor);
}

void ChatDisplay::DisplayAssistantPrefix(const std::string& modelName, const wxColour& accentColor)
{
    SetInsertionPointToEnd();

    // Reset state for the new message
    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
    m_activeAssistantColor = accentColor;
    m_markdownRenderer->Reset();

    wxFont baseFont = m_displayCtrl->GetFont();
    int baseSize = baseFont.GetPointSize();
    if (baseSize <= 0) baseSize = 14;

    wxRichTextAttr prefixAttr;
    prefixAttr.SetTextColour(accentColor);
    prefixAttr.SetFontWeight(wxFONTWEIGHT_BOLD);
    prefixAttr.SetFontStyle(wxFONTSTYLE_NORMAL);
    prefixAttr.SetFontSize(baseSize);

    if (!baseFont.GetFaceName().empty()) {
        prefixAttr.SetFontFaceName(baseFont.GetFaceName());
    }

    m_displayCtrl->BeginStyle(prefixAttr);
    m_displayCtrl->WriteText(wxString::FromUTF8(modelName + ": "));
    m_displayCtrl->EndStyle();
}

void ChatDisplay::DisplayAssistantDelta(const std::string& delta)
{
    SetInsertionPointToEnd();
    std::string remainingDelta = delta;

    const auto trimLeadingWhitespace = [](std::string& text)
        {
            size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                text.clear();
            }
            else if (first > 0) {
                text.erase(0, first);
            }
        };

    const auto hasVisibleChars = [](const std::string& text) -> bool
        {
            return text.find_first_not_of(" \t\r\n") != std::string::npos;
        };

    // Markers for different reasoning models
    const std::string thought_start_marker = "<think>";
    const std::string thought_end_marker = "</think>";

    // ── Probe phase: accumulate first few bytes to detect <think> ──
    // The tag is 7 characters and may arrive split across deltas
    // (e.g. "<thi" + "nk>\n...").  We buffer until we can decide.
    if (m_isFirstAssistantDelta) {
        m_thinkProbeBuffer += remainingDelta;

        // Check if buffer starts with <think>
        if (m_thinkProbeBuffer.size() >= thought_start_marker.size()) {
            if (m_thinkProbeBuffer.compare(0, thought_start_marker.size(),
                                           thought_start_marker) == 0) {
                // Confirmed: thinking model response
                m_isInThoughtBlock = true;
                m_isFirstAssistantDelta = false;
                remainingDelta = m_thinkProbeBuffer.substr(thought_start_marker.size());
                m_thinkProbeBuffer.clear();
                // Fall through to process remainingDelta as thought content
            }
            else {
                // Not a <think> tag — flush entire buffer as normal content
                m_isFirstAssistantDelta = false;
                remainingDelta = m_thinkProbeBuffer;
                m_thinkProbeBuffer.clear();
                // Fall through to process remainingDelta as normal content
            }
        }
        else if (thought_start_marker.compare(0, m_thinkProbeBuffer.size(),
                                              m_thinkProbeBuffer) == 0) {
            // Partial prefix match (e.g. "<thi") — keep buffering, don't render yet
            return;
        }
        else {
            // Buffer can't possibly match <think> — flush as normal content
            m_isFirstAssistantDelta = false;
            remainingDelta = m_thinkProbeBuffer;
            m_thinkProbeBuffer.clear();
            // Fall through to process remainingDelta as normal content
        }
    }

    if (m_isInThoughtBlock) {
        // Prepend any buffered tail from the previous delta so that a
        // </think> tag split across two deltas is detected correctly.
        std::string combined = m_thinkEndProbeBuffer + remainingDelta;
        m_thinkEndProbeBuffer.clear();

        size_t end_pos = combined.find(thought_end_marker);
        if (end_pos != std::string::npos) {
            // End marker found. Part is thought, rest is answer.
            std::string thought_part = combined.substr(0, end_pos);
            std::string answer_part = combined.substr(end_pos + thought_end_marker.length());

            // If nothing visible has been rendered yet, strip leading blank lines/spaces.
            if (!m_hasRenderedAssistantContent) {
                trimLeadingWhitespace(thought_part);
            }

            // Only render thought text if it actually contains visible characters.
            if (hasVisibleChars(thought_part)) {
                AppendFormattedText(thought_part, m_thoughtColor);
                m_hasRenderedAssistantContent = true;
            }

            m_isInThoughtBlock = false;

            // Trim leading blank space before the first visible answer text.
            if (!m_hasRenderedAssistantContent) {
                trimLeadingWhitespace(answer_part);
            }

            if (!answer_part.empty()) {
                // ProcessDelta scrolls internally — no extra scroll needed
                m_markdownRenderer->ProcessDelta(answer_part, m_activeAssistantColor);
                if (hasVisibleChars(answer_part)) {
                    m_hasRenderedAssistantContent = true;
                }
            }
            else {
                // Only thought text was rendered (via AppendFormattedText
                // which doesn't scroll) — scroll now.
                EnsureVisibleAtEnd();
            }
        }
        else {
            // No end marker yet. Hold back the last 7 chars (length of
            // "</think>" minus 1) so a split tag is caught on the next delta.
            const size_t kHoldBack = thought_end_marker.size() - 1;  // 7
            size_t safeLen = (combined.size() > kHoldBack)
                           ? combined.size() - kHoldBack : 0;
            std::string safeToRender = combined.substr(0, safeLen);
            m_thinkEndProbeBuffer = combined.substr(safeLen);

            if (!m_hasRenderedAssistantContent) {
                trimLeadingWhitespace(safeToRender);
            }

            if (hasVisibleChars(safeToRender)) {
                AppendFormattedText(safeToRender, m_thoughtColor);
                m_hasRenderedAssistantContent = true;
            }
            // AppendFormattedText doesn't scroll — do it here
            EnsureVisibleAtEnd();
        }
    }
    else {
        // Normal answer text — trim leading blank space only at the very start
        if (!m_hasRenderedAssistantContent) {
            trimLeadingWhitespace(remainingDelta);
        }

        if (!remainingDelta.empty()) {
            // ProcessDelta scrolls internally
            m_markdownRenderer->ProcessDelta(remainingDelta, m_activeAssistantColor);
            if (hasVisibleChars(remainingDelta)) {
                m_hasRenderedAssistantContent = true;
            }
        }
    }
}

void ChatDisplay::DisplayAssistantComplete()
{
    // If message ended while still probing for <think>, flush the
    // buffered bytes as normal content (it wasn't a thinking response).
    if (m_isFirstAssistantDelta && !m_thinkProbeBuffer.empty()) {
        m_isFirstAssistantDelta = false;
        m_markdownRenderer->ProcessDelta(m_thinkProbeBuffer, m_activeAssistantColor);
        m_thinkProbeBuffer.clear();
    }

    // Flush any remaining buffered text in the markdown renderer
    if (m_isInThoughtBlock) {
        // Message ended while still in thought block (unusual but handle it).
        // Flush any held-back tail from the split-boundary probe.
        if (!m_thinkEndProbeBuffer.empty()) {
            AppendFormattedText(m_thinkEndProbeBuffer, m_thoughtColor);
            m_thinkEndProbeBuffer.clear();
        }
        AppendFormattedText("\n\n", m_thoughtColor);
    }
    else {
        m_markdownRenderer->Flush(m_activeAssistantColor);
        AppendFormattedText("\n\n", m_activeAssistantColor);
    }

    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
}

void ChatDisplay::DisplayAssistantMessage(const std::string& modelName,
    const std::string& content,
    const wxColour& accentColor)
{
    DisplayAssistantPrefix(modelName, accentColor);

    if (!content.empty()) {
        // Strip leading whitespace/newlines so the first paragraph renders
        // flush with the prefix — matches the trim that DisplayAssistantDelta
        // performs on the streaming path.
        std::string trimmed = content;
        size_t first = trimmed.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            trimmed.clear();
        } else if (first > 0) {
            trimmed.erase(0, first);
        }

        if (!trimmed.empty()) {
            // ── Handle <think>...</think> blocks on replay ─────────────
            // The streaming path uses a state machine to detect and dimly
            // render thought blocks. The replay (non-streaming) path must
            // do the same, otherwise raw <think> tags appear literally.
            const std::string kThinkStart = "<think>";
            const std::string kThinkEnd   = "</think>";

            std::string toProcess = trimmed;
            bool renderedAny = false;

            while (!toProcess.empty()) {
                size_t ts = toProcess.find(kThinkStart);
                if (ts == std::string::npos) {
                    // No (more) think block — render remainder as answer
                    m_markdownRenderer->ProcessDelta(toProcess, accentColor);
                    renderedAny = true;
                    toProcess.clear();
                } else {
                    // Render any answer text that precedes the think block
                    if (ts > 0) {
                        m_markdownRenderer->ProcessDelta(toProcess.substr(0, ts), accentColor);
                        renderedAny = true;
                    }

                    size_t contentStart = ts + kThinkStart.size();
                    size_t te = toProcess.find(kThinkEnd, contentStart);

                    if (te == std::string::npos) {
                        // Malformed: no closing tag — treat rest as thought text
                        std::string thought = toProcess.substr(contentStart);
                        // strip leading whitespace
                        size_t f = thought.find_first_not_of(" \t\r\n");
                        if (f != std::string::npos && f > 0) thought.erase(0, f);
                        if (!thought.empty()) {
                            AppendFormattedText(thought, m_thoughtColor);
                            renderedAny = true;
                        }
                        toProcess.clear();
                    } else {
                        // Well-formed block — render thought dimly
                        std::string thought = toProcess.substr(contentStart, te - contentStart);
                        size_t f = thought.find_first_not_of(" \t\r\n");
                        if (f != std::string::npos && f > 0) thought.erase(0, f);
                        if (!thought.empty()) {
                            AppendFormattedText(thought, m_thoughtColor);
                            renderedAny = true;
                        }
                        // Continue with whatever follows </think>
                        toProcess = toProcess.substr(te + kThinkEnd.size());
                        // strip leading whitespace before the answer
                        size_t fa = toProcess.find_first_not_of(" \t\r\n");
                        if (fa == std::string::npos)
                            toProcess.clear();
                        else if (fa > 0)
                            toProcess.erase(0, fa);
                    }
                }
            }

            m_markdownRenderer->Flush(accentColor);
            (void)renderedAny;
        }
    }

    AppendFormattedText("\n\n", accentColor);

    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();

    EnsureVisibleAtEnd();
}

void ChatDisplay::Clear()
{
    if (m_displayCtrl) {
        m_displayCtrl->Clear();
    }
    if (m_markdownRenderer) {
        m_markdownRenderer->Reset();
        m_markdownRenderer->ClearCodeBlocks();
    }
}

void ChatDisplay::ScrollToBottom()
{
    EnsureVisibleAtEnd();
}

void ChatDisplay::SetUserColor(const wxColour& color)
{
    m_userColor = color;
}

void ChatDisplay::SetAssistantColor(const wxColour& color)
{
    m_assistantColor = color;
}

void ChatDisplay::SetSystemColor(const wxColour& color)
{
    m_systemColor = color;
}

void ChatDisplay::SetThoughtColor(const wxColour& color)
{
    m_thoughtColor = color;
}

void ChatDisplay::SetFont(const wxFont& font)
{
    if (m_displayCtrl) {
        m_displayCtrl->SetFont(font);
    }
}

void ChatDisplay::ApplyTheme(const ThemeData& theme)
{
    m_userColor = theme.chatUser;
    m_assistantColor = theme.chatAssistant;
    m_systemColor = theme.chatSystem;
    m_thoughtColor = theme.chatThought;

    if (m_markdownRenderer) {
        m_markdownRenderer->SetCodeColor(theme.mdCode);
        m_markdownRenderer->SetHeadingColor(theme.mdHeading);
        m_markdownRenderer->SetCodeLabelColor(theme.mdCodeLabel);
        m_markdownRenderer->SetHorizontalRuleColor(theme.mdHorizontalRule);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  ASCII Animation rendering
// ═══════════════════════════════════════════════════════════════════

void ChatDisplay::BeginAnimationFrame()
{
    m_displayCtrl->Freeze();

    if (m_animActive && m_animStartPos >= 0) {
        // Delete previous frame text
        long end = m_displayCtrl->GetLastPosition();
        if (end > m_animStartPos)
            m_displayCtrl->Remove(m_animStartPos, end);
    }

    m_displayCtrl->SetInsertionPointEnd();
    m_animStartPos = m_displayCtrl->GetInsertionPoint();
    m_animActive = true;
}

void ChatDisplay::WriteAnimationLine(const std::vector<ColoredChar>& line)
{
    // Build a monospace font matching the base size
    wxFont baseFont = m_displayCtrl->GetFont();
    int sz = baseFont.GetPointSize();
    if (sz <= 0) sz = 11;
    wxFont monoFont(sz - 1, wxFONTFAMILY_TELETYPE,
                    wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL,
                    false, "Consolas");

    // Batch consecutive chars of the same color into one WriteText call
    // to avoid per-character style overhead (huge perf win on wxRichTextCtrl)
    size_t i = 0;
    while (i < line.size()) {
        wxColour color = line[i].color;
        std::string run;
        while (i < line.size() && line[i].color == color) {
            run += line[i].ch;
            ++i;
        }

        wxRichTextAttr attr;
        attr.SetTextColour(color);
        attr.SetFontWeight(wxFONTWEIGHT_NORMAL);
        attr.SetFontStyle(wxFONTSTYLE_NORMAL);
        attr.SetFont(monoFont);
        m_displayCtrl->BeginStyle(attr);
        m_displayCtrl->WriteText(wxString::FromUTF8(run));
        m_displayCtrl->EndStyle();
    }

    // Newline between rows (plain, no special style needed)
    m_displayCtrl->WriteText("\n");
}

void ChatDisplay::EndAnimationFrame()
{
    m_displayCtrl->Thaw();
    EnsureVisibleAtEnd();
}

void ChatDisplay::ClearAnimation()
{
    m_animActive   = false;
    m_animStartPos = -1;
}

// Private helper methods

void ChatDisplay::AppendFormattedText(const std::string& text, const wxColour& color,
    bool bold, bool italic)
{
    if (text.empty()) return;

    SetInsertionPointToEnd();

    wxRichTextAttr attr;
    attr.SetTextColour(color);
    if (bold) attr.SetFontWeight(wxFONTWEIGHT_BOLD);
    if (italic) attr.SetFontStyle(wxFONTSTYLE_ITALIC);
    if (!bold) attr.SetFontWeight(wxFONTWEIGHT_NORMAL);

    m_displayCtrl->BeginStyle(attr);
    m_displayCtrl->WriteText(wxString::FromUTF8(text));
    m_displayCtrl->EndStyle();
}

void ChatDisplay::SetInsertionPointToEnd()
{
    if (m_displayCtrl) {
        m_displayCtrl->SetInsertionPointEnd();
    }
}

void ChatDisplay::EnsureVisibleAtEnd()
{
    if (m_displayCtrl) {
        m_displayCtrl->ShowPosition(m_displayCtrl->GetLastPosition());
    }
}