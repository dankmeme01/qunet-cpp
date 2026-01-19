#ifdef QUNET_QUIC_SUPPORT

#include <qunet/tls/QuicTlsContext.hpp>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

namespace qn {

QuicTlsContext::QuicTlsContext(WOLFSSL_CTX* ctx) : TlsContext(ctx) {}

TlsResult<std::shared_ptr<QuicTlsContext>> QuicTlsContext::create(const QuicTlsOptions& options) {
    auto ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());

    if (!ctx) {
        return Err(lastTlsError());
    }

    auto context = std::make_shared<QuicTlsContext>(ctx);

    if (ngtcp2_crypto_wolfssl_configure_client_context(ctx) != 0) { // this can never fail really
        return Err(lastTlsError());
    }

    GEODE_UNWRAP(context->configure(options));

    // // set cipher list
    // if (wolfSSL_CTX_set_cipher_list(ctx, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_CCM_SHA256") != WOLFSSL_SUCCESS) {
    //     wolfSSL_CTX_free(ctx);
    //     return Err(lastTlsError());
    // }

    // if (wolfSSL_CTX_set1_curves_list(ctx, "X25519:P-256:P-384:P-521") != WOLFSSL_SUCCESS) {
    //     wolfSSL_CTX_free(ctx);
    //     return Err(lastTlsError());
    // }

    // idk what this is
    /*
    if (config.session_file) {
        wolfSSL_CTX_UseSessionTicket(ssl_ctx_);
        wolfSSL_CTX_sess_set_new_cb(ssl_ctx_, new_session_cb);
    }
    */

    return Ok(std::move(context));
}

}

#endif
