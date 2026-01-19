#pragma once
#ifdef QUNET_QUIC_SUPPORT

#include "TlsContext.hpp"

namespace qn {

struct QuicTlsOptions : TlsOptions {};

class QuicTlsContext : public TlsContext {
public:
    static TlsResult<std::shared_ptr<QuicTlsContext>> create(const QuicTlsOptions& options = {});

    QuicTlsContext(WOLFSSL_CTX* ctx);
};

}

#endif
