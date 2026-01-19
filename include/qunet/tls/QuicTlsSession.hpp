#pragma once
#ifdef QUNET_QUIC_SUPPORT

#include "TlsSession.hpp"
#include "QuicTlsContext.hpp"

namespace qn {

class QuicConnection;

class QuicTlsSession : public TlsSession<QuicTlsContext> {
public:
    static TlsResult<QuicTlsSession> create(
        std::shared_ptr<Context> context,
        const qsox::SocketAddress& address,
        QuicConnection* conn,
        std::string_view serverName
    );

protected:
    QuicTlsSession(std::shared_ptr<Context> ctx, WOLFSSL* ssl);
};

}

#endif
