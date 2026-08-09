// model_service.cpp

#include "model_service.h"

#include "app_state.h"
#include "ui_event_post.h"

#include <algorithm>
#include <climits>

#include <wx/thread.h>

wxDEFINE_EVENT(wxEVT_MODEL_SERVICE_STATE_CHANGED, wxCommandEvent);

namespace {

std::string WxStringToUtf8(const wxString& value)
{
    const wxScopedCharBuffer bytes = value.ToUTF8();
    return bytes ? std::string(bytes.data()) : std::string();
}

} // namespace

ModelService::ModelService(AppState& appState)
    : m_alive(std::make_shared<std::atomic<bool>>(true))
    , m_appState(appState)
{
    Poco::Logger* logger = appState.GetLogger();
    m_serverManager = std::make_unique<ServerManager>(this, m_alive, logger);

    Bind(wxEVT_SERVER_READY, &ModelService::OnServerEvent, this);
    Bind(wxEVT_SERVER_ERROR, &ModelService::OnServerEvent, this);
}

ModelService::~ModelService()
{
    Shutdown();
}

void ModelService::AttachFrameSink(wxEvtHandler* handler,
                                   std::weak_ptr<std::atomic<bool>> aliveToken,
                                   std::function<FrameBusyKind()> busyProbe)
{
    wxASSERT(wxIsMainThread());
    if (!handler) return;
    for (auto& s : m_sinks) {
        if (s.handler == handler) {
            s.alive     = std::move(aliveToken);
            s.busyProbe = std::move(busyProbe);
            return;
        }
    }
    m_sinks.push_back({ handler, std::move(aliveToken), std::move(busyProbe) });
}

bool ModelService::AnyOtherWindowBusy(const wxEvtHandler* self) const
{
    wxASSERT(wxIsMainThread());
    for (const auto& sink : m_sinks) {
        if (sink.handler == self || !sink.busyProbe) continue;
        auto alive = sink.alive.lock();
        if (!alive || !alive->load()) continue;
        if (sink.busyProbe() != FrameBusyKind::Idle) return true;
    }
    return false;
}

bool ModelService::AnyOtherWindowBusyOnLocalServer(
    const wxEvtHandler* self) const
{
    wxASSERT(wxIsMainThread());
    for (const auto& sink : m_sinks) {
        if (sink.handler == self || !sink.busyProbe) continue;
        auto alive = sink.alive.lock();
        if (!alive || !alive->load()) continue;
        if (sink.busyProbe() == FrameBusyKind::BusyLocal) return true;
    }
    return false;
}

void ModelService::DetachFrameSink(wxEvtHandler* handler)
{
    wxASSERT(wxIsMainThread());
    m_sinks.erase(std::remove_if(m_sinks.begin(), m_sinks.end(),
                                 [handler](const FrameSink& s) {
                                     return s.handler == handler;
                                 }),
                  m_sinks.end());
}

wxCommandEvent* ModelService::MakeStateEvent(ModelServiceChange change) const
{
    auto* event = new wxCommandEvent(wxEVT_MODEL_SERVICE_STATE_CHANGED);
    event->SetInt(static_cast<int>(change));
    event->SetExtraLong(static_cast<long>(m_stateVersion));
    if (!m_lastError.empty())
        event->SetString(wxString::FromUTF8(m_lastError));
    return event;
}

void ModelService::PublishState(ModelServiceChange change,
                                const std::string& error)
{
    wxASSERT(wxIsMainThread());

    const unsigned long maxVersion = static_cast<unsigned long>(LONG_MAX);
    if (m_stateVersion >= maxVersion)
        m_stateVersion = 1;
    else
        ++m_stateVersion;

    m_lastChange = change;
    m_lastError  = error;

    for (const auto& sink : m_sinks)
        LbQueueEventIfAlive(sink.handler, sink.alive, MakeStateEvent(change));
}

