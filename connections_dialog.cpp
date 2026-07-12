// connections_dialog.cpp
#define _CRT_SECURE_NO_WARNINGS

#include "connections_dialog.h"
#include "secrets_store.h"
#include "theme.h"
#include "widgets.h"   // ApplyDialogThemeRecursive, ApplyDarkTitleBar

#include <wx/textdlg.h>
#include <wx/sizer.h>
#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>

#include <string>

// ─── Event table ────────────────────────────────────────────────

enum {
    ID_CONN_ADD = wxID_HIGHEST + 3000,
    ID_CONN_EDIT,
    ID_CONN_DELETE,
    ID_CONN_LIST
};

// ─────────────────────────────────────────────────────────────────
//  Button helpers (file-local)
// ─────────────────────────────────────────────────────────────────
//
// Same Telegram-style recipe as the rest of the dialog family
// (settings.cpp / model_manager.cpp / model_downloader.cpp /
// project_attach_dialog.cpp): wxButton + wxBORDER_NONE so Win11 doesn't
// paint native chrome over the solid fill, semibold 10pt label, theme
// palette applied in place.
//
//   MakeAccentButton      — t.accentButton fill, white label
//   MakeDestructiveButton — t.stopButton fill, white label (solid red)
//   MakeFlatButton        — borderless, dialog-surface fill, muted text
//
namespace {

wxButton* MakeAccentButton(wxWindow* parent, wxWindowID id,
                           const wxString& label, const ThemeData& t,
                           int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.accentButton);
    btn->SetForegroundColour(t.accentButtonText);
    return btn;
}

wxButton* MakeDestructiveButton(wxWindow* parent, wxWindowID id,
                                const wxString& label, const ThemeData& t,
                                int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.stopButton);
    btn->SetForegroundColour(t.stopButtonText);
    return btn;
}

wxButton* MakeFlatButton(wxWindow* parent, wxWindowID id,
                         const wxString& label, const ThemeData& t,
                         int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.bgDialogSurface);
    btn->SetForegroundColour(t.textMuted);
    return btn;
}

// Themed 1px separator — replaces wxStaticLine, which can't be
// cleanly recoloured on Windows. Used inside the small Edit modal
// to divide the form into a few visual sections without dropping
// chunky bands into a compact dialog.
wxPanel* MakeHairline(wxWindow* parent, const ThemeData& t)
{
    auto* line = new wxPanel(parent, wxID_ANY,
                             wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(t.borderSubtle);
    return line;
}

constexpr int kHintWrapWidth = 420;

bool IsSafeConnectionIdentifier(const wxString& s)
{
    if (s.IsEmpty()) return false;

    for (size_t i = 0; i < s.length(); ++i) {
        const wxUniChar c = s[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_') {
            continue;
        }
        return false;
    }

    return true;
}

void ShowInvalidIdentifierMessage(wxWindow* parent,
                                  const wxString& fieldName,
                                  const wxString& example)
{
    wxMessageBox(
        fieldName +
            " must use lowercase letters, numbers, and underscores only.\n\n"
            "Example: " + example,
        "Invalid " + fieldName.Lower(),
        wxOK | wxICON_WARNING,
        parent);
}

}  // anonymous namespace

