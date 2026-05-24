// project_status_strip.cpp
#include "project_status_strip.h"
#include "theme.h"

#include <wx/sizer.h>
#include <algorithm>

namespace {

// Middle dot separator used between count fields.  Rendered with
// surrounding spaces so it sits visually as a separator, not punctuation.
const char* kDot = " \xC2\xB7 ";  // " · "

std::string PluralSuffix(int n)
{
    return (n == 1) ? std::string() : std::string("s");
}

// ── Project formatters ──────────────────────────────────────────────

// Builds the left-side project state text from current state.  Pure
// formatter, no widget mutation -- caller assigns the result to the label.
std::string BuildProjectStateText(const ProjectStatusStrip::State& s)
{
    if (!s.hasProject) {
        return "No project attached";
    }

    std::string out = "Project: ";
    out += s.projectName.empty() ? std::string("(unnamed)") : s.projectName;
    out += kDot;
    out += std::to_string(s.sourceCount);
    out += " source";
    out += PluralSuffix(s.sourceCount);
    out += kDot;
    out += std::to_string(s.workflowCount);
    out += " workflow";
    out += PluralSuffix(s.workflowCount);

    // Scripts are an optional companion to workflows; only surface the
    // count when at least one exists, so the empty case stays quiet.
    if (s.scriptCount > 0) {
        out += kDot;
        out += std::to_string(s.scriptCount);
        out += " script";
        out += PluralSuffix(s.scriptCount);
    }

    return out;
}

// Builds the project-side affordance label.  Brackets are part of the
// label so the visual reads as a terminal-style clickable token.
std::string BuildProjectActionText(const ProjectStatusStrip::State& s)
{
    return s.hasProject
        ? std::string("[ \xE2\x8B\xAF ]")        // [ ⋯ ]
        : std::string("[ + attach ]");
}

// The no-project row keeps the Skill shortcut beside [ + attach ].
// It intentionally remains a project-menu shortcut for now so this
// pass adds the missing attach affordance without changing the current
// New Skill behavior.
std::string BuildNoProjectSkillActionText()
{
    return "[ + New Skill ]";
}

// ── Goal formatters ─────────────────────────────────────────────────

// Builds "Goal: none" or "Goal: <status> · <objective>".  Mirrors the
// old BuildGoalStatusStripText() that used to live in LlamaBoss.cpp.
std::string BuildGoalStateText(const ProjectStatusStrip::State& s)
{
    if (!s.hasGoal) {
        return "Goal: none";
    }

    std::string out = "Goal: ";
    out += s.goalStatusLabel.empty() ? std::string("active") : s.goalStatusLabel;
    if (!s.goalObjectiveCompact.empty()) {
        out += kDot;
        out += s.goalObjectiveCompact;
    }
    return out;
}

// Builds the goal-side affordance label.  Empty state surfaces the
// slash-command name to keep it discoverable; populated state opens
// the detail card.
std::string BuildGoalActionText(const ProjectStatusStrip::State& s)
{
    return s.hasGoal
        ? std::string("[ details ]")
        : std::string("[ /goal ]");
}

} // namespace

ProjectStatusStrip::ProjectStatusStrip(wxWindow* parent,
                                       const ThemeData& theme,
                                       const Callbacks& callbacks)
    : m_callbacks(callbacks)
{
    // Cache theme colors used during incremental updates.
    m_bgColor     = theme.bgToolbar;
    m_textColor   = theme.textPrimary;
    m_mutedColor  = theme.textMuted;
    m_actionColor = theme.chatAssistant;
    m_borderColor = theme.borderSubtle;

    m_panel = new wxPanel(parent, wxID_ANY);
    m_panel->SetBackgroundColour(m_bgColor);

    BuildContent();
    RelayoutCurrentState();
}

