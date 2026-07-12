// model_switcher.cpp
#include "model_switcher.h"

#include "model_service.h"
#include "app_state.h"
#include "server_manager.h"
#include "chat_display.h"
#include "chat_history.h"
#include "attachment_manager.h"
#include "widgets.h"
#include "model_manager.h"
#include "model_downloader.h"   // first-run onboarding
#include "theme.h"
#include "path_safety.h"
#include "secrets_store.h"     // resolve remote API key
#include "inference_target.h"  // InferenceTarget
#include "tool_protocol.h"     // ToolProtocol
#include "endpoint_store.h"    // 2d: configured remote endpoints

#include <algorithm>
#include <cctype>
#include <vector>
#include <unordered_map>

#include <wx/filename.h>

namespace {

// ── Remote selection key ─────────────────────────────────────────
// A remote selection is encoded as the model "key":
//   remote:<endpointId>/<wireModelId>
// e.g. remote:openrouter/anthropic/claude-sonnet-4.6
// The user never sees this string — it's just what a picker row hands
// to SwitchToModel so it can tell a remote selection from a local
// .gguf path. The endpoint and model come from EndpointStore.
constexpr char kRemotePrefix[] = "remote:";

bool IsRemoteModelKey(const std::string& key)
{
    const std::string prefix(kRemotePrefix);
    return key.size() >= prefix.size() &&
           key.compare(0, prefix.size(), prefix) == 0;
}

bool SameSelectionKey(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    if (IsRemoteModelKey(a) || IsRemoteModelKey(b)) return a == b;
    return path_safety::SameModelPath(a, b);
}

// Build the picker key for a configured (endpoint, model) pair.
std::string RemoteModelKey(const std::string& endpointId,
                           const std::string& wireModelId)
{
    return std::string(kRemotePrefix) + endpointId + "/" + wireModelId;
}

bool ParseRemoteModelKey(const std::string& key,
                         std::string& endpointId,
                         std::string& wireModelId)
{
    endpointId.clear();
    wireModelId.clear();
    if (!IsRemoteModelKey(key)) return false;

    const std::string rest = key.substr(std::string(kRemotePrefix).size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size())
        return false;

    endpointId  = rest.substr(0, slash);
    wireModelId = rest.substr(slash + 1);
    return true;
}

// Heuristic: does this token look like a GGUF or torch quantization
// label (so we can render it as a " · <q>" suffix in the pill)?
// Catches f16/fp16/bf16/f32/fp32 plus the qN... and iqN... families.
// False negatives are fine -- worst case we just hyphenate the token
// in with the rest of the model identity, which is still readable.
bool LooksLikeQuantization(const std::string& token)
{
    if (token.empty() || token.size() > 16) return false;

    std::string lower;
    lower.reserve(token.size());
    for (char c : token) {
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower == "f16"  || lower == "fp16" || lower == "bf16" ||
        lower == "f32"  || lower == "fp32") {
        return true;
    }

    auto isDigitCh = [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    };

    // "qN..."  -- q4, q4_0, q4_K_M, q5_K_S, q8_0, etc.
    if (lower[0] == 'q' && lower.size() >= 2 && isDigitCh(lower[1])) {
        return true;
    }
    // "iqN..." -- iq4_NL, iq3_XXS, etc.
    if (lower.size() >= 3 && lower[0] == 'i' && lower[1] == 'q' &&
        isDigitCh(lower[2])) {
        return true;
    }

    return false;
}

// Reformats a friendly display name ("gemma 4 e4b it f16") into the
// pill form ("gemma-4-e4b-it · f16").  Identity tokens are joined with
// hyphens; the final token, if it looks like a quantization label, is
// peeled off and rendered as a "· <q>" suffix.  No quantization match
// at the tail means everything hyphenates, which is still a readable
// fallback.  Returns the input unchanged if it doesn't tokenize.
std::string FormatModelNameForPill(const std::string& displayName)
{
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : displayName) {
        if (c == ' ') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);

    if (tokens.empty())     return displayName;
    if (tokens.size() == 1) return tokens.front();

    std::string quant;
    if (LooksLikeQuantization(tokens.back())) {
        quant = tokens.back();
        tokens.pop_back();
    }

    std::string out = tokens.front();
    for (size_t i = 1; i < tokens.size(); ++i) {
        out += '-';
        out += tokens[i];
    }

    if (!quant.empty()) {
        out += " \xC2\xB7 ";  // " · "
        out += quant;
    }

    return out;
}

std::string LowerAscii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool EndsWithGguf(const std::string& s)
{
    if (s.size() < 5) return false;
    return LowerAscii(s.substr(s.size() - 5)) == ".gguf";
}

bool ModelFileExists(const std::string& path)
{
    return !path.empty() && wxFileExists(wxString::FromUTF8(path.c_str()));
}

