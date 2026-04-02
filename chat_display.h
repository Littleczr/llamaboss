// chat_display.h
#ifndef CHAT_DISPLAY_H
#define CHAT_DISPLAY_H

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <string>

// Forward declarations
class MarkdownRenderer;
struct ThemeData;

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
    void DisplayUserMessage(const std::string& text);
    void DisplaySystemMessage(const std::string& text);
    void DisplayAssistantPrefix(const std::string& modelName);
    void DisplayAssistantDelta(const std::string& delta);
    void DisplayAssistantComplete();

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
    MarkdownRenderer* m_markdownRenderer;

    // Colors for different message types
    wxColour m_userColor;
    wxColour m_assistantColor;
    wxColour m_systemColor;
    wxColour m_thoughtColor;

    // State tracking for assistant messages
    bool m_isInThoughtBlock;      // True if we are currently printing thought text
    bool m_isFirstAssistantDelta; // True for the very first chunk of an assistant message

    // Helper methods for formatting
    void AppendFormattedText(const std::string& text, const wxColour& color,
        bool bold = false, bool italic = false);
    void SetInsertionPointToEnd();
    void EnsureVisibleAtEnd();
};

#endif // CHAT_DISPLAY_H