void ProjectStatusStrip::BuildContent()
{
    auto* outerSizer = new wxBoxSizer(wxVERTICAL);

    // ── Content row ──────────────────────────────────────────────
    m_row = new wxPanel(m_panel, wxID_ANY);
    m_row->SetBackgroundColour(m_bgColor);
    auto* rowSizer = new wxBoxSizer(wxHORIZONTAL);

    // Monospace "Consolas" matches the LlamaBoss terminal-status idiom
    // used by chat_display tool cards and command echoes.
    wxFont monoFont(11, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                    wxFONTWEIGHT_NORMAL, false, "Consolas");

    // Padding constants for the row.  Tight (4 px) between a state
    // label and its own action chip.  In the no-project state, the
    // [ + New Skill ] shortcut sits just after [ + attach ].  The
    // project pair stays anchored to the left edge while the goal pair
    // is pushed to the far right, leaving the center of the strip
    // visually quiet.
    const int kEdgePad        = 6;
    const int kVerticalPad    = 6;
    const int kStateActionGap = 4;

    rowSizer->AddSpacer(kEdgePad);

    // ── Project pair ─────────────────────────────────────────────
    m_stateLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_stateLabel->SetForegroundColour(m_textColor);
    m_stateLabel->SetFont(monoFont);
    rowSizer->Add(m_stateLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kVerticalPad);

    rowSizer->AddSpacer(kStateActionGap);

    // Project action.  Default stays muted so the strip is calm; hover
    // switches to the mint-green accent, matching the New Chat plus
    // button behavior.
    m_actionLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_actionLabel->SetForegroundColour(m_mutedColor);
    m_actionLabel->SetFont(monoFont);
    m_actionLabel->SetCursor(wxCURSOR_HAND);
    m_actionLabel->SetMinSize(wxSize(56, -1));
    rowSizer->Add(m_actionLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kVerticalPad);

    // No-project shortcut.  This preserves the existing visible
    // [ + New Skill ] affordance while restoring [ + attach ] as the
    // actual project-side action beside "No project attached".
    m_skillActionLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_skillActionLabel->SetForegroundColour(m_mutedColor);
    m_skillActionLabel->SetFont(monoFont);
    m_skillActionLabel->SetCursor(wxCURSOR_HAND);
    m_skillActionLabel->SetMinSize(wxSize(110, -1));
    rowSizer->Add(m_skillActionLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxLEFT, kStateActionGap);

    // Let the center of the strip absorb the unused width so the goal
    // pair reads as a separate right-aligned status cluster instead of
    // crowding the project state on the left.
    rowSizer->AddStretchSpacer(1);

    // ── Goal pair ────────────────────────────────────────────────
    m_goalStateLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_goalStateLabel->SetForegroundColour(m_textColor);
    m_goalStateLabel->SetFont(monoFont);
    rowSizer->Add(m_goalStateLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kVerticalPad);

    rowSizer->AddSpacer(kStateActionGap);

    m_goalActionLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_goalActionLabel->SetForegroundColour(m_mutedColor);
    m_goalActionLabel->SetFont(monoFont);
    m_goalActionLabel->SetCursor(wxCURSOR_HAND);
    m_goalActionLabel->SetMinSize(wxSize(78, -1));
    rowSizer->Add(m_goalActionLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kVerticalPad);

    rowSizer->AddSpacer(kEdgePad);  // symmetric right edge padding

    m_row->SetSizer(rowSizer);
    outerSizer->Add(m_row, 0, wxEXPAND);

    // ── Bottom separator (matches top-bar separator idiom) ───────
    m_separator = new wxPanel(m_panel, wxID_ANY,
                              wxDefaultPosition, wxSize(-1, 1));
    m_separator->SetBackgroundColour(m_borderColor);
    outerSizer->Add(m_separator, 0, wxEXPAND);

    m_panel->SetSizer(outerSizer);

    // ── Mouse routing ────────────────────────────────────────────
    // Project action: [ ⋯ ] opens the project popup menu when a
    // project is attached; [ + attach ] routes directly into the frame's
    // existing attach-project flow when none is attached.  Right-click
    // anywhere on the strip (outside the goal action) still opens the
    // project menu -- a holdover from the pre-goal layout where the whole
    // row was the project surface.  We keep the right-click-anywhere
    // behavior because the project menu is the primary action surface;
    // goal requires an explicit left-click on its chip.
    BindProjectActionEvents(m_actionLabel);

    m_actionLabel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
        if (m_actionLabel) {
            m_actionLabel->SetForegroundColour(m_actionColor);
            m_actionLabel->Refresh();
        }
        e.Skip();
    });
    m_actionLabel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
        if (m_actionLabel) {
            m_actionLabel->SetForegroundColour(m_mutedColor);
            m_actionLabel->Refresh();
        }
        e.Skip();
    });

    // No-project Skill shortcut.  Preserve the current visible entry
    // point by keeping it menu-backed, but route it separately so the
    // popup can prioritize Skill actions above Project actions.
    BindSkillActionEvents(m_skillActionLabel);

    m_skillActionLabel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
        if (m_skillActionLabel) {
            m_skillActionLabel->SetForegroundColour(m_actionColor);
            m_skillActionLabel->Refresh();
        }
        e.Skip();
    });
    m_skillActionLabel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
        if (m_skillActionLabel) {
            m_skillActionLabel->SetForegroundColour(m_mutedColor);
            m_skillActionLabel->Refresh();
        }
        e.Skip();
    });

    // Goal action: left-click only.  Hover behavior mirrors the project
    // action so the strip feels consistent.
    BindGoalActionEvents(m_goalActionLabel);

    m_goalActionLabel->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
        if (m_goalActionLabel) {
            m_goalActionLabel->SetForegroundColour(m_actionColor);
            m_goalActionLabel->Refresh();
        }
        e.Skip();
    });
    m_goalActionLabel->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
        if (m_goalActionLabel) {
            m_goalActionLabel->SetForegroundColour(m_mutedColor);
            m_goalActionLabel->Refresh();
        }
        e.Skip();
    });

    // Right-click anywhere on the strip (outside the goal action)
    // opens the project menu -- preserves prior behavior.
    auto rightClickToProjectMenu = [this](wxWindow* w) {
        w->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
            if (m_callbacks.onMenuRequested) m_callbacks.onMenuRequested(m_actionLabel);
        });
    };
    rightClickToProjectMenu(m_panel);
    rightClickToProjectMenu(m_row);
    rightClickToProjectMenu(m_stateLabel);
    rightClickToProjectMenu(m_goalStateLabel);
}

