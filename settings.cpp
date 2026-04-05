// settings.cpp
#include "settings.h"
#include <wx/fileconf.h>
#include <wx/msgdlg.h>
#include <wx/dirdlg.h>

// Poco headers for HTTP requests
#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Exception.h>

#include <sstream>

// Define custom events
wxDEFINE_EVENT(wxEVT_MODELS_RECEIVED, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_MODELS_FETCH_ERROR, wxCommandEvent);

wxBEGIN_EVENT_TABLE(SettingsDialog, wxDialog)
EVT_BUTTON(wxID_OK, SettingsDialog::OnOK)
EVT_BUTTON(wxID_CANCEL, SettingsDialog::OnCancel)
EVT_BUTTON(wxID_REFRESH, SettingsDialog::OnRefreshModels)
EVT_TEXT(wxID_ANY, SettingsDialog::OnApiUrlChanged)
EVT_COMMAND(wxID_ANY, wxEVT_MODELS_RECEIVED, SettingsDialog::OnModelsReceived)
EVT_COMMAND(wxID_ANY, wxEVT_MODELS_FETCH_ERROR, SettingsDialog::OnModelsFetchError)
wxEND_EVENT_TABLE()

SettingsDialog::SettingsDialog(wxWindow* parent, const std::string& currentModel,
                               const std::string& currentApiUrl, const std::string& currentTheme,
                               bool currentWorkspaceEnabled, const std::string& currentWorkspacePath)
    : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(500, 420))
    , m_selectedModel(currentModel)
    , m_selectedApiUrl(currentApiUrl)
    , m_selectedTheme(currentTheme)
    , m_originalModel(currentModel)
    , m_originalApiUrl(currentApiUrl)
    , m_originalTheme(currentTheme)
    , m_workspaceEnabled(currentWorkspaceEnabled)
    , m_workspacePath(currentWorkspacePath)
    , m_originalWorkspaceEnabled(currentWorkspaceEnabled)
    , m_originalWorkspacePath(currentWorkspacePath)
    , m_modelChanged(false)
    , m_apiUrlChanged(false)
    , m_themeChanged(false)
    , m_workspaceChanged(false)
    , m_isFetching(false)
    // [STEP 4] m_fetchThread removed — no stored thread pointer
{
    CreateControls();

    // Auto-fetch models on startup
    CallAfter([this]() {
        StartModelFetch();
        });
}

SettingsDialog::~SettingsDialog()
{
    // [STEP 4] Signal any running fetch thread to stop.
    // The thread checks this flag and will bail out + discard its event.
    // No dangling pointer — we never stored the thread pointer.
    if (m_cancelFlag) {
        m_cancelFlag->store(true);
    }
}

