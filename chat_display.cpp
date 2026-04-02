// chat_display.cpp
#include "chat_display.h"
#include "markdown_renderer.h"
#include "theme.h"

ChatDisplay::ChatDisplay(wxRichTextCtrl* displayCtrl)
    : m_displayCtrl(displayCtrl)
    , m_markdownRenderer(new MarkdownRenderer(displayCtrl))
    , m_userColor(108, 180, 238)      // Soft blue (#6CB4EE)
    , m_assistantColor(125, 212, 160)  // Mint green (#7DD4A0)
    , m_systemColor(136, 136, 136)     // Medium gray (#888888)
    , m_thoughtColor(154, 154, 154)    // Light gray (#9A9A9A)
    , m_isInThoughtBlock(false)
    , m_isFirstAssistantDelta(true)
{
    // Configure markdown renderer colors to match theme
    m_markdownRenderer->SetCodeColor(wxColour(232, 184, 77));    // Warm amber (#E8B84D)
    m_markdownRenderer->SetHeadingColor(wxColour(232, 232, 232)); // Near-white (#E8E8E8)
}

ChatDisplay::~ChatDisplay()
{
    delete m_markdownRenderer;
}

void ChatDisplay::DisplayUserMessage(const std::string& text)
{
    SetInsertionPointToEnd();

    wxRichTextAttr prefixAttr;
    prefixAttr.SetTextColour(m_userColor);
    prefixAttr.SetFontWeight(wxFONTWEIGHT_BOLD);
    m_displayCtrl->BeginStyle(prefixAttr);
    m_displayCtrl->WriteText("You: ");
    m_displayCtrl->EndStyle();

    wxRichTextAttr textAttr;
    textAttr.SetTextColour(m_userColor);
    textAttr.SetFontWeight(wxFONTWEIGHT_NORMAL);
    m_displayCtrl->BeginStyle(textAttr);
    m_displayCtrl->WriteText(wxString::FromUTF8(text + "\n\n"));
    m_displayCtrl->EndStyle();

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
    SetInsertionPointToEnd();

    // Reset state for the new message
    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_markdownRenderer->Reset();

    wxRichTextAttr prefixAttr;
    prefixAttr.SetTextColour(m_assistantColor);
    prefixAttr.SetFontWeight(wxFONTWEIGHT_BOLD);
    m_displayCtrl->BeginStyle(prefixAttr);
    m_displayCtrl->WriteText(wxString::FromUTF8(modelName + ": "));
    m_displayCtrl->EndStyle();
}

void ChatDisplay::DisplayAssistantDelta(const std::string& delta)
{
    SetInsertionPointToEnd();
    std::string remainingDelta = delta;

    // Markers for different reasoning models
    const std::string thought_start_marker = "<think>";
    const std::string thought_end_marker = "</think>";

    // On the first delta, check if this is a "thinking" model response
    if (m_isFirstAssistantDelta) {
        m_isFirstAssistantDelta = false;
        if (remainingDelta.rfind(thought_start_marker, 0) == 0) {
            m_isInThoughtBlock = true;
            remainingDelta.erase(0, thought_start_marker.length());
        }
    }

    if (m_isInThoughtBlock) {
        size_t end_pos = remainingDelta.find(thought_end_marker);
        if (end_pos != std::string::npos) {
            // End marker found. Part is thought, rest is answer.
            std::string thought_part = remainingDelta.substr(0, end_pos);
            std::string answer_part = remainingDelta.substr(end_pos + thought_end_marker.length());

            // Thought text: plain formatting (no markdown)
            AppendFormattedText(thought_part, m_thoughtColor);
            m_isInThoughtBlock = false;

            // Answer text: rendered with markdown
            if (!answer_part.empty()) {
                m_markdownRenderer->ProcessDelta(answer_part, m_assistantColor);
            }
        }
        else {
            // Still in thought block, no end marker in this delta
            AppendFormattedText(remainingDelta, m_thoughtColor);
        }
    }
    else {
        // Normal answer text — render with markdown formatting
        m_markdownRenderer->ProcessDelta(remainingDelta, m_assistantColor);
    }

    EnsureVisibleAtEnd();
}

void ChatDisplay::DisplayAssistantComplete()
{
    // Flush any remaining buffered text in the markdown renderer
    if (m_isInThoughtBlock) {
        // Message ended while still in thought block (unusual but handle it)
        AppendFormattedText("\n\n", m_thoughtColor);
    }
    else {
        m_markdownRenderer->Flush(m_assistantColor);
        AppendFormattedText("\n\n", m_assistantColor);
    }

    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
}

void ChatDisplay::Clear()
{
    if (m_displayCtrl) {
        m_displayCtrl->Clear();
    }
    if (m_markdownRenderer) {
        m_markdownRenderer->Reset();
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
    m_userColor      = theme.chatUser;
    m_assistantColor = theme.chatAssistant;
    m_systemColor    = theme.chatSystem;
    m_thoughtColor   = theme.chatThought;

    if (m_markdownRenderer) {
        m_markdownRenderer->SetCodeColor(theme.mdCode);
        m_markdownRenderer->SetHeadingColor(theme.mdHeading);
        m_markdownRenderer->SetCodeLabelColor(theme.mdCodeLabel);
        m_markdownRenderer->SetHorizontalRuleColor(theme.mdHorizontalRule);
    }
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
