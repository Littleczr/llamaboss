// lb_hover_tile.h
// Shared "hover tile" highlight for flat icon/text buttons.
//
// Bind()/Surface()/Highlight() lift the whole button surface a few
// shades toward the theme's foreground colour while the mouse is over
// it.  Used by the sidebar cards, archive strip and the sidebar's New
// Window button.  Toolbar / input icon buttons use glyph-only hover
// (BindGlyph below), matching the text-only hover of the ctx meter and
// [ Skills ].
//
// Why "lift toward textPrimary" rather than reusing theme.sidebarHover
// verbatim: several themes (Nord, Dracula, One Dark) define sidebarHover as
// the exact toolbar colour, so a literal copy would be invisible on the top
// bar.  Mixing toward the foreground works on every dark and light theme.
#pragma once

#include <wx/button.h>
#include <wx/utils.h>
#include <algorithm>
#include <functional>
#include <memory>
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

// Glyph-only hover for flat toolbar / input icon buttons (☰ + ⚙ ⓘ, the
// paperclip, the agent toggle).  The background never changes, so no
// square tile appears behind the icon; only the glyph colour reacts,
// matching the ctx meter, model pill and [ Skills ] / [ Project ] labels.
//   glyph  called with true/false on enter/leave.  For text glyphs it sets
//          the foreground colour; for SVG bitmap buttons it can be empty,
//          since SetBitmapCurrent() supplies the hover tint and this helper
//          just forces the owner-drawn button to repaint.
// The click hook clears the hover state for the same reason as Bind():
// a modal dialog swallows the LEAVE event, so the glyph would stay lit.
inline void BindGlyph(wxButton* button, std::function<void(bool)> glyph = {})
{
    if (!button) return;

    auto hoveredFlag = std::make_shared<bool>(false);
    auto setHover = [button, glyph, hoveredFlag](bool hovered) {
        if (hovered && !button->IsEnabled()) hovered = false;
        if (*hoveredFlag == hovered) return;
        *hoveredFlag = hovered;
        if (glyph) glyph(hovered);
        button->Refresh();
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