void SettingsDialog::CreateControls()
{
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // API URL section (at top so user can change it before fetching models)
    auto* apiUrlLabel = new wxStaticText(this, wxID_ANY, "API Base URL:");
    mainSizer->Add(apiUrlLabel, 0, wxALL, 5);

    auto* apiUrlSizer = new wxBoxSizer(wxHORIZONTAL);
    m_apiUrlTextCtrl = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(m_selectedApiUrl));
    apiUrlSizer->Add(m_apiUrlTextCtrl, 1, wxEXPAND | wxRIGHT, 5);

    m_refreshButton = new wxButton(this, wxID_REFRESH, "Refresh Models");
    apiUrlSizer->Add(m_refreshButton, 0, wxALIGN_CENTER_VERTICAL);

    mainSizer->Add(apiUrlSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // Model selection section
    auto* modelLabel = new wxStaticText(this, wxID_ANY, "Ollama Model:");
    mainSizer->Add(modelLabel, 0, wxLEFT | wxRIGHT | wxTOP, 5);

    m_modelComboBox = new wxComboBox(this, wxID_ANY, wxString::FromUTF8(m_selectedModel),
        wxDefaultPosition, wxDefaultSize, 0, nullptr,
        wxCB_DROPDOWN | wxCB_SORT);
    mainSizer->Add(m_modelComboBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // Status and progress section
    m_statusText = new wxStaticText(this, wxID_ANY, "Ready");
    mainSizer->Add(m_statusText, 0, wxLEFT | wxRIGHT, 5);

    m_progressGauge = new wxGauge(this, wxID_ANY, 100);
    m_progressGauge->Hide(); // Initially hidden
    mainSizer->Add(m_progressGauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // ── Theme selection ──────────────────────────────────────────
    mainSizer->AddSpacer(5);
    auto* themeLabel = new wxStaticText(this, wxID_ANY, "Theme:");
    mainSizer->Add(themeLabel, 0, wxLEFT | wxRIGHT | wxTOP, 5);

    wxArrayString themeChoices;
    themeChoices.Add("Dark");
    themeChoices.Add("Light");
    themeChoices.Add("System");
    m_themeComboBox = new wxComboBox(this, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, themeChoices,
        wxCB_DROPDOWN | wxCB_READONLY);

    // Select current theme
    if (m_selectedTheme == "light")
        m_themeComboBox->SetSelection(1);
    else if (m_selectedTheme == "system")
        m_themeComboBox->SetSelection(2);
    else
        m_themeComboBox->SetSelection(0);

    mainSizer->Add(m_themeComboBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // ── Workspace section ────────────────────────────────────────
    mainSizer->AddSpacer(5);
    auto* workspaceLabel = new wxStaticText(this, wxID_ANY, "Workspace:");
    mainSizer->Add(workspaceLabel, 0, wxLEFT | wxRIGHT | wxTOP, 5);

    m_workspaceCheckBox = new wxCheckBox(this, wxID_ANY, "Enable workspace commands (/search)");
    m_workspaceCheckBox->SetValue(m_workspaceEnabled);
    mainSizer->Add(m_workspaceCheckBox, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);

    auto* pathSizer = new wxBoxSizer(wxHORIZONTAL);
    m_workspacePathCtrl = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(m_workspacePath));
    pathSizer->Add(m_workspacePathCtrl, 1, wxEXPAND | wxRIGHT, 5);

    m_workspaceBrowseButton = new wxButton(this, wxID_ANY, "Browse...");
    m_workspaceBrowseButton->Bind(wxEVT_BUTTON, &SettingsDialog::OnBrowseWorkspace, this);
    pathSizer->Add(m_workspaceBrowseButton, 0, wxALIGN_CENTER_VERTICAL);

    mainSizer->Add(pathSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // Buttons
    auto* buttonSizer = CreateButtonSizer(wxOK | wxCANCEL);
    mainSizer->Add(buttonSizer, 0, wxEXPAND | wxALL, 5);

    SetSizer(mainSizer);
    m_apiUrlTextCtrl->SetFocus();
}

void SettingsDialog::StartModelFetch()
{
    if (m_isFetching) return;

    std::string apiUrl = m_apiUrlTextCtrl->GetValue().ToStdString();
    if (apiUrl.empty()) {
        m_statusText->SetLabel("Please enter API URL");
        return;
    }

    SetFetchingState(true);
    m_statusText->SetLabel("Fetching available models...");

    // [STEP 3/4] Create a fresh cancel flag for this fetch.
    // If a previous fetch is somehow still running, its old cancel flag
    // is now orphaned (but harmless — the thread holds a shared_ptr to it).
    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);

    auto* thread = new ModelFetchThread(this, apiUrl, m_cancelFlag);
    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_cancelFlag.reset();
        SetFetchingState(false);
        m_statusText->SetLabel("Failed to start model fetch");
    }
    // Thread is now detached and running — we do NOT store its pointer.
}

void SettingsDialog::SetFetchingState(bool fetching)
{
    m_isFetching = fetching;
    m_refreshButton->Enable(!fetching);
    m_progressGauge->Show(fetching);

    if (fetching) {
        m_progressGauge->Pulse();
    }
    else {
        m_progressGauge->SetValue(0);
    }

    Layout();
}

void SettingsDialog::OnRefreshModels(wxCommandEvent& /*event*/)
{
    StartModelFetch();
}

void SettingsDialog::OnApiUrlChanged(wxCommandEvent& event)
{
    // Only respond to changes in the API URL text control
    if (event.GetEventObject() == m_apiUrlTextCtrl) {
        // Don't auto-refresh immediately, just clear the status
        if (!m_isFetching) {
            m_statusText->SetLabel("Click 'Refresh Models' to load available models");
        }
    }
    event.Skip();  // Let other controls (combo box) process their text events
}

void SettingsDialog::OnBrowseWorkspace(wxCommandEvent& /*event*/)
{
    wxDirDialog dlg(this, "Select Workspace Folder",
                    m_workspacePathCtrl->GetValue(),
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);

    if (dlg.ShowModal() == wxID_OK) {
        m_workspacePathCtrl->SetValue(dlg.GetPath());
    }
}

void SettingsDialog::OnModelsReceived(wxCommandEvent& event)
{
    SetFetchingState(false);

    // [STEP 3] Parse newline-separated model list from the event.
    // The thread packs model names into the event string so it never
    // writes directly to dialog members from the worker thread.
    std::vector<std::string> models;
    std::istringstream stream(event.GetString().ToStdString());
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) models.push_back(line);
    }

    // Clear existing items
    m_modelComboBox->Clear();

    // Add new models
    for (const auto& model : models) {
        m_modelComboBox->Append(wxString::FromUTF8(model));
    }

    // Try to select the current model
    int index = m_modelComboBox->FindString(wxString::FromUTF8(m_selectedModel));
    if (index != wxNOT_FOUND) {
        m_modelComboBox->SetSelection(index);
    }
    else if (m_modelComboBox->GetCount() > 0) {
        // If current model not found, select the first one
        m_modelComboBox->SetSelection(0);
    }

    m_statusText->SetLabel(wxString::Format("Found %zu models", models.size()));
}