std::string PickExistingModelFallback(const std::string& preferred)
{
    if (ModelFileExists(preferred)) return preferred;

    auto models = ServerManager::ScanModelPaths();
    if (!models.empty()) return models.front();

    return {};
}

} // namespace

ModelSwitcher::ModelSwitcher(ModelService& service,
                             AppState& appState,
                             ServerManager& serverManager,
                             ChatDisplay* chatDisplay,
                             std::unique_ptr<ChatHistory>& chatHistory,
                             AttachmentManager& attachments,
                             StatusDot* statusDot,
                             wxStaticText* modelLabel,
                             wxWindow* parentFrame)
    : m_service(service)
    , m_appState(appState)
    , m_serverManager(serverManager)
    , m_chatDisplay(chatDisplay)
    , m_chatHistory(chatHistory)
    , m_attachments(attachments)
    , m_statusDot(statusDot)
    , m_modelLabel(modelLabel)
    , m_parentFrame(parentFrame)
{
    wxASSERT_MSG(m_chatDisplay, "ModelSwitcher requires a non-null ChatDisplay");
    wxASSERT_MSG(m_chatHistory, "ModelSwitcher requires a non-null ChatHistory");
    wxASSERT_MSG(m_statusDot, "ModelSwitcher requires a non-null StatusDot");
    wxASSERT_MSG(m_modelLabel, "ModelSwitcher requires a non-null model label");
}

bool ModelSwitcher::IsConversationTargetActive() const
{
    if (m_conversationSelectionKey.empty()) return true;
    return SameSelectionKey(m_conversationSelectionKey,
                            m_service.GetActiveSelectionKey());
}

bool ModelSwitcher::IsServerReady() const
{
    return m_service.IsServerReady() && IsConversationTargetActive();
}

void ModelSwitcher::MarkServerNotReady()
{
    // Shared readiness is service-owned.  This legacy helper now affects only
    // the current frame's projection of that state.
    if (m_statusDot) m_statusDot->SetConnected(false);
}

std::string ModelSwitcher::GetConversationModelForSave() const
{
    return m_conversationModelForSave.empty()
        ? m_appState.GetModel()
        : m_conversationModelForSave;
}

void ModelSwitcher::SetConversationPreferredLocalModel(
    const std::string& modelPath)
{
    m_conversationSelectionKey = modelPath;
    m_conversationModelForSave = modelPath;

    if (SameSelectionKey(modelPath, m_service.GetActiveSelectionKey()) &&
        m_service.IsServerReady()) {
        m_pendingDeferredModel.clear();
    }
    else {
        m_pendingDeferredModel = modelPath;
    }
}

void ModelSwitcher::SetConversationPreferredRemoteModel(
    const std::string& selectionKey,
    const std::string& wireModel)
{
    m_conversationSelectionKey = selectionKey;
    m_conversationModelForSave = wireModel;
    m_pendingDeferredModel.clear();
}


bool ModelSwitcher::SetConversationPreferredSavedModel(
    const std::string& savedModel)
{
    if (savedModel.empty()) return false;

    if (ModelFileExists(savedModel)) {
        SetConversationPreferredLocalModel(savedModel);
        return true;
    }

    // A conversation currently using the active remote model can retain the
    // endpoint identity exactly, even though older conversation files persist
    // only the wire model id.
    const InferenceTarget activeTarget = m_service.ResolveTarget();
    const std::string activeKey = m_service.GetActiveSelectionKey();
    if (!activeTarget.managed && activeTarget.modelId == savedModel &&
        IsRemoteModelKey(activeKey)) {
        SetConversationPreferredRemoteModel(activeKey, savedModel);
        return true;
    }

    EndpointStore* endpoints = m_appState.GetEndpointStore();
    if (!endpoints) return false;

    // Prefer the last explicit remote selection when it names this wire model.
    // This disambiguates common ids that appear under multiple providers.
    std::string lastEndpoint;
    std::string lastWireModel;
    const std::string lastSelection = m_appState.GetLastSelection();
    if (ParseRemoteModelKey(lastSelection, lastEndpoint, lastWireModel) &&
        lastWireModel == savedModel) {
        if (const EndpointStore::Endpoint* ep =
                endpoints->FindEndpoint(lastEndpoint)) {
            const bool stillConfigured = std::any_of(
                ep->models.begin(), ep->models.end(),
                [&savedModel](const EndpointStore::Model& model) {
                    return model.id == savedModel;
                });
            if (stillConfigured) {
                SetConversationPreferredRemoteModel(lastSelection, savedModel);
                return true;
            }
        }
    }

    // Otherwise accept only a unique endpoint/model match.  Guessing when two
    // endpoints expose the same wire id could silently send private chat data
    // to the wrong provider.
    std::string matchedKey;
    size_t matches = 0;
    for (const auto& ep : endpoints->Endpoints()) {
        for (const auto& model : ep.models) {
            if (model.id != savedModel) continue;
            matchedKey = RemoteModelKey(ep.id, model.id);
            ++matches;
        }
    }

    if (matches == 1) {
        SetConversationPreferredRemoteModel(matchedKey, savedModel);
        return true;
    }

    return false;
}