void ProjectStatusStrip::BindProjectActionEvents(wxWindow* w)
{
    w->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (!m_state.hasProject && m_callbacks.onAttachRequested) {
            m_callbacks.onAttachRequested();
            return;
        }

        if (m_callbacks.onMenuRequested) {
            m_callbacks.onMenuRequested(m_actionLabel);
        }
    });
    w->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
        if (m_callbacks.onMenuRequested) m_callbacks.onMenuRequested(m_actionLabel);
    });
}

void ProjectStatusStrip::BindSkillActionEvents(wxWindow* w)
{
    auto requestSkillMenu = [this]() {
        if (m_callbacks.onSkillMenuRequested) {
            m_callbacks.onSkillMenuRequested(m_skillActionLabel);
        } else if (m_callbacks.onMenuRequested) {
            m_callbacks.onMenuRequested(m_skillActionLabel);
        }
    };

    w->Bind(wxEVT_LEFT_UP, [requestSkillMenu](wxMouseEvent&) {
        requestSkillMenu();
    });
    w->Bind(wxEVT_RIGHT_UP, [requestSkillMenu](wxMouseEvent&) {
        requestSkillMenu();
    });
}

void ProjectStatusStrip::BindGoalActionEvents(wxWindow* w)
{
    w->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (m_callbacks.onGoalActionClicked) m_callbacks.onGoalActionClicked();
    });
    w->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
        if (m_callbacks.onGoalActionClicked) m_callbacks.onGoalActionClicked();
    });
}

