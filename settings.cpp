// settings.cpp (updated)
#include "settings.h"
#include <wx/fileconf.h>
#include <wx/msgdlg.h>

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
                               const std::string& currentApiUrl, const std::string& currentTheme)
    : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(500, 340))
    , m_selectedModel(currentModel)
    , m_selectedApiUrl(currentApiUrl)
    , m_selectedTheme(currentTheme)
    , m_originalModel(currentModel)
    , m_originalApiUrl(currentApiUrl)
    , m_originalTheme(currentTheme)
    , m_modelChanged(false)
    , m_apiUrlChanged(false)
    , m_themeChanged(false)
    , m_isFetching(false)
    , m_fetchThread(nullptr)
{
    CreateControls();

    // Auto-fetch models on startup
    CallAfter([this]() {
        StartModelFetch();
        });
}

SettingsDialog::~SettingsDialog()
{
    // NOTE: m_fetchThread is a detached thread; calling Delete() on it after
    // it has already finished is technically a dangling-pointer use.  Because
    // this is a modal dialog with a very short-lived thread (single GET),
    // the race window is negligible.  A future cleanup could migrate to the
    // same shared-atomic-cancel-flag pattern used in ChatClient.
    if (m_fetchThread && m_isFetching) {
        m_fetchThread->Delete();
        m_fetchThread = nullptr;
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

    // Future settings can be added here
    // Font size, timeout settings, etc.

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

    m_fetchThread = new ModelFetchThread(this, apiUrl);
    if (m_fetchThread->Run() != wxTHREAD_NO_ERROR) {
        delete m_fetchThread;
        m_fetchThread = nullptr;
        SetFetchingState(false);
        m_statusText->SetLabel("Failed to start model fetch");
    }
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

void SettingsDialog::OnModelsReceived(wxCommandEvent& event)
{
    SetFetchingState(false);

    // Clear existing items
    m_modelComboBox->Clear();

    // Add new models
    for (const auto& model : m_availableModels) {
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

    m_statusText->SetLabel(wxString::Format("Found %zu models", m_availableModels.size()));
    m_fetchThread = nullptr;
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

    m_fetchThread = nullptr;
}

void SettingsDialog::PostModelsReceived(const std::vector<std::string>& models)
{
    m_availableModels = models;
    wxCommandEvent* event = new wxCommandEvent(wxEVT_MODELS_RECEIVED);
    wxQueueEvent(this, event);
}

void SettingsDialog::PostModelsFetchError(const std::string& error)
{
    wxCommandEvent* event = new wxCommandEvent(wxEVT_MODELS_FETCH_ERROR);
    event->SetString(wxString::FromUTF8(error));
    wxQueueEvent(this, event);
}

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

    m_modelChanged = (m_selectedModel != m_originalModel);
    m_apiUrlChanged = (m_selectedApiUrl != m_originalApiUrl);
    m_themeChanged = (m_selectedTheme != m_originalTheme);

    // Save to config
    wxFileConfig cfg("OllamaChatApp");
    cfg.Write("Model", wxString::FromUTF8(m_selectedModel));
    cfg.Write("ApiBaseUrl", wxString::FromUTF8(m_selectedApiUrl));
    cfg.Flush();

    EndModal(wxID_OK);
}

void SettingsDialog::OnCancel(wxCommandEvent& event)
{
    // Stop any ongoing fetch
    if (m_fetchThread && m_isFetching) {
        m_fetchThread->Delete();
        m_fetchThread = nullptr;
    }

    EndModal(wxID_CANCEL);
}

// ═══════════════════════════════════════════════════════════════════
// ModelFetchThread Implementation
// ═══════════════════════════════════════════════════════════════════

ModelFetchThread::ModelFetchThread(SettingsDialog* dialog, const std::string& apiUrl)
    : wxThread(wxTHREAD_DETACHED)
    , m_dialog(dialog)
    , m_apiUrl(apiUrl)
{
}

wxThread::ExitCode ModelFetchThread::Entry()
{
    std::vector<std::string> models;

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
            if (!TestDestroy()) {
                m_dialog->PostModelsFetchError(
                    "HTTP " + std::to_string(resp.getStatus()) + ": " + resp.getReason()
                );
            }
            return (ExitCode)0;
        }

        // Parse the JSON response
        std::string responseBody;
        Poco::StreamCopier::copyToString(in, responseBody);

        if (TestDestroy()) return (ExitCode)0;

        Poco::JSON::Parser parser;
        auto result = parser.parse(responseBody);
        auto obj = result.extract<Poco::JSON::Object::Ptr>();

        if (obj->has("models")) {
            auto modelsArray = obj->getArray("models");
            for (size_t i = 0; i < modelsArray->size(); ++i) {
                if (TestDestroy()) return (ExitCode)0;

                auto modelObj = modelsArray->getObject(i);
                if (modelObj->has("name")) {
                    std::string modelName = modelObj->getValue<std::string>("name");
                    models.push_back(modelName);
                }
            }
        }

        if (!TestDestroy()) {
            m_dialog->PostModelsReceived(models);
        }
    }
    catch (const Poco::Net::HTTPException& ex) {
        if (!TestDestroy()) {
            m_dialog->PostModelsFetchError("HTTP Error: " + ex.displayText());
        }
    }
    catch (const Poco::Net::NetException& ex) {
        if (!TestDestroy()) {
            m_dialog->PostModelsFetchError("Network Error: " + ex.displayText());
        }
    }
    catch (const Poco::JSON::JSONException& ex) {
        if (!TestDestroy()) {
            m_dialog->PostModelsFetchError("JSON Parse Error: " + ex.displayText());
        }
    }
    catch (const Poco::Exception& ex) {
        if (!TestDestroy()) {
            m_dialog->PostModelsFetchError("Poco Error: " + ex.displayText());
        }
    }
    catch (const std::exception& ex) {
        if (!TestDestroy()) {
            m_dialog->PostModelsFetchError(std::string("Error: ") + ex.what());
        }
    }

    return (ExitCode)0;
}