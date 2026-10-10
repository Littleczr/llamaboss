// project_status_strip.cpp
#include "project_status_strip.h"
#include "theme.h"

#include <wx/sizer.h>

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
        return "";
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
std::string BuildProjectActionText(const ProjectStatusStrip::State& /*s*/)
{
    // "\xE2\x96\xBE" == U+25BE down-triangle, signalling a dropdown.  Both
    // states open the project menu on click (mirroring [ Skills v ]), so the
    // label is identical whether or not a project is attached; only the menu
    // contents differ -- New/Load/Delete when empty, full actions when set.
    return std::string("[ Project \xE2\x96\xBE ]");
}

// Skill shortcut shown beside the project action in both empty and
// attached-project states.
std::string BuildSkillActionText()
{
    // Opens the Skills dropdown (New / Open / Open Folder), so it reads as
    // a menu token rather than a single "new" verb.
    return "[ Skills \xE2\x96\xBE ]";
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
    // Hover colour: the shared toolbar accent (theme.h).  chatAssistant
    // would be plain foreground text in most themes, making the hover
    // effectively invisible.
    m_actionColor = LbInteractiveAccent(theme);
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
    // [ Skills v ] shortcut sits just after [ Project v ].
    // The project pair stays anchored to the left edge.
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
    // switches to the shared interactive accent, matching the New Chat
    // plus button behavior.
    m_actionLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_actionLabel->SetForegroundColour(m_mutedColor);
    m_actionLabel->SetFont(monoFont);
    m_actionLabel->SetCursor(wxCURSOR_HAND);
    m_actionLabel->SetMinSize(wxSize(56, -1));
    rowSizer->Add(m_actionLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kVerticalPad);

    // Skill shortcut.  Keep this visible in both empty and attached
    // project states so Skill creation remains one click away.
    m_skillActionLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_skillActionLabel->SetForegroundColour(m_mutedColor);
    m_skillActionLabel->SetFont(monoFont);
    m_skillActionLabel->SetCursor(wxCURSOR_HAND);
    m_skillActionLabel->SetMinSize(wxSize(110, -1));
    rowSizer->Add(m_skillActionLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxLEFT, kStateActionGap);

    rowSizer->AddStretchSpacer(1);
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
    // Project action: [ Project v ] opens the project popup menu in both
    // states (the menu adapts its contents to whether a project is
    // attached).  Right-click anywhere on the strip also opens the
    // project menu, since it is the primary action surface.
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

    // Skill shortcut.  Preserve the current visible entry point by
    // keeping it menu-backed, but route it separately so the popup can
    // prioritize Skill actions above Project actions.
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

    // Right-click anywhere on the strip
    // opens the project menu -- preserves prior behavior.
    auto rightClickToProjectMenu = [this](wxWindow* w) {
        w->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
            if (m_callbacks.onMenuRequested) m_callbacks.onMenuRequested(m_actionLabel);
        });
    };
    rightClickToProjectMenu(m_panel);
    rightClickToProjectMenu(m_row);
    rightClickToProjectMenu(m_stateLabel);
}

void ProjectStatusStrip::BindProjectActionEvents(wxWindow* w)
{
    // Both states open the project popup, consistent with the Skills
    // affordance.  The no-project menu offers New Project / Load-Attach /
    // Delete; the attached menu offers the full project actions.
    w->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (m_callbacks.onMenuRequested) m_callbacks.onMenuRequested(m_actionLabel);
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

void ProjectStatusStrip::RelayoutCurrentState()
{
    if (!m_stateLabel || !m_actionLabel || !m_skillActionLabel) {
        return;
    }

    // ── Project labels ───────────────────────────────────────────
    m_stateLabel->SetLabel(wxString::FromUTF8(BuildProjectStateText(m_state).c_str()));
    m_stateLabel->Show(m_state.hasProject);

    {
        const std::string actionText = BuildProjectActionText(m_state);
        m_actionLabel->SetLabel(wxString::FromUTF8(actionText.c_str()));
        m_actionLabel->SetForegroundColour(m_mutedColor);

        // Defensive sizing for the affordance.  Both states show
        // "[ Project v ]" now, so a single floor covers them.
        const int floorWidth = 108;
        const wxSize measured = m_actionLabel->GetTextExtent(m_actionLabel->GetLabel());
        const int actionWidth = std::max(floorWidth, measured.GetWidth() + 12);
        m_actionLabel->SetMinSize(wxSize(actionWidth, -1));
        m_actionLabel->InvalidateBestSize();
    }

    {
        const std::string skillActionText = BuildSkillActionText();
        m_skillActionLabel->SetLabel(wxString::FromUTF8(skillActionText.c_str()));
        m_skillActionLabel->SetForegroundColour(m_mutedColor);
        m_skillActionLabel->Show(true);

        const wxSize measured =
            m_skillActionLabel->GetTextExtent(m_skillActionLabel->GetLabel());
        const int actionWidth = std::max(110, measured.GetWidth() + 12);
        m_skillActionLabel->SetMinSize(wxSize(actionWidth, -1));
        m_skillActionLabel->InvalidateBestSize();
    }

    m_stateLabel->InvalidateBestSize();
    m_skillActionLabel->InvalidateBestSize();

    // State text changes width when project info changes; force
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
    m_actionColor = LbInteractiveAccent(theme);
    m_borderColor = theme.borderSubtle;

    if (m_panel)           m_panel->SetBackgroundColour(m_bgColor);
    if (m_row)             m_row->SetBackgroundColour(m_bgColor);
    if (m_stateLabel)       m_stateLabel->SetForegroundColour(m_textColor);
    if (m_actionLabel)      m_actionLabel->SetForegroundColour(m_mutedColor);
    if (m_skillActionLabel) m_skillActionLabel->SetForegroundColour(m_mutedColor);
    if (m_separator)       m_separator->SetBackgroundColour(m_borderColor);

    if (m_panel) m_panel->Refresh();
}