void ModelService::QueueCurrentStateTo(
    wxEvtHandler* handler,
    std::weak_ptr<std::atomic<bool>> aliveToken) const
{
    wxASSERT(wxIsMainThread());
    LbQueueEventIfAlive(handler, std::move(aliveToken),
                        MakeStateEvent(ModelServiceChange::Sync));
}

bool ModelService::RequestLocalModel(const std::string& modelPath,
                                     const ServerConfig& config)
{
    wxASSERT(wxIsMainThread());
    if (modelPath.empty()) return false;

    m_serverReady       = false;
    m_activeProtocol    = ToolProtocol::Unknown;
    m_activeSelectionKey = modelPath;
    m_lastError.clear();

    // Install local intent before StartServer.  Every request resolver now
    // agrees that a local transition is in progress, rather than retaining a
    // stale remote target until the health event arrives.
    m_appState.SetModel(modelPath);
    m_appState.SetActiveTarget(
        InferenceTarget::Local(m_serverManager->GetBaseUrl(), modelPath));
    m_appState.SaveSettings();

    PublishState(ModelServiceChange::LoadingLocal);
    return m_serverManager->StartServer(modelPath, config);
}

void ModelService::ActivateRemoteTarget(const InferenceTarget& target,
                                        const std::string& selectionKey)
{
    wxASSERT(wxIsMainThread());

    // Invalidates any queued event from the old local generation before the
    // remote target is made visible to frames.
    m_serverManager->StopServer();

    m_serverReady        = true;
    m_activeProtocol     = target.protocol;
    m_activeSelectionKey = selectionKey;
    m_lastError.clear();

    m_appState.SetModel(target.modelId);
    m_appState.SetActiveTarget(target);
    m_appState.SetLastSelection(selectionKey);
    m_appState.SaveSettings();

    PublishState(ModelServiceChange::ReadyRemote);
}

void ModelService::StopLocalServer()
{
    wxASSERT(wxIsMainThread());
    m_serverManager->StopServer();
    m_serverReady = false;
    m_activeProtocol = ToolProtocol::Unknown;
    m_activeSelectionKey.clear();
    m_lastError.clear();
    PublishState(ModelServiceChange::Stopped);
}

void ModelService::OnServerEvent(wxCommandEvent& ev)
{
    wxASSERT(wxIsMainThread());

    const ServerLaunchGeneration eventGeneration =
        GetServerEventGeneration(ev);
    const ServerLaunchGeneration currentGeneration =
        m_serverManager->GetLaunchGeneration();

    if (eventGeneration == kInvalidServerLaunchGeneration ||
        eventGeneration != currentGeneration) {
        if (auto* logger = m_appState.GetLogger()) {
            const char* kind = ev.GetEventType() == wxEVT_SERVER_READY
                ? "ready" : "error";
            logger->warning(
                std::string("Dropped stale server ") + kind +
                " event: eventGeneration=" +
                std::to_string(eventGeneration) +
                " currentGeneration=" +
                std::to_string(currentGeneration));
        }
        return;
    }

    if (auto* logger = m_appState.GetLogger()) {
        const char* kind = ev.GetEventType() == wxEVT_SERVER_READY
            ? "ready" : "error";
        logger->information(
            std::string("Accepted server ") + kind +
            " event: generation=" + std::to_string(eventGeneration) +
            " elapsedMs=" +
            std::to_string(m_serverManager->GetCurrentLaunchElapsedMs()));
    }

    if (ev.GetEventType() == wxEVT_SERVER_ERROR) {
        const std::string error = WxStringToUtf8(ev.GetString());

        // Exactly one component adjudicates retry state.  A retry starts a new
        // launch generation; the original event is consumed and never reaches
        // frame handlers.
        if (m_serverManager->MaybeRetryAfterStartupFailure(error)) {
            m_serverReady = false;
            m_activeProtocol = ToolProtocol::Unknown;
            PublishState(ModelServiceChange::LoadingLocal);
            return;
        }

        m_serverReady = false;
        m_activeProtocol = ToolProtocol::Unknown;
        PublishState(ModelServiceChange::ErrorLocal, error);
    }
    else {
        // Preserve the runtime Jinja result for frame-owned protocol probing
        // before NotifyServerReady clears the retry bookkeeping.
        const bool serverJinjaEnabled =
            m_serverManager->IsCurrentServerJinjaEnabled();
        ev.SetInt(serverJinjaEnabled ? 1 : 0);

        m_serverManager->NotifyServerReady();

        const std::string loadedModel = m_serverManager->GetLoadedModel();
        m_serverReady        = true;
        m_activeProtocol     = ToolProtocol::Unknown;
        m_activeSelectionKey = loadedModel;
        m_lastError.clear();

        m_appState.SetModel(loadedModel);
        m_appState.SetActiveTarget(
            InferenceTarget::Local(m_serverManager->GetBaseUrl(), loadedModel));
        m_appState.SetLastSelection(loadedModel);
        m_appState.SaveSettings();

        PublishState(ModelServiceChange::ReadyLocal);
    }

    // Frames now receive only accepted, fully adjudicated lifecycle events.
    // Their handlers may update frame-owned UI/deferred state, but must not
    // mutate ServerManager retry state or ModelService readiness.
    for (const auto& sink : m_sinks)
        LbQueueEventIfAlive(sink.handler, sink.alive, ev.Clone());
}

