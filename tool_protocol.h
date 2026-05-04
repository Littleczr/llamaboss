// tool_protocol.h
//
// Phase 3b: per-model tool-call protocol detection.
//
// llama-server can serve OpenAI-style native function calling for
// models whose chat template carries tool markers (Hermes 2 Pro,
// Qwen 2.5, Llama 3.x, etc.) when started with --jinja.  For models
// whose templates lack those markers, we fall back to LlamaBoss's
// existing XML <tool_call> protocol.
//
// This module probes a freshly-loaded model and decides which path
// it supports.  The probe runs in three steps, each gating the next:
//
//   0. server --jinja   — if the running llama-server process was not
//                          launched with --jinja, native tool calling is
//                          unavailable for this session and we return XML
//                          without reading/writing the persistent cache.
//
//   1. GET /props        — fetch the chat templates the server has
//                          compiled from the active model.  If this
//                          fails (network/parse), conclude XML.
//
//   2. Heuristic scan    — search both chat_template strings for
//                          tool markers ("tool_call", "tool_calls",
//                          "<|python_tag|>", etc.).  No markers =>
//                          XML; we don't even attempt the smoke
//                          test.
//
//   3. Smoke test        — POST a real /v1/chat/completions request
//                          with a one-tool toolset (the no-side-
//                          effect `pwd` tool) and tool_choice
//                          required.  If the response contains a
//                          well-formed tool_calls array naming
//                          `pwd`, native is confirmed.  Anything
//                          else (text-only response, error, wrong
//                          shape) => XML.
//
// The result is cached per (model, mmproj) pair only for --jinja-enabled
// server sessions, so a no-jinja fallback can never poison the native/XML
// cache for later successful --jinja launches.  See
// LoadCachedProtocol / SaveProtocolToCache below.
//
// All steps are blocking HTTP — the probe MUST run on a background
// thread (ProtocolProbeWorker) and post wxEVT_TOOL_PROTOCOL_DETECTED
// to the UI thread when complete.
//
#pragma once

#include <wx/event.h>
#include <wx/thread.h>

#include <atomic>
#include <memory>
#include <string>

// ─── Result enum ────────────────────────────────────────────────
enum class ToolProtocol {
    Unknown,    // Probe hasn't completed yet (or failed during probe)
    Xml,        // LlamaBoss's existing <tool_call>...</tool_call> path
    Native,     // OpenAI-style structured tool_calls via /v1/chat/completions
};

// String form for logging / display ("native" / "xml" / "unknown").
const char* ToolProtocolName(ToolProtocol p);

// ─── Result struct ──────────────────────────────────────────────
// Carried on the wxThreadEvent posted to the UI when the probe
// finishes.  `cacheHit == true` when no work was done because a
// fresh cache entry already covered this (model, mmproj).
struct ProtocolDetectionResult {
    ToolProtocol protocol = ToolProtocol::Unknown;
    bool         cacheHit = false;
    std::string  modelPath;   // echoed so the UI can ignore stale results
    std::string  reason;      // one-line diagnostic for the log
};

// ─── Detection event ────────────────────────────────────────────
wxDECLARE_EVENT(wxEVT_TOOL_PROTOCOL_DETECTED, wxThreadEvent);

// ─── Cache (wxFileConfig-backed, persistent across launches) ───
//
// Key:   SHA-1 hex of "<modelPath>\n<mmprojPath>"  (mmprojPath may
//        be empty for text-only models).  Stable across runs as
//        long as the model file isn't moved.
// Value: a single string in the format
//          "v1|<protocol>|<unix_seconds_when_detected>"
//        where <protocol> is "native" or "xml".  The leading "v1"
//        lets future schema bumps invalidate older entries without
//        a manual cache wipe.
//
// Both functions are thread-safe in the sense that wxFileConfig is
// process-singleton-friendly; they may be called from the worker
// thread.

bool LoadCachedProtocol(const std::string& modelPath,
                        const std::string& mmprojPath,
                        ToolProtocol&      out);

void SaveProtocolToCache(const std::string& modelPath,
                         const std::string& mmprojPath,
                         ToolProtocol       protocol);

// ─── Worker thread ──────────────────────────────────────────────
// Spawns, runs the three-step probe, posts wxEVT_TOOL_PROTOCOL_DETECTED
// to `handler`, and self-deletes (wxTHREAD_DETACHED).  `aliveToken`
// guards the post so a frame closing during probe doesn't see a
// dangling event.
class ProtocolProbeWorker : public wxThread
{
public:
    ProtocolProbeWorker(wxEvtHandler* handler,
                        std::weak_ptr<std::atomic<bool>> aliveToken,
                        std::string baseUrl,
                        std::string modelPath,
                        std::string mmprojPath);

protected:
    ExitCode Entry() override;

private:
    wxEvtHandler*                    m_handler;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    std::string                      m_baseUrl;
    std::string                      m_modelPath;
    std::string                      m_mmprojPath;

    bool SafePost(wxThreadEvent* ev);
};

// ─── Convenience: kick off detection ────────────────────────────
// Call from MyFrame::OnServerReady.  Reads the cache first; if a
// fresh entry covers (modelPath, mmprojPath), posts an immediate
// cacheHit=true event without spawning a thread.  Otherwise spawns
// a ProtocolProbeWorker.  Returns true if a probe was started or a
// cached event was posted.
bool KickOffToolProtocolDetection(
    wxEvtHandler*                    handler,
    std::weak_ptr<std::atomic<bool>> aliveToken,
    const std::string&               baseUrl,
    const std::string&               modelPath,
    const std::string&               mmprojPath,
    bool                             serverJinjaEnabled);
