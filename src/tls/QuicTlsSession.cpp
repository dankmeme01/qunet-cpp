#ifdef QUNET_QUIC_SUPPORT

#include <qunet/tls/QuicTlsSession.hpp>
#include <socket/transport/quic/QuicConnection.hpp>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace qn {

QuicTlsSession::QuicTlsSession(std::shared_ptr<Context> ctx, WOLFSSL* ssl) : TlsSession(std::move(ctx), ssl) {}

TlsResult<QuicTlsSession> QuicTlsSession::create(
    std::shared_ptr<Context> context,
    const qsox::SocketAddress& address,
    QuicConnection* conn,
    std::string_view serverName
) {
    auto ssl = wolfSSL_new(context->handle());
    if (!ssl) {
        return Err(lastTlsError());
    }

    // raii guard
    QuicTlsSession session(std::move(context), ssl);

    GEODE_UNWRAP(tlsWrap(wolfSSL_set_app_data(ssl, conn->connRef())));

    wolfSSL_set_connect_state(ssl);

    const uint8_t alpn[] = "\x06qunet1";
    GEODE_UNWRAP(tlsWrap(wolfSSL_set_alpn_protos(ssl, alpn, sizeof(alpn) - 1)));

    // use quic v1
    wolfSSL_set_quic_transport_version(ssl, 0x39);

    // set the sni
    GEODE_UNWRAP(session.useSNI(serverName));

    return Ok(std::move(session));
}

}

#endif
