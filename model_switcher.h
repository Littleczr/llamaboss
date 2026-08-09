// model_switcher.h
// Thin coordinator for model scanning, picker menu, switching,
// and server lifecycle events.  Extracted from MyFrame.
#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <utility>

#include "tool_protocol.h"     // ToolProtocol (for the remote-activated callback)
#include "inference_target.h"  // InferenceTarget (ResolveTargetForConversation)

// Forward declarations
class AppState;
class ModelService;
class ServerManager;
class ChatDisplay;
class ChatHistory;
class AttachmentManager;
class StatusDot;
struct ThemeData;

class ModelSwitcher
{
public:
    // Callbacks wired by MyFrame after construction.
    struct Callbacks {
        std::function<bool()>  isBusy;
        std::function<void()>  autoSave;
        std::function<void()>  updateWindowTitle;

        // Invoked after a remote model is activated (no server spawn).
        // MyFrame uses it to apply the frame-owned ready effects it
        // normally does in OnServerReady: apply the endpoint's configured
        // tool protocol and refresh the protocol chip.
        std::function<void(ToolProtocol)> onRemoteActivated;
    };

    // Takes both the service and its ServerManager (the latter is
    // always service.Server()) so the dozens of existing
    // m_serverManager call sites stay untouched; the service handle
    // is for the shared readiness flag.
    ModelSwitcher(ModelService& service,
                  AppState& appState,
                  ServerManager& serverManager,
                  ChatDisplay* chatDisplay,
                  std::unique_ptr<ChatHistory>& chatHistory,
                  AttachmentManager& attachments,
                  StatusDot* statusDot,
                  wxStaticText* modelLabel,
                  wxWindow* parentFrame);

    void SetCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // ── Server bootstrap ──────────────────────────────────────────
    void StartInitialServer();

    // ── Quick switch (model pill menu) ────────────────────────────
    void OnModelPillClick(wxWindow* popupParent);
    void OnModelPillRightClick(wxWindow* parent);

    // ── Core switch ──────────────────────────────────────────────
    void SwitchToModel(const std::string& newModel);

    // ── Server/service event handlers (called from MyFrame) ──────
    void OnServerReady();
    void OnServerError(const std::string& error);
    void OnServiceStateChanged();

    // ── Per-conversation model preference ────────────────────────
    // A saved conversation's preferred model belongs to this frame, not to
    // the app-global service.  selectionKey is a local GGUF path or a
    // "remote:<endpoint>/<model>" key; modelForSave is the value written to
    // the conversation file (local path or remote wire model id).
    void SetConversationPreferredLocalModel(const std::string& modelPath);
    void SetConversationPreferredRemoteModel(const std::string& selectionKey,
                                              const std::string& wireModel);
    // Resolve the model value persisted in a conversation. Local GGUF paths
    // remain paths; a remote wire-model id is mapped to the active/unique
    // configured endpoint without changing the shared target.
    bool SetConversationPreferredSavedModel(const std::string& savedModel);
    void AdoptActiveTargetForConversation();
    void ClearConversationPreference();
    std::string GetConversationModelForSave() const;
    bool IsConversationTargetActive() const;
    bool NeedsRemoteActivationForConversation() const;
    bool ActivateConversationPreferredRemoteTarget();

    // True when this frame's session (its conversation-preferred
    // model, falling back to the app-global selection) runs against
    // the shared llama-server rather than a remote endpoint.  Powers
    // this frame's FrameBusyKind probe.
    bool SessionUsesLocalServer() const;

    // Where does THIS frame's next request go?  Remote conversations
    // resolve their endpoint from EndpointStore on every send WITHOUT
    // touching the app-global target — pinning an in-flight agent
    // loop / goal run / skill draft to its own model even if another
    // window flips the global target mid-run.  Local conversations
    // (and frames with no preference yet) return the global target
    // unchanged.  This is the per-frame override seam the
    // ModelService::ResolveTarget comment anticipated.  Main-thread
    // only (reads SecretsStore).
    InferenceTarget ResolveTargetForConversation();

    // ── Shared helper ────────────────────────────────────────────
    void UpdateModelLabel();

    // Ready from this frame's perspective: the shared target is usable AND
    // it is the target preferred by this conversation.
    bool IsServerReady() const;

