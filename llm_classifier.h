// llm_classifier.h
// Uses a local Ollama model to classify natural language messages
// as workspace actions or regular chat.
//
// This is Layer 2 of the classification pipeline:
//   Layer 1: ClassifyAction()    — deterministic slash-command check (instant)
//   Layer 2: ClassifyWithLLM()   — natural language understanding (one inference call)
//
// Layer 2 only runs when:
//   - workspace is enabled
//   - Layer 1 returned Chat (no slash command matched)
//
// The classifier sends a short non-streaming request to the same
// Ollama instance the app already uses, asking the model to return
// a JSON object identifying the user's intent.

#pragma once

#include <string>

// ── Classification result from the LLM ───────────────────────────
struct LLMClassifyResult
{
    bool        success;        // true if the LLM call completed and parsed
    std::string errorMessage;   // non-empty if success == false

    std::string action;         // "chat", "save_chat", "workspace_search"
    std::string topic;          // extracted subject (e.g. "employee hours")
    std::string filenameHint;   // suggested filename (e.g. "employee_hours")

    std::string rawResponse;    // full model output (for debugging/logging)
};

// ── Classify a message using the local Ollama model ──────────────
//
//   message        : the user's natural language input
//   apiUrl         : Ollama API base URL (e.g. "http://127.0.0.1:11434")
//   model          : model name to use for classification (e.g. "gemma4:e4b-it-bf16")
//   timeoutSeconds : max wait for the classification call (default 5)
//
// Returns a LLMClassifyResult.  On any failure (timeout, parse error,
// Ollama unreachable), returns success=false with action="chat" so
// the message falls through to normal chat.  Never blocks indefinitely.
//
// Note: the first call may take 5-10 seconds if the model needs to
// load into VRAM.  Subsequent calls are typically 300-600ms.
LLMClassifyResult ClassifyWithLLM(
    const std::string& message,
    const std::string& apiUrl,
    const std::string& model,
    int timeoutSeconds = 15);
