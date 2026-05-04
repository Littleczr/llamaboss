// project_status_strip.h
// Single-line status strip showing the active project for the current chat.
// Replaces the native Windows menu bar with a content-forward, monospace,
// terminal-style strip that always shows project state at a glance.
//
// Layout:
//   ─ Project: <name>  ·  N sources  ·  M workflows                    [ ⋯ ] ─
//   ─ No project attached                                       [ + attach ] ─
//
// The strip never reads ChatHistory / ProjectManager itself.  The frame
// computes the current State and pushes it via Refresh().  All actions
// route back to the frame through the onMenuRequested callback.

#pragma once

#include <wx/wx.h>
#include <functional>
#include <string>

struct ThemeData;

class ProjectStatusStrip
{
public:
    // ── State pushed from the frame ─────────────────────────────────
    struct State {
        bool        hasProject    = false;
        std::string projectName;
        int         sourceCount   = 0;
        int         workflowCount = 0;
        int         scriptCount   = 0;
    };

    // ── Callbacks the frame provides ────────────────────────────────
    struct Callbacks {
        // Fired when the user clicks the right-side affordance or
        // right-clicks anywhere on the strip.  The frame builds and
        // shows a context-sensitive popup menu; |anchor| is the window
        // the menu should be parented to (for screen-coord conversion).
        std::function<void(wxWindow* anchor)> onMenuRequested;
    };

    ProjectStatusStrip(wxWindow* parent,
                       const ThemeData& theme,
                       const Callbacks& callbacks);
    ~ProjectStatusStrip() = default;

    // ── Layout access ───────────────────────────────────────────────
    wxPanel* GetPanel() const { return m_panel; }

    // ── State updates ───────────────────────────────────────────────
    void Refresh(const State& state);

    // ── Theming ─────────────────────────────────────────────────────
    void ApplyTheme(const ThemeData& theme);

private:
    void BuildContent();
    void RelayoutCurrentState();
    void BindMouseEvents(wxWindow* w);

    Callbacks m_callbacks;

    // Owned widgets
    wxPanel*      m_panel        = nullptr;  // top-level strip panel
    wxPanel*      m_row          = nullptr;  // horizontal content row
    wxStaticText* m_stateLabel   = nullptr;  // left-side state text
    wxStaticText* m_actionLabel  = nullptr;  // right-side affordance ([ ⋯ ] / [ + attach ])
    wxPanel*      m_separator    = nullptr;  // 1px bottom border

    // Current rendered state
    State m_state;

    // Cached theme colors so ApplyTheme can repaint without a Refresh
    wxColour m_bgColor;
    wxColour m_textColor;
    wxColour m_mutedColor;
    wxColour m_actionColor;
    wxColour m_borderColor;
};
