// lb_icons.h
// One home for LlamaBoss's embedded vector icons.
//
// Replaces the per-icon headers (settings_icon.h, attachment_icon.h,
// agent_icon.h).  The SVG masters live in a single table in lb_icons.cpp, so
// they compile once and adding an icon never needs a new header or include:
//
//   1. add a name to LbIcons::Id below (keep kCount last), and
//   2. add its SVG entry to kIcons[] in lb_icons.cpp, in the same order.
//
// Colour tokens inside each SVG master (replaced at render time):
//   #D0D5DC  primary  -- outlines / main glyph (theme text colour)
//   #7EE8A8  accent   -- optional second colour (e.g. a green highlight);
//                        defaults to the primary colour when not given
//
// Sizes are in DIPs.  Passing FromDIP(24) would scale twice: the returned
// wxBitmapBundle renders the SVG at each display's DPI by itself.
#pragma once

#include <wx/bmpbndl.h>
#include <wx/button.h>
#include <wx/colour.h>
#include <wx/gdicmn.h>
#include "theme.h"

namespace LbIcons {

enum class Id {
    Settings,   // toolbar cogwheel      (assets/icons/LlamaBoss_Cogwheel.svg)
    Attach,     // composer paperclip    (assets/icons/LlamaBoss_Paperclip.svg)
    Agent,      // composer agent robot  (assets/icons/LlamaBoss_Agent.svg)
    kCount      // keep last
};

// Themed bundle for any use: buttons, inline chat images, menus.
wxBitmapBundle Make(Id id,
                    const wxColour& primary,
                    const wxColour& accent = wxNullColour,
                    const wxSize& sizeDip = wxSize(24, 24));

// Full colour set for a flat SVG icon button.
struct ButtonColours {
    wxColour background;   // button surface (the bar it sits on)
    wxColour normal;       // resting glyph; also the foreground colour
    wxColour hover;        // SetBitmapCurrent
    wxColour focus;        // SetBitmapFocus
    wxColour pressed;      // SetBitmapPressed
    wxColour disabled;     // SetBitmapDisabled
};

// Applies all bitmap states to a wxBU_NOTEXT button in the order wxWidgets
// requires (normal first), then refreshes it.
void ApplyButton(wxButton* button, Id id, const ButtonColours& colours);

// Standard flat icon button (Settings on the toolbar, Attach on the composer):
// muted at rest, interactive accent on hover, primary text when focused,
// accent button colour while pressed, faded when disabled.
void ApplyFlatButton(wxButton* button, Id id, const ThemeData& theme,
                     const wxColour& background);

// The composer's agent toggle: accent while agent mode is on, muted (with a
// primary-text hover) while it is off.
void ApplyAgentToggle(wxButton* button, const ThemeData& theme,
                      bool agentEnabled);

} // namespace LbIcons
