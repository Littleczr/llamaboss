// theme.cpp
// Preset theme definitions for LlamaBoss.

#include "theme.h"

#ifdef __WXMSW__
#include <wx/msw/registry.h>
#endif

// ═══════════════════════════════════════════════════════════════════
//  ThemeManager
// ═══════════════════════════════════════════════════════════════════

ThemeManager::ThemeManager()
    : m_activeTheme(GetDarkTheme())  // Default to dark
{
}

void ThemeManager::SetActiveTheme(const std::string& themeName)
{
    m_activeTheme = GetThemeByName(themeName);
}

ThemeData ThemeManager::GetThemeByName(const std::string& name)
{
    if (name == "system") {
        std::string resolved = DetectSystemTheme();
        ThemeData t = (resolved == "light") ? GetLightTheme() : GetDarkTheme();
        t.name = "system";  // Preserve "system" so Settings knows what's selected
        return t;
    }
    if (name == "light")
        return GetLightTheme();
    return GetDarkTheme();  // Default fallback
}

std::string ThemeManager::DetectSystemTheme()
{
#ifdef __WXMSW__
    wxRegKey key(wxRegKey::HKCU, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
    if (key.Exists() && key.HasValue("AppsUseLightTheme")) {
        long value = 1;
        key.QueryValue("AppsUseLightTheme", &value);
        return (value == 0) ? "dark" : "light";
    }
#endif
    return "dark";  // Fallback
}
// ═══════════════════════════════════════════════════════════════════
//  Dark Theme — Telegram-inspired (original LlamaBoss colors)
// ═══════════════════════════════════════════════════════════════════

ThemeData ThemeManager::GetDarkTheme()
{
    ThemeData t;
    t.name = "dark";

    // Window / layout backgrounds
    t.bgMain            = wxColour(23, 33, 43);       // #17212B
    t.bgToolbar         = wxColour(28, 40, 54);       // #1C2836
    t.bgSidebar         = wxColour(14, 22, 33);       // #0E1621
    t.bgInputField      = wxColour(36, 47, 61);       // #242F3D
    t.bgInputArea       = wxColour(23, 33, 43);       // Same as main

    // Text
    t.textPrimary       = wxColour(245, 245, 245);    // #F5F5F5
    t.textMuted         = wxColour(109, 127, 142);    // #6D7F8E

    // Accent / action
    t.accentButton      = wxColour(94, 181, 247);     // #5EB5F7
    t.accentButtonText  = wxColour(255, 255, 255);    // White
    t.stopButton        = wxColour(180, 60, 60);      // Red
    t.stopButtonText    = wxColour(255, 255, 255);    // White

    // Borders
    t.borderSubtle      = wxColour(43, 56, 69);       // #2B3845

    // Component backgrounds
    t.modelPillBg       = wxColour(43, 82, 120);      // #2B5278
    t.sidebarHover      = wxColour(24, 36, 48);       // Slightly lighter than sidebar
    t.sidebarSelected   = wxColour(32, 56, 80);       // Blue tint for multi-select

    // Attachment
    t.attachIndicator   = wxColour(94, 181, 247);     // #5EB5F7
    t.attachChipBg      = wxColour(36, 52, 68);       // #243444 subtle raised surface

    // Chat message colors
    t.chatUser          = wxColour(108, 180, 238);     // Soft blue (#6CB4EE)
    t.chatAssistant     = wxColour(125, 212, 160);     // Mint green (#7DD4A0)
    t.chatAssistantB    = wxColour(232, 168, 124);     // Warm coral (#E8A87C)
    t.chatSystem        = wxColour(136, 136, 136);     // Medium gray
    t.chatThought       = wxColour(154, 154, 154);     // Light gray

    // Markdown
    t.mdCode            = wxColour(232, 184, 77);      // Warm amber (#E8B84D)
    t.mdHeading         = wxColour(232, 232, 232);     // Near-white (#E8E8E8)
    t.mdCodeLabel       = wxColour(120, 120, 120);     // Gray
    t.mdHorizontalRule  = wxColour(80, 80, 80);        // Dark gray

    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  Light Theme — clean, readable, easy on the eyes
// ═══════════════════════════════════════════════════════════════════

ThemeData ThemeManager::GetLightTheme()
{
    ThemeData t;
    t.name = "light";

    // Window / layout backgrounds
    t.bgMain            = wxColour(255, 255, 255);     // Pure white
    t.bgToolbar         = wxColour(225, 228, 235);     // Distinct cool gray toolbar
    t.bgSidebar         = wxColour(240, 241, 244);     // Light cool gray
    t.bgInputField      = wxColour(240, 241, 244);     // Subtle gray
    t.bgInputArea       = wxColour(248, 249, 251);     // Near-white, slight tint

    // Text
    t.textPrimary       = wxColour(28, 28, 32);        // Near-black
    t.textMuted         = wxColour(80, 90, 105);       // Dark enough for toolbar icons

    // Accent / action
    t.accentButton      = wxColour(59, 130, 246);      // Solid blue
    t.accentButtonText  = wxColour(255, 255, 255);     // White
    t.stopButton        = wxColour(220, 60, 60);       // Red
    t.stopButtonText    = wxColour(255, 255, 255);     // White

    // Borders
    t.borderSubtle      = wxColour(205, 208, 214);     // Visible but soft border

    // Component backgrounds
    t.modelPillBg       = wxColour(200, 218, 245);     // Soft blue tint
    t.sidebarHover      = wxColour(228, 230, 236);     // Slightly darker than sidebar
    t.sidebarSelected   = wxColour(210, 222, 240);     // Blue-gray tint for multi-select

    // Attachment
    t.attachIndicator   = wxColour(59, 130, 246);      // Blue
    t.attachChipBg      = wxColour(225, 232, 242);     // Soft blue-gray pill

    // Chat message colors
    t.chatUser          = wxColour(20, 75, 150);        // Deep blue (strong on white)
    t.chatAssistant     = wxColour(14, 105, 52);        // Deep green (strong on white)
    t.chatAssistantB    = wxColour(168, 82, 20);        // Warm burnt orange (strong on white)
    t.chatSystem        = wxColour(115, 118, 124);      // Medium gray (readable)
    t.chatThought       = wxColour(140, 143, 150);      // Lighter gray

    // Markdown
    t.mdCode            = wxColour(152, 78, 0);         // Rich amber (readable on white)
    t.mdHeading         = wxColour(18, 18, 22);         // Near-black
    t.mdCodeLabel       = wxColour(130, 133, 140);      // Gray
    t.mdHorizontalRule  = wxColour(195, 198, 205);      // Soft gray

    return t;
}
