// lb_hover_tile.h
// Shared "hover tile" highlight for flat icon/text buttons.
//
// The sidebar conversation cards lift their background slightly on hover.
// Icon buttons elsewhere (toolbar hamburger, +, settings cogwheel, about,
// attach, agent toggle) used to change only their glyph colour, which made
// the cogwheel in particular feel inert.  This helper gives them the same
// kind of feedback: the whole button surface lifts a few shades toward the
// theme's foreground colour while the mouse is over it.
//
// Why "lift toward textPrimary" rather than reusing theme.sidebarHover
// verbatim: several themes (Nord, Dracula, One Dark) define sidebarHover as
// the exact toolbar colour, so a literal copy would be invisible on the top
// bar.  Mixing toward the foreground works on every dark and light theme and
// produces a lift of similar strength to the sidebar card hover.
#pragma once

#include <wx/button.h>
#include <wx/utils.h>
#include <algorithm>
#include <functional>
#include "theme.h"

namespace LbHoverTile {

inline wxColour Mix(const wxColour& first, const wxColour& second,
                    int secondPercent)
{
    secondPercent = std::clamp(secondPercent, 0, 100);
    const int firstPercent = 100 - secondPercent;
    auto ch = [&](unsigned char a, unsigned char b) {
        return static_cast<unsigned char>(
            (static_cast<int>(a) * firstPercent +
             static_cast<int>(b) * secondPercent + 50) / 100);
    };
    return wxColour(ch(first.Red(),   second.Red()),
                    ch(first.Green(), second.Green()),
                    ch(first.Blue(),  second.Blue()));
}

// Hover surface for a control whose resting background is `base`.
inline wxColour Surface(const ThemeData& theme, const wxColour& base)
{
    return Mix(base, theme.textPrimary, 9);
}

// The one hover colour the toolbar icons show (☰ + ⚙ ⓘ).  Conversation
// rows use this exact colour too, so a hovered chat and a hovered toolbar
// icon match.  Surface() above lifts relative to whatever a control sits on,
// so a row card (darker than the toolbar) would otherwise land a few shades
// lower than the icons.
inline wxColour Highlight(const ThemeData& theme)
{
    return Surface(theme, theme.bgToolbar);
}

// Attach the hover tile to a button.
//   theme     returns the live theme (themes can change at runtime)
//   restingBg returns the button's normal background for that theme
//   glyph     optional: called with true/false so the caller can keep its
//             existing foreground hover colour in sync with the tile
//
// Bind this AFTER the button's own wxEVT_BUTTON handler: the click hook
// below clears the tile and Skip()s so the real handler still runs.  That
// stops the tile from sticking when a click opens a modal dialog and the
// LEAVE event is never delivered.  Motion re-applies the tile, so a cursor
// still resting on the button after the dialog closes lights it up again.
inline void Bind(wxButton* button,
                 std::function<const ThemeData&()> theme,
                 std::function<wxColour(const ThemeData&)> restingBg,
                 std::function<void(bool)> glyph = {})
{
    if (!button || !theme || !restingBg) return;

    auto setHover = [button, theme, restingBg, glyph](bool hovered) {
        if (hovered && !button->IsEnabled()) hovered = false;
        const ThemeData& t = theme();
        const wxColour rest = restingBg(t);
        const wxColour want = hovered ? Surface(t, rest) : rest;
        if (button->GetBackgroundColour() != want) {
            button->SetBackgroundColour(want);
            if (glyph) glyph(hovered);
            button->Refresh();
        }
    };

    button->Bind(wxEVT_ENTER_WINDOW, [setHover](wxMouseEvent& e) {
        setHover(true);
        e.Skip();
    });
    button->Bind(wxEVT_MOTION, [setHover](wxMouseEvent& e) {
        setHover(true);
        e.Skip();
    });
    button->Bind(wxEVT_LEAVE_WINDOW, [setHover](wxMouseEvent& e) {
        setHover(false);
        e.Skip();
    });
    button->Bind(wxEVT_BUTTON, [setHover](wxCommandEvent& e) {
        setHover(false);
        e.Skip();
    });
}

} // namespace LbHoverTile
