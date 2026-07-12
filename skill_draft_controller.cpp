#include "skill_draft_controller.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <utility>

#include "app_state.h"
#include "chat_client.h"
#include "chat_display.h"
#include "chat_history.h"
#include "conversation_controller.h"
#include "goal_verifier_support.h"
#include "lb_string_utils.h"
#include "model_switcher.h"
#include "server_manager.h"
#include "skill_authoring_support.h"
#include "skill_prompt_builder.h"

SkillDraftController::SkillDraftController(
    std::unique_ptr<ChatHistory>& chatHistory,
    ChatDisplay*                  chatDisplay,
    AppState&                     appState,
    ChatClient&                   chatClient,
    ModelSwitcher&                modelSwitcher,
    ConversationController&       convController)
    : m_chatHistory(chatHistory)
    , m_chatDisplay(chatDisplay)
    , m_appState(appState)
    , m_chatClient(chatClient)
    , m_modelSwitcher(modelSwitcher)
    , m_convController(convController)
{
}

void SkillDraftController::SetCallbacks(Callbacks cb)
{
#ifndef NDEBUG
    assert(cb.isBusy);
    assert(cb.isClosing);
    assert(cb.bumpGenerationId);
    assert(cb.setChatStateStreaming);
    assert(cb.setStreamingUi);
    assert(cb.discardPendingAssistantDelta);
    assert(cb.invalidateProjectContextCache);
    assert(cb.refreshProjectStrip);
#endif
    m_cb = std::move(cb);
}

void SkillDraftController::StartDesignSession(const std::string& skillName,
                                              const std::string& skillPath,
                                              bool requestedPythonScript)
{
    m_pending.active = true;
    m_pending.requestedPythonScript = requestedPythonScript;
    m_pending.skillName = skillName;
    m_pending.skillPath = skillPath;
    m_pending.pythonHelperPath.clear();
    m_pending.userDescription.clear();
    m_pending.conversationStartMessageIndex = m_chatHistory->GetMessageCount();
}

bool SkillDraftController::HasActiveDesignSession() const
{
    return m_pending.active;
}

bool SkillDraftController::IsDesignSessionForSkillPath(const std::string& skillPath) const
{
    return m_pending.active && m_pending.skillPath == skillPath;
}

void SkillDraftController::ClearDesignSession()
{
    m_pending = PendingSkillAuthoring{};
}

bool SkillDraftController::CancelForChatSwitch(bool notifyUser)
{
    if (!m_pending.active) return false;

    m_pending = PendingSkillAuthoring{};
    if (notifyUser && m_chatDisplay) {
        m_chatDisplay->DisplaySystemMessage(
            "Skill design session cancelled because this chat changed.");
    }
    return true;
}

std::string SkillDraftController::BuildPendingSkillAuthoringContextBlock() const
{
    if (!m_pending.active) return std::string();

    std::ostringstream p;
    p << "NEW SKILL DESIGN SESSION:\n"
      << "A reusable global LlamaBoss Skill is being designed conversationally before its files are drafted.\n"
      << "Skill name: " << m_pending.skillName << "\n"
      << "Reserved SKILL.md path: " << m_pending.skillPath << "\n"
      << "The reserved Skill folder exists, but the final Skill contract/helper files have not been drafted by the builder yet.\n"
      << "Stay in design-conversation mode. Help the user articulate the Skill's purpose and natural trigger phrases.\n"
      << "Bias toward FEWER clarifying questions, not more. When the user gives you a concrete value (a folder path, an output filename, a file-extension list, a specific URL or identifier), accept it as part of the Skill itself -- do not treat it as something to revisit later. The draft builder will encode those concrete values directly into the Skill's Steps, NOT into the 'Inputs to Ask For' section.\n"
      << "Only ask for an input value at use-time when the user explicitly says they want to choose that value each time the Skill runs. By default the Skill should hardcode every concrete value that came up in this conversation.\n"
      << "If the design conversation already used a real tool successfully (for example, an ls or notes_read whose result the user approved), the eventual Skill will mirror that exact tool sequence. Do not redesign the implementation around a different tool family just because it could also work.\n"
      << "Do not claim that the Skill has already been drafted, saved, or fully implemented until LlamaBoss later reports that the draft builder completed.\n"
      << "Do not create or edit the Skill files directly with general write/edit/overwrite tools during this design conversation. The application will run the dedicated Skill draft builder only after the user explicitly approves drafting.\n"
      << "Discuss practical implementation choices only when they are genuinely open. For straightforward local Windows/file operations, prefer existing LlamaBoss tools or approved PowerShell. Reserve Python for reusable custom logic those paths handle poorly.\n"
      << "Use tools when the user asks or when they materially help the design conversation, such as notes_read for a saved path the user references, or ls to confirm a folder layout.\n"
      << "When using tools in this Skill design conversation, follow the normal LlamaBoss tool-call protocol exactly. Do not improvise compact, partial, or alternate tool-call syntax.\n"
      << "When the design is clear enough, say this exact sentence on its own line: This design sounds ready. Say `draft this Skill` and I\xe2\x80\x99ll write the Skill files.\n"
      << "If the user wants to stop the design session, they can type cancel.\n\n";
    return p.str();
}

