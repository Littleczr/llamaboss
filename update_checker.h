#pragma once
//
// update_checker.h — on-demand "Check for updates" support.
//
// Fetches a small static JSON document hosted on llamaboss.com and compares
// its version against the running build. Network + parse happen in
// CheckBlocking(), which is BLOCKING and must run on a worker thread; marshal
// the result back to the UI with wxFrame::CallAfter().
//
// Transport is WinHTTP (Windows-native TLS, no extra link deps). JSON parsing
// uses Poco::JSON, which the app already links.
//
// Privacy: this is a single HTTPS GET of a public static file. It sends no
// identifying information beyond a generic User-Agent and reads nothing back
// except the version manifest.
//
#include <string>

namespace UpdateChecker {

struct UpdateInfo {
    bool        ok        = false;   // network + parse succeeded
    bool        available = false;   // a strictly newer version exists
    std::string latest;              // remote version, e.g. "0.1.5"
    std::string url;                 // download URL (may be empty)
    std::string notes;               // short release note (may be empty)
    std::string error;               // human-readable reason when !ok
};

// Blocking. Run on a worker thread, never on the UI thread.
//   currentVersion: the running build, e.g. LLAMABOSS_VERSION ("0.1.4").
UpdateInfo CheckBlocking(const std::string& currentVersion);

// True iff 'remote' is a strictly newer release than 'local'. Compares the
// dotted numeric core (major.minor.patch...) first. If the numeric core is
// equal, a final release is considered newer than a local pre-release with
// the same core (for example, "0.1.5" > "0.1.5-beta1"). Build metadata
// alone does not make a version newer. Exposed for unit testing.
bool IsNewer(const std::string& remote, const std::string& local);

} // namespace UpdateChecker
