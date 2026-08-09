// lb_ssl.h
//
// Process-wide, one-time SSL/TLS client initialization for Poco's
// HTTPSClientSession. Shared by EVERY outbound HTTPS path in
// LlamaBoss: the remote-inference transport (chat_client.cpp) and the
// model downloader (model_downloader.cpp). Neither may install its
// own SSLManager client configuration — Poco's SSLManager is a
// process singleton, and a second initializeClient() silently
// replaces whatever the first one installed.
//
// Idempotent: safe to call from any number of call sites and threads.
// The actual work runs exactly once, guarded by std::call_once.
//
// ── Certificate posture ──────────────────────────────────────────
// Server certificates are VERIFIED. The trust anchors are taken from
// the Windows system certificate stores ("ROOT" and "CA"), which is
// the same set Edge/Chrome validate against — so a corporate TLS
// inspection proxy whose root has been installed by IT is trusted
// automatically, while an attacker's self-signed certificate is not.
// This is the behavior the previous AcceptCertificateHandler(false)
// posture was reaching for, without the "accept literally anything"
// side effect.
//
// Escape hatch: setting the environment variable
//
//     LLAMABOSS_INSECURE_TLS=1
//
// restores the old accept-everything behavior for one run. It is a
// deliberate, per-machine action and is reported loudly through
// SSLInitSummary() so it can be surfaced in the UI/log rather than
// being an invisible default.
//
#pragma once

#include <string>

namespace lb {

// Initializes Poco's SSL library and installs the process-wide
// client-side Context plus certificate handler. Must be called before
// the first Poco::Net::HTTPSClientSession is constructed. Cheap (a
// no-op) after the first invocation.
void EnsureSSLInitialized();

// True when the installed context actually verifies server
// certificates. False means either the insecure escape hatch is
// active or no trust anchors could be loaded — see SSLInitSummary().
// Only meaningful after EnsureSSLInitialized() has run.
bool SSLVerificationEnabled();

// One-line, human-readable description of what the initializer did,
// e.g. "TLS: verifying, 148 trust anchors from Windows store" or
// "TLS: VERIFICATION DISABLED via LLAMABOSS_INSECURE_TLS".
// Suitable for a log line or a status tooltip.
std::string SSLInitSummary();

} // namespace lb