std::string SkillDraftController::BuildPendingSkillDesignConversationBrief() const
{
    if (!m_pending.active) return std::string();

    const auto& messages = m_chatHistory->GetMessages();
    const size_t start = std::min(
        m_pending.conversationStartMessageIndex,
        messages.size());

    std::ostringstream transcript;
    bool wroteAny = false;
    for (size_t i = start; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (!msg || !msg->has("role") || !msg->has("content"))
            continue;

        const std::string role =
            msg->getValue<std::string>("role");
        if (role != "user" && role != "assistant")
            continue;

        std::string content =
            LbTrimAscii(msg->getValue<std::string>("content"));
        if (content.empty())
            continue;

        transcript << (role == "user" ? "User" : "Assistant")
                   << ":\n"
                   << LbClipForGoalVerifier(content, 1800)
                   << "\n\n";
        wroteAny = true;
    }

    if (!wroteAny) return std::string();
    return LbClipForGoalVerifier(transcript.str(), 12000);
}

void SkillDraftController::PrepareDraftFromDesignConversationBrief(
    const std::string& authoringBrief)
{
    if (!m_pending.active) return;

    m_pending.userDescription = authoringBrief;
    // Only offer the builder a candidate Python helper path when the user picked
    // "New Skill with Python Script" at creation. Otherwise the builder's
    // "no Python helper" branch fires and no .py is emitted.
    if (m_pending.requestedPythonScript) {
        m_pending.pythonHelperPath =
            LbSkillPythonHelperPathFromContractPath(m_pending.skillPath);
    } else {
        m_pending.pythonHelperPath.clear();
    }
}

void SkillDraftController::BeginDraftBuildFromPendingDescription()
{
    if (m_cb.isClosing && m_cb.isClosing()) return;
    if (!m_pending.active) return;
    if (m_pending.skillPath.empty()) return;
    if (m_pending.userDescription.empty()) return;
    if (m_draftBuilderInFlight) return;
    if (m_cb.isBusy && m_cb.isBusy()) return;

    if (!m_modelSwitcher.IsServerReady()) {
        m_chatDisplay->DisplaySystemMessage(
            "Skill drafting was skipped because the model server is not ready. "
            "The design session is still active; say `draft this Skill` again once the model is ready.");
        return;
    }

    const std::string model = m_appState.GetModel();
    if (model.empty()) {
        m_chatDisplay->DisplaySystemMessage(
            "Skill drafting was skipped because no model is selected. "
            "Select a model, then say `draft this Skill` again.");
        return;
    }

    const SkillPromptBuilderInput skillPromptInput{
        m_pending.skillName,
        m_pending.skillPath,
        m_pending.pythonHelperPath,
        m_pending.userDescription
    };

    ChatHistory builderHistory;
    builderHistory.AddUserMessage(
        BuildSkillDraftBuilderUserPrompt(skillPromptInput));

    int ctxTokens = m_appState.GetCtxSize();
    if (ctxTokens <= 0) ctxTokens = 8192;

    std::string body = builderHistory.BuildChatRequestJson(
        model,
        true,
        BuildSkillDraftBuilderSystemPrompt(skillPromptInput),
        ctxTokens,
        "",
        false,
        true);

    m_cb.discardPendingAssistantDelta();
    m_draftBuilderInFlight = true;

    // KV fast path: this generation runs against the local slot with a
    // throwaway history, so the slot will no longer hold the active
    // conversation's state.  Forget ownership so a later switch-away
    // doesn't serialize Skill-builder KV under the conversation's
    // filename.  Harmless if SendMessage fails below.
    m_modelSwitcher.InvalidateKvSlotOwner();

    const unsigned long genId = m_cb.bumpGenerationId();
    m_cb.setChatStateStreaming();
    m_cb.setStreamingUi(true);

    m_chatDisplay->DisplaySystemMessage(
        "Skill builder is drafting the initial Skill definition and deciding whether a reusable Python helper belongs with it.");

    if (!m_chatClient.SendMessage(
            m_appState.GetActiveTarget(), body, genId)) {
        m_draftBuilderInFlight = false;
        m_cb.setStreamingUi(false);
        m_chatDisplay->DisplaySystemMessage(
            "Failed to start Skill drafting. The design session is still active; say `draft this Skill` again and I will retry.");
    }
}

bool SkillDraftController::ConsumeAssistantComplete(const std::string& fullResponse)
{
    if (!m_draftBuilderInFlight) return false;
    HandleDraftBuilderComplete(fullResponse);
    return true;
}

bool SkillDraftController::ConsumeAssistantError(const std::string& error)
{
    if (!m_draftBuilderInFlight) return false;
    HandleDraftBuilderError(error);
    return true;
}

