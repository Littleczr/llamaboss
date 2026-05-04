// tool_grep.h
//
// Implementation of the /grep slash command — Phase 3.
//
// Unlike /read and /ls, this one is THREADED: recursive search
// across a source tree can take seconds, and we don't want to pin
// the UI thread.  The executor lifetime follows CmdExecutor's
// pattern exactly:
//   - Fire-and-forget wxThread (detached).
//   - shared_ptr<atomic<bool>> cancel flag, visible to both sides.
//   - weak_ptr<atomic<bool>> alive token guards QueueEvent against
//     a destroyed MyFrame.
//   - On completion (success / cancelled / timed out / error) the
//     worker posts wxEVT_GREP_COMPLETE with a GrepResult packed
//     into the event's ClientObject.
//
#pragma once

#include "tool_context.h"

#include <wx/event.h>
#include <wx/thread.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// ─── Thread → UI event ───────────────────────────────────────────
wxDECLARE_EVENT(wxEVT_GREP_COMPLETE, wxCommandEvent);

// ─── Result payload ──────────────────────────────────────────────
// Mirrors ReadResult/LsResult in shape so the handler on MyFrame
// can pack it into a ChatDisplay::ToolBlock uniformly.  Extra
// fields (cancelled, timedOut) matter only to the handler for
// driving SetStreamingState and the chat-state reset.
struct GrepResult {
    // Echo back — caller already has these but round-tripping them
    // simplifies the event payload (single struct, no parallel args).
    std::string commandEcho;    // "/grep <pattern> [path]"

    // Chips for the header: "N matches", optionally "N files
    // scanned", plus special status chips ("truncated",
    // "timed out", "cancelled"), with elapsed always last.
    std::vector<std::string> chips;

    // Body — matches formatted as "path:line: content".  Relative
    // paths for recursive searches; basename for single-file.
    std::string body;

    // Populated when the search couldn't run at all (path resolution
    // failure, not-a-file-or-dir, worker spawn failure).  Does NOT
    // include per-file errors during walk — those are silently
    // skipped (permissions, transient IO).
    std::string errorBody;

    std::string bodyLang;       // always ""
    double      elapsedSec = 0.0;

    bool cancelled = false;
    bool timedOut  = false;
};

class GrepResultClientData : public wxClientData {
public:
    explicit GrepResultClientData(GrepResult r) : m_result(std::move(r)) {}
    const GrepResult& GetResult() const { return m_result; }
private:
    GrepResult m_result;
};

// ─── Executor facade ─────────────────────────────────────────────
class GrepExecutor {
public:
    // Hard ceilings applied independently of ctx.  Past any of
    // these we stop collecting and tag the result with "truncated".
    static constexpr size_t kMaxMatches       =  500;
    static constexpr size_t kMaxFilesScanned  = 5000;
    static constexpr size_t kMaxLineLength    =  200;  // per-match line truncation
    static constexpr size_t kMaxFileBytes     = 10 * 1024 * 1024;  // 10 MiB per file

    GrepExecutor(wxEvtHandler* eventHandler,
                 std::weak_ptr<std::atomic<bool>> aliveToken);
    ~GrepExecutor();

    // Kicks off a search on a detached worker thread.
    // Preconditions (caller responsibility; Start only checks
    // "pattern non-empty" and spawn success):
    //   - `pattern` is non-empty (validated by handler)
    //   - `resolvedPath` is an absolute path that exists and is
    //     either a file or a directory
    //   - `ctx.cwd` is populated (used as the search root prefix
    //     for computing relative paths in match output)
    // Returns false if a grep is already running or the worker
    // could not be spawned.
    bool Start(const std::string& pattern,
               const std::string& resolvedPath,
               const std::string& commandEcho,
               const ToolContext& ctx);

    // Safe to call whether or not anything is running.
    void Cancel();

    bool IsRunning() const {
        return m_isRunning && m_isRunning->load();
    }

private:
    wxEvtHandler*                      m_eventHandler;
    std::weak_ptr<std::atomic<bool>>   m_aliveToken;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::shared_ptr<std::atomic<bool>> m_isRunning;
};