void SettingsDialog::OnModelsFetchError(wxCommandEvent& event)
{
    SetFetchingState(false);
    std::string error = event.GetString().ToStdString();
    m_statusText->SetLabel("Error: " + error);

    // Allow manual entry if fetch fails
    if (m_modelComboBox->GetCount() == 0) {
        m_modelComboBox->SetValue(wxString::FromUTF8(m_selectedModel));
    }
}

// [STEP 3] PostModelsReceived and PostModelsFetchError removed —
// the thread now communicates entirely through wxCommandEvents.

void SettingsDialog::OnOK(wxCommandEvent& event)
{
    // Wait for any ongoing fetch to complete
    if (m_isFetching) {
        wxMessageBox("Please wait for model list to finish loading", "Please Wait",
            wxOK | wxICON_INFORMATION);
        return;
    }

    m_selectedModel = m_modelComboBox->GetValue().ToStdString();
    m_selectedApiUrl = m_apiUrlTextCtrl->GetValue().ToStdString();
    int themeSel = m_themeComboBox->GetSelection();
    m_selectedTheme = (themeSel == 2) ? "system" : (themeSel == 1) ? "light" : "dark";

    m_workspaceEnabled = m_workspaceCheckBox->GetValue();
    m_workspacePath = m_workspacePathCtrl->GetValue().ToStdString();

    m_modelChanged = (m_selectedModel != m_originalModel);
    m_apiUrlChanged = (m_selectedApiUrl != m_originalApiUrl);
    m_themeChanged = (m_selectedTheme != m_originalTheme);
    m_workspaceChanged = (m_workspaceEnabled != m_originalWorkspaceEnabled ||
                          m_workspacePath != m_originalWorkspacePath);

    // Save to config
    wxFileConfig cfg("OllamaChatApp");
    cfg.Write("Model", wxString::FromUTF8(m_selectedModel));
    cfg.Write("ApiBaseUrl", wxString::FromUTF8(m_selectedApiUrl));
    cfg.Flush();

    EndModal(wxID_OK);
}

void SettingsDialog::OnCancel(wxCommandEvent& event)
{
    // [STEP 4] Signal any running fetch to stop — no dangling pointer
    if (m_cancelFlag) {
        m_cancelFlag->store(true);
    }

    EndModal(wxID_CANCEL);
}

// ═══════════════════════════════════════════════════════════════════
// ModelFetchThread Implementation
// ═══════════════════════════════════════════════════════════════════

// [STEP 3] Takes a generic event handler + cancel flag instead of
// a raw SettingsDialog pointer.
ModelFetchThread::ModelFetchThread(wxEvtHandler* handler,
                                   const std::string& apiUrl,
                                   std::shared_ptr<std::atomic<bool>> cancelFlag)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_apiUrl(apiUrl)
    , m_cancelFlag(cancelFlag)
{
}

