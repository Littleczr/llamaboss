// lb_icons.cpp
// SVG master table and themed rendering for LbIcons (see lb_icons.h).
//
// Each entry is the embedded copy of an editable master in assets/icons/.
// If a design changes, update its SVG literal here to match the master.
// No image file is opened at runtime; wxWidgets renders each DPI from the SVG.
#include "lb_icons.h"

namespace LbIcons {
namespace {

constexpr const char kPrimaryToken[] = "#D0D5DC";
constexpr const char kAccentToken[]  = "#7EE8A8";

struct IconDef {
    Id          id;
    const char* svg;
};

// Order must match LbIcons::Id (checked by the static_assert below).
constexpr IconDef kIcons[] = {

    // ── Settings: assets/icons/LlamaBoss_Cogwheel.svg ──────────────
    { Id::Settings, R"LB_SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none">
  <title>LlamaBoss settings cogwheel</title>
  <desc>Eight rounded teeth and a circular center, drawn as editable vector outlines on a transparent background.</desc>
  <g stroke="#D0D5DC" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
    <path d="M 9.1158 4.8063 Q 9.8500 4.5022 9.9778 3.7124 L 10.1424 2.6951 Q 10.2462 2.0534 10.8962 2.0534 L 13.1038 2.0534 Q 13.7538 2.0534 13.8576 2.6951 L 14.0222 3.7124 Q 14.1500 4.5022 14.8842 4.8063 L 15.0473 4.8739 Q 15.7815 5.1780 16.4303 4.7099 L 17.2660 4.1069 Q 17.7931 3.7266 18.2527 4.1862 L 19.8138 5.7473 Q 20.2734 6.2069 19.8931 6.7340 L 19.2901 7.5697 Q 18.8220 8.2185 19.1261 8.9527 L 19.1937 9.1158 Q 19.4978 9.8500 20.2876 9.9778 L 21.3049 10.1424 Q 21.9466 10.2462 21.9466 10.8962 L 21.9466 13.1038 Q 21.9466 13.7538 21.3049 13.8576 L 20.2876 14.0222 Q 19.4978 14.1500 19.1937 14.8842 L 19.1261 15.0473 Q 18.8220 15.7815 19.2901 16.4303 L 19.8931 17.2660 Q 20.2734 17.7931 19.8138 18.2527 L 18.2527 19.8138 Q 17.7931 20.2734 17.2660 19.8931 L 16.4303 19.2901 Q 15.7815 18.8220 15.0473 19.1261 L 14.8842 19.1937 Q 14.1500 19.4978 14.0222 20.2876 L 13.8576 21.3049 Q 13.7538 21.9466 13.1038 21.9466 L 10.8962 21.9466 Q 10.2462 21.9466 10.1424 21.3049 L 9.9778 20.2876 Q 9.8500 19.4978 9.1158 19.1937 L 8.9527 19.1261 Q 8.2185 18.8220 7.5697 19.2901 L 6.7340 19.8931 Q 6.2069 20.2734 5.7473 19.8138 L 4.1862 18.2527 Q 3.7266 17.7931 4.1069 17.2660 L 4.7099 16.4303 Q 5.1780 15.7815 4.8739 15.0473 L 4.8063 14.8842 Q 4.5022 14.1500 3.7124 14.0222 L 2.6951 13.8576 Q 2.0534 13.7538 2.0534 13.1038 L 2.0534 10.8962 Q 2.0534 10.2462 2.6951 10.1424 L 3.7124 9.9778 Q 4.5022 9.8500 4.8063 9.1158 L 4.8739 8.9527 Q 5.1780 8.2185 4.7099 7.5697 L 4.1069 6.7340 Q 3.7266 6.2069 4.1862 5.7473 L 5.7473 4.1862 Q 6.2069 3.7266 6.7340 4.1069 L 7.5697 4.7099 Q 8.2185 5.1780 8.9527 4.8739 Z"/>
    <circle cx="12" cy="12" r="3.4"/>
  </g>
</svg>
)LB_SVG" },

    // ── Attach: assets/icons/LlamaBoss_Paperclip.svg ───────────────
    { Id::Attach, R"LB_SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none">
  <title>LlamaBoss attach files paperclip</title>
  <desc>A simple rounded diagonal paperclip outline on a transparent background.</desc>
  <g transform="rotate(40 12 12)" stroke="#D0D5DC" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
    <path d="M 17.5 8 L 17.5 16.5 A 5.5 5.5 0 0 1 6.5 16.5 L 6.5 6 A 4 4 0 0 1 14.5 6 L 14.5 16.5 A 2 2 0 0 1 10.5 16.5 L 10.5 7.5"/>
  </g>
</svg>
)LB_SVG" },

    // ── Agent: assets/icons/LlamaBoss_Agent.svg ────────────────────
    { Id::Agent, R"LB_SVG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none">
  <title>LlamaBoss Agent robot</title>
  <desc>A rounded robot head with two eyes, side ears and a short antenna.</desc>
  <g stroke="#D0D5DC" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="3.5" r="1.5"/>
    <path d="M12 5v3M4.5 11.5H3a1 1 0 0 0-1 1v3a1 1 0 0 0 1 1h1.5M19.5 11.5H21a1 1 0 0 1 1 1v3a1 1 0 0 1-1 1h-1.5"/>
    <rect x="4.5" y="8" width="15" height="12" rx="3"/>
    <circle cx="9" cy="14" r="0.9" fill="#D0D5DC" stroke="none"/>
    <circle cx="15" cy="14" r="0.9" fill="#D0D5DC" stroke="none"/>
  </g>
</svg>
)LB_SVG" },
};

static_assert(sizeof(kIcons) / sizeof(kIcons[0]) ==
                  static_cast<std::size_t>(Id::kCount),
              "kIcons[] needs exactly one entry per LbIcons::Id");

constexpr bool TableOrderMatches()
{
    for (std::size_t i = 0; i < sizeof(kIcons) / sizeof(kIcons[0]); ++i)
        if (static_cast<std::size_t>(kIcons[i].id) != i) return false;
    return true;
}
static_assert(TableOrderMatches(),
              "kIcons[] entries must be in LbIcons::Id order");

void ReplaceAll(std::string& text, const std::string& token,
                const std::string& value)
{
    std::string::size_type position = 0;
    while ((position = text.find(token, position)) != std::string::npos) {
        text.replace(position, token.size(), value);
        position += value.size();
    }
}

std::string Html(const wxColour& colour)
{
    return colour.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
}

// Halfway between a glyph colour and the surface it sits on.
wxColour Fade(const wxColour& glyph, const wxColour& surface)
{
    return wxColour((glyph.Red()   + surface.Red())   / 2,
                    (glyph.Green() + surface.Green()) / 2,
                    (glyph.Blue()  + surface.Blue())  / 2);
}

} // namespace

wxBitmapBundle Make(Id id, const wxColour& primary, const wxColour& accent,
                    const wxSize& sizeDip)
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(Id::kCount))
        return wxBitmapBundle();

    std::string svg(kIcons[index].svg);
    ReplaceAll(svg, kPrimaryToken, Html(primary));
    ReplaceAll(svg, kAccentToken, Html(accent.IsOk() ? accent : primary));
    return wxBitmapBundle::FromSVG(svg.c_str(), sizeDip);
}

