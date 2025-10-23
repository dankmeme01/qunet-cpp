#ifdef QUNET_TLS_SUPPORT

#include <qunet/socket/transport/tls/ClientTlsContext.hpp>
#include <utility>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

namespace qn {

bool TlsError::ok() const {
    return code == WOLFSSL_SUCCESS;
}

std::string_view TlsError::message() const {
    static thread_local char buffer[WOLFSSL_MAX_ERROR_SZ];

    return wolfSSL_ERR_error_string(code, buffer);
}

ClientTlsContext::ClientTlsContext(WOLFSSL_CTX* ctx) : m_ctx(ctx) {}

ClientTlsContext::~ClientTlsContext() {
    if (m_ctx) {
        wolfSSL_CTX_free(m_ctx);
    }
}

ClientTlsContext::ClientTlsContext(ClientTlsContext&& other) {
    *this = std::move(other);
}

ClientTlsContext& ClientTlsContext::operator=(ClientTlsContext&& other) {
    if (this != &other) {
        m_ctx = other.m_ctx;
        other.m_ctx = nullptr;
    }

    return *this;
}

TlsResult<ClientTlsContext> ClientTlsContext::create(bool insecure) {
    auto ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());

    if (!ctx) {
        return Err(wolfSSL_ERR_get_error());
    }

    if (ngtcp2_crypto_wolfssl_configure_client_context(ctx) != 0) { // this can never fail really
        wolfSSL_CTX_free(ctx);
        return Err(wolfSSL_ERR_get_error());
    }

    if (insecure) {
        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
    } else {
        if (wolfSSL_CTX_set_default_verify_paths(ctx) != WOLFSSL_SUCCESS) {
            wolfSSL_CTX_free(ctx);
            return Err(wolfSSL_ERR_get_error());
        }

        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
    }

    // set cipher list
    if (wolfSSL_CTX_set_cipher_list(ctx, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_CCM_SHA256") != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(ctx);
        return Err(wolfSSL_ERR_get_error());
    }

    if (wolfSSL_CTX_set1_curves_list(ctx, "X25519:P-256:P-384:P-521") != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(ctx);
        return Err(wolfSSL_ERR_get_error());
    }

    // idk what this is
    /*
    if (config.session_file) {
        wolfSSL_CTX_UseSessionTicket(ssl_ctx_);
        wolfSSL_CTX_sess_set_new_cb(ssl_ctx_, new_session_cb);
    }
    */

    return Ok(ClientTlsContext(ctx));
}

}

#endif