void ModelSwitcher::AdoptActiveTargetForConversation()
{
    m_conversationSelectionKey = m_service.GetActiveSelectionKey();
    m_conversationModelForSave = m_service.ResolveTarget().modelId;
    if (m_conversationModelForSave.empty())
        m_conversationModelForSave = m_appState.GetModel();
    m_pendingDeferredModel.clear();
}

void ModelSwitcher::ClearConversationPreference()
{
    m_conversationSelectionKey.clear();
    m_conversationModelForSave.clear();
    m_pendingDeferredModel.clear();
}

bool ModelSwitcher::NeedsRemoteActivationForConversation() const
{
    return IsRemoteModelKey(m_conversationSelectionKey) &&
           !IsConversationTargetActive();
}

bool ModelSwitcher::ActivateConversationPreferredRemoteTarget()
{
    if (!NeedsRemoteActivationForConversation())
        return IsServerReady();

    if (m_service.AnyOtherWindowBusy(m_parentFrame)) {
        const int r = wxMessageBox(
            "Another window is generating a response. Activating this "
            "conversation's remote model will interrupt it.\n\nSwitch anyway?",
            "Model Switch", wxYES_NO | wxICON_WARNING, m_parentFrame);
        if (r != wxYES) return false;
    }

    return ActivateRemoteModel(m_conversationSelectionKey);
}

void ModelSwitcher::OnServiceStateChanged()
{
    if (m_conversationSelectionKey.empty())
        AdoptActiveTargetForConversation();

    const bool targetMatches = IsConversationTargetActive();
    if (targetMatches) {
        if (m_service.IsServerReady())
            m_pendingDeferredModel.clear();
        if (m_statusDot)
            m_statusDot->SetConnected(m_service.IsServerReady());
    }
    else {
        if (!IsRemoteModelKey(m_conversationSelectionKey) &&
            !m_conversationSelectionKey.empty() &&
            wxFileExists(wxString::FromUTF8(m_conversationSelectionKey))) {
            m_pendingDeferredModel = m_conversationSelectionKey;
        }
        else {
            m_pendingDeferredModel.clear();
        }
        if (m_statusDot) m_statusDot->SetConnected(false);
    }

    UpdateModelLabel();
    if (m_cb.updateWindowTitle) m_cb.updateWindowTitle();
}

// ═════════════════════════════════════════════════════════════════
//  SERVER BOOTSTRAP
// ═════════════════════════════════════════════════════════════════

// ── KV slot ownership forwarding ─────────────────────────────────
// Invalidation stays a thin pass-through (always safe).  The stamp
// routes through ModelService (Phase 3c) so a goal auto-continuation
// dispatched while another window is mid-generation invalidates
// instead of claiming — same adjudication as the main send path.

void ModelSwitcher::InvalidateKvSlotOwner()
{
    m_serverManager.InvalidateSlotOwner();
}

void ModelSwitcher::NoteKvSlotOwner(const std::string& conversationPath)
{
    m_service.NoteSlotOwner(m_parentFrame, conversationPath);
}

