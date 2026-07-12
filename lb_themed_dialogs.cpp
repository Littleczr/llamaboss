#include "lb_themed_dialogs.h"

#include "widgets.h"   // ApplyDarkTitleBar

#include <wx/button.h>
#include <wx/listbox.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <utility>

namespace {

wxButton* LbMakeThemedAccentButton(wxWindow* parent, wxWindowID id,
                                   const wxString& label, const ThemeData& theme,
                                   int height = 32)
{
    auto* button = new wxButton(parent, id, label,
                                wxDefaultPosition, wxSize(-1, height),
                                wxBORDER_NONE);
    wxFont font = button->GetFont();
    font.SetPointSize(10);
    font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    button->SetFont(font);
    button->SetBackgroundColour(theme.accentButton);
    button->SetForegroundColour(theme.accentButtonText);
    return button;
}

wxButton* LbMakeThemedFlatButton(wxWindow* parent, wxWindowID id,
                                 const wxString& label, const ThemeData& theme,
                                 int height = 32)
{
    auto* button = new wxButton(parent, id, label,
                                wxDefaultPosition, wxSize(-1, height),
                                wxBORDER_NONE);
    wxFont font = button->GetFont();
    font.SetPointSize(10);
    font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    button->SetFont(font);
    button->SetBackgroundColour(theme.bgDialogSurface);
    button->SetForegroundColour(theme.textMuted);
    return button;
}

} // namespace

LbThemedTextEntryDialog::LbThemedTextEntryDialog(wxWindow* parent,
                                                 const ThemeData& theme,
                                                 const wxString& title,
                                                 const wxString& prompt,
                                                 const wxString& actionLabel)
    : wxDialog(parent, wxID_ANY, title,
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
{
    SetBackgroundColour(m_theme.bgDialogSurface);

    wxFont base = GetFont();
    base.SetPointSize(11);
    SetFont(base);

    auto* top = new wxBoxSizer(wxVERTICAL);

    auto* label = new wxStaticText(this, wxID_ANY, prompt);
    label->SetForegroundColour(m_theme.textPrimary);
    top->Add(label, 0, wxALL, 12);

    m_input = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                             wxDefaultPosition, wxSize(340, -1),
                             wxTE_PROCESS_ENTER);
    // Match Settings-window native edit controls: the dialog surface stays
    // themed, but the single-line entry well remains the familiar high-
    // contrast Windows input style instead of the darker chat-input gray.
    m_input->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    m_input->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    top->Add(m_input, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    auto* line = new wxPanel(this, wxID_ANY,
                             wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(m_theme.borderSubtle);
    top->Add(line, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);

    m_okButton = LbMakeThemedAccentButton(this, wxID_OK, actionLabel, m_theme);
    m_okButton->SetMinSize(wxSize(96, 32));

    auto* cancelButton = LbMakeThemedFlatButton(this, wxID_CANCEL, "Cancel", m_theme);
    cancelButton->SetMinSize(wxSize(96, 32));

    buttons->Add(m_okButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    buttons->Add(cancelButton, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    SetSizerAndFit(top);
    SetMinSize(wxSize(420, 190));
    CentreOnParent();
    SetAffirmativeId(wxID_OK);
    SetEscapeId(wxID_CANCEL);
    m_okButton->SetDefault();
    UpdateOkButton();

    m_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        UpdateOkButton();
    });
    m_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
        if (m_okButton && m_okButton->IsEnabled()) {
            EndModal(wxID_OK);
        }
    });

    ApplyDarkTitleBar(this, m_theme.name != "light");

    // Do not steal focus from the constructor. The modal scrim is shown
    // after the dialog object is constructed but before ShowModal() makes
    // the dialog visible. Focusing a child control here can give the hidden
    // dialog an early native z-order, then the scrim may be raised above it
    // and swallow all input. Focus the entry after the dialog is actually
    // shown instead.
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        event.Skip();
        if (!event.IsShown()) return;
        CallAfter([this]() {
            if (m_input) {
                m_input->SetFocus();
                m_input->SetInsertionPointEnd();
            }
        });
    });
}

wxString LbThemedTextEntryDialog::GetValue() const
{
    return m_input ? m_input->GetValue() : wxString();
}

