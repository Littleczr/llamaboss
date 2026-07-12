// theme.h
// Centralized theme definitions for LlamaBoss.
// ThemeData holds every color used across the UI.
// ThemeManager provides preset themes and tracks the active selection.

#pragma once

#include <wx/colour.h>
#include <string>
#include <vector>

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
    wxColour bgDialogSurface;   // Raised modal/dialog surface background
    wxColour modalScrim;        // Tint used for translucent modal dimming overlays

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

    // ── Preset theme definitions ─────────────────────────────────
    //
    // Original two themes. "Dark" is the canonical Telegram-inspired
    // LlamaBoss palette; "Light" is the matching clean light variant.
    static ThemeData GetDarkTheme();
    static ThemeData GetLightTheme();

    // ── Designer-curated themes ──────────────────────────────────
    //
    // These map well-known published palettes to LlamaBoss's ThemeData
    // slots. Each function comments the design choices made — for example,
    // Nord publishes colors in named groups (Frost, Aurora, etc.), so the
    // mapping reads like "frost → accent, aurora red → stopButton".
    // Where a published palette doesn't have an obvious muted-text color,
    // the function lifts the comment color or picks a custom mid-tone so
    // UI labels remain readable on the surface.
    static ThemeData GetAyuMirageTheme();         // ayu-theme/ayu-colors (mirage variant)
    static ThemeData GetCobalt2Theme();           // Wes Bos Cobalt2
    static ThemeData GetDraculaTheme();           // draculatheme.com
    static ThemeData GetNordTheme();              // arcticicestudio/nord
    static ThemeData GetNordLightTheme();         // arcticicestudio/nord (bright Snow Storm variant)
    static ThemeData GetOneDarkTheme();           // Atom One Dark-inspired
    static ThemeData GetTokyoNightTheme();        // enkia/tokyo-night (storm variant)
    static ThemeData GetRosePineMoonTheme();      // rosepinetheme.com (moon)
    static ThemeData GetRosePineDawnTheme();      // rosepinetheme.com (dawn)

    // Active theme management
    void SetActiveTheme(const std::string& themeName);
    const ThemeData& GetActiveTheme() const { return m_activeTheme; }
    std::string GetActiveThemeName() const { return m_activeTheme.name; }

    // Convenience: get a theme by name. Recognized values:
    //   "dark", "light", "system",
    //   "ayu-mirage", "cobalt2", "dracula",
    //   "nord", "nord-light", "one-dark",
    //   "tokyo-night", "rose-pine-moon", "rose-pine-dawn".
    // Unknown values fall back to the dark theme.
    static ThemeData GetThemeByName(const std::string& name);

    // Detect Windows dark/light preference from registry
    static std::string DetectSystemTheme();

    // ── Settings dropdown support ────────────────────────────────
    //
    // Pairs every selectable theme with its display name, in the
    // order they should appear in the Settings dropdown. Originals
    // (Dark / Light / System) lead; designer-curated themes follow
    // alphabetically. Adding a new theme is a one-line entry here
    // plus a Get<Name>Theme() above — no Settings code touches.
    struct ThemeChoice
    {
        std::string internalName;   // Persisted form, e.g. "tokyo-night"
        std::string displayName;    // Shown in the dropdown, e.g. "Tokyo Night"
    };
    static const std::vector<ThemeChoice>& GetThemeChoices();

private:
    ThemeData m_activeTheme;
};
