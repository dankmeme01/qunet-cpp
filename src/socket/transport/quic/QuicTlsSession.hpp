#pragma once

#ifdef QUNET_QUIC_SUPPORT

#include <xtls/Session.hpp>
#include <xtls/Context.hpp>

#ifdef QUNET_ENABLE_OPENSSL
# include <ngtcp2/ngtcp2_crypto_ossl.h>
#elif defined(QUNET_ENABLE_WOLFSSL)
# include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#else
# error "unsupported crypto backend"
#endif

namespace qn {

class QuicConnection;

class QuicTlsSession {
public:
    static xtls::TlsResult<QuicTlsSession> create(
        std::shared_ptr<xtls::Context> ctx,
        QuicConnection*,
        const std::string& hostname
    );

    QuicTlsSession() = default;
    QuicTlsSession(QuicTlsSession&&) = default;
    QuicTlsSession& operator=(QuicTlsSession&&) = default;

    ~QuicTlsSession();

    void* handle() const;
    void* ngtcp2_handle() const;
    xtls::TlsError lastError() const;

private:
    std::shared_ptr<xtls::Context> m_xctx;
    std::shared_ptr<xtls::Session> m_session;
#ifdef QUNET_ENABLE_OPENSSL
    struct Ngtcp2CtxDeleter {
        void operator()(ngtcp2_crypto_ossl_ctx* ctx) {
            ngtcp2_crypto_ossl_ctx_del(ctx);
        }
    };

    std::unique_ptr<ngtcp2_crypto_ossl_ctx, Ngtcp2CtxDeleter> m_ngtcp2ctx;
#endif
};

}

#endif
