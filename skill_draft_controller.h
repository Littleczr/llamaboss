// skill_draft_controller.h — hidden Skill draft builder orchestration.
//
// Owns the transient Skill authoring/design session state that previously
// lived inline in MyFrame, plus the hidden Skill Draft Builder control turn.
// The frame still owns wx UI widgets and input clearing; this controller owns
// the state, prompt handoff, hidden streaming lifecycle, save-to-disk result,
// and stop/error handling for the builder turn.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

class AppState;
class ChatClient;
class ChatDisplay;
class ChatHistory;
class ConversationController;
class ModelSwitcher;

class SkillDraftController {
public:
    struct Callbacks {
        // m_chatState != ChatState::Idle.  Gates draft-builder starts.
        std::function<bool()> isBusy;

        // m_isClosing.  Checked before starting hidden turns.
        std::function<bool()> isClosing;

        // ++m_generationId; returns the new id.  Used to start hidden turns
        // and to orphan late deltas when the user stops a hidden turn.
        std::function<unsigned long()> bumpGenerationId;

        // m_chatState = ChatState::Streaming.  Paired with setStreamingUi(true)
        // when the hidden builder turn starts.
        std::function<void()> setChatStateStreaming;

        // SetStreamingState(bool) — stop-button / input-lock UI.
        std::function<void(bool)> setStreamingUi;

        // DiscardPendingAssistantDelta().  Hidden builder deltas are never
        // transcript-visible, so buffered text must be dropped before start/end.
        std::function<void()> discardPendingAssistantDelta;

        // InvalidateProjectContextCache().  A saved Skill changes the global
        // Skill inventory, which is part of prompt/project context.
        std::function<void()> invalidateProjectContextCache;

        // RefreshProjectStrip().  The strip includes Skill/Project affordances.
        std::function<void()> refreshProjectStrip;
    };

    SkillDraftController(std::unique_ptr<ChatHistory>& chatHistory,
                         ChatDisplay*                  chatDisplay,
                         AppState&                     appState,
                         ChatClient&                   chatClient,
                         ModelSwitcher&                modelSwitcher,
                         ConversationController&       convController);

    void SetCallbacks(Callbacks cb);

    // ── Design-session state ───────────────────────────────────────
    void StartDesignSession(const std::string& skillName,
                            const std::string& skillPath,
                            bool requestedPythonScript);
    bool HasActiveDesignSession() const;
    bool IsDesignSessionForSkillPath(const std::string& skillPath) const;
    void ClearDesignSession();
    bool CancelForChatSwitch(bool notifyUser);

    // System-prompt block used while the user/model are designing a new Skill.
    std::string BuildPendingSkillAuthoringContextBlock() const;

    // Transcript slice handed to the hidden builder after the user says
    // "draft this Skill".
    std::string BuildPendingSkillDesignConversationBrief() const;

    // Stores the brief and derives the optional helper path if the user created
    // the Skill via "New Skill with Python Script".
    void PrepareDraftFromDesignConversationBrief(const std::string& authoringBrief);

    // ── Hidden builder turn ────────────────────────────────────────
    void BeginDraftBuildFromPendingDescription();
    bool ConsumeAssistantComplete(const std::string& fullResponse);
    bool ConsumeAssistantError(const std::string& error);
    bool AnyHiddenTurnInFlight() const { return m_draftBuilderInFlight; }
    bool HandleStopGeneration();

private:
    struct PendingSkillAuthoring {
        bool        active = false;
        bool        requestedPythonScript = false;
        std::string skillName;
        std::string skillPath;
        std::string pythonHelperPath;
        std::string userDescription;
        size_t      conversationStartMessageIndex = 0;
    };

    void HandleDraftBuilderComplete(const std::string& builderResponse);
    void HandleDraftBuilderError(const std::string& error);

    std::unique_ptr<ChatHistory>& m_chatHistory;
    ChatDisplay*                  m_chatDisplay;
    AppState&                     m_appState;
    ChatClient&                   m_chatClient;
    ModelSwitcher&                m_modelSwitcher;
    ConversationController&       m_convController;
    Callbacks                     m_cb;

    PendingSkillAuthoring m_pending;
    bool                  m_draftBuilderInFlight = false;
};
