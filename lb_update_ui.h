#pragma once

#include <string>


std::string LbUpdateFallbackUrl();
bool LbIsTrustedUpdateUrl(const std::string& url);
