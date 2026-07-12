// lb_ssl.cpp
//
// Single authority for client-side SSL initialization. The cert
// posture here mirrors the behavior the model downloader has shipped
// with (see note below) so that moving HTTPS paths onto this shared
// helper does not change how TLS behaves on users' machines.
//
#include "lb_ssl.h"

#include <mutex>

#include <Poco/SharedPtr.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/AcceptCertificateHandler.h>

namespace lb {

void EnsureSSLInitialized()
{
    static std::once_flag s_flag;
    std::call_once(s_flag, []()
    {
        Poco::Net::initializeSSL();

        // Tolerant client posture: accept the peer chain rather than
        // hard-failing on it. This matches the behavior the model
        // downloader has shipped with — HTTPS traffic on corporate /
        // work laptops is frequently inspected by a local security
        // proxy, and a strict RejectCertificateHandler + VERIFY_STRICT
        // fails ("certificate verify failed") even when a browser can
        // reach the same host. Remote inference endpoints inherit the
        // same posture for the same reason.
        //
        // Hardening path (future, opt-in): VERIFY_STRICT with a real
        // CA bundle, surfaced as an Advanced setting.
        Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> pCert(
            new Poco::Net::AcceptCertificateHandler(false));

        Poco::Net::Context::Ptr pCtx(new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE, ""));

        Poco::Net::SSLManager::instance().initializeClient(
            nullptr, pCert, pCtx);
    });
}

} // namespace lb
