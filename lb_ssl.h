// lb_ssl.h
//
// Process-wide, one-time SSL/TLS client initialization for Poco's
// HTTPSClientSession. Shared by every outbound HTTPS path in
// LlamaBoss — currently the remote-inference transport, and (in a
// later cleanup) the model downloader, which today carries its own
// private copy of the same logic.
//
// Idempotent: safe to call from any number of call sites and threads.
// The actual work runs exactly once, guarded by std::call_once.
//
#pragma once

namespace lb {

// Initializes Poco's SSL library and installs a default client-side
// Context plus certificate handler. Must be called before the first
// Poco::Net::HTTPSClientSession is constructed. Cheap (a no-op) after
// the first invocation.
void EnsureSSLInitialized();

} // namespace lb
