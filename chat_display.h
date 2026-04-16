// chat_display.h
#pragma once

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <string>
#include <vector>
#include <memory>

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
    void DisplayAssistantPrefix(const std::string& modelName);
    void DisplayAssistantPrefix(const std::string& modelName, const wxColour& accentColor);
    void DisplayAssistantDelta(const std::string& delta);
    void DisplayAssistantComplete();
    void DisplayAssistantMessage(const std::string& modelName,
        const std::string& content,
        const wxColour& accentColor);

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

    // State tracking for assistant messages
    bool m_isInThoughtBlock;            // True if we are currently printing thought text
    bool m_isFirstAssistantDelta;       // True while probing for <think> at message start
    bool m_hasRenderedAssistantContent; // True once visible assistant content has been rendered
    wxColour m_activeAssistantColor;    // Color used for the current streaming response
    std::string m_thinkProbeBuffer;     // Accumulates first few bytes to detect <think> across deltas
    std::string m_thinkEndProbeBuffer;  // Holds last 7 chars of thought text to detect </think> across deltas

    // ── Animation state ──────────────────────────────────────────
    long m_animStartPos = -1;           // Char position where animation frame begins
    bool m_animActive   = false;

    // Helper methods for formatting
    void AppendFormattedText(const std::string& text, const wxColour& color,
        bool bold = false, bool italic = false);
    void SetInsertionPointToEnd();
    void EnsureVisibleAtEnd();

    // Image thumbnail limits for inline display
    static constexpr int kImageMaxWidth  = 300;
    static constexpr int kImageMaxHeight = 300;
};

