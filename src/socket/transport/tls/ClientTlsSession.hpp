#pragma once

#include <qunet/socket/transport/tls/ClientTlsContext.hpp>

struct WOLFSSL;

namespace qn {

class QuicConnection;

class ClientTlsSession {
public:
    ~ClientTlsSession();
    ClientTlsSession(const ClientTlsSession&) = delete;
    ClientTlsSession& operator=(const ClientTlsSession&) = delete;
    ClientTlsSession(ClientTlsSession&&);
    ClientTlsSession& operator=(ClientTlsSession&&);

    static TlsResult<ClientTlsSession> create(
        const ClientTlsContext& context,
        const qsox::SocketAddress& address,
        QuicConnection* quicConn,
        const std::string& serverName
    );

    void* nativeHandle() const;
    static TlsError lastError();

private:
    WOLFSSL* m_ssl = nullptr;

    ClientTlsSession(WOLFSSL* m_ssl);
};

}