void ModelSwitcher::StartInitialServer()
{
    // 2d-iv: restore the last selection if it was a remote endpoint —
    // before any local-model scan or first-run flow. A remote-only user
    // (no local GGUFs) should land back on their remote model rather than
    // the first-run downloader. If the endpoint or its key is gone,
    // ActivateRemoteModel fails and we fall through to the local boot path,
    // which overwrites the stale remote selection once a local model loads.
    {
        const std::string lastSel = m_appState.GetLastSelection();
        if (IsRemoteModelKey(lastSel) && ActivateRemoteModel(lastSel)) {
            return;
        }
    }

    auto models = ServerManager::ScanModelPaths();

    if (models.empty()) {
        // ── First-run onboarding path ─────────────────────────────
        // A fresh install with no models AND the first-run flag still
        // set means this is someone's very first encounter with the
        // app. Open the downloader in first-run mode — it pre-highlights
        // the starter model and auto-closes on successful download so
        // the caller (here) can pick up and load it immediately.
        //
        // Existing users landing here (e.g., they deleted all their
        // models) have been migrated past first-run in AppState::
        // Initialize(), so they take the else branch below and see
        // the normal empty-state message instead.
        if (m_appState.IsFirstRun() && m_parentFrame) {
            std::string downloaded = LaunchFirstRunDownloader();
            if (!downloaded.empty()) {
                // Success — persist the choice and fall through to the
                // load-model block below. Rescanning picks up the new
                // bundle folder we just populated.
                bool mc, ac;
                m_appState.UpdateSettings(downloaded, m_appState.GetApiUrl(),
                                          mc, ac);
                m_completingFirstRun = true;
                models = ServerManager::ScanModelPaths();
                if (models.empty()) {
                    m_completingFirstRun = false;
                    m_statusDot->SetConnected(false);
                    m_chatDisplay->DisplaySystemMessage(
                        "Download finished, but LlamaBoss could not find "
                        "the model when rescanning the models folder.\n\n"
                        "Open Settings → Download Models, or drop a .gguf "
                        "file into: " + ServerManager::GetModelsDir());
                    ServerManager::EnsureDataDirs();
                    return;
                }
                // Fall through — models is now non-empty.
            } else {
                // Dismissed without downloading. Leave the first-run
                // flag set so they get the onboarding again next launch.
                ShowFirstRunDismissedMessage();
                ServerManager::EnsureDataDirs();
                return;
            }
        } else {
            // Existing user with an empty folder, or first-run user on
            // a build without a parent frame wired up. Show the original
            // empty-state message unchanged.
            m_statusDot->SetConnected(false);
            m_chatDisplay->DisplaySystemMessage(
                "No models found.\n\n"
                "To get started:\n"
                "1. Download a model through Settings \xe2\x86\x92 Download Models\n"
                "2. Or drop a .gguf file into: " + ServerManager::GetModelsDir() + "\n"
                "3. Open Settings to select it\n\n"
                "The models folder has been created for you.");

            ServerManager::EnsureDataDirs();
            return;
        }
    }

    // Use saved model if it still exists, otherwise first available
    std::string savedModel = m_appState.GetModel();
    std::string modelToLoad;

    if (!savedModel.empty() && wxFileExists(wxString::FromUTF8(savedModel.c_str()))) {
        modelToLoad = savedModel;
    }
    else {
        modelToLoad = models.front();
        bool mc, ac;
        m_appState.UpdateSettings(modelToLoad, m_appState.GetApiUrl(), mc, ac);
    }

    SetConversationPreferredLocalModel(modelToLoad);
    UpdateModelLabel();
    m_chatDisplay->DisplaySystemMessage(
        "Loading " + ServerManager::ModelDisplayName(modelToLoad) + "...");
    m_statusDot->SetConnected(false);

    m_service.RequestLocalModel(modelToLoad, m_appState.MakeServerConfig());
}

// ═════════════════════════════════════════════════════════════════
//  SERVER EVENT HANDLERS
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::OnServerReady()
{
    if (!IsConversationTargetActive()) {
        if (m_statusDot) m_statusDot->SetConnected(false);
        return;
    }

    m_statusDot->SetConnected(true);

    // A ready event only satisfies a parked deferred-load intent when the
    // model that became ready is the deferred model.  This matters during
    // startup: the initial model may finish loading after the user has already
    // browsed to a saved conversation that wants a different model.  In that
    // case, keep the marker so the first send still loads the conversation's
    // intended model instead of silently using the startup model.
    if (!m_pendingDeferredModel.empty() &&
        path_safety::SameModelPath(m_serverManager.GetLoadedModel(),
                      m_pendingDeferredModel)) {
        m_pendingDeferredModel.clear();
    }


    std::string displayName = ServerManager::ModelDisplayName(
        m_serverManager.GetLoadedModel());

    m_chatDisplay->DisplaySystemMessage(displayName + " ready.");
    UpdateModelLabel();

    if (auto* logger = m_appState.GetLogger())
        logger->information("Server ready: " + displayName);

    // ── First-run onboarding completion ──────────────────────────
    // The flag is only true after LaunchFirstRunDownloader returned
    // a path on the first-run path. Every other "server ready" event
    // (model switch, initial load for a returning user, restart after
    // error) leaves the flag false and this block is a no-op.
    if (m_completingFirstRun) {
        m_completingFirstRun = false;
        m_appState.MarkFirstRunComplete();
    }
}

void ModelSwitcher::OnServerError(const std::string& error)
{
    if (!IsConversationTargetActive()) {
        if (m_statusDot) m_statusDot->SetConnected(false);
        return;
    }

    m_statusDot->SetConnected(false);
    m_chatDisplay->DisplaySystemMessage("Server error: " + error);

    // First-run completion is tied to the specific starter-model load that
    // followed a successful downloader run.  If that load fails, do not let
    // some later unrelated ready event mark onboarding complete.
    m_completingFirstRun = false;

    if (auto* logger = m_appState.GetLogger())
        logger->error("Server error: " + error);
}

// ═════════════════════════════════════════════════════════════════
//  MODEL PILL CLICKS
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::OnModelPillClick(wxWindow* popupParent)
{
    if (m_cb.isBusy && m_cb.isBusy()) {
        m_chatDisplay->DisplaySystemMessage(
            "Can't switch models while a response is streaming. Stop the response first.");
        return;
    }

    auto models = ServerManager::ScanModelPaths();
    if (models.empty()) {
        m_chatDisplay->DisplaySystemMessage(
            "No models found in " + ServerManager::GetModelsDir() +
            "\nDownload one through Settings, or drop a .gguf file there.");
        return;
    }

    ShowModelPickerMenu(popupParent, models);
}