InferenceTarget ModelService::ResolveTarget() const
{
    return m_appState.GetActiveTarget();
}

// ── KV slot actions, multi-window adjudicated (Phase 3c) ─────────
// See the header comment.  All three run on the main thread (send
// path, New Chat, conversation load — all UI-driven), so the
// AnyOtherWindowBusy pull is race-free against sink registration.

void ModelService::NoteSlotOwner(const wxEvtHandler* self,
                                 const std::string& conversationPath)
{
    wxASSERT(wxIsMainThread());
    if (AnyOtherWindowBusyOnLocalServer(self)) {
        // This request queues behind another window's stream inside
        // llama-server, so the slot can't verifiably end up holding
        // this conversation's KV.  Invalidate instead of claiming —
        // otherwise a later switch-away could pass ServerManager's
        // ownership check and save someone else's KV under this
        // conversation's cache name.
        m_serverManager->InvalidateSlotOwner();
        if (auto* logger = m_appState.GetLogger())
            logger->information(
                "kvslot: ownership not stamped (another window is "
                "generating); KV fast path skipped for this turn");
        return;
    }
    m_serverManager->NoteSlotOwner(conversationPath);
}

void ModelService::SaveSlotStateForConversation(
    const wxEvtHandler* self, const std::string& conversationPath)
{
    wxASSERT(wxIsMainThread());
    if (AnyOtherWindowBusyOnLocalServer(self)) {
        // Don't fire a /slots save against the slot another window's
        // stream is actively using.  ServerManager's ownership guard
        // usually skips this anyway (the other window's guarded stamp
        // invalidated ownership), but this also covers the queued-not-
        // yet-started window and keeps slot actions off a busy slot.
        if (auto* logger = m_appState.GetLogger())
            logger->information(
                "kvslot: save skipped (another window is generating)");
        return;
    }
    m_serverManager->SaveSlotStateForConversation(conversationPath);
}

void ModelService::RestoreSlotStateForConversation(
    const wxEvtHandler* self, const std::string& conversationPath)
{
    wxASSERT(wxIsMainThread());
    if (AnyOtherWindowBusyOnLocalServer(self)) {
        // Restoring would overwrite the KV the other window's stream
        // is generating into.  Skip; the load proceeds without the
        // fast path and the first send re-stamps ownership (guarded).
        if (auto* logger = m_appState.GetLogger())
            logger->information(
                "kvslot: restore skipped (another window is generating)");
        return;
    }
    m_serverManager->RestoreSlotStateForConversation(conversationPath);
}

void ModelService::Shutdown()
{
    LbMarkUiEventTargetDead(m_alive);
    if (m_serverManager)
        m_serverManager->StopServer();
}