void LbThemedTextEntryDialog::UpdateOkButton()
{
    if (!m_okButton || !m_input) return;
    wxString value = m_input->GetValue();
    value.Trim(true);
    value.Trim(false);
    m_okButton->Enable(!value.empty());
}

LbThemedSingleChoiceDialog::LbThemedSingleChoiceDialog(wxWindow* parent,
                                                       const ThemeData& theme,
                                                       const wxString& title,
                                                       const wxString& prompt,
                                                       const wxArrayString& choices,
                                                       const wxString& actionLabel)
    : wxDialog(parent, wxID_ANY, title,
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
{
    SetBackgroundColour(m_theme.bgDialogSurface);

    wxFont base = GetFont();
    base.SetPointSize(11);
    SetFont(base);

    auto* top = new wxBoxSizer(wxVERTICAL);

    auto* label = new wxStaticText(this, wxID_ANY, prompt);
    label->SetForegroundColour(m_theme.textPrimary);
    top->Add(label, 0, wxALL, 12);

    m_list = new wxListBox(this, wxID_ANY,
                           wxDefaultPosition, wxSize(560, 210),
                           choices,
                           wxLB_SINGLE | wxBORDER_SIMPLE);
    m_list->SetBackgroundColour(m_theme.bgInputField);
    m_list->SetForegroundColour(m_theme.textPrimary);
    top->Add(m_list, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    auto* line = new wxPanel(this, wxID_ANY,
                             wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(m_theme.borderSubtle);
    top->Add(line, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);

    m_okButton = LbMakeThemedAccentButton(this, wxID_OK, actionLabel, m_theme);
    m_okButton->SetMinSize(wxSize(96, 32));

    auto* cancelButton = LbMakeThemedFlatButton(this, wxID_CANCEL, "Cancel", m_theme);
    cancelButton->SetMinSize(wxSize(96, 32));

    buttons->Add(m_okButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    buttons->Add(cancelButton, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    SetSizerAndFit(top);
    SetMinSize(wxSize(620, 360));
    CentreOnParent();
    SetAffirmativeId(wxID_OK);
    SetEscapeId(wxID_CANCEL);
    m_okButton->SetDefault();

    if (m_list && m_list->GetCount() > 0) {
        m_list->SetSelection(0);
    }
    UpdateOkButton();

    m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
        UpdateOkButton();
    });
    m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
        if (m_okButton && m_okButton->IsEnabled()) {
            EndModal(wxID_OK);
        }
    });
    Bind(wxEVT_CHAR_HOOK, &LbThemedSingleChoiceDialog::OnCharHook, this);

    ApplyDarkTitleBar(this, m_theme.name != "light");

    // Match the text-entry dialog: let the modal scrim appear first, then
    // focus the list after ShowModal() has shown this dialog. This keeps the
    // scrim behind the dialog instead of above it.
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        event.Skip();
        if (!event.IsShown()) return;
        CallAfter([this]() {
            if (m_list) m_list->SetFocus();
        });
    });
}

int LbThemedSingleChoiceDialog::GetSelection() const
{
    return m_list ? m_list->GetSelection() : wxNOT_FOUND;
}

void LbThemedSingleChoiceDialog::SetDeleteHandler(DeleteHandler handler)
{
    m_deleteHandler = std::move(handler);
}

void LbThemedSingleChoiceDialog::UpdateOkButton()
{
    if (!m_okButton || !m_list) return;
    m_okButton->Enable(m_list->GetSelection() != wxNOT_FOUND);
}

void LbThemedSingleChoiceDialog::OnCharHook(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    if ((key == WXK_DELETE || key == WXK_NUMPAD_DELETE) && TryDeleteSelection()) {
        return;
    }
    event.Skip();
}

bool LbThemedSingleChoiceDialog::TryDeleteSelection()
{
    if (!m_list || !m_deleteHandler) return false;

    const int sel = m_list->GetSelection();
    if (sel == wxNOT_FOUND) return false;

    const wxString label = m_list->GetString(sel);
    if (!m_deleteHandler(sel, label)) return true;

    m_list->Delete(static_cast<unsigned int>(sel));
    const unsigned int count = m_list->GetCount();
    if (count > 0) {
        const unsigned int next = static_cast<unsigned int>(
            std::min<int>(sel, static_cast<int>(count) - 1));
        m_list->SetSelection(next);
    }
    UpdateOkButton();
    return true;
}
