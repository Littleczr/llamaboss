#include "lb_update_ui.h"
#include "lb_string_utils.h"
#include "lb_ascii.h"

namespace {

} // namespace

std::string LbUpdateFallbackUrl()
{
    return "https://llamaboss.com";
}

bool LbIsTrustedUpdateUrl(const std::string& url)
{
    try {
        Poco::URI uri(url);
        const std::string scheme = LbLowerAscii(uri.getScheme());
        const std::string host   = LbLowerAscii(uri.getHost());

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
