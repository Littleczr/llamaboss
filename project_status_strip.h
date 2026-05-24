// project_status_strip.h
// Single-line status strip showing both the active project and the
// active goal for the current chat.  Replaces the native Windows menu
// bar plus the old separate goal strip with one content-forward,
// monospace, terminal-style row that always shows both pieces of
// state at a glance.
//
// Layout (packed left, stretch space on the right):
//   ─ Project: <name> · N sources · M workflows  [ ⋯ ]    Goal: <status> · <objective>  [ details ] ─
//   ─ No project attached  [ + attach ]  [ + New Skill ]   Goal: none  [ /goal ] ─
//
// The strip never reads ChatHistory / ProjectManager / GoalState
// itself.  The frame computes the current State and pushes it via
// Refresh().  Both right-side affordances route back to the frame
// through the Callbacks.
//
// TODO(rename): now that this widget carries both Project AND Goal
// state on a single row, "ProjectStatusStrip" is a slight misnomer.
// Consider renaming to ContextStrip or ChatStatusStrip in a follow-up
// pass once we're done iterating on the merged layout.  The rename
// touches every #include and member reference, so it's deferred to
// its own commit rather than bundled with this UI polish.

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
        // ── Project ─────────────────────────────────────────────────
        bool        hasProject    = false;
        std::string projectName;
        int         sourceCount   = 0;
        int         workflowCount = 0;
        int         scriptCount   = 0;

        // ── Goal ────────────────────────────────────────────────────
        // The strip stitches "Goal: " + status + " · " + objective.  The
        // frame passes the status label (e.g. "active", "awaiting user")
        // and a pre-compacted objective string -- byte-budget compaction
        // is display knowledge that lives on the frame side, alongside
        // the GoalState that owns the raw text.
        bool        hasGoal              = false;
        std::string goalStatusLabel;
        std::string goalObjectiveCompact;
    };

    // ── Callbacks the frame provides ────────────────────────────────
    struct Callbacks {
        // Fired when the user clicks the project right-side affordance
        // or right-clicks anywhere on the strip.  The frame builds and
        // shows a context-sensitive popup menu; |anchor| is the window
        // the menu should be parented to (for screen-coord conversion).
        std::function<void(wxWindow* anchor)> onMenuRequested;

        // Fired when the user left-clicks [ + attach ] while no project
        // is attached.  The frame routes this directly into its existing
        // OnProjectAttach() flow so the chip behaves like the menu action,
        // without forcing the extra popup-menu hop.
        std::function<void()> onAttachRequested;

        // Fired when the user clicks the no-project [ + New Skill ]
        // shortcut.  This is intentionally separate from onMenuRequested
        // so the frame can show the same popup actions with Skill actions
        // prioritized at the top.
        std::function<void(wxWindow* anchor)> onSkillMenuRequested;

        // Fired when the user clicks the goal right-side affordance
        // ([ /goal ] when there is no goal, [ details ] otherwise).
        // The frame routes both to its existing DisplayGoalStatus()
        // flow so the slash-command path and the click path stay
        // unified.
        std::function<void()> onGoalActionClicked;
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
    void BindProjectActionEvents(wxWindow* w);
    void BindSkillActionEvents(wxWindow* w);
    void BindGoalActionEvents(wxWindow* w);

    Callbacks m_callbacks;

    // Owned widgets
    wxPanel*      m_panel           = nullptr;  // top-level strip panel
    wxPanel*      m_row             = nullptr;  // horizontal content row
    wxStaticText* m_stateLabel       = nullptr;  // project state text
    wxStaticText* m_actionLabel      = nullptr;  // project action ([ ⋯ ] / [ + attach ])
    wxStaticText* m_skillActionLabel = nullptr;  // no-project shortcut ([ + New Skill ])
    wxStaticText* m_goalStateLabel   = nullptr;  // goal state text
    wxStaticText* m_goalActionLabel = nullptr;  // goal action ([ details ] / [ /goal ])
    wxPanel*      m_separator       = nullptr;  // 1px bottom border

    // Current rendered state
    State m_state;

    // Cached theme colors so ApplyTheme can repaint without a Refresh
    wxColour m_bgColor;
    wxColour m_textColor;
    wxColour m_mutedColor;
    wxColour m_actionColor;
    wxColour m_borderColor;
};