void ModelSwitcher::OnModelPillRightClick(wxWindow* parent)
{
    if (m_cb.isBusy && m_cb.isBusy()) {
        m_chatDisplay->DisplaySystemMessage(
            "Can't open model settings while a response is streaming. Stop the response first.");
        return;
    }
    const std::string configuredModel = !m_pendingDeferredModel.empty()
        ? m_pendingDeferredModel
        : GetConversationModelForSave();

    ModelManagerDialog dlg(parent, &m_appState.GetTheme(),
                           m_serverManager.GetLoadedModel(),
                           configuredModel);
    dlg.ShowModal();

    // The manager dialog can delete the model that was parked as a deferred
    // load for the current conversation.  Clear that intent immediately so the
    // next Send does not try to lazy-load a file that no longer exists.
    bool changedSelection = false;
    bool deletedDeferred = false;
    if (!m_pendingDeferredModel.empty() &&
        !ModelFileExists(m_pendingDeferredModel)) {
        m_pendingDeferredModel.clear();
        changedSelection = true;
        deletedDeferred = true;
    }

    // If this conversation's preferred local model was deleted, repair only
    // this frame's preference.  Browsing or managing a conversation must not
    // rewrite the app-global active service target used by other windows.
    const std::string configuredAfter = GetConversationModelForSave();
    if (!configuredAfter.empty() && !IsRemoteModelKey(m_conversationSelectionKey) &&
        !ModelFileExists(configuredAfter)) {
        const std::string loaded = m_serverManager.GetLoadedModel();
        const std::string replacement = PickExistingModelFallback(loaded);

        if (!replacement.empty()) {
            SetConversationPreferredLocalModel(replacement);
            OnServiceStateChanged();
        } else {
            ClearConversationPreference();
            m_statusDot->SetConnected(false);
        }

        changedSelection = true;
        if (replacement.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "The selected model was deleted. Add or download a model before sending.");
        } else {
            m_chatDisplay->DisplaySystemMessage(
                "The selected model was deleted. Switched the model pill to " +
                ServerManager::ModelDisplayName(replacement) + ".");
        }
    } else if (deletedDeferred) {
        m_chatDisplay->DisplaySystemMessage(
            "The deferred model for this chat was deleted. Pick another model from the model pill before sending.");
    }

    if (changedSelection) {
        UpdateModelLabel();
        if (m_cb.updateWindowTitle) m_cb.updateWindowTitle();
    }
}

// ═════════════════════════════════════════════════════════════════
//  PICKER MENU
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::ShowModelPickerMenu(wxWindow* anchor,
                                        const std::vector<std::string>& ggufPaths)
{
    if (ggufPaths.empty()) return;

    std::vector<std::string> pickerModels = ggufPaths;
    std::unordered_map<int, size_t> menuIdMap;

    wxMenu menu;
    const std::string currentModel = !m_pendingDeferredModel.empty()
        ? m_pendingDeferredModel
        : GetConversationModelForSave();

    for (size_t i = 0; i < pickerModels.size(); ++i) {
        const std::string display = ServerManager::ModelDisplayName(pickerModels[i]);
        wxMenuItem* item = menu.AppendCheckItem(
            wxID_ANY, wxString::FromUTF8(display.c_str()));
        menuIdMap[item->GetId()] = i;
        if (path_safety::SameModelPath(pickerModels[i], currentModel)) {
            item->Check(true);
        }
    }

    // ── Remote endpoint rows (from EndpointStore) ────────────────
    // One row per configured (endpoint, model) pair, below a separator
    // that only appears if at least one remote model exists. Selecting a
    // row routes through SwitchToModel with a remote key, which branches
    // to the no-spawn remote path. Checked when its wire model is active.
    std::unordered_map<int, std::string> remoteIdMap;  // menu id -> remote key
    if (EndpointStore* endpoints = m_appState.GetEndpointStore()) {
        bool addedSeparator = false;
        for (const auto& ep : endpoints->Endpoints()) {
            for (const auto& model : ep.models) {
                if (!addedSeparator) {
                    menu.AppendSeparator();
                    addedSeparator = true;
                }
                const std::string label = ep.displayName + " - " + model.displayName;
                wxMenuItem* item = menu.AppendCheckItem(
                    wxID_ANY, wxString::FromUTF8(label.c_str()));
                remoteIdMap[item->GetId()] = RemoteModelKey(ep.id, model.id);
                if (currentModel == model.id) item->Check(true);
            }
        }
    }

    menu.Bind(wxEVT_MENU,
        [this, pickerModels, menuIdMap, remoteIdMap](wxCommandEvent& e) {
            auto rit = remoteIdMap.find(e.GetId());
            if (rit != remoteIdMap.end()) {
                SwitchToModel(rit->second);
                return;
            }
            auto it = menuIdMap.find(e.GetId());
            if (it != menuIdMap.end() && it->second < pickerModels.size()) {
                SwitchToModel(pickerModels[it->second]);
            }
        });

    // Show popup below the model pill.  No concrete panel type is required;
    // the position and size methods live on wxWindow.
    wxWindow* pill = m_modelLabel ? m_modelLabel->GetParent() : nullptr;
    if (!pill || !anchor) return;

    wxPoint pos = pill->GetScreenPosition();
    pos = anchor->ScreenToClient(pos);
    pos.y += pill->GetSize().y;
    anchor->PopupMenu(&menu, pos);
}

