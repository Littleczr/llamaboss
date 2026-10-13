// transient_error_tests.cpp -- standalone checks for transient_error.h.
// Build: cl /std:c++17 /EHsc transient_error_tests.cpp   (or g++ -std=c++17)
#include "transient_error.h"
#include <cstdio>

static int g_fail = 0;
static void Expect(bool got, bool want, const char* text)
{
    if (got != want) {
        ++g_fail;
        std::printf("FAIL: expected %s for: %s\n", want ? "transient" : "permanent", text);
    }
}

int main()
{
    using lb_transient_error::IsTransient;
    const char* yes[] = {
        "Network Error: Connection reset by peer",
        "Network Error: Timeout",
        "HTTP Error: No message received",
        "Stream ended unexpectedly before the server sent a terminal event.",
        "Stream ended before the server sent any response data.",
        "API Error (502): Bad Gateway - <html>",
        "API Error (503): Service Unavailable - ",
        "API Error (529): - {\"error\":{\"type\":\"overloaded_error\"}}",
        "API Error (429): Too Many Requests - rate limited",
        "ChatGPT plan usage is temporarily unavailable. Try again in a minute.",
        "ChatGPT plan request failed (HTTP 500).",
        "ChatGPT plan rate limit reached. Wait a moment and try again.",
        "Stream error: Overloaded",
        "Stream error: Server returned an error mid-stream",
        "Stream error: An error occurred while processing your request. [server_error]",
        "Responses request failed. An error occurred while processing your request. [server_error]",
        "ChatGPT plan usage is temporarily unavailable. Try again in a minute. [subscription_sharing_usage_unavailable]",
    };
    const char* no[] = {
        "",
        "API Error (400): Bad Request - invalid tool schema",
        "API Error (401): Unauthorized - bad key",
        "API Error (404): Not Found - model",
        "ChatGPT sign-in was not accepted. Open Settings > Connections, edit ChatGPT plan.",
        "ChatGPT plan usage was blocked by an account, workspace or region policy.",
        "Stopped this reply: the model's tool-call arguments kept repeating.",
        "Stream aborted: response exceeded the maximum allowed size.",
        "Stream error: This model's maximum context length is 200000 tokens",
        "Invalid JSON in the Responses stream.",
        "Failed to start chat request",
        "API Error (402): Payment Required - insufficient credits",
        "ChatGPT plan request failed (HTTP 429). Details: usage limit reached [usage_limit_reached]",
        "ChatGPT plan rate limit reached. Wait a moment and try again. Details: [usage_limit_reached]",
        "You've reached the ChatGPT plan usage limit for LlamaBoss. Check usage under ChatGPT Settings > Usage, then try again later. [subscription_sharing_usage_limit_exceeded]",
        "Responses request failed. [subscription_sharing_usage_limit_exceeded]",
        "Responses request incomplete. Reason: max_output_tokens",
        "This ChatGPT account or workspace isn't eligible to use its plan in other apps (Plus or Pro is required).",
    };
    for (const char* s : yes) Expect(IsTransient(s), true, s);
    for (const char* s : no)  Expect(IsTransient(s), false, s);

    if (lb_transient_error::RetryDelayMs(1) != 3000 ||
        lb_transient_error::RetryDelayMs(2) != 10000) {
        ++g_fail; std::printf("FAIL: retry delays\n");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all transient_error tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
