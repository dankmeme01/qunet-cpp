#include <qunet/tls/TlsContext.hpp>
#include <qunet/Log.hpp>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace qn {

void WolfsslCtxDeleter::operator()(WOLFSSL_CTX* ctx) const {
    wolfSSL_CTX_free(ctx);
}
void WolfsslDeleter::operator()(WOLFSSL* ssl) const {
    wolfSSL_free(ssl);
}

TlsError lastTlsError() {
    return TlsError(wolfSSL_ERR_get_error());
}

TlsResult<> tlsWrap(int rcode) {
    if (rcode == WOLFSSL_SUCCESS) {
        return Ok();
    } else {
        return Err(lastTlsError());
    }
}

bool TlsError::ok() const {
    return code == WOLFSSL_SUCCESS;
}

std::string_view TlsError::message() const {
    static thread_local char buffer[WOLFSSL_MAX_ERROR_SZ];

    return wolfSSL_ERR_error_string(code, buffer);
}

TlsContext::TlsContext(WOLFSSL_CTX* ctx) : m_ctx(ctx) {}

TlsContext::~TlsContext() {}

WOLFSSL_CTX* TlsContext::handle() const {
    return m_ctx.get();
}

static std::string pathToString(const std::filesystem::path& path) {
#ifdef _WIN32
    auto& wstr = path.native();
    int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.length(), NULL, 0, NULL, NULL);
    std::string str(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], count, NULL, NULL);
    return str;
#else
    return path.string();
#endif
}

TlsResult<> TlsContext::configure(const TlsOptions& opts) {
    auto ctx = this->handle();

    // cacerts
    if (opts.insecure) {
        log::info("Skipping certificate verification (insecure mode)");
        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
    } else {
        if (!opts.caCerts.empty()) {
            // custom ca certs
            GEODE_UNWRAP(this->loadCerts(std::string_view{opts.caCerts}));
        } else if (!opts.caCertPath.empty()) {
            // custom ca cert path
            GEODE_UNWRAP(this->loadCerts(opts.caCertPath));
        } else {
            // system certs
            GEODE_UNWRAP(this->loadSystemCerts());
        }

        wolfSSL_CTX_set_verify(m_ctx.get(), WOLFSSL_VERIFY_PEER, nullptr);
    }

    return Ok();
}

TlsResult<> TlsContext::loadCerts(std::string_view certs) {
    log::info("Loading certificates from string ({} bytes)", certs.size());

    return tlsWrap(wolfSSL_CTX_load_verify_buffer(
        m_ctx.get(),
        (const unsigned char*)certs.data(),
        (long)certs.size(),
        WOLFSSL_FILETYPE_PEM
    ));
}

TlsResult<> TlsContext::loadCerts(const std::filesystem::path& certs) {
    log::info("Loading certificates from {}", certs);

    std::string path = pathToString(certs);
    return tlsWrap(wolfSSL_CTX_load_verify_locations(m_ctx.get(), path.c_str(), nullptr));
}

TlsResult<> TlsContext::loadSystemCerts() {
    log::info("Loading system certificates");

    return tlsWrap(wolfSSL_CTX_set_default_verify_paths(m_ctx.get()));
}


}
