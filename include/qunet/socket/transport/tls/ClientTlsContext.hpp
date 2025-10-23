#pragma once
#ifdef QUNET_TLS_SUPPORT

#include <qunet/socket/transport/Error.hpp>

#include <qsox/SocketAddress.hpp>

struct WOLFSSL_CTX;

namespace qn {

template <typename T = void>
using TlsResult = geode::Result<T, TlsError>;

class ClientTlsContext {
public:
    ~ClientTlsContext();
    ClientTlsContext(const ClientTlsContext&) = delete;
    ClientTlsContext& operator=(const ClientTlsContext&) = delete;
    ClientTlsContext(ClientTlsContext&&);
    ClientTlsContext& operator=(ClientTlsContext&&);

    static TlsResult<ClientTlsContext> create(bool insecure);

protected:
    friend class ClientTlsSession;

    ClientTlsContext(WOLFSSL_CTX* ctx);

    WOLFSSL_CTX* m_ctx = nullptr;
};

}

#endif
