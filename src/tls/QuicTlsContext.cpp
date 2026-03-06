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

    // for some reason, the server (s2n-quic) implementation prioritizes only P-384, so we want to use it to avoid a HelloRetryRequest
    if (wolfSSL_CTX_set1_groups_list(ctx, "P-384:P-256:X25519") != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(ctx);
        return Err(lastTlsError());
    }

    wolfSSL_CTX_set_session_cache_mode(ctx, WOLFSSL_SESS_CACHE_CLIENT);

    return Ok(std::move(context));
}

}

#endif