bool SkillDraftController::HandleStopGeneration()
{
    if (!m_draftBuilderInFlight) return false;

    m_cb.discardPendingAssistantDelta();
    m_cb.bumpGenerationId();
    m_chatClient.StopGeneration();
    m_draftBuilderInFlight = false;
    m_cb.setStreamingUi(false);

    m_chatDisplay->DisplaySystemMessage(
        "Skill drafting stopped by user. The design session is still active. Say `draft this Skill` again when you are ready.");
    return true;
}

void SkillDraftController::HandleDraftBuilderComplete(const std::string& builderResponse)
{
    m_draftBuilderInFlight = false;
    m_chatClient.ResetStreamingState();
    m_cb.discardPendingAssistantDelta();
    m_cb.setStreamingUi(false);

    if (!m_pending.active || m_pending.skillPath.empty()) {
        return;
    }

    LbSkillDraftBuilderPayload payload =
        LbParseSkillDraftBuilderPayload(builderResponse);
    std::string markdown = std::move(payload.markdown);
    std::string pythonHelperSource = std::move(payload.pythonHelperSource);

    const bool draftRequestsPythonHelper =
        LbSkillMarkdownRequestsPythonHelper(markdown);
    const bool generatedPythonHelper = !pythonHelperSource.empty();

    if (payload.malformedHelperBlock ||
        !LbLooksLikeSkillMarkdown(markdown) ||
        draftRequestsPythonHelper != generatedPythonHelper ||
        (generatedPythonHelper && m_pending.pythonHelperPath.empty())) {
        m_chatDisplay->DisplaySystemMessage(
            "The Skill builder did not return a complete, internally consistent Skill draft. "
            "The design session is still active; refine it if needed, then say `draft this Skill` again.");
        if (!m_chatHistory->IsEmpty())
            m_convController.AutoSaveConversation();
        return;
    }

    if (markdown.empty() || markdown.back() != '\n')
        markdown.push_back('\n');
    if (generatedPythonHelper &&
        (pythonHelperSource.empty() || pythonHelperSource.back() != '\n')) {
        pythonHelperSource.push_back('\n');
    }

    if (!LbWriteUtf8TextFile(m_pending.skillPath, markdown)) {
        m_chatDisplay->DisplaySystemMessage(
            "The Skill draft was generated, but LlamaBoss could not save it to disk. "
            "The starter SKILL.md file was left as-is.");
        if (!m_chatHistory->IsEmpty())
            m_convController.AutoSaveConversation();
        return;
    }

    const std::string savedPath = m_pending.skillPath;
    const std::string pythonHelperPath = generatedPythonHelper
        ? m_pending.pythonHelperPath
        : std::string();

    bool pythonHelperCreated = false;
    bool pythonHelperWriteFailed = false;
    if (generatedPythonHelper) {
        pythonHelperCreated = LbWriteUtf8TextFile(
            pythonHelperPath,
            pythonHelperSource);
        pythonHelperWriteFailed = !pythonHelperCreated;
    }

    m_pending = PendingSkillAuthoring{};
    m_cb.invalidateProjectContextCache();
    m_cb.refreshProjectStrip();

    std::ostringstream msg;
    msg << "Skill definition drafted and saved:\n"
        << savedPath;
    if (pythonHelperCreated) {
        msg << "\n\nThe Skill builder determined that reusable Python code would help here, so I created a Python helper script for this Skill:\n"
            << pythonHelperPath;
    } else if (pythonHelperWriteFailed) {
        msg << "\n\nThe Skill builder drafted a Python helper for this Skill, but LlamaBoss could not save the `.py` file. "
            << "The SKILL.md draft was still saved, so review the Skill before trying to run it.";
    } else {
        msg << "\n\nNo separate Python helper was created. This Skill draft relies on LlamaBoss's available tools or approved command execution when it runs.";
    }
    msg << "\n\nYou can now ask me to use this Skill whenever you are ready, or open it from the Skills menu to review it. I will not run it until you ask.";

    const std::string completion = msg.str();
    const std::string model = m_appState.GetModel();
    m_chatDisplay->DisplayAssistantMessage(
        ServerManager::ModelDisplayName(model),
        completion,
        m_appState.GetTheme().chatAssistant);
    m_chatHistory->AddAssistantMessage(completion, model);

    if (!m_chatHistory->IsEmpty())
        m_convController.AutoSaveConversation();
}

void SkillDraftController::HandleDraftBuilderError(const std::string& error)
{
    m_draftBuilderInFlight = false;
    m_cb.discardPendingAssistantDelta();
    m_chatClient.ResetStreamingState();
    m_cb.setStreamingUi(false);

    std::string msg =
        "Skill drafting failed. The design session is still active; refine it if needed, then say `draft this Skill` again.";
    const std::string trimmed = LbTrimAscii(error);
    if (!trimmed.empty())
        msg += " " + trimmed.substr(0, std::min<size_t>(trimmed.size(), 240));

    m_chatDisplay->DisplaySystemMessage(msg);
    if (!m_chatHistory->IsEmpty())
        m_convController.AutoSaveConversation();
}
