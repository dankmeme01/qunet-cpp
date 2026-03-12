#include "QuicTlsSession.hpp"
#include "QuicConnection.hpp"

using namespace geode;

namespace qn {

xtls::TlsResult<QuicTlsSession> QuicTlsSession::create(
    std::shared_ptr<xtls::Context> ctx,
    QuicConnection* conn,
    const std::string& hostname
) {
    auto s = GEODE_UNWRAP(ctx->createSession());
    // s->setHostname(hostname);

    QuicTlsSession ret;
    ret.m_session = s;
    ret.m_xctx = ctx;

#ifdef QUNET_ENABLE_OPENSSL
    ngtcp2_crypto_ossl_ctx* ngtcp2ctxp = nullptr;

    auto ssl = static_cast<SSL*>(s->handle_());
    ngtcp2_crypto_ossl_ctx_new(&ngtcp2ctxp, ssl);

    auto ngtcp2ctx = std::unique_ptr<ngtcp2_crypto_ossl_ctx, Ngtcp2CtxDeleter>(ngtcp2ctxp);
    if (0 != ngtcp2_crypto_ossl_configure_client_session(ssl)) {
        return Err(xtls::TlsError::custom("ngtcp2 ossl configure failed"));
    }

    SSL_set_app_data(ssl, conn->connRef());
    SSL_set_connect_state(ssl);

    // use h3 as the alpn, for higher likelihood of bypassing firewalls/dpi systems
    const uint8_t alpn[] = "\x02h3";
    SSL_set_alpn_protos(ssl, alpn, sizeof(alpn) - 1);
    ret.m_ngtcp2ctx = std::move(ngtcp2ctx);
#endif

    return Ok(std::move(ret));
}

xtls::TlsError QuicTlsSession::lastError() const {
    return m_session->lastError();
}

void* QuicTlsSession::handle() const {
    return m_session->handle_();
}

void* QuicTlsSession::ngtcp2_handle() const {
    return m_ngtcp2ctx.get();
}

QuicTlsSession::~QuicTlsSession() {
    m_ngtcp2ctx.reset();

    if (!m_session) return;
    auto ssl = static_cast<SSL*>(m_session->handle_());
    if (ssl) {
        SSL_set_app_data(ssl, nullptr);
    }
}

}