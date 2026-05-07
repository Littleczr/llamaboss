// theme.h
// Centralized theme definitions for LlamaBoss.
// ThemeData holds every color used across the UI.
// ThemeManager provides preset themes and tracks the active selection.

#pragma once

#include <wx/colour.h>
#include <string>

// ═══════════════════════════════════════════════════════════════════
//  ThemeData — every color the application uses, in one place
// ═══════════════════════════════════════════════════════════════════

struct ThemeData
{
    std::string name;   // "dark" or "light"

    // ── Window / layout backgrounds ──────────────────────────────
    wxColour bgMain;            // Main chat area background
    wxColour bgToolbar;         // Top bar background
    wxColour bgSidebar;         // Sidebar panel background
    wxColour bgInputField;      // Text input field background
    wxColour bgInputArea;       // Container around the input row

    // ── Text colors ──────────────────────────────────────────────
    wxColour textPrimary;       // Primary text (titles, input text)
    wxColour textMuted;         // Secondary text (toolbar buttons, timestamps)

    // ── Accent / action colors ───────────────────────────────────
    wxColour accentButton;      // Send button background, status dot
    wxColour accentButtonText;  // Send button text
    wxColour stopButton;        // Stop button background
    wxColour stopButtonText;    // Stop button text

    // ── Borders / separators ─────────────────────────────────────
    wxColour borderSubtle;      // Separator lines, sidebar border

    // ── Component backgrounds ────────────────────────────────────
    wxColour modelPillBg;       // Model pill, sidebar "New Chat" button, active conversation
    wxColour sidebarHover;      // Sidebar conversation item hover
    wxColour sidebarSelected;   // Sidebar multi-select highlight

    // ── Attachment indicator ─────────────────────────────────────
    wxColour attachIndicator;   // Attachment label text color
    wxColour attachChipBg;      // Attachment chip pill background

    // ── Chat message colors ──────────────────────────────────────
    wxColour chatUser;          // User message text
    wxColour chatAssistant;     // Assistant message text (Model A / single)
    wxColour chatAssistantB;    // Assistant message text (Model B in group chat)
    wxColour chatSystem;        // System message text (italic)
    wxColour chatThought;       // <think> block text

    // ── Markdown renderer colors ─────────────────────────────────
    wxColour mdCode;            // Inline code and code block text
    wxColour mdHeading;         // Heading text
    wxColour mdCodeLabel;       // Language label on code fences
    wxColour mdHorizontalRule;  // Horizontal rule line
};

// ═══════════════════════════════════════════════════════════════════
//  ThemeManager — preset themes and active theme tracking
// ═══════════════════════════════════════════════════════════════════

class ThemeManager
{
public:
    ThemeManager();

    // Get preset theme definitions
    static ThemeData GetDarkTheme();
    static ThemeData GetLightTheme();

    // Active theme management
    void SetActiveTheme(const std::string& themeName);
    const ThemeData& GetActiveTheme() const { return m_activeTheme; }
    std::string GetActiveThemeName() const { return m_activeTheme.name; }

    // Convenience: get a theme by name ("dark", "light", or "system")
    static ThemeData GetThemeByName(const std::string& name);

    // Detect Windows dark/light preference from registry
    static std::string DetectSystemTheme();

private:
    ThemeData m_activeTheme;
};
