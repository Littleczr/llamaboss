// context_hud.h
//
// Context details panel ("HUD"): a small themed panel pinned to the
// bottom-right corner of the chat view, opened and closed by clicking the
// ctx meter in the top bar.  Shows what the context window is made of,
// the last reply's speed/timings, and averages for this chat.
//
// It stays open while you keep chatting and updates live: MyFrame calls
// SetModel() whenever the meter refreshes (every completed reply, New
// Chat, loads, model switches) and Reposition() when the window moves or
// the chat view resizes.
//
// The panel only draws a HudModel (context_stats.h); MyFrame builds the
// model from ChatHistory's request breakdown, the meter numbers and the
// TurnStats history.  Everything is painted by hand -- no child controls
// -- so it keeps the monospace, bracketed look of the rest of the chrome.
//
// Size is capped to the work area of the monitor it sits on (long model
// names, many rows, 150-200% display scaling).  When the content is taller
// than that, the sections scroll with the mouse wheel under a fixed footer,
// and a thin thumb on the right edge shows the position; text that is wider
// than the capped panel is ellipsized instead of running off the edge.
//
// It is a wxPopupWindow (owned, non-activating, always above the frame),
// not a transient popup: clicking elsewhere in the app does not close it.
// Close it with [ Close ] or by clicking the ctx meter again.

#pragma once

#include <wx/popupwin.h>
#include <wx/timer.h>

#include <functional>
#include <vector>

#include "context_stats.h"
#include "theme.h"

class ContextHud : public wxPopupWindow
{
public:
    struct Actions {
        std::function<void()> openLog;   // "[ Open log ]" -- may be empty (hidden)
        std::function<void()> close;     // "[ Close ]" -- owner destroys the panel
    };

    ContextHud(wxWindow* parent, const ThemeData& theme, HudModel model,
               Actions actions);

    // Pin to the bottom-right corner of `area` (the chat view), inset so it
    // clears the vertical scrollbar, and show.
    void ShowInCorner(wxWindow* area);

    // Re-place in the corner of the area passed to ShowInCorner.  Call on
    // frame move / chat view resize.  No-op when hidden.
    void Reposition();

    // Replace the content (live update).  Resizes to fit and re-pins.
    void SetModel(HudModel model);

    // Theme switched while open.
    void ApplyTheme(const ThemeData& theme);

    // Owner-provided "Open log" target can change when the chat changes.
    void SetOpenLog(std::function<void()> openLog);

#ifdef __WXMSW__
protected:
    // Never take activation: a click on the panel must leave the main
    // window active, otherwise the panel itself becomes the window that
    // later loses focus and MyFrame's "hide when LlamaBoss is not in the
    // foreground" logic never sees it (it floated over other apps after
    // the Snipping Tool / app switches).
    WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

private:
    struct Button {
        wxString label;
        wxRect   rect;
        bool     alignRight = false;
        std::function<void()> action;
    };

    void   RebuildButtons();
    wxSize Measure();
    // Measure() capped to the monitor work area; also clamps m_scrollY.
    wxSize FittedSize();
    wxRect WorkArea() const;
    int    SectionsViewHeight(int clientH) const;
    void   ClampScroll(int clientH);
    void   OnWheel(wxMouseEvent&);
    void   OnPaint(wxPaintEvent&);
    void   OnMotion(wxMouseEvent&);
    void   OnLeave(wxMouseEvent&);
    void   OnLeftUp(wxMouseEvent&);
    wxColour ToneColour(HudRow::Tone tone) const;

    ThemeData m_theme;
    HudModel  m_model;
    Actions   m_actions;
    wxWindow* m_area = nullptr;

    wxFont m_font;
    wxFont m_bold;

    // Layout (DIP-scaled pixels), computed by Measure().
    int m_pad = 0, m_lineH = 0, m_gap = 0, m_barW = 0, m_barH = 0;
    int m_labelW = 0, m_valueW = 0, m_width = 0;
    int m_sectionsH = 0;   // natural height of all sections (no padding/footer)
    int m_footerH   = 0;   // divider gap + footer line
    int m_scrollY   = 0;   // pixels scrolled into the sections

    std::vector<Button> m_buttons;
    int      m_hoverButton = -1;
    wxTimer  m_copiedTimer;
    bool     m_showCopied = false;
};
