// endpoints_dialog.cpp
#define _CRT_SECURE_NO_WARNINGS

#include "endpoints_dialog.h"
#include "endpoint_store.h"
#include "theme.h"
#include "widgets.h"   // ApplyDialogThemeRecursive, ApplyDarkTitleBar

#include <wx/sizer.h>
#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>

#include <algorithm>   // std::transform ([image] tag, case-insensitive)
#include <cctype>      // std::tolower
#include <sstream>
#include <string>

// ─── Event table ────────────────────────────────────────────────

enum {
    ID_EP_ADD = wxID_HIGHEST + 3200,
    ID_EP_EDIT,
    ID_EP_DELETE,
    ID_EP_LIST
};

// ─────────────────────────────────────────────────────────────────
//  Button / helper recipes (file-local) — same family as
//  connections_dialog.cpp so this dialog matches the rest.
// ─────────────────────────────────────────────────────────────────
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

wxPanel* MakeHairline(wxWindow* parent, const ThemeData& t)
{
    auto* line = new wxPanel(parent, wxID_ANY,
                             wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(t.borderSubtle);
    return line;
}

constexpr int kHintWrapWidth = 460;

// Endpoint ids, secret provider/key names: lowercase letters, digits,
// underscores — same identifier rule the Connections dialog uses.
bool IsSafeIdentifier(const wxString& s)
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

// ── Model-list text <-> vector ───────────────────────────────────
// The edit dialog presents the model list as one line per model:
//   anthropic/claude-sonnet-4.6 = Claude Sonnet 4.6
//   google/gemini-2.5-flash-image = Nano Banana [image]
// The part after the first '=' is the display name (optional; defaults
// to the id). A trailing "[image]" tag (case-insensitive) marks an
// image-generation model. Flags-as-line-suffix keeps the editor a
// single monospace text box rather than a nested list control with a
// checkbox column.
std::vector<EndpointStore::Model> ParseModelsText(const wxString& text)
{
    std::vector<EndpointStore::Model> out;
    std::istringstream ss(std::string(text.ToUTF8().data()));
    std::string line;
    while (std::getline(ss, line)) {
        // trim
        auto a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        auto b = line.find_last_not_of(" \t\r\n");
        line = line.substr(a, b - a + 1);
        if (line.empty()) continue;

        EndpointStore::Model m;

        // Strip a trailing [image] tag before the '=' split so it can
        // follow either a bare id or a display name.
        {
            static const std::string kTag = "[image]";
            if (line.size() >= kTag.size()) {
                std::string tail = line.substr(line.size() - kTag.size());
                std::transform(tail.begin(), tail.end(), tail.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (tail == kTag) {
                    m.imageOutput = true;
                    line.erase(line.size() - kTag.size());
                    auto e = line.find_last_not_of(" \t");
                    line = (e == std::string::npos)
                               ? std::string()
                               : line.substr(0, e + 1);
                    if (line.empty()) continue;  // tag with no model id
                }
            }
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            m.id = line;
            m.displayName = line;
        } else {
            std::string id   = line.substr(0, eq);
            std::string disp = line.substr(eq + 1);
            auto trim = [](std::string s) {
                auto x = s.find_first_not_of(" \t");
                if (x == std::string::npos) return std::string();
                auto y = s.find_last_not_of(" \t");
                return s.substr(x, y - x + 1);
            };
            m.id = trim(id);
            m.displayName = trim(disp);
            if (m.id.empty()) continue;
            if (m.displayName.empty()) m.displayName = m.id;
        }
        out.push_back(std::move(m));
    }
    return out;
}

wxString ModelsToText(const std::vector<EndpointStore::Model>& models)
{
    std::string out;
    for (const auto& m : models) {
        out += m.id;
        if (!m.displayName.empty() && m.displayName != m.id) {
            out += " = ";
            out += m.displayName;
        }
        if (m.imageOutput) out += " [image]";
        out += "\n";
    }
    return wxString::FromUTF8(out.c_str());
}

// ─── Composite Add/Edit dialog ──────────────────────────────────

class EndpointEditDialog : public wxDialog
{
public:
    EndpointEditDialog(wxWindow* parent,
                       const wxString& title,
                       const EndpointStore::Endpoint& seed,
                       const ThemeData& theme)
        : wxDialog(parent, wxID_ANY, title,
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE)
    {
        SetBackgroundColour(theme.bgDialogSurface);

        wxFont base = GetFont();
        base.SetPointSize(11);
        SetFont(base);

        auto* root = new wxBoxSizer(wxVERTICAL);

        auto labeledRow = [&](const wxString& label,
                              const wxString& value,
                              int width = 320) -> wxTextCtrl* {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(this, wxID_ANY, label);
            lbl->SetMinSize(wxSize(150, -1));
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            auto* field = new wxTextCtrl(this, wxID_ANY, value,
                                         wxDefaultPosition, wxSize(width, -1));
            row->Add(field, 1, wxALIGN_CENTER_VERTICAL);
            root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
            return field;
        };

        auto hint = [&](const wxString& text) {
            auto* h = new wxStaticText(this, wxID_ANY, text);
            wxFont f = h->GetFont(); f.SetPointSize(10); h->SetFont(f);
            h->SetForegroundColour(theme.textMuted);
            h->Wrap(kHintWrapWidth);
            root->Add(h, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
            m_hints.push_back(h);
        };

        // ── Identity ────────────────────────────────────────────
        m_idField = labeledRow("Endpoint id:",
                               wxString::FromUTF8(seed.id.c_str()));
        hint("Lowercase letters, numbers, underscores. Internal id, e.g. "
             "openrouter. Cannot be changed after creation is easiest, but "
             "renaming here just relabels it.");

        m_nameField = labeledRow("Display name:",
                                 wxString::FromUTF8(seed.displayName.c_str()));
        hint("Shown in the model picker, e.g. OpenRouter.");

        root->Add(MakeHairline(this, theme), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        // ── Transport ───────────────────────────────────────────
        m_baseUrlField = labeledRow("Base URL:",
                                    wxString::FromUTF8(seed.baseUrl.c_str()));
        hint("Scheme + host, no trailing slash. e.g. https://openrouter.ai/api");

        m_chatPathField = labeledRow("Chat path:",
                                     wxString::FromUTF8(seed.chatPath.c_str()));
        hint("Appended to the base URL. Default /v1/chat/completions.");

        root->Add(MakeHairline(this, theme), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        // ── Auth ────────────────────────────────────────────────
        auto* authLabel = new wxStaticText(this, wxID_ANY, "Auth header:");
        { wxFont f = authLabel->GetFont(); f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
          authLabel->SetFont(f); }
        root->Add(authLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        m_bearerRadio = new wxRadioButton(this, wxID_ANY,
            "Authorization: Bearer <key>   (OpenAI, OpenRouter)",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_xApiKeyRadio = new wxRadioButton(this, wxID_ANY,
            "x-api-key: <key>");
        const bool isXApiKey =
            (seed.authScheme == EndpointStore::AuthScheme::XApiKey);
        m_bearerRadio->SetValue(!isXApiKey);
        m_xApiKeyRadio->SetValue(isXApiKey);
        root->Add(m_bearerRadio,  0, wxLEFT | wxRIGHT | wxTOP, 12);
        root->Add(m_xApiKeyRadio, 0, wxLEFT | wxRIGHT | wxTOP, 6);

        m_providerField = labeledRow("Key provider:",
            wxString::FromUTF8(seed.secretProvider.c_str()));
        m_keyField = labeledRow("Key name:",
            wxString::FromUTF8(seed.secretKey.c_str()));
        hint("The API key is read from Connections using this provider and "
             "key name. Set the actual key value under Settings -> "
             "Connections.");

        root->Add(MakeHairline(this, theme), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        // ── Protocol ────────────────────────────────────────────
        auto* protoLabel = new wxStaticText(this, wxID_ANY, "Tool protocol:");
        { wxFont f = protoLabel->GetFont(); f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
          protoLabel->SetFont(f); }
        root->Add(protoLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        m_nativeRadio = new wxRadioButton(this, wxID_ANY,
            "Native (OpenAI-style tool calls)",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_xmlRadio = new wxRadioButton(this, wxID_ANY,
            "XML (LlamaBoss fallback)");
        const bool isXml = (seed.protocol == ToolProtocol::Xml);
        m_nativeRadio->SetValue(!isXml);
        m_xmlRadio->SetValue(isXml);
        root->Add(m_nativeRadio, 0, wxLEFT | wxRIGHT | wxTOP, 12);
        root->Add(m_xmlRadio,    0, wxLEFT | wxRIGHT | wxTOP, 6);

        root->Add(MakeHairline(this, theme), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        // ── Models ──────────────────────────────────────────────
        auto* modelsLabel = new wxStaticText(this, wxID_ANY, "Models:");
        { wxFont f = modelsLabel->GetFont(); f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
          modelsLabel->SetFont(f); }
        root->Add(modelsLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        m_modelsField = new wxTextCtrl(this, wxID_ANY,
            ModelsToText(seed.models),
            wxDefaultPosition, wxSize(-1, 110),
            wxTE_MULTILINE | wxHSCROLL);
        // Monospace for the id list — fits the rest of the app's voice.
        {
            wxFont mono(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
            m_modelsField->SetFont(mono);
        }
        root->Add(m_modelsField, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        hint("One model per line: <model id> = <display name>. The display "
             "name is optional. Append [image] to mark an image-generation "
             "model. e.g. google/gemini-2.5-flash-image = Nano Banana "
             "[image]");

        // ── Buttons ─────────────────────────────────────────────
        root->Add(MakeHairline(this, theme), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
        btnRow->AddStretchSpacer();
        auto* cancelBtn = MakeFlatButton(this, wxID_CANCEL, "Cancel", theme);
        auto* okBtn     = MakeAccentButton(this, wxID_OK,    "OK",     theme);
        okBtn->SetMinSize(wxSize(96, 32));
        btnRow->Add(cancelBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        btnRow->Add(okBtn,     0, wxALIGN_CENTER_VERTICAL);
        root->Add(btnRow, 0, wxEXPAND | wxALL, 12);

        SetSizerAndFit(root);
        SetMinSize(GetSize());

        okBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnOK, this);
        okBtn->SetDefault();
        m_idField->SetFocus();

        // ── Theming ─────────────────────────────────────────────
        ApplyDialogThemeRecursive(this,
                                  theme.textPrimary,
                                  theme.bgInputField,
                                  theme.textPrimary);

        okBtn    ->SetBackgroundColour(theme.accentButton);
        okBtn    ->SetForegroundColour(theme.accentButtonText);
        cancelBtn->SetBackgroundColour(theme.bgDialogSurface);
        cancelBtn->SetForegroundColour(theme.textMuted);

        for (auto* h : m_hints) h->SetForegroundColour(theme.textMuted);

        for (wxRadioButton* rb : { m_bearerRadio, m_xApiKeyRadio,
                                   m_nativeRadio, m_xmlRadio }) {
            rb->SetForegroundColour(theme.textPrimary);
            rb->SetBackgroundColour(theme.bgDialogSurface);
        }

        ApplyDarkTitleBar(this, theme.name != "light");
    }

    EndpointStore::Endpoint GetEndpoint() const
    {
        EndpointStore::Endpoint ep;
        auto trimmed = [](const wxTextCtrl* c) {
            return std::string(c->GetValue().Trim().Trim(false).ToUTF8().data());
        };
        ep.id          = trimmed(m_idField);
        ep.displayName = trimmed(m_nameField);
        if (ep.displayName.empty()) ep.displayName = ep.id;
        ep.baseUrl     = trimmed(m_baseUrlField);
        ep.chatPath    = trimmed(m_chatPathField);
        if (ep.chatPath.empty()) ep.chatPath = "/v1/chat/completions";
        ep.authScheme  = m_xApiKeyRadio->GetValue()
                             ? EndpointStore::AuthScheme::XApiKey
                             : EndpointStore::AuthScheme::Bearer;
        ep.secretProvider = trimmed(m_providerField);
        ep.secretKey      = trimmed(m_keyField);
        if (ep.secretKey.empty()) ep.secretKey = "api_key";
        ep.protocol    = m_xmlRadio->GetValue() ? ToolProtocol::Xml
                                                : ToolProtocol::Native;
        ep.models      = ParseModelsText(m_modelsField->GetValue());
        return ep;
    }

private:
    void OnOK(wxCommandEvent& evt)
    {
        EndpointStore::Endpoint ep = GetEndpoint();

        if (ep.id.empty()) {
            wxMessageBox("Endpoint id is required.", "Missing field",
                         wxOK | wxICON_INFORMATION, this);
            m_idField->SetFocus();
            return;
        }
        if (!IsSafeIdentifier(wxString::FromUTF8(ep.id.c_str()))) {
            wxMessageBox("Endpoint id must use lowercase letters, numbers, "
                         "and underscores only.\n\nExample: openrouter",
                         "Invalid id", wxOK | wxICON_WARNING, this);
            m_idField->SetFocus();
            return;
        }
        const bool urlOk =
            ep.baseUrl.rfind("http://", 0) == 0 ||
            ep.baseUrl.rfind("https://", 0) == 0;
        if (!urlOk) {
            wxMessageBox("Base URL must start with http:// or https://.",
                         "Invalid base URL", wxOK | wxICON_WARNING, this);
            m_baseUrlField->SetFocus();
            return;
        }
        if (ep.secretProvider.empty() ||
            !IsSafeIdentifier(wxString::FromUTF8(ep.secretProvider.c_str()))) {
            wxMessageBox("Key provider is required and must use lowercase "
                         "letters, numbers, and underscores.\n\n"
                         "Example: openrouter",
                         "Invalid key provider", wxOK | wxICON_WARNING, this);
            m_providerField->SetFocus();
            return;
        }
        if (!IsSafeIdentifier(wxString::FromUTF8(ep.secretKey.c_str()))) {
            wxMessageBox("Key name must use lowercase letters, numbers, and "
                         "underscores.\n\nExample: api_key",
                         "Invalid key name", wxOK | wxICON_WARNING, this);
            m_keyField->SetFocus();
            return;
        }
        if (ep.models.empty()) {
            wxMessageBox("Add at least one model (one per line).",
                         "No models", wxOK | wxICON_INFORMATION, this);
            m_modelsField->SetFocus();
            return;
        }

        evt.Skip();   // proceed with EndModal(wxID_OK)
    }

    wxTextCtrl*    m_idField       = nullptr;
    wxTextCtrl*    m_nameField     = nullptr;
    wxTextCtrl*    m_baseUrlField  = nullptr;
    wxTextCtrl*    m_chatPathField = nullptr;
    wxRadioButton* m_bearerRadio   = nullptr;
    wxRadioButton* m_xApiKeyRadio  = nullptr;
    wxTextCtrl*    m_providerField = nullptr;
    wxTextCtrl*    m_keyField      = nullptr;
    wxRadioButton* m_nativeRadio   = nullptr;
    wxRadioButton* m_xmlRadio      = nullptr;
    wxTextCtrl*    m_modelsField   = nullptr;
    std::vector<wxStaticText*> m_hints;
};

}  // anonymous namespace

// ─── Event table ────────────────────────────────────────────────

wxBEGIN_EVENT_TABLE(EndpointsDialog, wxDialog)
    EVT_BUTTON(ID_EP_ADD,    EndpointsDialog::OnAdd)
    EVT_BUTTON(ID_EP_EDIT,   EndpointsDialog::OnEdit)
    EVT_BUTTON(ID_EP_DELETE, EndpointsDialog::OnDelete)
    EVT_BUTTON(wxID_CLOSE,   EndpointsDialog::OnClose)
    EVT_LIST_ITEM_ACTIVATED(ID_EP_LIST, EndpointsDialog::OnItemActivated)
    EVT_LIST_ITEM_SELECTED  (ID_EP_LIST, EndpointsDialog::OnSelectionChanged)
    EVT_LIST_ITEM_DESELECTED(ID_EP_LIST, EndpointsDialog::OnSelectionChanged)
wxEND_EVENT_TABLE()

// ─── ctor / layout ──────────────────────────────────────────────

EndpointsDialog::EndpointsDialog(wxWindow* parent,
                                 EndpointStore* store,
                                 const ThemeData& theme)
    : wxDialog(parent, wxID_ANY, "Remote Endpoints",
               wxDefaultPosition, wxSize(620, 440),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_store(store)
    , m_theme(&theme)
{
    wxFont base = GetFont();
    base.SetPointSize(11);
    SetFont(base);

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* help = new wxStaticText(this, wxID_ANY,
        "Remote OpenAI-compatible inference endpoints. Each endpoint's "
        "models appear in the model picker. The API key is read from "
        "Connections by the provider/key name set on the endpoint.");
    help->Wrap(580);
    root->Add(help, 0, wxALL, 12);

    m_list = new wxListCtrl(this, ID_EP_LIST,
                            wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    m_list->InsertColumn(0, "Endpoint", wxLIST_FORMAT_LEFT, 150);
    m_list->InsertColumn(1, "Base URL", wxLIST_FORMAT_LEFT, 230);
    m_list->InsertColumn(2, "Models",   wxLIST_FORMAT_LEFT, 200);
    root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    m_addBtn   = MakeAccentButton     (this, ID_EP_ADD,    "Add...",  theme);
    m_editBtn  = MakeAccentButton     (this, ID_EP_EDIT,   "Edit...", theme);
    m_delBtn   = MakeDestructiveButton(this, ID_EP_DELETE, "Delete",  theme);
    m_closeBtn = MakeFlatButton       (this, wxID_CLOSE,   "Close",   theme);

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

    m_closeBtn->SetDefault();
    SetEscapeId(wxID_CLOSE);
}

void EndpointsDialog::ApplyTheme()
{
    if (!m_theme) return;

    SetBackgroundColour(m_theme->bgDialogSurface);

    m_list->SetBackgroundColour(m_theme->bgInputArea);
    m_list->SetForegroundColour(m_theme->textPrimary);

    ApplyDialogThemeRecursive(this,
                              m_theme->textPrimary,
                              m_theme->bgInputField,
                              m_theme->textPrimary);

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

void EndpointsDialog::RebuildList(const wxString& selectId)
{
    m_list->DeleteAllItems();
    m_rowIds.clear();
    if (!m_store) return;

    long idx = 0;
    for (const auto& ep : m_store->Endpoints()) {
        const wxString name = wxString::FromUTF8(ep.displayName.c_str());
        long item = m_list->InsertItem(idx, name);
        m_list->SetItem(item, 1, wxString::FromUTF8(ep.baseUrl.c_str()));

        std::string modelsCol;
        for (size_t i = 0; i < ep.models.size(); ++i) {
            if (i) modelsCol += ", ";
            modelsCol += ep.models[i].displayName;
        }
        m_list->SetItem(item, 2, wxString::FromUTF8(modelsCol.c_str()));

        m_rowIds.push_back(ep.id);

        if (!selectId.IsEmpty() &&
            wxString::FromUTF8(ep.id.c_str()) == selectId) {
            m_list->SetItemState(item,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_list->EnsureVisible(item);
        }
        ++idx;
    }
}

void EndpointsDialog::UpdateButtonState()
{
    bool hasSelection = (m_list->GetSelectedItemCount() > 0);
    m_editBtn->Enable(hasSelection);
    m_delBtn->Enable(hasSelection);
}

void EndpointsDialog::OnSelectionChanged(wxListEvent&)
{
    UpdateButtonState();
}

void EndpointsDialog::OnItemActivated(wxListEvent&)
{
    wxCommandEvent dummy;
    OnEdit(dummy);
}

// ─── Add / Edit / Delete ────────────────────────────────────────

void EndpointsDialog::OnAdd(wxCommandEvent&)
{
    if (!m_store) return;

    EndpointStore::Endpoint seed;   // sensible defaults from the struct
    EndpointEditDialog dlg(this, "Add endpoint", seed, *m_theme);
    if (dlg.ShowModal() != wxID_OK) return;

    EndpointStore::Endpoint ep = dlg.GetEndpoint();

    if (m_store->FindEndpoint(ep.id)) {
        int ans = wxMessageBox(
            wxString::Format("An endpoint with id '%s' already exists. "
                             "Replace it?",
                             wxString::FromUTF8(ep.id.c_str())),
            "Replace existing endpoint?",
            wxYES_NO | wxICON_WARNING, this);
        if (ans != wxYES) return;
    }

    m_store->UpsertEndpoint(ep);
    RebuildList(wxString::FromUTF8(ep.id.c_str()));
    UpdateButtonState();
}

void EndpointsDialog::OnEdit(wxCommandEvent&)
{
    if (!m_store) return;

    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= (long)m_rowIds.size()) return;

    const std::string originalId = m_rowIds[sel];
    const EndpointStore::Endpoint* existing = m_store->FindEndpoint(originalId);
    if (!existing) {
        RebuildList();
        UpdateButtonState();
        return;
    }

    // Seed from the existing endpoint (this carries extra_headers through
    // even though the dialog doesn't expose them for editing).
    EndpointStore::Endpoint seed = *existing;
    EndpointEditDialog dlg(this, "Edit endpoint", seed, *m_theme);
    if (dlg.ShowModal() != wxID_OK) return;

    EndpointStore::Endpoint ep = dlg.GetEndpoint();
    ep.extraHeaders = existing->extraHeaders;   // preserve non-editable headers

    const bool idChanged = (ep.id != originalId);
    if (idChanged && m_store->FindEndpoint(ep.id)) {
        int ans = wxMessageBox(
            wxString::Format("An endpoint with id '%s' already exists. "
                             "Replace it?",
                             wxString::FromUTF8(ep.id.c_str())),
            "Replace existing endpoint?",
            wxYES_NO | wxICON_WARNING, this);
        if (ans != wxYES) return;
    }

    if (idChanged) m_store->RemoveEndpoint(originalId);
    m_store->UpsertEndpoint(ep);

    RebuildList(wxString::FromUTF8(ep.id.c_str()));
    UpdateButtonState();
}

void EndpointsDialog::OnDelete(wxCommandEvent&)
{
    if (!m_store) return;

    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= (long)m_rowIds.size()) return;

    const std::string id = m_rowIds[sel];
    int ans = wxMessageBox(
        wxString::Format("Delete endpoint '%s'?",
                         wxString::FromUTF8(id.c_str())),
        "Confirm", wxYES_NO | wxICON_WARNING, this);
    if (ans != wxYES) return;

    m_store->RemoveEndpoint(id);
    RebuildList();
    UpdateButtonState();
}

void EndpointsDialog::OnClose(wxCommandEvent&)
{
    EndModal(wxID_CLOSE);
}
