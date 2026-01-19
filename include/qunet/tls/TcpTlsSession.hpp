#pragma once
#ifdef QUNET_TLS_SUPPORT

#include "TlsSession.hpp"
#include "TcpTlsContext.hpp"

namespace qn {

class TcpTlsSession : public TlsSession<TcpTlsContext> {
public:
    static TlsResult<TcpTlsSession> create(
        std::shared_ptr<Context> context,
        const qsox::SocketAddress& address,
        std::string_view serverName
    );

protected:
    TcpTlsSession(std::shared_ptr<Context> ctx, WOLFSSL* ssl);
};

}

#endif
