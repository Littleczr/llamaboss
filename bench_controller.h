// bench_controller.h
//
// Runs /bench: sends a fixed prompt N times to the current conversation's
// model and reports per-run and median speeds, split by cold / warm prompt
// cache.  The logic that doesn't need I/O (arguments, prompt, medians, TSV
// columns) lives in bench_stats.h.
//
// Isolation from the conversation:
//   * Own ChatClient bound to this handler, so its stream events never reach
//     MyFrame's assistant handlers and nothing is added to chat history.
//   * Requests are built by a scratch ChatHistory holding just the bench
//     prompt, so they get exactly the same request shaping as normal chat
//     for this target (local sampling defaults, /think dialect, Responses
//     conversion) -- then a few benchmark-only fields are added for local
//     llama-server: cache_prompt, ignore_eos, max_tokens, seed, temperature 0.
//   * MyFrame shows the Stop button and treats itself as busy while a
//     benchmark runs; Stop cancels it.
//
// Results: one progress line per run (via the progress callback), a summary
// at the end, and a TSV in %USERPROFILE%\LlamaBoss\Shared\Benchmarks.

#pragma once

#include <wx/event.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bench_stats.h"
#include "chat_history.h"
#include "inference_target.h"

class ChatClient;

class BenchController : public wxEvtHandler
{
public:
    struct Callbacks {
        std::function<void(const std::string&)> progress;   // one line per run / notices
        std::function<void(const std::string&)> finished;   // summary (or why it stopped)
    };

    struct Setup {
        InferenceTarget target;                 // conversation's resolved target
        std::string     bodyModel;              // "model" value normal chat sends
        std::string     label;                  // short name for messages / TSV
        ChatHistory::ThinkOverride think = ChatHistory::ThinkOverride::Auto;
        std::string     outDir;                 // ...\Shared\Benchmarks
        bench::Options  options;
    };

    explicit BenchController(Callbacks callbacks);
    ~BenchController() override;

    // Starts run 1.  False (with `error`) if already running or the first
    // request can't be sent.
    bool Start(const Setup& setup, std::string& error);

    // Cancels the in-flight run; summarizes whatever completed.
    void Stop();

    bool IsRunning() const { return m_running; }

private:
    bool        SendNext(std::string& error);
    std::string BuildBody(bool forceCold) const;
    void        OnComplete(wxCommandEvent& e);
    void        OnError(wxCommandEvent& e);
    void        Finish(const std::string& note);
    void        AppendTsv(const bench::Run& run);

    Callbacks   m_cb;
    Setup       m_setup;
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::unique_ptr<ChatClient>        m_client;

    std::vector<bench::Run> m_runs;
    std::string   m_prompt;
    std::string   m_tsvPath;
    std::string   m_startedAt;
    unsigned long m_genId   = 0;
    bool          m_running = false;
};
