// lb_ssl.cpp
//
// Single authority for client-side SSL initialization. See lb_ssl.h
// for the certificate posture and the escape hatch.
//
#include "lb_ssl.h"

// ── Header ordering note (do not reshuffle) ──────────────────────
// <wincrypt.h> #defines X509_NAME, X509_EXTENSIONS, OCSP_REQUEST and
// friends as integer constants. OpenSSL — which Poco::Crypto pulls in
// — uses those same names as typedefs. Including wincrypt.h AFTER the
// OpenSSL headers produces a wall of syntax errors deep inside
// <openssl/x509.h>. So: Windows first, #undef the offenders, Poco
// second.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIALNUMBER
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#endif // _WIN32

#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <Poco/SharedPtr.h>
#include <Poco/Base64Encoder.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/RejectCertificateHandler.h>
#include <Poco/Net/AcceptCertificateHandler.h>

// Poco::Crypto only exists in the OpenSSL-backed NetSSL build. The
// SChannel-backed NetSSL_Win build validates against the Windows
// stores natively and needs no anchor injection at all, so detect
// which one we have rather than assuming.
#if defined(__has_include)
#  if __has_include(<Poco/Crypto/X509Certificate.h>)
#    define LB_HAVE_POCO_CRYPTO 1
#    include <Poco/Crypto/X509Certificate.h>
#  endif
#endif
#ifndef LB_HAVE_POCO_CRYPTO
#define LB_HAVE_POCO_CRYPTO 0
#endif

namespace lb {

namespace {

bool        g_verifying = false;
std::string g_summary   = "TLS: not initialized";

// True when the user has deliberately opted out of verification for
// this run. Checked once, at init.
bool InsecureOptOutRequested()
{
    const char* v = std::getenv("LLAMABOSS_INSECURE_TLS");
    if (!v) return false;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' ||
           v[0] == 't' || v[0] == 'T';
}

#if LB_HAVE_POCO_CRYPTO && defined(_WIN32)

// DER blob -> PEM text. Poco::Crypto::X509Certificate reads PEM from
// a stream; going through PEM avoids touching raw OpenSSL types here
// (and therefore avoids caring which OpenSSL version Poco was built
// against).
std::string DerToPem(const unsigned char* der, std::size_t len)
{
    std::ostringstream b64;
    {
        Poco::Base64Encoder enc(b64);
        enc.write(reinterpret_cast<const char*>(der),
                  static_cast<std::streamsize>(len));
        enc.close();
    }

    std::string body = b64.str();
    if (!body.empty() && body.back() != '\n')
        body += '\n';

    return "-----BEGIN CERTIFICATE-----\n" + body +
           "-----END CERTIFICATE-----\n";
}

// Copies the Windows system trust anchors into the Poco context.
// Returns how many were accepted.
//
// "ROOT" holds the machine/user trusted root authorities — including
// any root a corporate TLS-inspection appliance has had IT install,
// which is exactly the case the old accept-everything posture was
// working around. "CA" holds intermediates, which are cheap to add
// and spare us a chain-building failure when a server ships an
// incomplete chain.
std::size_t AddWindowsTrustAnchors(Poco::Net::Context::Ptr pCtx)
{
    std::size_t added = 0;

    const wchar_t* kStores[] = { L"ROOT", L"CA" };
    for (const wchar_t* storeName : kStores) {
        HCERTSTORE hStore = ::CertOpenSystemStoreW(
            static_cast<HCRYPTPROV_LEGACY>(NULL), storeName);
        if (!hStore) continue;

        PCCERT_CONTEXT pCert = nullptr;
        while ((pCert = ::CertEnumCertificatesInStore(hStore, pCert)) != nullptr) {
            if (!pCert->pbCertEncoded || pCert->cbCertEncoded == 0)
                continue;

            try {
                const std::string pem =
                    DerToPem(pCert->pbCertEncoded, pCert->cbCertEncoded);
                std::istringstream is(pem);
                Poco::Crypto::X509Certificate cert(is);
                pCtx->addCertificateAuthority(cert);
                ++added;
            }
            catch (...) {
                // A store can hold entries OpenSSL will not parse
                // (legacy encodings, cross-signed oddities). Skipping
                // one anchor is harmless; aborting the whole load
                // because of one would not be.
            }
        }

        ::CertCloseStore(hStore, 0);
    }

    return added;
}

#endif // LB_HAVE_POCO_CRYPTO && _WIN32

} // namespace

void EnsureSSLInitialized()
{
    static std::once_flag s_flag;
    std::call_once(s_flag, []()
    {
        Poco::Net::initializeSSL();

        // ── Deliberate opt-out ───────────────────────────────────
        if (InsecureOptOutRequested()) {
            Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> pCert(
                new Poco::Net::AcceptCertificateHandler(false));
            Poco::Net::Context::Ptr pCtx(new Poco::Net::Context(
                Poco::Net::Context::CLIENT_USE, "",
                Poco::Net::Context::VERIFY_NONE));
            Poco::Net::SSLManager::instance().initializeClient(
                nullptr, pCert, pCtx);

            g_verifying = false;
            g_summary   = "TLS: VERIFICATION DISABLED via "
                          "LLAMABOSS_INSECURE_TLS - API keys and model "
                          "downloads are exposed to interception";
            return;
        }

        // ── Verifying context ────────────────────────────────────
        // VERIFY_RELAXED is the correct client-side mode: the server
        // certificate is verified and a failure routes to the
        // handler. Paired with RejectCertificateHandler that means
        // the handshake fails closed. loadDefaultCAs=true picks up
        // whatever bundle the Poco/OpenSSL build ships with; the
        // Windows anchors below are what actually carries the load on
        // a normal Windows install.
        Poco::Net::Context::Ptr pCtx(new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE,
            "",                                   // caLocation
            Poco::Net::Context::VERIFY_RELAXED,
            9,                                    // verificationDepth
            true));                               // loadDefaultCAs

        // Hostname must match the certificate. Without this a valid
        // certificate for ANY host would satisfy verification, which
        // defeats most of the point.
        try {
            pCtx->enableExtendedCertificateVerification(true);
        } catch (...) {
            // Older Poco: extended verification is on by default.
        }

        std::size_t anchors = 0;
#if LB_HAVE_POCO_CRYPTO && defined(_WIN32)
        try {
            anchors = AddWindowsTrustAnchors(pCtx);
        } catch (...) {
            anchors = 0;
        }
#endif

        Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> pCert(
            new Poco::Net::RejectCertificateHandler(false));

        Poco::Net::SSLManager::instance().initializeClient(
            nullptr, pCert, pCtx);

        g_verifying = true;

        std::ostringstream s;
        s << "TLS: verifying";
#if LB_HAVE_POCO_CRYPTO && defined(_WIN32)
        s << ", " << anchors << " trust anchors from Windows store";
        if (anchors == 0) {
            s << " (NONE loaded - falling back to the OpenSSL default "
                 "bundle; if HTTPS fails, set LLAMABOSS_INSECURE_TLS=1 "
                 "to confirm that is the cause)";
        }
#else
        s << " against the system trust store";
#endif
        g_summary = s.str();
    });
}

bool SSLVerificationEnabled()
{
    return g_verifying;
}

std::string SSLInitSummary()
{
    return g_summary;
}

} // namespace lb
