#include "lb_update_ui.h"

#include <algorithm>
#include <cctype>

#include <Poco/URI.h>

namespace {

std::string LbUpdateLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

} // namespace

std::string LbUpdateFallbackUrl()
{
    return "https://llamaboss.com";
}

bool LbIsTrustedUpdateUrl(const std::string& url)
{
    try {
        Poco::URI uri(url);
        const std::string scheme = LbUpdateLowerAscii(uri.getScheme());
        const std::string host   = LbUpdateLowerAscii(uri.getHost());

        if (scheme != "https")
            return false;

        // The manifest is trusted only to point at the public website or the
        // release bucket currently used by llamaboss.com. This keeps a bad
        // manifest from opening an arbitrary third-party download URL.
        return host == "llamaboss.com" ||
               host == "www.llamaboss.com" ||
               host == "pub-2d2e18de339c4fe3ab067f6afd1e7656.r2.dev";
    } catch (...) {
        return false;
    }
}

wxString LbBuildAboutMessage(const char* version,
                             const std::string& modelDisplayName,
                             const std::string& apiUrl,
                             const std::string& modelsDir)
{
    wxString msg;
    msg << "LlamaBoss Beta v" << version << "\n\n"
        << "Private local desktop AI assistant for Windows.\n"
        << "Built for local LLMs, files, projects, skills, and approved tools.\n"
        << "Powered by llama.cpp\n\n"
        << "Beta notice:\n"
        << "This is beta software. Features, workflows, file handling, and UI behavior may change before a stable release.\n\n"
        << "Created by Cesar Avelar\n"
        << "Website: llamaboss.com\n\n"
        << "Built with wxWidgets + Poco\n"
        << "License: MIT\n\n"
        << wxString::FromUTF8("Model: ") << wxString::FromUTF8(modelDisplayName) << "\n"
        << wxString::FromUTF8("Server: ") << wxString::FromUTF8(apiUrl) << "\n"
        << wxString::FromUTF8("Models: ") << wxString::FromUTF8(modelsDir);
    return msg;
}

wxString LbBuildUpdateAvailableMessage(const char* currentVersion,
                                       const UpdateChecker::UpdateInfo& info)
{
    wxString body;
    body << "A new version of LlamaBoss is available.\n\n"
         << "Installed: v" << currentVersion << "\n"
         << "Latest:    v" << wxString::FromUTF8(info.latest) << "\n";
    if (!info.notes.empty())
        body << "\n" << wxString::FromUTF8(info.notes) << "\n";
    body << "\nOpen the download page now?";
    return body;
}
