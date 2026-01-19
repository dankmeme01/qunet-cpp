#pragma once

#ifdef QUNET_TLS_SUPPORT

#include "TlsContext.hpp"

namespace qn {

struct TcpTlsOptions : TlsOptions {};

class TcpTlsContext : public TlsContext {
public:
    static TlsResult<std::shared_ptr<TcpTlsContext>> create(const TcpTlsOptions& options = {});

    TcpTlsContext(WOLFSSL_CTX* ctx);
};

}

#endif
