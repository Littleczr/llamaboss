#pragma once

#include "tool_context.h"

#include <string>
#include <vector>

struct WebFetchResult {
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
};

// Fetches a public http/https webpage using WinHTTP, extracts readable text,
// saves raw HTML + cleaned text artifacts, and returns a compact preview for
// the model. This is intentionally a simple webpage inspector, not a browser:
// it does not execute JavaScript, reuse cookies, submit forms, or access local
// files/private network hosts.
WebFetchResult FetchWebPageUrl(const std::string& urlArg,
                               const ToolContext& ctx);