// ─── Composite Add/Edit dialog ──────────────────────────────────
//
// Replaces the previous three-step wxGetTextFromUser chain.  Shows
// every field on screen at once so users can see what they're
// entering and which radio mode they've selected.  OK validates and
// refuses to close with a short message if anything important is
// missing; direct-value edits may leave the value blank to keep the
// existing hidden secret.
//
// The Value field's label switches between "API key value" and
// "Environment variable name" based on the radio choice — so the
// user always knows what kind of thing belongs in that field.
//
namespace {

class ConnectionEditDialog : public wxDialog
{
public:
    ConnectionEditDialog(wxWindow* parent,
                         const wxString& title,
                         const wxString& seedProvider,
                         const wxString& seedKey,
                         bool            seedIsEnvRef,
                         const wxString& seedValue,
                         bool            allowEmptyDirectValue,
                         const ThemeData& theme)
        : wxDialog(parent, wxID_ANY, title,
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE),
          m_allowEmptyDirectValue(allowEmptyDirectValue)
    {
        // Use the dialog-surface colour to match the rest of the
        // dialog family rather than the darker chat-area bgMain.
        SetBackgroundColour(theme.bgDialogSurface);

        // Bump the dialog default font to match settings.cpp (11pt body).
        wxFont base = GetFont();
        base.SetPointSize(11);
        SetFont(base);

        auto* root = new wxBoxSizer(wxVERTICAL);

        // ── Provider ────────────────────────────────────────────
        auto* providerRow = new wxBoxSizer(wxHORIZONTAL);
        auto* providerLabel = new wxStaticText(this, wxID_ANY,
            "Provider name:");
        providerLabel->SetMinSize(wxSize(140, -1));
        providerRow->Add(providerLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        m_providerField = new wxTextCtrl(this, wxID_ANY, seedProvider,
                                         wxDefaultPosition,
                                         wxSize(280, -1));
        providerRow->Add(m_providerField, 1, wxALIGN_CENTER_VERTICAL);
        root->Add(providerRow, 0, wxEXPAND | wxALL, 12);

        auto* providerHint = new wxStaticText(this, wxID_ANY,
            "Lowercase letters, numbers, and underscores. "
            "Examples: gmail, smartsheet, runpod.");
        { wxFont f = providerHint->GetFont(); f.SetPointSize(10);
          providerHint->SetFont(f); }
        providerHint->SetForegroundColour(theme.textMuted);
        providerHint->Wrap(kHintWrapWidth);
        root->Add(providerHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        // ── Key ─────────────────────────────────────────────────
        auto* keyRow = new wxBoxSizer(wxHORIZONTAL);
        auto* keyLabel = new wxStaticText(this, wxID_ANY,
            "Key name:");
        keyLabel->SetMinSize(wxSize(140, -1));
        keyRow->Add(keyLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        m_keyField = new wxTextCtrl(this, wxID_ANY,
            seedKey.IsEmpty() ? wxString("api_key") : seedKey,
            wxDefaultPosition, wxSize(280, -1));
        keyRow->Add(m_keyField, 1, wxALIGN_CENTER_VERTICAL);
        root->Add(keyRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

        auto* keyHint = new wxStaticText(this, wxID_ANY,
            "Lowercase identifier. Becomes the suffix of the injected "
            "env var, e.g. 'api_key' \xE2\x86\x92 GMAIL_API_KEY.");
        { wxFont f = keyHint->GetFont(); f.SetPointSize(10);
          keyHint->SetFont(f); }
        keyHint->SetForegroundColour(theme.textMuted);
        keyHint->Wrap(kHintWrapWidth);
        root->Add(keyHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        // ── Storage radio ───────────────────────────────────────
        root->Add(MakeHairline(this, theme), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        auto* storageLabel = new wxStaticText(this, wxID_ANY, "Storage:");
        { wxFont f = storageLabel->GetFont(); f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
          storageLabel->SetFont(f); }
        root->Add(storageLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        m_directRadio = new wxRadioButton(this, wxID_ANY,
            "Paste value directly into secrets.json",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_envRefRadio = new wxRadioButton(this, wxID_ANY,
            "Reference an environment variable");
        m_directRadio->SetValue(!seedIsEnvRef);
        m_envRefRadio->SetValue(seedIsEnvRef);

        root->Add(m_directRadio, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
        root->Add(m_envRefRadio, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        // ── Value ───────────────────────────────────────────────
        auto* valueRow = new wxBoxSizer(wxHORIZONTAL);
        m_valueLabel = new wxStaticText(this, wxID_ANY, "API key value:");
        m_valueLabel->SetMinSize(wxSize(140, -1));
        valueRow->Add(m_valueLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        m_valueField = new wxTextCtrl(this, wxID_ANY, seedValue,
                                      wxDefaultPosition, wxSize(280, -1));
        valueRow->Add(m_valueField, 1, wxALIGN_CENTER_VERTICAL);
        root->Add(valueRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

        m_valueHint = new wxStaticText(this, wxID_ANY, wxEmptyString);
        { wxFont f = m_valueHint->GetFont(); f.SetPointSize(10);
          m_valueHint->SetFont(f); }
        m_valueHint->SetForegroundColour(theme.textMuted);
        root->Add(m_valueHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        // ── Buttons ─────────────────────────────────────────────
        root->Add(MakeHairline(this, theme), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
        btnRow->AddStretchSpacer();
        auto* cancelBtn = MakeFlatButton(this, wxID_CANCEL, "Cancel", theme);
        auto* okBtn     = MakeAccentButton(this, wxID_OK,    "OK",     theme);
        okBtn->SetMinSize(wxSize(96, 32));
        btnRow->Add(cancelBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        btnRow->Add(okBtn,     0, wxALIGN_CENTER_VERTICAL);
        root->Add(btnRow, 0, wxEXPAND | wxALL, 12);

        // Populate and wrap the value hint before fitting the dialog.
        // Otherwise the dialog can be min-sized while the hint is empty,
        // and long env-ref guidance may clip after the radio toggle fills it.
        UpdateValueLabel();

        SetSizerAndFit(root);
        SetMinSize(GetSize());

        // ── Bindings ────────────────────────────────────────────
        m_directRadio->Bind(wxEVT_RADIOBUTTON,
            &ConnectionEditDialog::OnRadioChanged, this);
        m_envRefRadio->Bind(wxEVT_RADIOBUTTON,
            &ConnectionEditDialog::OnRadioChanged, this);
        okBtn->Bind(wxEVT_BUTTON, &ConnectionEditDialog::OnOK, this);

        okBtn->SetDefault();   // Enter activates OK
        m_providerField->SetFocus();

        // ── Theming ─────────────────────────────────────────────
        // ApplyDialogThemeRecursive paints foregrounds on wxStaticText
        // and bg/fg on wxButton across the whole dialog. We re-tint
        // our accent/flat buttons afterwards because the recursive
        // pass would have flattened them to the neutral input-field
        // surface.
        ApplyDialogThemeRecursive(this,
                                  theme.textPrimary,
                                  theme.bgInputField,
                                  theme.textPrimary);

        // Restore the accent/flat palette the helpers applied.
        okBtn    ->SetBackgroundColour(theme.accentButton);
        okBtn    ->SetForegroundColour(theme.accentButtonText);
        cancelBtn->SetBackgroundColour(theme.bgDialogSurface);
        cancelBtn->SetForegroundColour(theme.textMuted);

        // Restore muted-text colour on the hint labels, which the
        // recursive helper just painted with textPrimary.
        providerHint->SetForegroundColour(theme.textMuted);
        keyHint     ->SetForegroundColour(theme.textMuted);
        m_valueHint ->SetForegroundColour(theme.textMuted);

        // Radio buttons aren't touched by the recursive helper --
        // ApplyDialogThemeRecursive only matches wxStaticText and
        // wxButton.  Set their colors explicitly here, plus their
        // backgrounds so the radio "halo" doesn't show a Windows-
        // gray fringe on the dark dialog.
        for (wxRadioButton* rb : { m_directRadio, m_envRefRadio }) {
            rb->SetForegroundColour(theme.textPrimary);
            rb->SetBackgroundColour(theme.bgDialogSurface);
        }

        // Match the title bar (Win11) to the body when we're on a
        // dark theme; no-op on light.
        ApplyDarkTitleBar(this, theme.name != "light");
    }

    wxString GetProvider() const { return m_providerField->GetValue().Trim().Trim(false); }
    wxString GetKey()      const { return m_keyField->GetValue().Trim().Trim(false); }
    wxString GetValue()    const { return m_valueField->GetValue().Trim().Trim(false); }
    bool     IsEnvRef()    const { return m_envRefRadio->GetValue(); }

private:
    void OnRadioChanged(wxCommandEvent&)
    {
        UpdateValueLabel();
    }

    void UpdateValueLabel()
    {
        if (m_envRefRadio->GetValue()) {
            m_valueLabel->SetLabel("Env var name:");
            m_valueHint->SetLabel(
                "Name of the environment variable holding the actual "
                "key, e.g. GMAIL_API_KEY.");
        } else {
            m_valueLabel->SetLabel("API key value:");
            if (m_allowEmptyDirectValue) {
                m_valueHint->SetLabel(
                    "Stored as plaintext in secrets.json. Leave blank "
                    "to keep the existing secret.");
            } else {
                m_valueHint->SetLabel(
                    "Stored as plaintext in secrets.json (user-only file "
                    "ACL).");
            }
        }

        m_valueHint->Wrap(kHintWrapWidth);
        if (GetSizer()) Layout();
    }

    void OnOK(wxCommandEvent& evt)
    {
        wxString provider = GetProvider();
        wxString key      = GetKey();
        wxString value    = GetValue();

        // Validate.  Short, specific messages so the user knows
        // which field to fix.
        if (provider.IsEmpty()) {
            wxMessageBox("Provider name is required.",
                "Missing field", wxOK | wxICON_INFORMATION, this);
            m_providerField->SetFocus();
            return;
        }
        if (key.IsEmpty()) {
            wxMessageBox("Key name is required.",
                "Missing field", wxOK | wxICON_INFORMATION, this);
            m_keyField->SetFocus();
            return;
        }
        if (!IsSafeConnectionIdentifier(provider)) {
            ShowInvalidIdentifierMessage(this, "Provider name", "gmail");
            m_providerField->SetFocus();
            return;
        }

        if (!IsSafeConnectionIdentifier(key)) {
            ShowInvalidIdentifierMessage(this, "Key name", "api_key");
            m_keyField->SetFocus();
            return;
        }

        const bool allowBlankDirectValue =
            !m_envRefRadio->GetValue() && m_allowEmptyDirectValue;

        if (value.IsEmpty() && !allowBlankDirectValue) {
            wxMessageBox(
                m_envRefRadio->GetValue()
                    ? wxString("Environment variable name is required.")
                    : wxString("API key value is required."),
                "Missing field", wxOK | wxICON_INFORMATION, this);
            m_valueField->SetFocus();
            return;
        }

        evt.Skip();   // proceed with EndModal(wxID_OK)
    }

    wxTextCtrl*    m_providerField = nullptr;
    wxTextCtrl*    m_keyField      = nullptr;
    wxRadioButton* m_directRadio   = nullptr;
    wxRadioButton* m_envRefRadio   = nullptr;
    wxStaticText*  m_valueLabel    = nullptr;
    wxStaticText*  m_valueHint     = nullptr;
    wxTextCtrl*    m_valueField    = nullptr;
    bool           m_allowEmptyDirectValue = false;
};

}  // anonymous namespace

wxBEGIN_EVENT_TABLE(ConnectionsDialog, wxDialog)
    EVT_BUTTON(ID_CONN_ADD,    ConnectionsDialog::OnAdd)
    EVT_BUTTON(ID_CONN_EDIT,   ConnectionsDialog::OnEdit)
    EVT_BUTTON(ID_CONN_DELETE, ConnectionsDialog::OnDelete)
    EVT_BUTTON(wxID_CLOSE,     ConnectionsDialog::OnClose)
    EVT_LIST_ITEM_ACTIVATED(ID_CONN_LIST, ConnectionsDialog::OnItemActivated)
    EVT_LIST_ITEM_SELECTED  (ID_CONN_LIST, ConnectionsDialog::OnSelectionChanged)
    EVT_LIST_ITEM_DESELECTED(ID_CONN_LIST, ConnectionsDialog::OnSelectionChanged)
wxEND_EVENT_TABLE()

// ─── ctor / layout ──────────────────────────────────────────────

ConnectionsDialog::ConnectionsDialog(wxWindow* parent,
                                     SecretsStore* store,
                                     const ThemeData& theme)
    : wxDialog(parent, wxID_ANY, "Connections",
               wxDefaultPosition, wxSize(580, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_store(store)
    , m_theme(&theme)
{
    // Bump the dialog default font to match settings.cpp (11pt body).
    wxFont base = GetFont();
    base.SetPointSize(11);
    SetFont(base);

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* help = new wxStaticText(this, wxID_ANY,
        "Stored API keys for service skills. Skills running through "
        "python_run_script see these as environment variables named "
        "<PROVIDER>_<KEY> (e.g. GMAIL_API_KEY).");
    help->Wrap(540);
    root->Add(help, 0, wxALL, 12);

    m_list = new wxListCtrl(this, ID_CONN_LIST,
                            wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    m_list->InsertColumn(0, "Provider", wxLIST_FORMAT_LEFT, 140);
    m_list->InsertColumn(1, "Key",      wxLIST_FORMAT_LEFT, 140);
    m_list->InsertColumn(2, "Value",    wxLIST_FORMAT_LEFT, 240);
    root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Button row:
    //   Add (accent)  Edit (accent)  Delete (red)  ──stretch──  Close (flat)
    //
    // Management trio on the left clusters by function (the list-row
    // actions). Delete carries the destructive solid-red palette so the
    // visual hierarchy reads correctly even though it's grouped with
    // the additive actions. Close sits alone on the right as a flat
    // dismissal — same shape Telegram uses for modal close buttons.
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    m_addBtn   = MakeAccentButton     (this, ID_CONN_ADD,    "Add...",  theme);
    m_editBtn  = MakeAccentButton     (this, ID_CONN_EDIT,   "Edit...", theme);
    m_delBtn   = MakeDestructiveButton(this, ID_CONN_DELETE, "Delete",  theme);
    m_closeBtn = MakeFlatButton       (this, wxID_CLOSE,     "Close",   theme);

    btnRow->Add(m_addBtn,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    btnRow->Add(m_editBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    btnRow->Add(m_delBtn,  0, wxALIGN_CENTER_VERTICAL);
    btnRow->AddStretchSpacer();
    btnRow->Add(m_closeBtn, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(btnRow, 0, wxEXPAND | wxALL, 12);

    SetSizer(root);
    ApplyTheme();
    RebuildList();
    UpdateButtonState();

    // Enter activates the default Close button when focus is not on the list;
    // Esc closes through SetEscapeId.
    m_closeBtn->SetDefault();
    SetEscapeId(wxID_CLOSE);
}

void ConnectionsDialog::ApplyTheme()
{
    if (!m_theme) return;

    // Use the dialog-surface colour for consistency with the rest of
    // the dialog family (settings, model_manager, etc.). Was bgMain
    // previously — the chat area shade — which made this modal read
    // one tier darker than its parent Settings dialog.
    SetBackgroundColour(m_theme->bgDialogSurface);

    // bgInputArea is the theme's slightly-lifted surface used for
    // input controls and chip bars — the closest match for a
    // listbox-style background that won't clash on dark or light.
    m_list->SetBackgroundColour(m_theme->bgInputArea);
    m_list->SetForegroundColour(m_theme->textPrimary);

    // Walk the dialog tree applying foreground/button colors to
    // every wxStaticText and wxButton.  Without this, the help
    // text renders with the system default (near-black), which is
    // unreadable on the dark surface.
    ApplyDialogThemeRecursive(this,
                              m_theme->textPrimary,
                              m_theme->bgInputField,
                              m_theme->textPrimary);

    // Re-apply our accent/destructive/flat palette over the neutral
    // cascade. The recursive helper paints every wxButton with
    // bgInputField; the four buttons here have specific roles that
    // demand their own colours.
    m_addBtn  ->SetBackgroundColour(m_theme->accentButton);
    m_addBtn  ->SetForegroundColour(m_theme->accentButtonText);
    m_editBtn ->SetBackgroundColour(m_theme->accentButton);
    m_editBtn ->SetForegroundColour(m_theme->accentButtonText);
    m_delBtn  ->SetBackgroundColour(m_theme->stopButton);
    m_delBtn  ->SetForegroundColour(m_theme->stopButtonText);
    m_closeBtn->SetBackgroundColour(m_theme->bgDialogSurface);
    m_closeBtn->SetForegroundColour(m_theme->textMuted);

    ApplyDarkTitleBar(this, m_theme->name != "light");
}

// ─── List management ────────────────────────────────────────────

void ConnectionsDialog::RebuildList(const wxString& selectProvider,
                                    const wxString& selectKey)
{
    m_list->DeleteAllItems();
    if (!m_store) return;

    const bool wantSelection = !selectProvider.IsEmpty() && !selectKey.IsEmpty();

    auto rows = m_store->ListConnections();
    long idx = 0;
    for (const auto& r : rows) {
        const wxString provider = wxString::FromUTF8(r.provider.c_str());
        const wxString key      = wxString::FromUTF8(r.key.c_str());

        long item = m_list->InsertItem(idx, provider);
        m_list->SetItem(item, 1, key);
        m_list->SetItem(item, 2, wxString::FromUTF8(r.displayHint.c_str()));

        if (wantSelection && provider == selectProvider && key == selectKey) {
            m_list->SetItemState(item,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_list->EnsureVisible(item);
        }

        ++idx;
    }
}

void ConnectionsDialog::UpdateButtonState()
{
    bool hasSelection = (m_list->GetSelectedItemCount() > 0);
    m_editBtn->Enable(hasSelection);
    m_delBtn->Enable(hasSelection);
}

void ConnectionsDialog::OnSelectionChanged(wxListEvent&)
{
    UpdateButtonState();
}

void ConnectionsDialog::OnItemActivated(wxListEvent&)
{
    wxCommandEvent dummy;
    OnEdit(dummy);
}

// ─── Add / Edit / Delete ────────────────────────────────────────

bool ConnectionsDialog::PromptForConnection(const wxString& title,
                                            wxString& ioProvider,
                                            wxString& ioKey,
                                            wxString& ioValue,
                                            bool&     ioIsEnvRef,
                                            bool      allowEmptyDirectValue)
{
    // Single composite dialog -- every field on screen at once, with
    // a radio toggle for storage mode and a value label that updates
    // to match.  Replaces the previous three-step wxGetTextFromUser
    // chain that was easy to misread.
    ConnectionEditDialog dlg(this, title,
                             ioProvider, ioKey,
                             ioIsEnvRef, ioValue,
                             allowEmptyDirectValue,
                             *m_theme);

    if (dlg.ShowModal() != wxID_OK) return false;

    ioProvider = dlg.GetProvider();
    ioKey      = dlg.GetKey();
    ioValue    = dlg.GetValue();
    ioIsEnvRef = dlg.IsEnvRef();
    return true;
}

void ConnectionsDialog::OnAdd(wxCommandEvent&)
{
    if (!m_store) return;

    wxString provider, key, value;
    bool isEnvRef = false;
    if (!PromptForConnection("Add connection", provider, key, value, isEnvRef))
        return;

    std::string p = provider.ToUTF8().data();
    std::string k = key.ToUTF8().data();
    std::string v = value.ToUTF8().data();

    if (m_store->HasSecret(p, k)) {
        int ans = wxMessageBox(
            wxString::Format(
                "A connection named %s.%s already exists. Replace it?",
                provider, key),
            "Replace existing connection?",
            wxYES_NO | wxICON_WARNING,
            this);
        if (ans != wxYES) return;
    }

    if (isEnvRef) m_store->SetSecretEnvRef(p, k, v);
    else          m_store->SetSecret(p, k, v);

    RebuildList(provider, key);
    UpdateButtonState();
}

void ConnectionsDialog::OnEdit(wxCommandEvent&)
{
    if (!m_store) return;

    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) return;

    // Capture the row's original identity so we can remove it if the
    // user renames the provider or key in the dialog -- otherwise we
    // leave the old entry orphaned and end up with two rows.
    wxString originalProvider = m_list->GetItemText(sel, 0);
    wxString originalKey      = m_list->GetItemText(sel, 1);

    const std::string originalP = std::string(originalProvider.ToUTF8().data());
    const std::string originalK = std::string(originalKey.ToUTF8().data());

    SecretsStore::SecretEntry originalEntry;
    if (!m_store->TryGetSecretEntry(originalP, originalK, originalEntry)) {
        wxMessageBox(
            "Could not read the selected connection. The list will be refreshed.",
            "Connection unavailable",
            wxOK | wxICON_WARNING,
            this);
        RebuildList();
        UpdateButtonState();
        return;
    }

    wxString provider = originalProvider;
    wxString key      = originalKey;
    bool isEnvRef     = originalEntry.isEnvRef;
    wxString value;

    // Env-ref names are safe to show because they are not the secret.
    // Direct values are intentionally not preloaded; an empty direct-value
    // field means "keep existing" for edits of an existing direct secret.
    if (originalEntry.isEnvRef) {
        value = wxString::FromUTF8(originalEntry.value.c_str());
    }

    if (!PromptForConnection("Edit connection",
                             provider, key, value, isEnvRef,
                             !originalEntry.isEnvRef)) {
        return;
    }

    std::string p = provider.ToUTF8().data();
    std::string k = key.ToUTF8().data();
    std::string v = value.ToUTF8().data();

    const bool identityChanged =
        (originalProvider != provider || originalKey != key);

    if (identityChanged && m_store->HasSecret(p, k)) {
        int ans = wxMessageBox(
            wxString::Format(
                "A connection named %s.%s already exists. Replace it?",
                provider, key),
            "Replace existing connection?",
            wxYES_NO | wxICON_WARNING,
            this);
        if (ans != wxYES) return;
    }

    // For direct-value edits, blank means keep the existing hidden secret.
    if (!isEnvRef && v.empty() && !originalEntry.isEnvRef) {
        v = originalEntry.value;
    }

    // Drop the original row if its identity changed.  If the user kept
    // the same provider+key, SetSecret below overwrites the existing
    // value in place.
    if (identityChanged) {
        m_store->RemoveSecret(originalP, originalK);
    }

    if (isEnvRef) m_store->SetSecretEnvRef(p, k, v);
    else          m_store->SetSecret(p, k, v);

    RebuildList(provider, key);
    UpdateButtonState();
}

void ConnectionsDialog::OnDelete(wxCommandEvent&)
{
    if (!m_store) return;

    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) return;

    wxString provider = m_list->GetItemText(sel, 0);
    wxString key      = m_list->GetItemText(sel, 1);

    int ans = wxMessageBox(
        wxString::Format("Delete %s.%s?", provider, key),
        "Confirm", wxYES_NO | wxICON_WARNING, this);
    if (ans != wxYES) return;

    m_store->RemoveSecret(
        std::string(provider.ToUTF8().data()),
        std::string(key.ToUTF8().data()));

    RebuildList();
    UpdateButtonState();
}

void ConnectionsDialog::OnClose(wxCommandEvent&)
{
    EndModal(wxID_CLOSE);
}
