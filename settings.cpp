// settings.cpp
//
// Settings dialog for LlamaBoss.
// Section-organised layout with dark-themed surfaces:
//   • Model (combo, action buttons, status)
//   • Context length (snap-to-tick slider)
//   • Behavior (agent-default checkbox)
//   • Connections (skill API keys)
//   • Appearance (theme dropdown, font size slider)
//   • OK / Cancel footer
//
// The section body is a vertical wxScrolledWindow sized to the content
// and clamped to ~90% of the monitor work area, so adding sections can
// never silently clip the ones at the bottom again (the Appearance
// section was invisible for a while for exactly that reason).
//
// Model selection scans %LOCALAPPDATA%\LlamaBoss\models\ for .gguf files.
// No Ollama API dependency.
//
// Button language: Telegram-flavoured. All committal actions are plain
// wxButtons rendered with t.accentButton fill + t.accentButtonText label
// (the same DNA as the chat Send button). Cancel is a flat borderless
// wxButton with a muted label, sitting as a quiet secondary affordance
// next to the solid OK. The previous BracketButton / OutlinedButton
// affordances were the right read for the chat surface (monospace,
// crisp, terminal-like) but felt out of place for a settings sheet.

#include "settings.h"
#include "connections_dialog.h"
#include "endpoints_dialog.h"
#include "endpoint_store.h"
#include "model_downloader.h"
#include "model_manager.h"
#include "secrets_store.h"
#include "server_manager.h"
#include "theme.h"
#include "widgets.h"   // TickSlider, ApplyDialogThemeRecursive, ApplyDarkTitleBar

#include <wx/fileconf.h>
#include <wx/msgdlg.h>
#include <wx/filename.h>
#include <wx/statline.h>
#include <wx/dirdlg.h>
#include <wx/display.h>

#include <algorithm>
#include <sstream>
#include <iomanip>

// ── Helper: human-readable file size ─────────────────────────────
static std::string FormatFileSize(const std::string& path)
{
    wxFileName fn(path);
    if (!fn.FileExists()) return "";

    wxULongLong size = fn.GetSize();
    if (size == wxInvalidSize) return "";

    double s = size.ToDouble();
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    int idx = 0;
    while (s >= 1024.0 && idx < 4) { s /= 1024.0; idx++; }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 1) << s << " " << units[idx];
    return oss.str();
}

// ── Human-readable ctx size ("8k tokens") ─────────────────────────
static std::string FormatCtxSize(int tokens)
{
    std::ostringstream oss;
    if (tokens >= 1024 && (tokens % 1024 == 0)) {
        oss << (tokens / 1024) << "k tokens";
    } else {
        oss << tokens << " tokens";
    }
    return oss.str();
}

wxBEGIN_EVENT_TABLE(SettingsDialog, wxDialog)
    EVT_BUTTON(wxID_OK,     SettingsDialog::OnOK)
    EVT_BUTTON(wxID_CANCEL, SettingsDialog::OnCancel)
wxEND_EVENT_TABLE()

SettingsDialog::SettingsDialog(wxWindow* parent,
                               const std::string& currentModelPath,
                               const std::string& loadedModelPath,
                               const std::string& currentTheme,
                               int currentCtxSize,
                               int currentFontSize,
                               bool currentAgentDefaultOn,
                               bool currentContextMeterOn,
                               bool currentKvCacheQ8,
                               bool currentMtpEnabled,
                               const ThemeData& theme,
                               SecretsStore* secretsStore,
                               EndpointStore* endpointStore)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxDefaultSize)
      // Height is computed at the end of CreateControls() from the actual
      // content, clamped to the monitor's work area. The old hard-coded
      // wxSize(600, 840) silently clipped the Appearance section once the
      // Behavior + Remote Endpoints sections grew the body past 840px.
    , m_selectedModel(currentModelPath)
    , m_loadedModelPath(loadedModelPath)
    , m_selectedTheme(currentTheme)
    , m_selectedCtxSize(currentCtxSize)
    , m_selectedFontSize(currentFontSize)
    , m_selectedAgentDefault(currentAgentDefaultOn)
    , m_selectedContextMeter(currentContextMeterOn)
    , m_selectedKvCacheQ8(currentKvCacheQ8)
    , m_selectedMtpEnabled(currentMtpEnabled)
    , m_originalModel(currentModelPath)
    , m_originalTheme(currentTheme)
    , m_originalCtxSize(currentCtxSize)
    , m_originalFontSize(currentFontSize)
    , m_originalAgentDefault(currentAgentDefaultOn)
    , m_originalContextMeter(currentContextMeterOn)
    , m_originalKvCacheQ8(currentKvCacheQ8)
    , m_originalMtpEnabled(currentMtpEnabled)
    , m_originalFolderOverride(ServerManager::GetModelsDirOverride())
    , m_theme(&theme)
{
    m_secretsStore = secretsStore;
    m_endpointStore = endpointStore;

    wxFont f = GetFont();
    f.SetPointSize(11);
    f.SetWeight(wxFONTWEIGHT_NORMAL);
    SetFont(f);

    CreateControls();
    PopulateModelList();
    UpdateFolderUi();
    UpdateConnectionsLabel();
    UpdateEndpointsLabel();
    ApplyTheme();

    // Match the title bar to the body when dark theme is active.
    ApplyDarkTitleBar(this, theme.name != "light");

    Centre();
}

