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

        // Keep System visually distinct from the explicit Dark preset.
        // Dark preserves the original LlamaBoss mint-green assistant voice,
        // while System-on-dark follows the OS dark surface but uses the
        // normal foreground color for assistant body text.  If Windows is in
        // light mode, keep the light theme's existing readable dark text.
        if (resolved != "light") {
            t.chatAssistant = t.textPrimary;
        }

        t.name = "system";  // Preserve "system" so Settings knows what's selected
        return t;
    }
    if (name == "light")             return GetLightTheme();
    if (name == "ayu-mirage")        return GetAyuMirageTheme();
    if (name == "cobalt2")           return GetCobalt2Theme();
    if (name == "dracula")           return GetDraculaTheme();
    if (name == "nord")              return GetNordTheme();
    if (name == "nord-light")        return GetNordLightTheme();
    if (name == "one-dark")          return GetOneDarkTheme();
    if (name == "rose-pine-moon")    return GetRosePineMoonTheme();
    if (name == "rose-pine-dawn")    return GetRosePineDawnTheme();
    if (name == "tokyo-night")       return GetTokyoNightTheme();
    return GetDarkTheme();  // Default fallback for unrecognized names
}

// Ordered list of every theme the Settings dropdown should offer.
// Originals (Dark / Light / System) lead so users on the existing
// installs still see what they expect at the top; designer-curated
// themes follow alphabetically. Add a new entry here AND a
// Get<Name>Theme() function below to introduce a new theme.
const std::vector<ThemeManager::ThemeChoice>&
ThemeManager::GetThemeChoices()
{
    static const std::vector<ThemeChoice> kChoices = {
        { "dark",             "Dark"             },
        { "light",            "Light"            },
        { "system",           "System"           },
        { "ayu-mirage",       "Ayu Mirage"       },
        { "cobalt2",          "Cobalt2"          },
        { "dracula",          "Dracula"          },
        { "nord",             "Nord"             },
        { "nord-light",       "Nord Light"       },
        { "one-dark",         "One Dark"         },
        { "rose-pine-moon",   "Rose Pine Moon"   },
        { "rose-pine-dawn",   "Rose Pine Dawn"   },
        { "tokyo-night",      "Tokyo Night"      },
    };
    return kChoices;
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
    t.bgMain            = wxColour(14, 22, 33);       // #0E1621
    t.bgToolbar         = wxColour(23, 33, 43);       //
    t.bgSidebar         = wxColour(14, 22, 33);       // #0E1621
    t.bgInputField      = wxColour(36, 47, 61);       // #242F3D
    t.bgInputArea       = wxColour(23, 33, 43);       // #17212B — composer/footer surface
    t.bgDialogSurface   = wxColour(23, 33, 43);       // #17212B — Settings/modal surface
    t.modalScrim        = wxColour(0, 0, 0);          // Black backdrop; alpha applied by caller

    // Text
    t.textPrimary       = wxColour(245, 245, 245);    // #F5F5F5
    t.textMuted         = wxColour(109, 127, 142);    // #6D7F8E

    // Accent / action
    t.accentButton      = wxColour(43, 136, 216);     // #2B88D8 — richer, true-blue accent (was #5EB5F7)
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
    t.bgDialogSurface   = wxColour(255, 255, 255);     // Keep modal surfaces bright in light mode
    t.modalScrim        = wxColour(0, 0, 0);          // Black backdrop; alpha applied by caller

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

// ═══════════════════════════════════════════════════════════════════
//  Nord — arctic-inspired palette by Arctic Ice Studio
// ═══════════════════════════════════════════════════════════════════
//
//  https://www.nordtheme.com
//
//  Color groups (using Nord's own names):
//    Polar Night — backgrounds (nord0..nord3, dark to mid)
//    Snow Storm  — text (nord4..nord6, mid to light)
//    Frost       — accent / informational blues (nord7..nord10)
//    Aurora      — semantic colors (nord11 red, nord12 orange,
//                                   nord13 yellow, nord14 green,
//                                   nord15 purple)
//
//  Mapping notes:
//    • accentButton uses Frost 1 (#88C0D0) — the most "Nord" feel,
//      cyan-leaning blue
//    • assistant body text follows Snow Storm instead of Aurora green;
//      Aurora remains reserved for semantic accents like stop/orange/model-B
//    • all Nord-dark slots below use published Nord palette colors rather
//      than lifted/custom mid-tones
ThemeData ThemeManager::GetNordTheme()
{
    ThemeData t;
    t.name = "nord";

    // Backgrounds — Polar Night
    t.bgMain            = wxColour(46, 52, 64);      // #2E3440 nord0
    t.bgSidebar         = wxColour(46, 52, 64);      // #2E3440 nord0
    t.bgToolbar         = wxColour(59, 66, 82);      // #3B4252 nord1
    t.bgDialogSurface   = wxColour(59, 66, 82);      // #3B4252 nord1 — modal surface lifts above main
    t.bgInputField      = wxColour(67, 76, 94);      // #434C5E nord2
    t.bgInputArea       = wxColour(59, 66, 82);      // #3B4252 nord1
    t.modalScrim        = wxColour(0, 0, 0);

    // Text — Snow Storm
    t.textPrimary       = wxColour(236, 239, 244);   // #ECEFF4 nord6
    t.textMuted         = wxColour(216, 222, 233);   // #D8DEE9 nord4 — subtle Snow Storm text

    // Accent / action
    t.accentButton      = wxColour(136, 192, 208);   // #88C0D0 frost (nord8)
    t.accentButtonText  = wxColour(46, 52, 64);      // #2E3440 — dark on light blue
    t.stopButton        = wxColour(191, 97, 106);    // #BF616A aurora red (nord11)
    t.stopButtonText    = wxColour(236, 239, 244);   // #ECEFF4

    // Borders
    t.borderSubtle      = wxColour(67, 76, 94);      // #434C5E nord2

    // Component backgrounds
    t.modelPillBg       = wxColour(94, 129, 172);    // #5E81AC frost deep (nord10)
    t.sidebarHover      = wxColour(59, 66, 82);      // #3B4252 nord1
    t.sidebarSelected   = wxColour(67, 76, 94);      // #434C5E nord2

    // Attachment
    t.attachIndicator   = wxColour(136, 192, 208);   // #88C0D0 frost (nord8)
    t.attachChipBg      = wxColour(67, 76, 94);      // #434C5E nord2

    // Chat message colors — Frost / Snow Storm / Aurora
    t.chatUser          = wxColour(129, 161, 193);   // #81A1C1 nord9 frost blue
    t.chatAssistant     = wxColour(236, 239, 244);   // #ECEFF4 nord6 plain assistant text
    t.chatAssistantB    = wxColour(208, 135, 112);   // #D08770 nord12 aurora orange for Model B
    t.chatSystem        = wxColour(216, 222, 233);   // #D8DEE9 nord4 muted Snow Storm
    t.chatThought       = wxColour(76, 86, 106);     // #4C566A nord3 comment/dim text

    // Markdown
    t.mdCode            = wxColour(235, 203, 139);   // #EBCB8B aurora yellow (nord13)
    t.mdHeading         = wxColour(236, 239, 244);   // #ECEFF4 nord6
    t.mdCodeLabel       = wxColour(76, 86, 106);     // #4C566A nord3 comment/dim text
    t.mdHorizontalRule  = wxColour(67, 76, 94);      // #434C5E nord2

    return t;
}


// ═══════════════════════════════════════════════════════════════════
//  Nord Light — bright Snow Storm variant using the Nord palette
// ═══════════════════════════════════════════════════════════════════
//
//  Nord's Snow Storm colors make good light surfaces, while Frost and
//  Aurora still provide the same blue/green/orange identity as the dark
//  Nord theme.  A few chat colors are slightly darkened versions of the
//  published accents so message text stays readable on bright surfaces.
ThemeData ThemeManager::GetNordLightTheme()
{
    ThemeData t;
    t.name = "nord-light";

    // Backgrounds — Snow Storm
    t.bgMain            = wxColour(236, 239, 244);   // #ECEFF4 nord6
    t.bgSidebar         = wxColour(229, 233, 240);   // #E5E9F0 nord5
    t.bgToolbar         = wxColour(216, 222, 233);   // #D8DEE9 nord4
    t.bgDialogSurface   = wxColour(229, 233, 240);   // #E5E9F0 nord5
    t.bgInputField      = wxColour(255, 255, 255);   // White input well for contrast
    t.bgInputArea       = wxColour(216, 222, 233);   // #D8DEE9 nord4
    t.modalScrim        = wxColour(0, 0, 0);

    // Text — Polar Night
    t.textPrimary       = wxColour(46, 52, 64);      // #2E3440 nord0
    t.textMuted         = wxColour(76, 86, 106);     // #4C566A nord3

    // Accent / action — Frost / Aurora
    t.accentButton      = wxColour(94, 129, 172);    // #5E81AC nord10, stronger on light surfaces
    t.accentButtonText  = wxColour(255, 255, 255);   // White
    t.stopButton        = wxColour(191, 97, 106);    // #BF616A nord11
    t.stopButtonText    = wxColour(255, 255, 255);   // White

    // Borders
    t.borderSubtle      = wxColour(180, 188, 202);   // Softened Snow Storm edge

    // Component backgrounds
    t.modelPillBg       = wxColour(216, 229, 238);   // Pale Frost-tinted pill
    t.sidebarHover      = wxColour(216, 222, 233);   // #D8DEE9 nord4
    t.sidebarSelected   = wxColour(196, 213, 229);   // Light Frost selection tint

    // Attachment
    t.attachIndicator   = wxColour(94, 129, 172);    // #5E81AC nord10
    t.attachChipBg      = wxColour(216, 229, 238);   // Pale Frost-tinted chip

    // Chat message colors — contrast-tuned Frost / Aurora
    t.chatUser          = wxColour(69, 104, 150);    // Darkened Frost blue
    t.chatAssistant     = wxColour(46, 52, 64);      // #2E3440 nord0 plain assistant text
    t.chatAssistantB    = wxColour(160, 89, 58);     // Darkened Aurora orange
    t.chatSystem        = wxColour(94, 104, 124);    // Muted Polar Night
    t.chatThought       = wxColour(112, 122, 142);   // Softer muted text

    // Markdown
    t.mdCode            = wxColour(143, 103, 0);     // Darkened Aurora yellow
    t.mdHeading         = wxColour(46, 52, 64);      // #2E3440 nord0
    t.mdCodeLabel       = wxColour(94, 104, 124);    // Muted Polar Night
    t.mdHorizontalRule  = wxColour(180, 188, 202);   // Softened Snow Storm edge

    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  Ayu Mirage — soft, warm dark variant of ayu
// ═══════════════════════════════════════════════════════════════════
//
//  https://github.com/ayu-theme/ayu-colors
//
//  Ayu's official palette exposes separate ui/editor/common groups.
//  This maps editor.bg/ui.bg into the app surfaces, editor.fg into
//  body text, common.accent into primary actions, and syntax colors
//  into secondary chat/markdown accents.
ThemeData ThemeManager::GetAyuMirageTheme()
{
    ThemeData t;
    t.name = "ayu-mirage";

    // Backgrounds
    t.bgMain            = wxColour(36, 41, 54);      // #242936 editor.bg
    t.bgSidebar         = wxColour(31, 36, 48);      // #1F2430 ui.bg
    t.bgToolbar         = wxColour(31, 36, 48);      // #1F2430 ui.bg
    t.bgDialogSurface   = wxColour(36, 41, 54);      // #242936 editor.bg
    t.bgInputField      = wxColour(28, 33, 43);      // #1C212B panel.bg
    t.bgInputArea       = wxColour(31, 36, 48);      // #1F2430 ui.bg
    t.modalScrim        = wxColour(0, 0, 0);

    // Text
    t.textPrimary       = wxColour(204, 202, 194);   // #CCCAC2 editor.fg
    t.textMuted         = wxColour(112, 122, 140);   // #707A8C ui.fg

    // Accent / action
    t.accentButton      = wxColour(255, 204, 102);   // #FFCC66 common.accent
    t.accentButtonText  = wxColour(31, 36, 48);      // #1F2430 ui.bg
    t.stopButton        = wxColour(255, 102, 102);   // #FF6666 common.error
    t.stopButtonText    = wxColour(31, 36, 48);      // #1F2430 ui.bg

    // Borders
    t.borderSubtle      = wxColour(54, 60, 74);      // fitted between ui.bg and ui.fg

    // Component backgrounds
    t.modelPillBg       = wxColour(64, 76, 94);      // soft selected blue-gray
    t.sidebarHover      = wxColour(42, 49, 64);      // raised surface
    t.sidebarSelected   = wxColour(53, 64, 87);      // selected surface

    // Attachment
    t.attachIndicator   = wxColour(115, 208, 255);   // #73D0FF syntax.entity
    t.attachChipBg      = wxColour(42, 49, 64);      // raised surface

    // Chat — assistant body text stays on Ayu foreground; bright syntax colors stay accents.
    t.chatUser          = wxColour(115, 208, 255);   // #73D0FF syntax.entity
    t.chatAssistant     = wxColour(204, 202, 194);   // #CCCAC2 editor.fg
    t.chatAssistantB    = wxColour(255, 209, 115);   // #FFD173 syntax.func
    t.chatSystem        = wxColour(112, 122, 140);   // #707A8C ui.fg
    t.chatThought       = wxColour(92, 103, 115);    // dimmed comment approximation

    // Markdown
    t.mdCode            = wxColour(255, 204, 102);   // #FFCC66 common.accent
    t.mdHeading         = wxColour(92, 207, 230);    // #5CCFE6 syntax.tag
    t.mdCodeLabel       = wxColour(112, 122, 140);   // #707A8C ui.fg
    t.mdHorizontalRule  = wxColour(54, 60, 74);      // subtle divider

    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  Cobalt2 — Wes Bos's bold blue/yellow Sublime theme
// ═══════════════════════════════════════════════════════════════════
//
//  https://packagecontrol.io/packages/Theme%20-%20Cobalt2
//
//  Official colors include Blue (#193549), Off Blue (#0D3A58),
//  Dark Blue (#15232D), Dusty Blue (#35434d), Yellow (#ffc600),
//  Mint (#2AFFDF), and Light Blue (#9EFFFF).  This uses the deep
//  blues for surfaces and yellow as the primary action accent.
ThemeData ThemeManager::GetCobalt2Theme()
{
    ThemeData t;
    t.name = "cobalt2";

    // Backgrounds
    t.bgMain            = wxColour(25, 53, 73);      // #193549 blue
    t.bgSidebar         = wxColour(21, 35, 45);      // #15232D dark blue
    t.bgToolbar         = wxColour(21, 35, 45);      // #15232D dark blue
    t.bgDialogSurface   = wxColour(25, 53, 73);      // #193549 blue
    t.bgInputField      = wxColour(13, 58, 88);      // #0D3A58 off blue
    t.bgInputArea       = wxColour(21, 35, 45);      // #15232D dark blue
    t.modalScrim        = wxColour(0, 0, 0);

    // Text
    t.textPrimary       = wxColour(255, 255, 255);   // crisp foreground on cobalt blue
    t.textMuted         = wxColour(158, 255, 255);   // #9EFFFF light blue

    // Accent / action
    t.accentButton      = wxColour(255, 198, 0);     // #ffc600 yellow
    t.accentButtonText  = wxColour(21, 35, 45);      // #15232D dark blue
    t.stopButton        = wxColour(255, 157, 0);     // #FF9D00 orange
    t.stopButtonText    = wxColour(21, 35, 45);      // #15232D dark blue

    // Borders
    t.borderSubtle      = wxColour(53, 67, 77);      // #35434d dusty blue

    // Component backgrounds
    t.modelPillBg       = wxColour(13, 58, 88);      // #0D3A58 off blue
    t.sidebarHover      = wxColour(13, 58, 88);      // #0D3A58 off blue
    t.sidebarSelected   = wxColour(53, 67, 77);      // #35434d dusty blue

    // Attachment
    t.attachIndicator   = wxColour(42, 255, 223);    // #2AFFDF mint
    t.attachChipBg      = wxColour(13, 58, 88);      // #0D3A58 off blue

    // Chat — assistant body text remains white; Cobalt colors become accents.
    t.chatUser          = wxColour(158, 255, 255);   // #9EFFFF light blue
    t.chatAssistant     = wxColour(255, 255, 255);   // white foreground
    t.chatAssistantB    = wxColour(255, 198, 0);     // #ffc600 yellow
    t.chatSystem        = wxColour(158, 255, 255);   // #9EFFFF light blue
    t.chatThought       = wxColour(99, 142, 160);    // dimmed light-blue approximation

    // Markdown
    t.mdCode            = wxColour(255, 198, 0);     // #ffc600 yellow
    t.mdHeading         = wxColour(42, 255, 223);    // #2AFFDF mint
    t.mdCodeLabel       = wxColour(158, 255, 255);   // #9EFFFF light blue
    t.mdHorizontalRule  = wxColour(53, 67, 77);      // #35434d dusty blue

    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  One Dark — Atom-inspired developer classic
// ═══════════════════════════════════════════════════════════════════
//
//  https://github.com/joshdick/onedark.vim
//
//  The palette reference measures Atom One Dark's core colors:
//  background #282c34, foreground #abb2bf, comment #5c6370,
//  blue #61afef, green #98c379, yellow #e5c07b, red #e06c75,
//  magenta #c678dd, and cyan #56b6c2.
ThemeData ThemeManager::GetOneDarkTheme()
{
    ThemeData t;
    t.name = "one-dark";

    // Backgrounds
    t.bgMain            = wxColour(40, 44, 52);      // #282c34 background
    t.bgSidebar         = wxColour(33, 37, 43);      // #21252b Atom UI side surface
    t.bgToolbar         = wxColour(33, 37, 43);      // #21252b Atom UI side surface
    t.bgDialogSurface   = wxColour(44, 50, 60);      // #2c323c cursor gray
    t.bgInputField      = wxColour(62, 68, 82);      // #3e4452 menu/visual gray
    t.bgInputArea       = wxColour(33, 37, 43);      // #21252b Atom UI side surface
    t.modalScrim        = wxColour(0, 0, 0);

    // Text
    t.textPrimary       = wxColour(171, 178, 191);   // #abb2bf foreground/white
    t.textMuted         = wxColour(92, 99, 112);     // #5c6370 comment gray

    // Accent / action
    t.accentButton      = wxColour(97, 175, 239);    // #61afef blue
    t.accentButtonText  = wxColour(40, 44, 52);      // #282c34 background
    t.stopButton        = wxColour(224, 108, 117);   // #e06c75 red
    t.stopButtonText    = wxColour(40, 44, 52);      // #282c34 background

    // Borders
    t.borderSubtle      = wxColour(62, 68, 82);      // #3e4452 menu/visual gray

    // Component backgrounds
    t.modelPillBg       = wxColour(62, 68, 82);      // #3e4452 menu/visual gray
    t.sidebarHover      = wxColour(44, 50, 60);      // #2c323c cursor gray
    t.sidebarSelected   = wxColour(62, 68, 82);      // #3e4452 menu/visual gray

    // Attachment
    t.attachIndicator   = wxColour(86, 182, 194);    // #56b6c2 cyan
    t.attachChipBg      = wxColour(59, 64, 72);      // #3b4048 special gray

    // Chat — assistant body text uses foreground; syntax colors remain accents.
    t.chatUser          = wxColour(97, 175, 239);    // #61afef blue
    t.chatAssistant     = wxColour(171, 178, 191);   // #abb2bf foreground
    t.chatAssistantB    = wxColour(229, 192, 123);   // #e5c07b yellow
    t.chatSystem        = wxColour(92, 99, 112);     // #5c6370 comment gray
    t.chatThought       = wxColour(92, 99, 112);     // #5c6370 comment gray

    // Markdown
    t.mdCode            = wxColour(229, 192, 123);   // #e5c07b yellow
    t.mdHeading         = wxColour(198, 120, 221);   // #c678dd magenta
    t.mdCodeLabel       = wxColour(92, 99, 112);     // #5c6370 comment gray
    t.mdHorizontalRule  = wxColour(62, 68, 82);      // #3e4452 menu/visual gray

    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  Dracula — vibrant accent palette by Zeno Rocha
// ═══════════════════════════════════════════════════════════════════
//
//  https://draculatheme.com/contribute (official spec)
//
//  Dracula is famously vivid — 8 high-saturation accent colors over
//  a single near-black background. Purple is the signature accent;
//  pink/cyan/green/orange/yellow fill out the chat + markdown roles.
//  The published comment color (#6272A4 muted blue) is the canonical
//  textMuted — Dracula intentionally designed it with enough contrast
//  on the #282A36 background to be the default "low-emphasis" tone.
ThemeData ThemeManager::GetDraculaTheme()
{
    ThemeData t;
    t.name = "dracula";

    // Backgrounds
    t.bgMain            = wxColour(40, 42, 54);      // #282A36 background
    t.bgSidebar         = wxColour(40, 42, 54);      // #282A36
    t.bgToolbar         = wxColour(45, 47, 61);      // slightly lifted
    t.bgDialogSurface   = wxColour(45, 47, 61);      // slightly lifted
    t.bgInputField      = wxColour(68, 71, 90);      // #44475A current line
    t.bgInputArea       = wxColour(45, 47, 61);      // slightly lifted
    t.modalScrim        = wxColour(0, 0, 0);

    // Text
    t.textPrimary       = wxColour(248, 248, 242);   // #F8F8F2 foreground
    t.textMuted         = wxColour(98, 114, 164);    // #6272A4 comment — Dracula's canonical muted

    // Accent — purple is Dracula's signature
    t.accentButton      = wxColour(189, 147, 249);   // #BD93F9 purple
    t.accentButtonText  = wxColour(40, 42, 54);      // #282A36 dark on light purple
    t.stopButton        = wxColour(255, 85, 85);     // #FF5555 red
    t.stopButtonText    = wxColour(248, 248, 242);   // #F8F8F2

    // Borders
    t.borderSubtle      = wxColour(68, 71, 90);      // #44475A current line

    // Component backgrounds
    t.modelPillBg       = wxColour(68, 71, 90);      // #44475A current line
    t.sidebarHover      = wxColour(45, 47, 61);      // lifted
    t.sidebarSelected   = wxColour(56, 58, 71);      // between current-line and bg

    // Attachment
    t.attachIndicator   = wxColour(139, 233, 253);   // #8BE9FD cyan
    t.attachChipBg      = wxColour(68, 71, 90);      // #44475A

    // Chat — assistant body text uses Dracula Foreground; vivid colors stay accents.
    t.chatUser          = wxColour(139, 233, 253);   // #8BE9FD cyan
    t.chatAssistant     = wxColour(248, 248, 242);   // #F8F8F2 foreground plain assistant text
    t.chatAssistantB    = wxColour(255, 184, 108);   // #FFB86C orange
    t.chatSystem        = wxColour(98, 114, 164);    // #6272A4 comment
    t.chatThought       = wxColour(98, 114, 164);    // #6272A4

    // Markdown
    t.mdCode            = wxColour(241, 250, 140);   // #F1FA8C yellow
    t.mdHeading         = wxColour(255, 121, 198);   // #FF79C6 pink
    t.mdCodeLabel       = wxColour(98, 114, 164);    // #6272A4
    t.mdHorizontalRule  = wxColour(68, 71, 90);      // #44475A

    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  Tokyo Night (Storm) — by enkia
// ═══════════════════════════════════════════════════════════════════
//
//  https://github.com/enkia/tokyo-night-vscode-theme
//
//  Storm is the most popular Tokyo Night variant — a bit lighter than
//  the "Night" variant, with the same blue/purple-leaning accents.
//  Tokyo Night's signature is its calm cool blues (#7aa2f7 accent,
//  #7dcfff cyan) paired with mid-saturation greens/oranges. Muted
//  text uses the official "dark5" stop (#737aa2) which has good
//  contrast on #1a1b26.
//
//  LlamaBoss mapping note: after visual testing, Tokyo Night reads best
//  here with plain white app/window/chat text over the blue-purple surfaces.
//  The official Tokyo Night blue/cyan/yellow accents remain for buttons,
//  attachment affordances, and inline code, but normal body text is kept
//  white so the UI feels crisp instead of washed-out.
//
ThemeData ThemeManager::GetTokyoNightTheme()
{
    ThemeData t;
    t.name = "tokyo-night";

    // Backgrounds
    t.bgMain            = wxColour(26, 27, 38);      // #1a1b26 bg
    t.bgSidebar         = wxColour(22, 22, 30);      // #16161e bg_dark
    t.bgToolbar         = wxColour(31, 32, 48);      // between bg and bg_highlight
    t.bgDialogSurface   = wxColour(31, 32, 48);
    t.bgInputField      = wxColour(41, 46, 66);      // #292e42 bg_highlight
    t.bgInputArea       = wxColour(22, 22, 30);      // #16161e bg_dark
    t.modalScrim        = wxColour(0, 0, 0);

    // Text
    t.textPrimary       = wxColour(255, 255, 255);   // #FFFFFF plain white window/chat text
    t.textMuted         = wxColour(192, 202, 245);   // #c0caf5 soft secondary text

    // Accent — signature Tokyo Night blue
    t.accentButton      = wxColour(122, 162, 247);   // #7aa2f7 blue
    t.accentButtonText  = wxColour(26, 27, 38);      // #1a1b26 — dark on light blue
    t.stopButton        = wxColour(247, 118, 142);   // #f7768e red
    t.stopButtonText    = wxColour(255, 255, 255);   // #FFFFFF

    // Borders
    t.borderSubtle      = wxColour(59, 66, 97);      // #3b4261 fg_gutter

    // Component backgrounds
    t.modelPillBg       = wxColour(57, 75, 112);     // #394b70 blue7 (deep blue panel)
    t.sidebarHover      = wxColour(41, 46, 66);      // #292e42 bg_highlight
    t.sidebarSelected   = wxColour(59, 66, 97);      // #3b4261 fg_gutter

    // Attachment
    t.attachIndicator   = wxColour(125, 207, 255);   // #7dcfff cyan
    t.attachChipBg      = wxColour(41, 46, 66);      // #292e42

    // Chat — keep the chat body plain white for readability.
    // Tokyo Night still keeps its blue/purple surfaces and accent buttons,
    // but normal conversation text no longer competes with cyan/yellow
    // syntax colors. Thought/detail text stays slightly softer.
    t.chatUser          = wxColour(255, 255, 255);   // #FFFFFF plain chat text
    t.chatAssistant     = wxColour(255, 255, 255);   // #FFFFFF plain chat text
    t.chatAssistantB    = wxColour(255, 255, 255);   // #FFFFFF plain chat text
    t.chatSystem        = wxColour(255, 255, 255);   // #FFFFFF tool/system text
    t.chatThought       = wxColour(192, 202, 245);   // #c0caf5 softer detail/thought text

    // Markdown
    t.mdCode            = wxColour(224, 175, 104);   // #e0af68 yellow
    t.mdHeading         = wxColour(255, 255, 255);   // #FFFFFF headings
    t.mdCodeLabel       = wxColour(192, 202, 245);   // #c0caf5 soft code label
    t.mdHorizontalRule  = wxColour(41, 46, 66);      // #292e42

    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  Rose Pine Moon — the mid-dark variant of Rose Pine
// ═══════════════════════════════════════════════════════════════════
//
//  Moon is slightly lighter than the main Rose Pine, with cooler
//  blue-leaning backgrounds and a couple of subtly different accent
//  shades (rose shifts warmer to #ea9a97, pine brightens to #3e8fb0).
//  Same role mapping as Rose Pine — Iris on accent, Love on stop.
ThemeData ThemeManager::GetRosePineMoonTheme()
{
    ThemeData t;
    t.name = "rose-pine-moon";

    // Backgrounds
    t.bgMain            = wxColour(35, 33, 54);      // #232136 base
    t.bgSidebar         = wxColour(42, 39, 63);      // #2a273f surface
    t.bgToolbar         = wxColour(42, 39, 63);      // #2a273f surface
    t.bgDialogSurface   = wxColour(42, 39, 63);      // #2a273f surface
    t.bgInputField      = wxColour(57, 53, 82);      // #393552 overlay
    t.bgInputArea       = wxColour(42, 39, 63);      // #2a273f surface
    t.modalScrim        = wxColour(0, 0, 0);

    // Text
    t.textPrimary       = wxColour(224, 222, 244);   // #e0def4 text
    t.textMuted         = wxColour(144, 140, 170);   // #908caa subtle

    // Accent — Iris
    t.accentButton      = wxColour(196, 167, 231);   // #c4a7e7 iris
    t.accentButtonText  = wxColour(35, 33, 54);      // #232136 base
    t.stopButton        = wxColour(235, 111, 146);   // #eb6f92 love
    t.stopButtonText    = wxColour(224, 222, 244);   // #e0def4

    // Borders
    t.borderSubtle      = wxColour(57, 53, 82);      // #393552 overlay

    // Component backgrounds
    t.modelPillBg       = wxColour(62, 143, 176);    // #3e8fb0 pine (brighter in moon)
    t.sidebarHover      = wxColour(57, 53, 82);      // #393552 overlay
    t.sidebarSelected   = wxColour(68, 65, 90);      // slightly above overlay

    // Attachment
    t.attachIndicator   = wxColour(156, 207, 216);   // #9ccfd8 foam
    t.attachChipBg      = wxColour(57, 53, 82);      // #393552 overlay

    // Chat — assistant body text uses Rose Pine Moon text; rose/gold/foam stay accents.
    t.chatUser          = wxColour(156, 207, 216);   // #9ccfd8 foam
    t.chatAssistant     = wxColour(224, 222, 244);   // #e0def4 text plain assistant text
    t.chatAssistantB    = wxColour(246, 193, 119);   // #f6c177 gold
    t.chatSystem        = wxColour(110, 106, 134);   // #6e6a86 muted
    t.chatThought       = wxColour(110, 106, 134);   // #6e6a86 muted

    // Markdown
    t.mdCode            = wxColour(246, 193, 119);   // #f6c177 gold
    t.mdHeading         = wxColour(196, 167, 231);   // #c4a7e7 iris
    t.mdCodeLabel       = wxColour(110, 106, 134);   // #6e6a86
    t.mdHorizontalRule  = wxColour(57, 53, 82);      // #393552 overlay

    return t;
}


// ═══════════════════════════════════════════════════════════════════
//  Rose Pine Dawn — warm, elegant light variant of Rose Pine
// ═══════════════════════════════════════════════════════════════════
//
//  https://rosepinetheme.com/palette/ingredients/
//
//  Dawn uses cream backgrounds (base/surface/overlay), a muted purple
//  foreground family (text/subtle/muted), and soft rose/iris/foam/pine
//  accents.  This keeps assistant text on Dawn's Text color so the
//  theme feels calm and readable rather than syntax-colored.
ThemeData ThemeManager::GetRosePineDawnTheme()
{
    ThemeData t;
    t.name = "rose-pine-dawn";

    // Backgrounds
    t.bgMain            = wxColour(250, 244, 237);   // #faf4ed base
    t.bgSidebar         = wxColour(255, 250, 243);   // #fffaf3 surface
    t.bgToolbar         = wxColour(255, 250, 243);   // #fffaf3 surface
    t.bgDialogSurface   = wxColour(255, 250, 243);   // #fffaf3 surface
    t.bgInputField      = wxColour(242, 233, 225);   // #f2e9e1 overlay
    t.bgInputArea       = wxColour(244, 237, 232);   // #f4ede8 highlight low
    t.modalScrim        = wxColour(0, 0, 0);

    // Text
    t.textPrimary       = wxColour(87, 82, 121);     // #575279 text
    t.textMuted         = wxColour(121, 117, 147);   // #797593 subtle

    // Accent / action — Iris / Love
    t.accentButton      = wxColour(144, 122, 169);   // #907aa9 iris
    t.accentButtonText  = wxColour(255, 250, 243);   // #fffaf3 surface
    t.stopButton        = wxColour(180, 99, 122);    // #b4637a love
    t.stopButtonText    = wxColour(255, 250, 243);   // #fffaf3 surface

    // Borders
    t.borderSubtle      = wxColour(223, 218, 217);   // #dfdad9 highlight med

    // Component backgrounds
    t.modelPillBg       = wxColour(223, 218, 217);   // #dfdad9 highlight med
    t.sidebarHover      = wxColour(242, 233, 225);   // #f2e9e1 overlay
    t.sidebarSelected   = wxColour(223, 218, 217);   // #dfdad9 highlight med

    // Attachment
    t.attachIndicator   = wxColour(86, 148, 159);    // #56949f foam
    t.attachChipBg      = wxColour(242, 233, 225);   // #f2e9e1 overlay

    // Chat — assistant body text uses Dawn Text; decorative colors remain accents.
    t.chatUser          = wxColour(40, 105, 131);    // #286983 pine
    t.chatAssistant     = wxColour(87, 82, 121);     // #575279 text
    t.chatAssistantB    = wxColour(234, 157, 52);    // #ea9d34 gold
    t.chatSystem        = wxColour(152, 147, 165);   // #9893a5 muted
    t.chatThought       = wxColour(121, 117, 147);   // #797593 subtle

    // Markdown
    t.mdCode            = wxColour(215, 130, 126);   // #d7827e rose
    t.mdHeading         = wxColour(144, 122, 169);   // #907aa9 iris
    t.mdCodeLabel       = wxColour(152, 147, 165);   // #9893a5 muted
    t.mdHorizontalRule  = wxColour(223, 218, 217);   // #dfdad9 highlight med

    return t;
}