// ═════════════════════════════════════════════════════════════════
//  CORE SWITCH
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::SwitchToModel(const std::string& newModel)
{
    // ── Remote selection short-circuit ───────────────────────────
    // A "remote:" key is never a live llama-server process or a GGUF
    // path, so it must branch before any of the local lane logic
    // below (targetIsLive / IsProcessRunning / StartServer).
    if (IsRemoteModelKey(newModel)) {
        if (m_cb.isBusy && m_cb.isBusy()) {
            m_chatDisplay->DisplaySystemMessage(
                "Can't switch models while a response is streaming. "
                "Stop the response first.");
            return;
        }

        // ── Multi-window courtesy check (Phase 3c) ───────────────
        // Going remote retires the local server, which kills any
        // stream another window has in flight.  Confirm first.
        if (m_service.AnyOtherWindowBusy(m_parentFrame)) {
            const int r = wxMessageBox(
                "Another window is generating a response. Switching "
                "models will interrupt it.\n\nSwitch anyway?",
                "Model Switch", wxYES_NO | wxICON_WARNING, m_parentFrame);
            if (r != wxYES) return;
        }

        ActivateRemoteModel(newModel);
        return;
    }

    const std::string loadedModel = m_serverManager.GetLoadedModel();
    const bool targetIsLive =
        !newModel.empty() &&
        !loadedModel.empty() &&
        m_serverManager.IsProcessRunning() &&
        path_safety::SameModelPath(newModel, loadedModel);

    // If the requested model is already the live llama-server process, adopt
    // it without a VRAM unload/reload.  This is especially important after a
    // lazy conversation load: AppState may point at a deferred model while the
    // old server is still healthy.  Picking the old live model should simply
    // cancel the deferral and restore the ready UI state.
    if (targetIsLive) {
        const bool changed =
            !SameSelectionKey(newModel, m_conversationSelectionKey) ||
            !IsServerReady();

        SetConversationPreferredLocalModel(newModel);
        OnServiceStateChanged();

        if (changed) {
            m_chatDisplay->DisplaySystemMessage(
                ServerManager::ModelDisplayName(newModel) +
                " is already loaded.");
        }
        return;
    }

    // Same configured-model clicks are normally no-ops, but after a server
    // error the natural retry is to pick the same model again.  Because the
    // live-server fast path above already handled healthy loaded models, a
    // not-ready/dead same-model selection falls through and restarts.
    if (m_service.IsServerReady() &&
        SameSelectionKey(newModel, m_service.GetActiveSelectionKey())) {
        SetConversationPreferredLocalModel(newModel);
        OnServiceStateChanged();
        return;
    }

    if (m_cb.isBusy && m_cb.isBusy()) {
        m_chatDisplay->DisplaySystemMessage(
            "Can't switch models while a response is streaming. Stop the response first.");
        return;
    }

    // ── Multi-window courtesy check (Phase 3c) ───────────────────
    // Restarting the local server kills any stream another window
    // has in flight.  Confirm before doing that to someone else's
    // response.
    if (m_service.AnyOtherWindowBusy(m_parentFrame)) {
        const int r = wxMessageBox(
            "Another window is generating a response. Switching "
            "models will interrupt it.\n\nSwitch anyway?",
            "Model Switch", wxYES_NO | wxICON_WARNING, m_parentFrame);
        if (r != wxYES) return;
    }

    // An explicit switch supersedes any deferred-load intent from browsing.
    m_pendingDeferredModel.clear();

    // Switching models CONTINUES the current conversation with the new
    // model rather than starting a blank chat.  Each assistant turn is
    // already stamped with the model that produced it, so a single chat can
    // span models — and, importantly, a chat stays usable even after its
    // original model is deleted: just switch to one you still have and keep
    // going.  Persist current state first, then leave history, display, and
    // pending attachments intact across the swap.
    const bool continuing = !m_chatHistory->IsEmpty();
    if (continuing && m_cb.autoSave) {
        m_cb.autoSave();
    }

    SetConversationPreferredLocalModel(newModel);
    UpdateModelLabel();
    if (m_cb.updateWindowTitle) m_cb.updateWindowTitle();

    m_statusDot->SetConnected(false);
    m_chatDisplay->DisplaySystemMessage(
        "Loading " + ServerManager::ModelDisplayName(newModel) +
        (continuing ? "... new messages in this chat will use it."
                    : "..."));
    m_service.RequestLocalModel(newModel, m_appState.MakeServerConfig());

    if (auto* logger = m_appState.GetLogger())
        logger->information(
            std::string("Switched to model (") +
            (continuing ? "continuing chat" : "empty chat") + "): " + newModel);
}

