#include <qunet/tls/TcpTlsSession.hpp>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace qn {

TcpTlsSession::TcpTlsSession(std::shared_ptr<Context> ctx, WOLFSSL* ssl) : TlsSession(std::move(ctx), ssl) {}

TlsResult<TcpTlsSession> TcpTlsSession::create(
    std::shared_ptr<Context> context,
    const qsox::SocketAddress& address,
    std::string_view serverName
) {
    auto ssl = wolfSSL_new(context->handle());
    if (!ssl) {
        return Err(lastTlsError());
    }

    // raii guard
    TcpTlsSession session(std::move(context), ssl);

    // set the sni
    GEODE_UNWRAP(session.useSNI(serverName));

    return Ok(std::move(session));
}

}
