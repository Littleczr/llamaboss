// theme.h
// Centralized theme definitions for LlamaBoss.
// ThemeData holds every color used across the UI.
// ThemeManager provides preset themes and tracks the active selection.

#pragma once

#include <wx/colour.h>
#include <cmath>
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

// Hover / "this is clickable" highlight used by every toolbar control
// (hamburger, +, settings cogwheel, about, model pill, thinking chip,
// [ Project ] / [ Skills ], attach, agent toggle).  One rule, one place:
// the original LlamaBoss Dark theme uses its mint assistant colour; every
// other theme uses its palette accent, because in those themes
// chatAssistant is plain foreground text and would barely change on hover.
inline const wxColour& LbInteractiveAccent(const ThemeData& theme)
{
    return (theme.name == "dark") ? theme.chatAssistant : theme.accentButton;
}

// Steps a text colour away from bg (toward black on light backgrounds,
// white on dark) until it reaches 4.5:1. Colours that already pass come
// back unchanged.
inline wxColour LbReadableOn(wxColour c, const wxColour& bgColour)
{
    auto lum = [](const wxColour& k) {
        auto ch = [](unsigned char v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * ch(k.Red()) + 0.7152 * ch(k.Green()) + 0.0722 * ch(k.Blue());
    };
    const double bg = lum(bgColour);
    const int toward = bg > 0.5 ? 0 : 255;
    for (int step = 0; step < 20; ++step) {
        const double fg = lum(c);
        const double hi = fg > bg ? fg : bg, lo = fg > bg ? bg : fg;  // no std::max: Windows macros
        if ((hi + 0.05) / (lo + 0.05) >= 4.5) break;
        auto mix = [toward](unsigned char v) {
            return static_cast<unsigned char>((v * 92 + toward * 8) / 100);
        };
        c = wxColour(mix(c.Red()), mix(c.Green()), mix(c.Blue()));
    }
    return c;
}

// Code-block [  Copy  ]: bold, in the theme's accent so it reads as THE
// action in a code box.  Dark's accent is the mint its replies are written
// in, so Dark uses the soft blue instead.  Light palettes' accents are
// button fills and get darkened to read as text.
inline wxColour LbTranscriptActionColour(const ThemeData& theme)
{
    return LbReadableOn((theme.name == "dark") ? wxColour(170, 190, 230)
                                               : LbInteractiveAccent(theme),
                        theme.bgMain);
}

// Quieter click targets in the transcript: tool-card [show details] /
// [hide details] and chevron, file-chip actions, approval buttons.  The
// long-standing soft blue (170,190,230), exact on every dark theme;
// darkened on light themes, where it was ~1.7:1 and near-invisible.
inline wxColour LbTranscriptLinkColour(const ThemeData& theme)
{
    return LbReadableOn(wxColour(170, 190, 230), theme.bgMain);
}

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
    static ThemeData GetRetroHiFiTheme();        // Charcoal, cyan VFD, warm amber

    // Active theme management
    void SetActiveTheme(const std::string& themeName);
    const ThemeData& GetActiveTheme() const { return m_activeTheme; }
    std::string GetActiveThemeName() const { return m_activeTheme.name; }

    // Convenience: get a theme by name. Recognized values:
    //   "dark", "light", "system",
    //   "ayu-mirage", "cobalt2", "dracula",
    //   "nord", "nord-light", "one-dark",
    //   "tokyo-night", "rose-pine-moon", "rose-pine-dawn", "retro-hi-fi".
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
