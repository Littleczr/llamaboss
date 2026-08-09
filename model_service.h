// model_service.h
#pragma once

// ── ModelService ─────────────────────────────────────────────────
// App-level home for the local-model/inference machinery (multi-
// window plan, shared-lifecycle boundary through Finding 4).
//
// Ownership: MyApp owns the service; llama-server's lifetime is tied
// to the application, not to any window.  Constructed in
// MyApp::OnInit right after AppState (whose logger it borrows),
// before any frame exists; frames borrow it via wxGetApp().
//
// Event routing: ServerManager posts generation-stamped lifecycle events to
// this service.  ModelService validates/adjudicates each event once, owns
// readiness and retry mutation, then rebroadcasts accepted UI notifications
// under each frame's lifetime token.  A separate versioned state event covers
// local loading, remote activation, and late-joining frames.
//
// Threading: ServerManager's worker threads post here via
// LbQueueEventIfAlive, so OnServerEvent runs on the main thread;
// Attach/Detach are main-thread-only (frame ctor / OnClose / dtor),
// so m_sinks needs no locking.
//
// Conversation model preference remains per-frame; AppState and the service
// describe only the active app-wide inference target.  Remaining multi-window
// work includes request serialization and stronger sink-registry cleanup.
// NOTE: ModelSwitcher deliberately stays per-frame — it is a view
// coordinator (pill, picker, dialogs), not a service.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wx/event.h>

#include <Poco/Logger.h>

#include "inference_target.h"
#include "server_manager.h"

class AppState;

// Application-level model target/readiness notification.  Unlike the raw
// server ready/error events, this also covers non-process transitions such as
// remote activation and the beginning of a local-model switch.  Handlers read
// the authoritative snapshot back from ModelService; ExtraLong carries the
// state version so queued superseded notifications can be discarded.
enum class ModelServiceChange : int
{
    Sync = 0,
    LoadingLocal,
    ReadyLocal,
    ErrorLocal,
    ReadyRemote,
    Stopped
};

// ── Frame busy classification (Phase 3c refinement) ──────────────
// What an attached frame's in-flight work depends on.  BusyLocal:
// the session runs against the shared llama-server, so stopping or
// restarting the process breaks it.  BusyRemote: the session is
// pinned to a remote endpoint (ModelSwitcher::
// ResolveTargetForConversation), so the local server's lifecycle is
// irrelevant to it.
enum class FrameBusyKind { Idle, BusyLocal, BusyRemote };

wxDECLARE_EVENT(wxEVT_MODEL_SERVICE_STATE_CHANGED, wxCommandEvent);

class ModelService : public wxEvtHandler
{
public:
    // Borrows the app-owned AppState (constructed just before the
    // service in MyApp::OnInit) for the logger and for target
    // resolution.  Lifetime is safe by MyApp member order: AppState
    // outlives the service.
    explicit ModelService(AppState& appState);
    ~ModelService() override;

    ModelService(const ModelService&)            = delete;
    ModelService& operator=(const ModelService&) = delete;

    ServerManager&       Server()       { return *m_serverManager; }
    const ServerManager& Server() const { return *m_serverManager; }

    // ── Authoritative target/readiness snapshot ──────────────────
    // Only ModelService mutates these fields.  Frames consume them and update
    // frame-owned UI/protocol/deferred state; they never adjudicate a server
    // lifecycle event or write shared readiness themselves.
    bool IsServerReady() const { return m_serverReady; }
    unsigned long GetStateVersion() const { return m_stateVersion; }
    ModelServiceChange GetLastChange() const { return m_lastChange; }
    ToolProtocol GetActiveProtocol() const { return m_activeProtocol; }
    const std::string& GetActiveSelectionKey() const
    {
        return m_activeSelectionKey;
    }
    const std::string& GetLastError() const { return m_lastError; }

    // Begin a managed-local transition.  This is the only public path that
    // installs local target intent, marks the service not ready, broadcasts
    // loading state, and starts llama-server.
    bool RequestLocalModel(const std::string& modelPath,
                           const ServerConfig& config);

    // Install a fully resolved remote target, retire any local process, and
    // publish synthesized-ready state to every attached frame.
    void ActivateRemoteTarget(const InferenceTarget& target,
                              const std::string& selectionKey);

    // Explicit non-shutdown stop used by Settings/folder changes.
    void StopLocalServer();

    // Queue the current authoritative snapshot to one frame (late joiners).
    void QueueCurrentStateTo(wxEvtHandler* handler,
                             std::weak_ptr<std::atomic<bool>> aliveToken) const;

    // ── One-shot initial-server bootstrap (Chunk D) ──────────────
    // StartInitialServer must run once per application, not once per
    // window: the first frame consumes this and boots the model the
    // user last had loaded; every later frame (Phase 3's New Window)
    // sees false and simply joins the already-running server.
    bool ConsumeInitialBootstrap()
    {
        const bool first = !m_initialBootstrapDone;
        m_initialBootstrapDone = true;
        return first;
    }