// [STEP 3] Post event only if not cancelled.
bool ModelFetchThread::SafePost(wxCommandEvent* event)
{
    if (m_cancelFlag->load()) {
        delete event;
        return false;
    }
    wxQueueEvent(m_handler, event);
    return true;
}

wxThread::ExitCode ModelFetchThread::Entry()
{
    // Local cancellation check
    auto isCancelled = [this]() { return m_cancelFlag->load(); };

    try {
        // Use Ollama's /api/tags endpoint to get available models
        Poco::URI uri(m_apiUrl + "/api/tags");
        Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
        sess.setTimeout(Poco::Timespan(10, 0)); // 10 second timeout

        Poco::Net::HTTPRequest req(
            Poco::Net::HTTPRequest::HTTP_GET,
            uri.getPathAndQuery(),
            Poco::Net::HTTPMessage::HTTP_1_1
        );

        sess.sendRequest(req);

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            std::string err;
            Poco::StreamCopier::copyToString(in, err);
            if (!isCancelled()) {
                auto* event = new wxCommandEvent(wxEVT_MODELS_FETCH_ERROR);
                event->SetString(wxString::FromUTF8(
                    "HTTP " + std::to_string(resp.getStatus()) + ": " + resp.getReason()
                ));
                SafePost(event);
            }
            return (ExitCode)0;
        }

        // Parse the JSON response
        std::string responseBody;
        Poco::StreamCopier::copyToString(in, responseBody);

        if (isCancelled()) return (ExitCode)0;

        Poco::JSON::Parser parser;
        auto result = parser.parse(responseBody);
        auto obj = result.extract<Poco::JSON::Object::Ptr>();

        // [STEP 3] Pack model names into a newline-separated string
        // and send them through the event — no direct member writes.
        wxString modelList;
        if (obj->has("models")) {
            auto modelsArray = obj->getArray("models");
            for (size_t i = 0; i < modelsArray->size(); ++i) {
                if (isCancelled()) return (ExitCode)0;

                auto modelObj = modelsArray->getObject(i);
                if (modelObj->has("name")) {
                    if (!modelList.empty()) modelList += "\n";
                    modelList += wxString::FromUTF8(
                        modelObj->getValue<std::string>("name"));
                }
            }
        }

        if (!isCancelled()) {
            auto* event = new wxCommandEvent(wxEVT_MODELS_RECEIVED);
            event->SetString(modelList);
            SafePost(event);
        }
    }
    catch (const Poco::Net::HTTPException& ex) {
        if (!isCancelled()) {
            auto* event = new wxCommandEvent(wxEVT_MODELS_FETCH_ERROR);
            event->SetString(wxString::FromUTF8("HTTP Error: " + ex.displayText()));
            SafePost(event);
        }
    }
    catch (const Poco::Net::NetException& ex) {
        if (!isCancelled()) {
            auto* event = new wxCommandEvent(wxEVT_MODELS_FETCH_ERROR);
            event->SetString(wxString::FromUTF8("Network Error: " + ex.displayText()));
            SafePost(event);
        }
    }
    catch (const Poco::JSON::JSONException& ex) {
        if (!isCancelled()) {
            auto* event = new wxCommandEvent(wxEVT_MODELS_FETCH_ERROR);
            event->SetString(wxString::FromUTF8("JSON Parse Error: " + ex.displayText()));
            SafePost(event);
        }
    }
    catch (const Poco::Exception& ex) {
        if (!isCancelled()) {
            auto* event = new wxCommandEvent(wxEVT_MODELS_FETCH_ERROR);
            event->SetString(wxString::FromUTF8("Poco Error: " + ex.displayText()));
            SafePost(event);
        }
    }
    catch (const std::exception& ex) {
        if (!isCancelled()) {
            auto* event = new wxCommandEvent(wxEVT_MODELS_FETCH_ERROR);
            event->SetString(wxString::FromUTF8(std::string("Error: ") + ex.what()));
            SafePost(event);
        }
    }

    return (ExitCode)0;
}