void ApplyButton(wxButton* button, Id id, const ButtonColours& c)
{
    if (!button) return;
    button->SetBackgroundColour(c.background);
    button->SetForegroundColour(c.normal);
    // Set the normal bundle before the other states, as wxWidgets requires.
    button->SetBitmap(Make(id, c.normal));
    button->SetBitmapCurrent(Make(id, c.hover));
    button->SetBitmapFocus(Make(id, c.focus));
    button->SetBitmapPressed(Make(id, c.pressed));
    button->SetBitmapDisabled(Make(id, c.disabled));
    button->SetBitmapMargins(0, 0);
    button->Refresh();
}

void ApplyFlatButton(wxButton* button, Id id, const ThemeData& theme,
                     const wxColour& background)
{
    ButtonColours c;
    c.background = background;
    c.normal     = theme.textMuted;
    // Same hover colour as every other toolbar control (theme.h).
    c.hover      = LbInteractiveAccent(theme);
    c.focus      = theme.textPrimary;
    c.pressed    = theme.accentButton;
    c.disabled   = Fade(theme.textMuted, background);
    ApplyButton(button, id, c);
}

void ApplyAgentToggle(wxButton* button, const ThemeData& theme,
                      bool agentEnabled)
{
    ButtonColours c;
    c.background = theme.bgInputArea;
    c.normal     = agentEnabled ? LbInteractiveAccent(theme) : theme.textMuted;
    const wxColour active =
        agentEnabled ? LbInteractiveAccent(theme) : theme.textPrimary;
    c.hover      = active;
    c.focus      = active;
    c.pressed    = active;
    c.disabled   = Fade(c.normal, theme.bgInputArea);
    ApplyButton(button, Id::Agent, c);
}

} // namespace LbIcons