void ProjectStatusStrip::RelayoutCurrentState()
{
    if (!m_stateLabel || !m_actionLabel || !m_skillActionLabel ||
        !m_goalStateLabel || !m_goalActionLabel) {
        return;
    }

    // ── Project labels ───────────────────────────────────────────
    m_stateLabel->SetLabel(wxString::FromUTF8(BuildProjectStateText(m_state).c_str()));

    {
        const std::string actionText = BuildProjectActionText(m_state);
        m_actionLabel->SetLabel(wxString::FromUTF8(actionText.c_str()));
        m_actionLabel->SetForegroundColour(m_mutedColor);

        // Defensive sizing for the affordance.  Keep the attached
        // token compact ("[ ⋯ ]"), but leave enough room for the
        // no-project "[ + attach ]" token.
        const int floorWidth = m_state.hasProject ? 56 : 92;
        const wxSize measured = m_actionLabel->GetTextExtent(m_actionLabel->GetLabel());
        const int actionWidth = std::max(floorWidth, measured.GetWidth() + 12);
        m_actionLabel->SetMinSize(wxSize(actionWidth, -1));
        m_actionLabel->InvalidateBestSize();
    }

    {
        const std::string skillActionText = BuildNoProjectSkillActionText();
        m_skillActionLabel->SetLabel(wxString::FromUTF8(skillActionText.c_str()));
        m_skillActionLabel->SetForegroundColour(m_mutedColor);
        m_skillActionLabel->Show(!m_state.hasProject);

        const wxSize measured =
            m_skillActionLabel->GetTextExtent(m_skillActionLabel->GetLabel());
        const int actionWidth = std::max(110, measured.GetWidth() + 12);
        m_skillActionLabel->SetMinSize(wxSize(actionWidth, -1));
        m_skillActionLabel->InvalidateBestSize();
    }

    // ── Goal labels ──────────────────────────────────────────────
    m_goalStateLabel->SetLabel(wxString::FromUTF8(BuildGoalStateText(m_state).c_str()));

    {
        const std::string actionText = BuildGoalActionText(m_state);
        m_goalActionLabel->SetLabel(wxString::FromUTF8(actionText.c_str()));
        m_goalActionLabel->SetForegroundColour(m_mutedColor);

        // Mirror the project-side dynamic min-size logic: attached
        // ("[ details ]") ~88 px, empty ("[ /goal ]") ~78 px.
        const int floorWidth = m_state.hasGoal ? 88 : 78;
        const wxSize measured = m_goalActionLabel->GetTextExtent(m_goalActionLabel->GetLabel());
        const int actionWidth = std::max(floorWidth, measured.GetWidth() + 12);
        m_goalActionLabel->SetMinSize(wxSize(actionWidth, -1));
        m_goalActionLabel->InvalidateBestSize();
    }

    m_stateLabel->InvalidateBestSize();
    m_skillActionLabel->InvalidateBestSize();
    m_goalStateLabel->InvalidateBestSize();

    // State text changes width when project / goal info changes; force
    // the row and owning parent to relayout so everything stays aligned.
    if (m_row) m_row->Layout();
    if (m_panel) {
        m_panel->Layout();
        if (m_panel->GetParent()) m_panel->GetParent()->Layout();
        m_panel->Refresh();
    }
}

void ProjectStatusStrip::Refresh(const State& state)
{
    m_state = state;
    RelayoutCurrentState();
}

void ProjectStatusStrip::ApplyTheme(const ThemeData& theme)
{
    m_bgColor     = theme.bgToolbar;
    m_textColor   = theme.textPrimary;
    m_mutedColor  = theme.textMuted;
    m_actionColor = theme.chatAssistant;
    m_borderColor = theme.borderSubtle;

    if (m_panel)           m_panel->SetBackgroundColour(m_bgColor);
    if (m_row)             m_row->SetBackgroundColour(m_bgColor);
    if (m_stateLabel)       m_stateLabel->SetForegroundColour(m_textColor);
    if (m_actionLabel)      m_actionLabel->SetForegroundColour(m_mutedColor);
    if (m_skillActionLabel) m_skillActionLabel->SetForegroundColour(m_mutedColor);
    if (m_goalStateLabel)   m_goalStateLabel->SetForegroundColour(m_textColor);
    if (m_goalActionLabel) m_goalActionLabel->SetForegroundColour(m_mutedColor);
    if (m_separator)       m_separator->SetBackgroundColour(m_borderColor);

    if (m_panel) m_panel->Refresh();
}
