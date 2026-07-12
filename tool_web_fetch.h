#pragma once

#include "tool_context.h"

#include <wx/event.h>
#include <wx/thread.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Thread -> UI events for the asynchronous webpage inspector.
wxDECLARE_EVENT(wxEVT_WEB_FETCH_COMPLETE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_WEB_FETCH_ERROR,    wxCommandEvent);

struct WebFetchResult {
    std::string commandEcho;
    std::vector<std::string> chips;
    std::string body;
    std::string errorBody;
    std::string bodyLang;

    std::string rawHtmlPath;
    std::string rawHtmlDisplayName;
    std::string textPath;
    std::string textDisplayName;

    std::size_t htmlBytes = 0;
    std::size_t textBytes = 0;
    int textLineCount = 0;

    bool cancelled = false;
};

class WebFetchResultClientData : public wxClientData {
public:
    explicit WebFetchResultClientData(WebFetchResult r)
        : m_result(std::move(r)) {}
    const WebFetchResult& GetResult() const { return m_result; }
private:
    WebFetchResult m_result;
};

class WebFetchExecutor {
public:
    static constexpr unsigned long kDefaultTimeoutMs = 15000;

    WebFetchExecutor(wxEvtHandler* eventHandler,
                     std::weak_ptr<std::atomic<bool>> aliveToken);
    ~WebFetchExecutor();

    bool Start(const std::string& urlArg,
               const std::string& commandEcho,
               const ToolContext& ctx);

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

// Fetches a public http/https webpage using WinHTTP, extracts readable text,
// saves raw HTML + cleaned text artifacts, and returns a compact preview for
// the model. This is intentionally a simple webpage inspector, not a browser:
// it does not execute JavaScript, reuse cookies, submit forms, or access local
// files/private network hosts.
WebFetchResult FetchWebPageUrl(const std::string& urlArg,
                               const ToolContext& ctx);