    // ── Target resolution seam (Chunk D) ─────────────────────────
    // "Which endpoint/model does this frame's next request go to?"
    // Today: the global active target on AppState, for every frame.
    // Later (per-window remote models): this call grows a per-frame
    // override lookup, and no call site changes.
    InferenceTarget ResolveTarget() const;

    // ── Frame sink registry ──────────────────────────────────────
    // Rebroadcast targets for server lifecycle events.  Each frame
    // attaches itself with its own alive token (the same m_alive it
    // already uses for every other worker-thread post), so a closing
    // frame stops receiving events the instant it marks itself dead
    // — the registry entry is then just a harmless stale row until
    // DetachFrameSink removes it.  Attach is idempotent per handler;
    // Detach of an unknown handler is a no-op.
    // busyProbe (optional): "is this window mid-generation right
    // now, and does that work depend on the local llama-server or a
    // remote endpoint?" — main-thread pull, used by the
    // AnyOtherWindowBusy* queries below.
    void AttachFrameSink(wxEvtHandler* handler,
                         std::weak_ptr<std::atomic<bool>> aliveToken,
                         std::function<FrameBusyKind()> busyProbe = {});
    void DetachFrameSink(wxEvtHandler* handler);

    // True if any attached window other than |self| is currently
    // generating.  Powers two Phase 3c behaviors: the "your request
    // will queue behind another window" notice at send time (llama-
    // server runs a single slot, so a second request waits silently
    // inside the server otherwise), and the "switching models will
    // interrupt window X's stream" confirmation.  Pull-based on
    // purpose: every frame already knows its own IsBusy(), so there
    // is no generation-lifecycle bookkeeping to get wrong.
    bool AnyOtherWindowBusy(const wxEvtHandler* self) const;

    // Like AnyOtherWindowBusy, but true only when the busy window's
    // session depends on the shared llama-server.  Powers the model
    // switch / settings / deferred-load confirmations and the KV
    // slot guards: a window busy on a REMOTE endpoint is pinned to
    // that endpoint (ResolveTargetForConversation) and is not
    // interrupted by a local server stop/restart, so it must not
    // trigger those.
    bool AnyOtherWindowBusyOnLocalServer(const wxEvtHandler* self) const;

    // ── KV slot actions, multi-window adjudicated (Phase 3c) ─────
    // The KV fast path was designed single-window: ownership is
    // stamped at send time, and save/restore fire /slots actions
    // against the single server slot.  With another window mid-
    // generation, a stamp can claim KV the slot doesn't hold yet
    // (this request queues behind the other stream inside llama-
    // server), a save can serialize the wrong conversation's KV
    // under this one's cache name, and a restore can clobber the
    // slot that stream is actively using.  These wrappers consult
    // AnyOtherWindowBusy and degrade to "no KV fast path" under
    // contention — correct by omission, the same philosophy as
    // ServerManager's own ownership guard.  |self| is the calling
    // frame (the one whose generation/switch this action belongs
    // to), same identity every frame already passes to
    // AnyOtherWindowBusy.  Call sites must use these instead of
    // reaching through Server() directly.
    void NoteSlotOwner(const wxEvtHandler* self,
                       const std::string& conversationPath);
    void SaveSlotStateForConversation(const wxEvtHandler* self,
                                      const std::string& conversationPath);
    void RestoreSlotStateForConversation(const wxEvtHandler* self,
                                         const std::string& conversationPath);

    // Number of currently attached windows.  MyFrame::OnClose detaches
    // itself first and then consults this: 0 means "I was the last
    // window", which is the trigger for stopping llama-server while a
    // frame and logger still exist (MyApp::OnExit remains the backstop).
    size_t AttachedFrameCount() const { return m_sinks.size(); }

    // Explicit, idempotent teardown: blocks any further posts from
    // ServerManager's workers (marks the service token dead), then
    // stops llama-server.  MyFrame::OnClose calls StopLocalServer()
    // for deterministic VRAM release while its logger is alive;
    // MyApp::OnExit calls Shutdown() as
    // the backstop.  ServerManager's dtor also stops the server, so
    // triple coverage is harmless.
    void Shutdown();

private:
    void OnServerEvent(wxCommandEvent& ev);
    void PublishState(ModelServiceChange change,
                      const std::string& error = std::string());
    wxCommandEvent* MakeStateEvent(ModelServiceChange change) const;

    struct FrameSink {
        wxEvtHandler*                    handler = nullptr;
        std::weak_ptr<std::atomic<bool>> alive;
        std::function<FrameBusyKind()>   busyProbe;
    };

    // App-lifetime alive token for posts targeting the service
    // itself.  Distinct from any frame's token on purpose: the
    // service outlives every window.
    std::shared_ptr<std::atomic<bool>> m_alive;

    AppState& m_appState;
    bool m_serverReady          = false;
    bool m_initialBootstrapDone = false;
    unsigned long m_stateVersion = 0;
    ModelServiceChange m_lastChange = ModelServiceChange::Sync;
    ToolProtocol m_activeProtocol = ToolProtocol::Unknown;
    std::string m_activeSelectionKey;
    std::string m_lastError;

    std::vector<FrameSink>         m_sinks;
    std::unique_ptr<ServerManager> m_serverManager;
};