    // ── KV slot ownership forwarding ─────────────────────────────
    // GoalController and SkillDraftController hold a ModelSwitcher&
    // but not a ServerManager&; these thin pass-throughs let them
    // participate in KV slot ownership tracking without growing a
    // new dependency.  See ServerManager for semantics.
    //
    // InvalidateKvSlotOwner: call at the dispatch of a generation
    // that runs against the local slot with a throwaway history
    // (goal contract builder, goal verifier, Skill draft builder) —
    // the slot is about to hold state belonging to no conversation.
    //
    // NoteKvSlotOwner: call at the dispatch of a generation that
    // extends a real conversation (goal auto-continuation) —
    // mirrors the main chat send path's NoteSlotOwner stamp.
    void InvalidateKvSlotOwner();
    void NoteKvSlotOwner(const std::string& conversationPath);
    void MarkServerNotReady();

    // Deferred-model slot used by lazy conversation loading.
    const std::string& PendingDeferredModel() const
    {
        return m_pendingDeferredModel;
    }

    void SetPendingDeferredModel(std::string modelPath)
    {
        m_pendingDeferredModel = std::move(modelPath);
    }

    std::string TakePendingDeferredModel()
    {
        std::string modelPath = std::move(m_pendingDeferredModel);
        m_pendingDeferredModel.clear();
        return modelPath;
    }

    void ClearPendingDeferredModel()
    {
        m_pendingDeferredModel.clear();
    }

private:
    // Lazy model loading.  When the user opens a saved conversation whose
    // model isn't the one the running server has, we DON'T reload the model
    // immediately — browsing between different-model conversations would
    // otherwise force a full VRAM swap on every click.  Instead the
    // conversation's .gguf path is parked here and the first prompt the user
    // sends triggers the load (MyFrame::OnSendMessage).  Empty = nothing
    // deferred.  Set by ConversationController::LoadConversationFromPath;
    // cleared by explicit switches/reloads or by the send path when it
    // consumes the deferred intent.
    std::string m_pendingDeferredModel;

    // ── Remote endpoint activation ───────────────────────────────
    // Handles a remote model selection (a "remote:<endpoint>/<model>"
    // key). Resolves the endpoint + API key, then asks ModelService to
    // install the non-managed target and synthesize ready state without
    // spawning or health-checking a llama-server.
    // Returns true if the remote model was activated; false if the
    // endpoint is unknown or has no configured API key (so startup can
    // fall back to a local model).
    bool ActivateRemoteModel(const std::string& remoteKey);

    void ShowModelPickerMenu(wxWindow* anchor,
                             const std::vector<std::string>& ggufPaths);

    // ── First-run onboarding ─────────────────────────────────────
    // Opens the model downloader in first-run mode, blocks until the
    // user either downloads a model (dialog auto-closes) or dismisses.
    // Returns the full path of the downloaded .gguf on success, or
    // empty string on dismiss.
    std::string LaunchFirstRunDownloader();

    // System message shown when the user dismisses the first-run
    // downloader without completing a download. Points them at the
    // model pill so they can reopen the downloader without hunting.
    void ShowFirstRunDismissedMessage();

    // Shared target/readiness truth lives on ModelService.  This per-frame
    // coordinator only projects that snapshot through the current
    // conversation's preferred target.
    ModelService&                   m_service;

    AppState&                       m_appState;
    ServerManager&                  m_serverManager;
    ChatDisplay*                    m_chatDisplay;
    std::unique_ptr<ChatHistory>&   m_chatHistory;
    AttachmentManager&              m_attachments;
    StatusDot*                      m_statusDot;
    wxStaticText*                   m_modelLabel;
    wxWindow*                       m_parentFrame;  // Owner for modal dialogs

    Callbacks                       m_cb;

    // Per-frame conversation preference. Empty means "adopt the current
    // service target" (fresh window/chat).  Keeping this out of AppState
    // prevents opening a saved chat in one window from changing every other
    // window's active model metadata or readiness.
    std::string m_conversationSelectionKey;
    std::string m_conversationModelForSave;

    // True between "first-run download succeeded, server is loading"
    // and "server became ready." Gates the one-shot MarkFirstRunComplete
    // call in OnServerReady so routine ready events don't touch the flag.
    bool m_completingFirstRun = false;
};