// ═════════════════════════════════════════════════════════════════
//  REMOTE ACTIVATION (no server spawn)
// ═════════════════════════════════════════════════════════════════

bool ModelSwitcher::ActivateRemoteModel(const std::string& remoteKey)
{
    // Parse "remote:<endpointId>/<wireModelId>".
    std::string endpointId;
    std::string wireModel;
    if (!ParseRemoteModelKey(remoteKey, endpointId, wireModel)) {
        m_chatDisplay->DisplaySystemMessage("Invalid remote model selection.");
        return false;
    }

    EndpointStore* endpoints = m_appState.GetEndpointStore();
    SecretsStore*  secrets   = m_appState.GetSecretsStore();
    if (!endpoints || !secrets) {
        m_chatDisplay->DisplaySystemMessage("Remote endpoints are unavailable.");
        return false;
    }

    // Resolve into a ready-to-send target (pulls the API key from
    // SecretsStore on the UI thread). On failure the reason explains
    // whether the endpoint is unknown or the key is missing.
    InferenceTarget target;
    std::string reason;
    if (!endpoints->ResolveTarget(endpointId, wireModel, *secrets, target, reason)) {
        m_chatDisplay->DisplaySystemMessage(reason);
        return false;
    }

    // Continue the current conversation across the switch, like a local
    // model switch — each assistant turn is stamped with its own model.
    const bool continuing = !m_chatHistory->IsEmpty();
    if (continuing && m_cb.autoSave) m_cb.autoSave();

    // This conversation adopts the chosen remote target.  ModelService
    // retires the local process, installs the app-global target once, and
    // publishes synthesized-ready state to every frame.
    SetConversationPreferredRemoteModel(remoteKey, wireModel);
    m_service.ActivateRemoteTarget(target, remoteKey);
    m_statusDot->SetConnected(true);

    UpdateModelLabel();
    if (m_cb.updateWindowTitle) m_cb.updateWindowTitle();

    // Friendly "<model> ready (remote: <endpoint>)", using configured
    // display names where available.
    const EndpointStore::Endpoint* ep = endpoints->FindEndpoint(endpointId);
    std::string endpointDisplay = ep ? ep->displayName : endpointId;
    std::string modelDisplay    = wireModel;
    if (ep) {
        for (const auto& m : ep->models) {
            if (m.id == wireModel) { modelDisplay = m.displayName; break; }
        }
    }
    m_chatDisplay->DisplaySystemMessage(
        modelDisplay + " ready (remote: " + endpointDisplay + ")" +
        (continuing ? ". New messages in this chat will use it."
                    : "."));

    if (m_cb.onRemoteActivated) m_cb.onRemoteActivated(target.protocol);

    if (auto* logger = m_appState.GetLogger())
        logger->information("Activated remote model: " + wireModel +
                            " via " + endpointId);

    return true;
}

