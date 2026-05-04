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

// Builds the left-side state text from current state.  Pure formatter,
// no widget mutation — caller assigns the result to the label.
std::string BuildStateText(const ProjectStatusStrip::State& s)
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

// Builds the right-side affordance label.  Brackets are part of the
// label so the visual reads as a terminal-style clickable token.
std::string BuildActionText(const ProjectStatusStrip::State& s)
{
    return s.hasProject
        ? std::string("[ \xE2\x8B\xAF ]")        // [ ⋯ ]
        : std::string("[ + attach ]");
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

    // Left: state text — primary color, muted weight.
    m_stateLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_stateLabel->SetForegroundColour(m_textColor);
    m_stateLabel->SetFont(monoFont);
    rowSizer->Add(m_stateLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 6);

    rowSizer->AddStretchSpacer(1);

    // Right: action affordance — default stays muted so the strip is
    // calm.  Hover switches to the mint-green accent, matching the
    // New Chat plus button behavior.
    m_actionLabel = new wxStaticText(m_row, wxID_ANY, "");
    m_actionLabel->SetForegroundColour(m_mutedColor);
    m_actionLabel->SetFont(monoFont);
    m_actionLabel->SetCursor(wxCURSOR_HAND);

    // Give the right-side affordance a small safety floor.  The
    // attached-project label is intentionally compact ("[ ⋯ ]"),
    // while the no-project attach label needs more room.  We still
    // re-measure dynamically in RelayoutCurrentState().
    m_actionLabel->SetMinSize(wxSize(56, -1));

    rowSizer->Add(m_actionLabel, 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT | wxTOP | wxBOTTOM, 8);

    m_row->SetSizer(rowSizer);
    outerSizer->Add(m_row, 0, wxEXPAND);

    // ── Bottom separator (matches top-bar separator idiom) ───────
    m_separator = new wxPanel(m_panel, wxID_ANY,
                              wxDefaultPosition, wxSize(-1, 1));
    m_separator->SetBackgroundColour(m_borderColor);
    outerSizer->Add(m_separator, 0, wxEXPAND);

    m_panel->SetSizer(outerSizer);

    // Mouse routing: click the affordance to open the menu, right-click
    // anywhere on the strip to do the same.  Action label gets both
    // left-up and right-up; the rest of the strip gets right-up only so
    // a stray left-click on empty space doesn't trigger anything.
    BindMouseEvents(m_actionLabel);

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

    auto rightClickOnly = [this](wxWindow* w) {
        w->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
            if (m_callbacks.onMenuRequested) m_callbacks.onMenuRequested(m_actionLabel);
        });
    };
    rightClickOnly(m_panel);
    rightClickOnly(m_row);
    rightClickOnly(m_stateLabel);
}

void ProjectStatusStrip::BindMouseEvents(wxWindow* w)
{
    w->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (m_callbacks.onMenuRequested) m_callbacks.onMenuRequested(m_actionLabel);
    });
    w->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
        if (m_callbacks.onMenuRequested) m_callbacks.onMenuRequested(m_actionLabel);
    });
}

void ProjectStatusStrip::RelayoutCurrentState()
{
    if (!m_stateLabel || !m_actionLabel) return;

    m_stateLabel->SetLabel(wxString::FromUTF8(BuildStateText(m_state).c_str()));

    const std::string actionText = BuildActionText(m_state);
    m_actionLabel->SetLabel(wxString::FromUTF8(actionText.c_str()));
    m_actionLabel->SetForegroundColour(m_mutedColor);

    // Defensive sizing for the right-side affordance.  Keep the
    // attached-project token compact, but leave enough room for
    // "[ + attach ]" when no project is active.
    if (m_actionLabel) {
        const int floorWidth = m_state.hasProject ? 56 : 110;
        const wxSize measured = m_actionLabel->GetTextExtent(m_actionLabel->GetLabel());
        const int actionWidth = std::max(floorWidth, measured.GetWidth() + 12);
        m_actionLabel->SetMinSize(wxSize(actionWidth, -1));
        m_actionLabel->InvalidateBestSize();
    }

    if (m_stateLabel) {
        m_stateLabel->InvalidateBestSize();
    }

    // The state text changes width when project info changes; force the
    // row and owning parent to relayout so the right-side affordance stays
    // flush right and fully visible.
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

    if (m_panel)       m_panel->SetBackgroundColour(m_bgColor);
    if (m_row)         m_row->SetBackgroundColour(m_bgColor);
    if (m_stateLabel)  m_stateLabel->SetForegroundColour(m_textColor);
    if (m_actionLabel) {
        m_actionLabel->SetForegroundColour(m_mutedColor);
    }
    if (m_separator)   m_separator->SetBackgroundColour(m_borderColor);

    if (m_panel) m_panel->Refresh();
}