// ═══════════════════════════════════════════════════════════════════
//  Destructor — revert folder override if dialog wasn't confirmed
// ═══════════════════════════════════════════════════════════════════
//
// Change/Reset persist to wxFileConfig immediately so PopulateModelList()
// can see the new location. If the user closes the dialog without clicking
// OK (Cancel, ESC, X, anything), we roll that back here. One safety net
// covers every exit path.
SettingsDialog::~SettingsDialog()
{
    if (!m_confirmedOk) {
        const std::string current = ServerManager::GetModelsDirOverride();
        if (current != m_originalFolderOverride) {
            ServerManager::SetModelsDirOverride(m_originalFolderOverride);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Construction helpers
// ═══════════════════════════════════════════════════════════════════

wxPanel* SettingsDialog::MakeSectionDivider(wxWindow* parent)
{
    // Telegram-style gap band: 10px tall, painted with t.bgMain in
    // ApplyTheme() so it reads darker than the dialog surface — feels
    // like a slot between sections rather than a thin border. Tracked
    // in m_dividers so we paint it deterministically instead of doing
    // the old "find every 1px panel" recursive walk.
    auto* line = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 10));
    m_dividers.push_back(line);
    return line;
}

wxStaticText* SettingsDialog::MakeSectionHeader(wxWindow* parent, const wxString& text)
{
    auto* lbl = new wxStaticText(parent, wxID_ANY, text);
    wxFont hf = lbl->GetFont();
    hf.SetPointSize(12);
    hf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    lbl->SetFont(hf);
    return lbl;
}

// ── Helper: make an accent (Send-style) push button ──────────────
//
// Mirrors the chat Send button: plain wxButton with t.accentButton fill
// and t.accentButtonText label. Native Win11 chrome handles the rounded
// corners. We register every accent button so ApplyTheme() can re-tint
// them after ApplyDialogThemeRecursive() runs the neutral cascade.
//
// `height = 32` is the standard footer/action-row size. Use 26 for the
// smaller inline buttons that sit next to text (Change, Reset, Manage).
wxButton* SettingsDialog::MakeAccentButton(wxWindow* parent,
                                           wxWindowID id,
                                           const wxString& label,
                                           int height)
{
    // wxBORDER_NONE is the magic ingredient: without it, the Win11
    // native button chrome paints a tinted outline over our solid fill
    // (the result reads as a light-cyan ring around the button instead
    // of a clean solid). Same trick the sidebar's New Chat and the chat
    // Send button use.
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition,
                             wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    m_accentBtns.push_back(btn);
    return btn;
}

// ═══════════════════════════════════════════════════════════════════
//  Layout
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::CreateControls()
{
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    // Content lives inside a padded body panel so we can give the dialog
    // consistent 18px margins without every child having to specify them.
    //
    // The body is a wxScrolledWindow (vertical only) rather than a plain
    // wxPanel: on short displays — or whenever a future section makes the
    // content taller than the screen — the body scrolls instead of
    // silently clipping the sections at the bottom. On displays tall
    // enough to fit everything, the scrollbar never appears and the
    // dialog behaves exactly as before.
    auto* body = new wxScrolledWindow(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    body->SetScrollRate(0, 12);
    auto* bodySizer = new wxBoxSizer(wxVERTICAL);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 1 — MODEL
    // ─────────────────────────────────────────────────────────────
    bodySizer->Add(MakeSectionHeader(body, "Model"), 0, wxBOTTOM, 6);

    m_modelComboBox = new wxComboBox(body, wxID_ANY, "",
        wxDefaultPosition, wxSize(-1, 28), 0, nullptr,
        wxCB_DROPDOWN | wxCB_READONLY);
    bodySizer->Add(m_modelComboBox, 0, wxEXPAND | wxBOTTOM, 8);

    // Action buttons row — solid accent push buttons, Send-style.
    // The three actions read as peers (open three modal flows), so they
    // share size and weight; horizontal gap keeps them readable as a
    // group rather than a single chunky bar.
    auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* downloadBtn = MakeAccentButton(body, wxID_ANY, "Download models");
    auto* manageBtn   = MakeAccentButton(body, wxID_ANY, "Manage models");
    auto* openBtn     = MakeAccentButton(body, wxID_ANY, "Open folder");
    downloadBtn->Bind(wxEVT_BUTTON, &SettingsDialog::OnDownloadModels, this);
    manageBtn  ->Bind(wxEVT_BUTTON, &SettingsDialog::OnManageModels,   this);
    openBtn    ->Bind(wxEVT_BUTTON, &SettingsDialog::OnOpenModelsFolder, this);
    actionSizer->Add(downloadBtn, 0, wxRIGHT, 8);
    actionSizer->Add(manageBtn,   0, wxRIGHT, 8);
    actionSizer->Add(openBtn,     0);
    bodySizer->Add(actionSizer, 0, wxBOTTOM, 10);

    // ── Location row ─────────────────────────────────────────────
    // Shows the active models folder with Change/Reset affordances.
    // Reset is hidden when no override is active (casual mode). The
    // muted "Default: ..." line below is likewise shown only when
    // the user has pointed at a custom folder.
    //
    // Inline buttons get the 26px short variant so they don't tower
    // over the text label they sit beside.
    auto* locRow = new wxBoxSizer(wxHORIZONTAL);

    auto* locLabel = new wxStaticText(body, wxID_ANY, "Location:");
    { wxFont lf = locLabel->GetFont(); lf.SetPointSize(10); locLabel->SetFont(lf); }
    locRow->Add(locLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    m_locationPath = new wxStaticText(body, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    { wxFont pf = m_locationPath->GetFont(); pf.SetPointSize(10); m_locationPath->SetFont(pf); }
    // Critical for long Windows paths: wxST_ELLIPSIZE_MIDDLE only
    // affects painting, not the label's best/min size. If we let the
    // full path report its natural width, the scrolled-window virtual
    // width grows and right-edge controls (Change/Manage) get clipped
    // instead of the path shrinking. Give the path a small floor and
    // let the row allocate whatever remaining width is available.
    m_locationPath->SetMinSize(wxSize(120, -1));
    locRow->Add(m_locationPath, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    m_changeBtn = MakeAccentButton(body, wxID_ANY, "Change", 26);
    m_resetBtn  = MakeAccentButton(body, wxID_ANY, "Reset",  26);
    m_changeBtn->Bind(wxEVT_BUTTON, &SettingsDialog::OnChangeFolder, this);
    m_resetBtn ->Bind(wxEVT_BUTTON, &SettingsDialog::OnResetFolder,  this);
    locRow->Add(m_changeBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    locRow->Add(m_resetBtn,  0, wxALIGN_CENTER_VERTICAL);

    bodySizer->Add(locRow, 0, wxEXPAND | wxBOTTOM, 4);

    // Muted "Default: <path>" row — hidden when no override is active.
    m_defaultPathRow = new wxStaticText(body, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    { wxFont df = m_defaultPathRow->GetFont(); df.SetPointSize(10); m_defaultPathRow->SetFont(df); }
    // Same shrinkability issue as the active Location path above.
    // This row is usually hidden, but custom model-folder users may
    // show a long default path; never let it widen the scroll body.
    m_defaultPathRow->SetMinSize(wxSize(120, -1));
    bodySizer->Add(m_defaultPathRow, 0, wxEXPAND | wxBOTTOM, 6);

    // Status line (muted)
    m_statusText = new wxStaticText(body, wxID_ANY, "");
    wxFont sf = m_statusText->GetFont();
    sf.SetPointSize(10);
    m_statusText->SetFont(sf);
    bodySizer->Add(m_statusText, 0, wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 2 — CONTEXT LENGTH
    // ─────────────────────────────────────────────────────────────
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);

    auto* ctxHeaderRow = new wxBoxSizer(wxHORIZONTAL);
    ctxHeaderRow->Add(MakeSectionHeader(body, "Context length"), 0);
    ctxHeaderRow->AddStretchSpacer();
    auto* ctxValueLabel = new wxStaticText(body, wxID_ANY,
        wxString::FromUTF8(FormatCtxSize(m_selectedCtxSize)));
    wxFont cvf = ctxValueLabel->GetFont();
    cvf.SetPointSize(10);
    cvf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    ctxValueLabel->SetFont(cvf);
    ctxHeaderRow->Add(ctxValueLabel, 0, wxALIGN_CENTER_VERTICAL);
    bodySizer->Add(ctxHeaderRow, 0, wxEXPAND | wxBOTTOM, 4);

    auto* ctxHint = new wxStaticText(body, wxID_ANY,
        "How much of the conversation the model can keep in memory.");
    wxFont chf = ctxHint->GetFont();
    chf.SetPointSize(10);
    ctxHint->SetFont(chf);
    bodySizer->Add(ctxHint, 0, wxBOTTOM, 8);

    m_ctxSlider = new TickSlider(
        body,
        { 4096, 8192, 16384, 32768, 65536, 131072, 262144 },
        { "4k", "8k", "16k", "32k", "64k", "128k", "256k" },
        m_selectedCtxSize,
        [this, ctxValueLabel](int newVal) {
            m_selectedCtxSize = newVal;
            ctxValueLabel->SetLabel(wxString::FromUTF8(FormatCtxSize(newVal)));
        });
    bodySizer->Add(m_ctxSlider, 0, wxEXPAND | wxBOTTOM, 14);

    // 8-bit KV cache lives in the Context section, not Behavior:
    // it is a context-capacity control (same -c launch-argument
    // family as the slider above) and reads naturally next to it.
    m_kvCacheQ8CheckBox = new wxCheckBox(
        body, wxID_ANY, "8-bit KV cache (recommended)");
    { wxFont kvf = m_kvCacheQ8CheckBox->GetFont();
      kvf.SetPointSize(11);
      m_kvCacheQ8CheckBox->SetFont(kvf); }
    m_kvCacheQ8CheckBox->SetValue(m_selectedKvCacheQ8);
    bodySizer->Add(m_kvCacheQ8CheckBox, 0, wxBOTTOM, 4);

    auto* kvHint = new wxStaticText(body, wxID_ANY,
        "Halves context memory use, so about twice the context fits in "
        "VRAM. Changing this reloads the model.");
    { wxFont kh = kvHint->GetFont(); kh.SetPointSize(10); kvHint->SetFont(kh); }
    bodySizer->Add(kvHint, 0, wxBOTTOM, 14);

    // Multi-token prediction lives in the Context section with the
    // other launch-argument controls (same restart semantics as the
    // ctx slider and KV cache toggle above).
    m_mtpCheckBox = new wxCheckBox(
        body, wxID_ANY, "Multi-token prediction (auto)");
    { wxFont mf = m_mtpCheckBox->GetFont();
      mf.SetPointSize(11);
      m_mtpCheckBox->SetFont(mf); }
    m_mtpCheckBox->SetValue(m_selectedMtpEnabled);
    bodySizer->Add(m_mtpCheckBox, 0, wxBOTTOM, 4);

    auto* mtpHint = new wxStaticText(body, wxID_ANY,
        "Speeds up generation on models with built-in MTP heads "
        "(GLM 4.5/4.6, Qwen MTP builds). Detected automatically; has "
        "no effect on other models. Changing this reloads the model.");
    { wxFont mh = mtpHint->GetFont(); mh.SetPointSize(10); mtpHint->SetFont(mh); }
    // Keep this long hint from widening the vertical-only scrolled body.
    // Without wrapping, FitInside() uses the text's full best width and
    // pushes right-aligned Manage buttons outside the visible viewport.
    mtpHint->Wrap(520);
    bodySizer->Add(mtpHint, 0, wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 3 — BEHAVIOR
    // ─────────────────────────────────────────────────────────────
    //  Controls how chats behave. Currently only hosts the agent-mode
    //  default; this section is the natural home for future toggles
    //  like auto-titling, streaming, or tool-call confirmation.
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);
    bodySizer->Add(MakeSectionHeader(body, "Behavior"), 0, wxBOTTOM, 8);

    m_agentDefaultCheckBox = new wxCheckBox(
        body, wxID_ANY, "Start new chats with agent mode enabled");
    { wxFont acf = m_agentDefaultCheckBox->GetFont();
      acf.SetPointSize(11);
      m_agentDefaultCheckBox->SetFont(acf); }
    m_agentDefaultCheckBox->SetValue(m_selectedAgentDefault);
    bodySizer->Add(m_agentDefaultCheckBox, 0, wxBOTTOM, 4);

    auto* agentHint = new wxStaticText(body, wxID_ANY,
        "The robot button toggles agent mode for the current chat.");
    { wxFont ah = agentHint->GetFont(); ah.SetPointSize(10); agentHint->SetFont(ah); }
    bodySizer->Add(agentHint, 0, wxBOTTOM, 14);

    m_contextMeterCheckBox = new wxCheckBox(
        body, wxID_ANY, "Show context meter in the top bar");
    { wxFont cmf = m_contextMeterCheckBox->GetFont();
      cmf.SetPointSize(11);
      m_contextMeterCheckBox->SetFont(cmf); }
    m_contextMeterCheckBox->SetValue(m_selectedContextMeter);
    bodySizer->Add(m_contextMeterCheckBox, 0, wxBOTTOM, 4);

    auto* meterHint = new wxStaticText(body, wxID_ANY,
        "Shows how full the model's context window is (tokens used / total).");
    { wxFont mh = meterHint->GetFont(); mh.SetPointSize(10); meterHint->SetFont(mh); }
    bodySizer->Add(meterHint, 0, wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 3b — CONNECTIONS
    // ─────────────────────────────────────────────────────────────
    //  API keys for service skills.  Each entry maps to one
    //  environment variable exposed to skill scripts running through
    //  python_run_script.  Stored as plaintext JSON at
    //  %LOCALAPPDATA%\LlamaBoss\secrets.json (user-only ACL).
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);
    bodySizer->Add(MakeSectionHeader(body, "Connections"), 0, wxBOTTOM, 8);

    auto* connRow = new wxBoxSizer(wxHORIZONTAL);
    m_connectionsLabel = new wxStaticText(body, wxID_ANY,
        "No connections configured");
    { wxFont cf = m_connectionsLabel->GetFont(); cf.SetPointSize(11);
      m_connectionsLabel->SetFont(cf); }
    // Keep the right-side Manage button visible even if this text grows
    // in a future build (or localization makes it longer).
    m_connectionsLabel->SetMinSize(wxSize(120, -1));
    connRow->Add(m_connectionsLabel, 1, wxALIGN_CENTER_VERTICAL);

    m_manageConnBtn = MakeAccentButton(body, wxID_ANY, "Manage", 26);
    m_manageConnBtn->Bind(wxEVT_BUTTON,
                          &SettingsDialog::OnManageConnections, this);
    connRow->Add(m_manageConnBtn, 0, wxLEFT, 10);
    bodySizer->Add(connRow, 0, wxEXPAND | wxBOTTOM, 6);

    auto* connHint = new wxStaticText(body, wxID_ANY,
        "Skill scripts read these via os.environ (e.g. GMAIL_API_KEY).");
    { wxFont ch = connHint->GetFont(); ch.SetPointSize(10); connHint->SetFont(ch); }
    bodySizer->Add(connHint, 0, wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 3b — REMOTE ENDPOINTS
    // ─────────────────────────────────────────────────────────────
    //  Remote OpenAI-compatible inference endpoints (OpenRouter, OpenAI,
    //  etc.). Each endpoint's models appear in the model picker; the API
    //  key is resolved from a Connections entry by provider/key name.
    //  Stored as JSON at %LOCALAPPDATA%\LlamaBoss\endpoints.json.
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);
    bodySizer->Add(MakeSectionHeader(body, "Remote Endpoints"), 0, wxBOTTOM, 8);

    auto* epRow = new wxBoxSizer(wxHORIZONTAL);
    m_endpointsLabel = new wxStaticText(body, wxID_ANY,
        "No remote endpoints configured");
    { wxFont ef = m_endpointsLabel->GetFont(); ef.SetPointSize(11);
      m_endpointsLabel->SetFont(ef); }
    // Same shrinkability guard as Connections.
    m_endpointsLabel->SetMinSize(wxSize(120, -1));
    epRow->Add(m_endpointsLabel, 1, wxALIGN_CENTER_VERTICAL);

    m_manageEndpointsBtn = MakeAccentButton(body, wxID_ANY, "Manage", 26);
    m_manageEndpointsBtn->Bind(wxEVT_BUTTON,
                               &SettingsDialog::OnManageEndpoints, this);
    epRow->Add(m_manageEndpointsBtn, 0, wxLEFT, 10);
    bodySizer->Add(epRow, 0, wxEXPAND | wxBOTTOM, 6);

    auto* epHint = new wxStaticText(body, wxID_ANY,
        "Each endpoint's models show in the model picker. Set the API key "
        "under Connections, using the endpoint's provider/key name.");
    { wxFont eh = epHint->GetFont(); eh.SetPointSize(10); epHint->SetFont(eh); }
    epHint->Wrap(520);   // 540 → 520: clearance for the body scrollbar when it shows
    bodySizer->Add(epHint, 0, wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 4 — APPEARANCE
    // ─────────────────────────────────────────────────────────────
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);
    bodySizer->Add(MakeSectionHeader(body, "Appearance"), 0, wxBOTTOM, 8);

    // Theme row
    auto* themeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* themeLabel = new wxStaticText(body, wxID_ANY, "Theme");
    themeLabel->SetMinSize(wxSize(90, -1));
    themeRow->Add(themeLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    wxArrayString themeChoices;
    for (const auto& c : ThemeManager::GetThemeChoices()) {
        themeChoices.Add(wxString::FromUTF8(c.displayName));
    }
    m_themeComboBox = new wxComboBox(body, wxID_ANY, "",
        wxDefaultPosition, wxSize(-1, 28), themeChoices,
        wxCB_DROPDOWN | wxCB_READONLY);

    // Find the index matching the user's stored theme. If the stored
    // value doesn't appear in the list (e.g. a theme was removed in a
    // future build), fall back to index 0 (Dark) so the dropdown
    // always has a valid selection.
    int initialIdx = 0;
    {
        const auto& choices = ThemeManager::GetThemeChoices();
        for (size_t i = 0; i < choices.size(); ++i) {
            if (m_selectedTheme == choices[i].internalName) {
                initialIdx = static_cast<int>(i);
                break;
            }
        }
    }
    m_themeComboBox->SetSelection(initialIdx);
    themeRow->Add(m_themeComboBox, 1);
    bodySizer->Add(themeRow, 0, wxEXPAND | wxBOTTOM, 14);

    // Font size row (header + live value)
    auto* fontHeaderRow = new wxBoxSizer(wxHORIZONTAL);
    auto* fontLabel = new wxStaticText(body, wxID_ANY, "Chat font size");
    { wxFont ff = fontLabel->GetFont(); ff.SetWeight(wxFONTWEIGHT_SEMIBOLD); fontLabel->SetFont(ff); }
    fontHeaderRow->Add(fontLabel, 0, wxALIGN_CENTER_VERTICAL);
    fontHeaderRow->AddStretchSpacer();
    auto* fontValueLabel = new wxStaticText(body, wxID_ANY,
        wxString::Format("%dpt", m_selectedFontSize));
    wxFont fvf = fontValueLabel->GetFont();
    fvf.SetPointSize(10);
    fvf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    fontValueLabel->SetFont(fvf);
    fontHeaderRow->Add(fontValueLabel, 0, wxALIGN_CENTER_VERTICAL);
    bodySizer->Add(fontHeaderRow, 0, wxEXPAND | wxBOTTOM, 8);

    m_fontSlider = new TickSlider(
        body,
        { 10, 12, 14, 16, 18, 20, 22, 24 },
        { "10", "12", "14", "16", "18", "20", "22", "24" },
        m_selectedFontSize,
        [this, fontValueLabel](int newVal) {
            m_selectedFontSize = newVal;
            fontValueLabel->SetLabel(wxString::Format("%dpt", newVal));
        });
    bodySizer->Add(m_fontSlider, 0, wxEXPAND | wxBOTTOM, 14);

    body->SetSizer(bodySizer);
    body->FitInside();   // set the scrolled window's virtual size from content
    rootSizer->Add(body, 1, wxEXPAND | wxALL, 18);

    // ─────────────────────────────────────────────────────────────
    //  FOOTER — flat Cancel + solid OK
    // ─────────────────────────────────────────────────────────────
    //  Asymmetric on purpose — same idea as Telegram's modal footers:
    //  the commit action carries the accent fill, the dismissal sits
    //  as a quiet borderless label nearby. Both are real wxButtons so
    //  the wxDialog default-button machinery handles Enter (→ OK) and
    //  Escape (→ Cancel) for free, no manual char hook needed.
    auto* footer = new wxPanel(this, wxID_ANY);
    auto* footSizer = new wxBoxSizer(wxHORIZONTAL);
    footSizer->AddStretchSpacer();

    m_cancelBtn = new wxButton(footer, wxID_CANCEL, "Cancel",
                               wxDefaultPosition, wxSize(-1, 32),
                               wxBORDER_NONE);
    { wxFont cbf = m_cancelBtn->GetFont();
      cbf.SetPointSize(10);
      cbf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
      m_cancelBtn->SetFont(cbf); }

    m_okBtn = MakeAccentButton(footer, wxID_OK, "OK", 32);
    m_okBtn->SetMinSize(wxSize(96, 32));
    m_okBtn->SetDefault();

    footSizer->Add(m_cancelBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    footSizer->Add(m_okBtn,     0, wxALIGN_CENTER_VERTICAL);
    footer->SetSizer(footSizer);
    rootSizer->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 18);

    SetSizer(rootSizer);

    // ─────────────────────────────────────────────────────────────
    //  SIZING — fit the content, clamp to the screen
    // ─────────────────────────────────────────────────────────────
    //  Ideal client height = full body content + margins + footer.
    //  If that fits within ~90% of the monitor's work area, the dialog
    //  shows every section with no scrollbar — same look as before.
    //  If not (short display, or a future section pushes the content
    //  taller), the dialog caps at the work area and the body scrolls.
    //  Nothing gets clipped either way.
    const int contentH = bodySizer->GetMinSize().GetHeight();
    const int footerH  = footSizer->GetMinSize().GetHeight();
    const int wantClientH = 18 + contentH + 18   // body + top/bottom margins
                          + footerH + 18;        // footer + bottom margin
    const int wantWinH =
        ClientToWindowSize(wxSize(0, wantClientH)).GetHeight();

    int maxWinH = 840;   // conservative floor if display lookup fails
    int winW    = 720;   // wider default: room for new KV/endpoint rows
    const int dispIdx = wxDisplay::GetFromWindow(
        GetParent() ? GetParent() : static_cast<wxWindow*>(this));
    if (dispIdx != wxNOT_FOUND) {
        const wxRect workArea = wxDisplay(static_cast<unsigned>(dispIdx))
                                    .GetClientArea();
        maxWinH = workArea.GetHeight() * 9 / 10;

        // Old 600px width was barely enough before the KV-cache and
        // remote-endpoint sections; with a vertical scrollbar visible,
        // right-edge buttons could look cut off. Prefer 720px, but keep
        // the dialog inside small monitors.
        winW = std::min(720, std::max(600, workArea.GetWidth() * 9 / 10));
    }

    SetSize(wxSize(winW, std::min(wantWinH, maxWinH)));
    SetMinSize(wxSize(600, std::min(wantWinH, 480)));
}

// ═══════════════════════════════════════════════════════════════════
//  Theming
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::ApplyTheme()
{
    if (!m_theme) return;
    const ThemeData& t = *m_theme;

    SetBackgroundColour(t.bgDialogSurface);

    for (auto* child : GetChildren()) {
        if (auto* p = dynamic_cast<wxPanel*>(child))
            p->SetBackgroundColour(t.bgDialogSurface);
    }

    // Apply label colors + neutral button fill to every widget recursively.
    // The recursive helper tints every wxButton with bgInputField — we
    // re-tint the accent buttons and Cancel below to override that.
    ApplyDialogThemeRecursive(this, t.textPrimary, t.bgInputField, t.textPrimary);

    // Section gap bands — paint every registered divider with
    // borderSubtle so they read lighter than the dialog surface
    // (#2B3845 vs #17212B), landing a raised-separator look rather
    // than a dark slot. The old recursive "find every 1px panel"
    // walker is gone; dividers are now 10px tall and tracked
    // explicitly by MakeSectionDivider().
    for (auto* div : m_dividers) {
        if (div) div->SetBackgroundColour(t.borderSubtle);
    }

    // Combo boxes — Windows still renders OS chrome on the dropdown arrow,
    // but at least the edit field matches the dark surfaces.
    if (m_modelComboBox) {
        m_modelComboBox->SetBackgroundColour(t.bgInputField);
        m_modelComboBox->SetForegroundColour(t.textPrimary);
    }
    if (m_themeComboBox) {
        m_themeComboBox->SetBackgroundColour(t.bgInputField);
        m_themeComboBox->SetForegroundColour(t.textPrimary);
    }

    // Status line gets the muted text color
    if (m_statusText)
        m_statusText->SetForegroundColour(t.textMuted);

    // "Default: <path>" hint is muted metadata, not primary content.
    if (m_defaultPathRow)
        m_defaultPathRow->SetForegroundColour(t.textMuted);

    // wxCheckBox isn't touched by ApplyDialogThemeRecursive (it handles
    // wxStaticText and wxButton only), so tint the label + surface here.
    if (m_kvCacheQ8CheckBox) {
        m_kvCacheQ8CheckBox->SetForegroundColour(t.textPrimary);
        m_kvCacheQ8CheckBox->SetBackgroundColour(t.bgDialogSurface);
    }
    if (m_mtpCheckBox) {
        m_mtpCheckBox->SetForegroundColour(t.textPrimary);
        m_mtpCheckBox->SetBackgroundColour(t.bgDialogSurface);
    }
    if (m_agentDefaultCheckBox) {
        m_agentDefaultCheckBox->SetForegroundColour(t.textPrimary);
        m_agentDefaultCheckBox->SetBackgroundColour(t.bgDialogSurface);
    }
    if (m_contextMeterCheckBox) {
        m_contextMeterCheckBox->SetForegroundColour(t.textPrimary);
        m_contextMeterCheckBox->SetBackgroundColour(t.bgDialogSurface);
    }

    // Hand the sliders our theme palette
    if (m_ctxSlider) {
        m_ctxSlider->SetColors(
            t.borderSubtle,     // track
            t.accentButton,     // fill
            t.accentButton,     // knob
            t.textPrimary,      // active label
            t.textMuted);       // dimmed labels
    }
    if (m_fontSlider) {
        m_fontSlider->SetColors(
            t.borderSubtle,
            t.accentButton,
            t.accentButton,
            t.textPrimary,
            t.textMuted);
    }

    // Accent buttons — Send-style solid fill. Mirrors what
    // _sendButton gets in MyFrame::ApplyTheme(): accentButton bg,
    // accentButtonText fg. Native Win11 chrome supplies the rounded
    // corners; we just paint the surface.
    for (auto* btn : m_accentBtns) {
        if (!btn) continue;
        btn->SetBackgroundColour(t.accentButton);
        btn->SetForegroundColour(t.accentButtonText);
        btn->Refresh();
    }

    // Cancel: flat, borderless, muted text. The dialog surface bg lets
    // it disappear into the footer until the user reaches for it.
    if (m_cancelBtn) {
        m_cancelBtn->SetBackgroundColour(t.bgDialogSurface);
        m_cancelBtn->SetForegroundColour(t.textMuted);
        m_cancelBtn->Refresh();
    }

    Refresh();
}

// ═══════════════════════════════════════════════════════════════════
//  Populating / reading selection
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::PopulateModelList()
{
    m_modelComboBox->Clear();
    m_modelPaths.clear();

    auto models = ServerManager::ScanModelPaths();

    if (models.empty()) {
        m_statusText->SetLabel("No .gguf files found in: " +
            ServerManager::GetModelsDir());
        return;
    }

    int selectedIdx = -1;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& path = models[i];
        std::string displayName = ServerManager::ModelDisplayName(path);
        std::string sizeStr = FormatFileSize(path);
        if (!sizeStr.empty())
            displayName += "  (" + sizeStr + ")";

        m_modelComboBox->Append(wxString::FromUTF8(displayName));
        m_modelPaths.push_back(path);

        if (path == m_selectedModel)
            selectedIdx = (int)i;
    }

    if (selectedIdx >= 0)
        m_modelComboBox->SetSelection(selectedIdx);
    else if (m_modelComboBox->GetCount() > 0)
        m_modelComboBox->SetSelection(0);

    m_statusText->SetLabel(wxString::Format("Found %zu model(s)", models.size()));
}

// ═══════════════════════════════════════════════════════════════════
//  Event handlers
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::OnOK(wxCommandEvent&)
{
    int sel = m_modelComboBox->GetSelection();
    if (sel >= 0 && sel < (int)m_modelPaths.size()) {
        m_selectedModel = m_modelPaths[sel];
    }

    int themeSel = m_themeComboBox->GetSelection();
    {
        const auto& choices = ThemeManager::GetThemeChoices();
        if (themeSel >= 0 && static_cast<size_t>(themeSel) < choices.size()) {
            m_selectedTheme = choices[static_cast<size_t>(themeSel)].internalName;
        }
        // else: leave m_selectedTheme at its current value — no valid
        // selection means we don't overwrite the user's prior choice.
    }

    // Checkbox is authoritative — the live value may have been toggled
    // since construction without m_selectedAgentDefault being updated.
    if (m_agentDefaultCheckBox)
        m_selectedAgentDefault = m_agentDefaultCheckBox->GetValue();
    if (m_contextMeterCheckBox)
        m_selectedContextMeter = m_contextMeterCheckBox->GetValue();
    if (m_kvCacheQ8CheckBox)
        m_selectedKvCacheQ8 = m_kvCacheQ8CheckBox->GetValue();
    if (m_mtpCheckBox)
        m_selectedMtpEnabled = m_mtpCheckBox->GetValue();

    // Sliders keep m_selectedCtxSize / m_selectedFontSize live via their
    // onChange callbacks — nothing to do here beyond diff'ing.
    m_modelChanged        = (m_selectedModel         != m_originalModel);
    m_themeChanged        = (m_selectedTheme         != m_originalTheme);
    m_ctxSizeChanged      = (m_selectedCtxSize       != m_originalCtxSize);
    m_fontSizeChanged     = (m_selectedFontSize      != m_originalFontSize);
    m_agentDefaultChanged = (m_selectedAgentDefault  != m_originalAgentDefault);
    m_contextMeterChanged = (m_selectedContextMeter  != m_originalContextMeter);
    m_kvCacheQ8Changed    = (m_selectedKvCacheQ8     != m_originalKvCacheQ8);
    m_mtpEnabledChanged   = (m_selectedMtpEnabled    != m_originalMtpEnabled);

    // Folder override was committed live by Change/Reset; compare current
    // value to what we captured at construction.
    m_modelsFolderChanged =
        (ServerManager::GetModelsDirOverride() != m_originalFolderOverride);

    // Tell the destructor the override stands as-is — don't revert.
    m_confirmedOk = true;

    EndModal(wxID_OK);
}

void SettingsDialog::OnCancel(wxCommandEvent&)
{
    EndModal(wxID_CANCEL);
}

void SettingsDialog::OnDownloadModels(wxCommandEvent&)
{
    ModelDownloaderDialog dlg(this, m_theme);
    dlg.ShowModal();

    if (dlg.HadSuccessfulDownload())
        PopulateModelList();
}

void SettingsDialog::OnManageModels(wxCommandEvent&)
{
    ModelManagerDialog dlg(this, m_theme, m_loadedModelPath, m_originalModel);
    dlg.ShowModal();
    PopulateModelList();
}

void SettingsDialog::OnOpenModelsFolder(wxCommandEvent&)
{
    std::string modelsDir = ServerManager::GetModelsDir();
    ServerManager::EnsureDataDirs();

#ifdef __WXMSW__
    wxExecute("explorer \"" + wxString::FromUTF8(modelsDir) + "\"", wxEXEC_ASYNC);
#endif
}

// ═══════════════════════════════════════════════════════════════════
//  Folder-override UI
// ═══════════════════════════════════════════════════════════════════
//
// Reflects the current override state into the Location row. Called
// from the constructor and after every Change/Reset. Hides the Reset
// button and the "Default:" hint when we're in casual mode.
void SettingsDialog::UpdateFolderUi()
{
    if (!m_locationPath) return;

    const std::string active = ServerManager::GetModelsDir();
    const std::string def    = ServerManager::GetDefaultModelsDir();
    const bool        casual = ServerManager::IsCasualMode();

    m_locationPath->SetLabel(wxString::FromUTF8(active));

    if (m_resetBtn)
        m_resetBtn->Show(!casual);

    if (m_defaultPathRow) {
        if (casual) {
            m_defaultPathRow->SetLabel("");
            m_defaultPathRow->Show(false);
        } else {
            m_defaultPathRow->SetLabel(
                wxString::FromUTF8("Default:  " + def));
            m_defaultPathRow->Show(true);
        }
    }

    // Re-lay the dialog so the collapsed/shown rows reclaim space.
    Layout();
}

void SettingsDialog::OnChangeFolder(wxCommandEvent&)
{
    wxDirDialog dlg(this,
        "Choose models folder",
        wxString::FromUTF8(ServerManager::GetModelsDir()),
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);

    if (dlg.ShowModal() != wxID_OK) return;

    const std::string picked(dlg.GetPath().ToUTF8().data());

    // Normalize: if the user picked the exact default path, drop the
    // override entirely — they're effectively back in casual mode and
    // should get bundle semantics + deterministic mmproj pairing.
    wxFileName pickedFn(wxString::FromUTF8(picked));
    wxFileName defaultFn(wxString::FromUTF8(ServerManager::GetDefaultModelsDir()));
    if (pickedFn.SameAs(defaultFn))
        ServerManager::SetModelsDirOverride("");
    else
        ServerManager::SetModelsDirOverride(picked);

    UpdateFolderUi();
    PopulateModelList();
}

void SettingsDialog::OnResetFolder(wxCommandEvent&)
{
    ServerManager::SetModelsDirOverride("");
    UpdateFolderUi();
    PopulateModelList();
}

// ─── Connections ────────────────────────────────────────────────

void SettingsDialog::OnManageConnections(wxCommandEvent&)
{
    if (!m_secretsStore) {
        wxMessageBox(
            "Secrets store is not available. Restart LlamaBoss "
            "and try again.",
            "Connections", wxOK | wxICON_INFORMATION, this);
        return;
    }

    ConnectionsDialog dlg(this, m_secretsStore, *m_theme);
    dlg.ShowModal();

    // Save immediately on dialog close.  Connections are user-edited
    // state that should persist even if the user later hits Cancel on
    // Settings — keys are not bundled with the rest of the dialog
    // settings.
    const bool saved = m_secretsStore->Save();
    if (!saved) {
        wxMessageBox(
            "Connections were updated for this session, but LlamaBoss "
            "could not save them to disk. They may be lost after restart.",
            "Connections Not Saved", wxOK | wxICON_WARNING, this);
    }

    UpdateConnectionsLabel();
}

void SettingsDialog::UpdateConnectionsLabel()
{
    if (!m_connectionsLabel) return;
    size_t count = 0;
    if (m_secretsStore) count = m_secretsStore->ListConnections().size();

    if (count == 0) {
        m_connectionsLabel->SetLabel("No connections configured");
    } else if (count == 1) {
        m_connectionsLabel->SetLabel("1 connection configured");
    } else {
        m_connectionsLabel->SetLabel(
            wxString::Format("%zu connections configured", count));
    }
    if (m_connectionsLabel->GetParent())
        m_connectionsLabel->GetParent()->Layout();
}

// ─── Remote Endpoints ───────────────────────────────────────────

void SettingsDialog::OnManageEndpoints(wxCommandEvent&)
{
    if (!m_endpointStore) {
        wxMessageBox(
            "Endpoint store is not available. Restart LlamaBoss "
            "and try again.",
            "Remote Endpoints", wxOK | wxICON_INFORMATION, this);
        return;
    }

    EndpointsDialog dlg(this, m_endpointStore, *m_theme);
    dlg.ShowModal();

    // Persist immediately on close, like Connections — endpoint config is
    // user-edited state that should survive even if Settings is later
    // cancelled.
    const bool saved = m_endpointStore->Save();
    if (!saved) {
        wxMessageBox(
            "Endpoints were updated for this session, but LlamaBoss could "
            "not save them to disk. They may be lost after restart.",
            "Endpoints Not Saved", wxOK | wxICON_WARNING, this);
    }

    UpdateEndpointsLabel();
}

void SettingsDialog::UpdateEndpointsLabel()
{
    if (!m_endpointsLabel) return;
    size_t count = 0;
    if (m_endpointStore) count = m_endpointStore->Endpoints().size();

    if (count == 0) {
        m_endpointsLabel->SetLabel("No remote endpoints configured");
    } else if (count == 1) {
        m_endpointsLabel->SetLabel("1 remote endpoint configured");
    } else {
        m_endpointsLabel->SetLabel(
            wxString::Format("%zu remote endpoints configured", count));
    }
    if (m_endpointsLabel->GetParent())
        m_endpointsLabel->GetParent()->Layout();
}