// ═════════════════════════════════════════════════════════════════
//  SHARED HELPER
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::UpdateModelLabel()
{
    std::string model = GetConversationModelForSave();
    const bool preferredRemote =
        IsRemoteModelKey(m_conversationSelectionKey);

    // If the stored model looks like a filesystem path (contains a
    // separator) or a .gguf filename, run it through ModelDisplayName
    // for a human-friendly short name. Otherwise it's already a bare
    // model id (e.g. an Ollama tag from the legacy path) — leave it.
    auto shortenModel = [](const std::string& m) -> std::string {
        const bool hasSeparator =
            m.find('\\') != std::string::npos ||
            m.find('/')  != std::string::npos;
        const bool endsWithGguf = EndsWithGguf(m);

        if (hasSeparator || endsWithGguf) {
            return ServerManager::ModelDisplayName(m);
        }
        return m;
    };

    std::string display;
    if (preferredRemote) {
        // Remote endpoint: show the endpoint's configured model display
        // name verbatim. The wire id (e.g. "anthropic/claude-sonnet-4.6")
        // contains a '/', so the GGUF-oriented prettifier above would
        // mangle it ("claude-sonnet-4"); skip it entirely here.
        if (EndpointStore* endpoints = m_appState.GetEndpointStore()) {
            std::string endpointId;
            std::string wireModel;
            if (ParseRemoteModelKey(m_conversationSelectionKey,
                                    endpointId, wireModel)) {
                if (const EndpointStore::Endpoint* ep =
                        endpoints->FindEndpoint(endpointId)) {
                    for (const auto& configuredModel : ep->models) {
                        if (configuredModel.id == wireModel) {
                            display = configuredModel.displayName;
                            break;
                        }
                    }
                }
            }
        }
        if (display.empty()) display = model;   // ad hoc / unknown id
    } else {
        // Take the friendly display ("gemma 4 e4b it f16") and reformat it
        // for the pill: "gemma-4-e4b-it · f16".  Hyphens replace the spaces
        // in the identity tokens, and any trailing quantization-like token
        // (f16, bf16, q4_K_M, iq4_NL, ...) gets peeled off as a " · <q>"
        // suffix.  The ▾ caret is no longer appended -- the brackets that
        // now wrap the pill (added in ui_builder.cpp) carry the affordance.
        display = FormatModelNameForPill(shortenModel(model));
    }
    const wxString displayWx = wxString::FromUTF8(display.c_str());

    if (m_modelLabel->GetLabel() == displayWx) {
        return;
    }

    m_modelLabel->SetLabel(displayWx);

    // Keep the pill compact for short names while still protecting the
    // toolbar from very long GGUF filenames. The static text is created
    // with wxST_NO_AUTORESIZE in ui_builder.cpp, so we manually size it
    // to the measured text width, capped at the old 360px limit where
    // wxST_ELLIPSIZE_MIDDLE can take over.
    int textW = 0;
    int textH = 0;
    m_modelLabel->GetTextExtent(displayWx, &textW, &textH);

    constexpr int kMinModelLabelWidth = 24;
    constexpr int kMaxModelLabelWidth = 360;
    const int labelWidth = std::max(
        kMinModelLabelWidth,
        std::min(textW + 2, kMaxModelLabelWidth));
    const int labelHeight = std::max(textH + 2,
                                     m_modelLabel->GetBestSize().GetHeight());

    m_modelLabel->SetMinSize(wxSize(labelWidth, labelHeight));
    m_modelLabel->SetSize(wxSize(labelWidth, labelHeight));
    m_modelLabel->InvalidateBestSize();

    // Full name stays available on hover even if the toolbar ellipsizes it.
    m_modelLabel->SetToolTip(displayWx);
    if (auto* parent = m_modelLabel->GetParent()) {
        parent->SetToolTip(displayWx);
        parent->Layout();

        if (auto* grandparent = parent->GetParent()) {
            grandparent->Layout();
        }
    }
}

// ═════════════════════════════════════════════════════════════════
//  FIRST-RUN ONBOARDING
// ═════════════════════════════════════════════════════════════════
//
// Two helpers used only from StartInitialServer's first-run branch.
// Kept here (rather than inline) so the branch reads as a sequence of
// high-level steps: "launch downloader → act on result."

std::string ModelSwitcher::LaunchFirstRunDownloader()
{
    if (!m_parentFrame) return "";

    // The dialog reorders Llama 3.2 3B to the top, renders a "Start here"
    // badge next to it, and auto-closes ~1s after a successful download.
    // ShowModal blocks until that happens (or the user dismisses).
    ModelDownloaderDialog dlg(m_parentFrame, &m_appState.GetTheme(),
                              /*firstRunMode=*/true);
    int result = dlg.ShowModal();

    // EndModal(wxID_OK) from the auto-close timer signals success.
    // Any other path (user clicked Close, pressed Escape, X-button)
    // returns wxID_CANCEL and we treat it as a dismissal. The explicit
    // path-nonempty check is belt-and-braces: if something upstream
    // ever returns wxID_OK without a completed download, we'd still
    // refuse to pretend onboarding succeeded.
    if (result == wxID_OK && !dlg.GetDownloadedModelPath().empty()) {
        if (auto* logger = m_appState.GetLogger())
            logger->information("First-run download succeeded: " +
                                dlg.GetDownloadedModelPath());
        return dlg.GetDownloadedModelPath();
    }

    if (auto* logger = m_appState.GetLogger())
        logger->information("First-run downloader dismissed without download");
    return "";
}

void ModelSwitcher::ShowFirstRunDismissedMessage()
{
    // Option B copy — the existing "No models found" body with one
    // added sentence pointing at the model pill. That pill is where
    // a dismissed user would otherwise have no idea to click, so
    // naming it explicitly gives them a direct way back to the
    // downloader without opening Settings.
    m_statusDot->SetConnected(false);
    m_chatDisplay->DisplaySystemMessage(
        "No model installed yet.\n\n"
        "LlamaBoss needs a local AI model before you can start chatting.\n"
        "You can return to the model downloader any time by clicking\n"
        "the model name at the top of the window, or by opening\n"
        "Settings \xe2\x86\x92 Download Models.\n\n"
        "Models are saved to:\n" + ServerManager::GetModelsDir());
}
