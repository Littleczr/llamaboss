#pragma once

#include <string>

#include <wx/string.h>

#include "update_checker.h"

std::string LbUpdateFallbackUrl();
bool LbIsTrustedUpdateUrl(const std::string& url);

wxString LbBuildAboutMessage(const char* version,
                             const std::string& modelDisplayName,
                             const std::string& apiUrl,
                             const std::string& modelsDir);

wxString LbBuildUpdateAvailableMessage(const char* currentVersion,
                                       const UpdateChecker::UpdateInfo& info);
