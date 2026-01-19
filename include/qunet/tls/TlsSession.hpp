#pragma once
#ifdef QUNET_TLS_SUPPORT

#include "TlsContext.hpp"

namespace qn {

struct TlsSessionBase {
    std::unique_ptr<WOLFSSL, WolfsslDeleter> m_ssl;

    WOLFSSL* handle() const;
    TlsResult<> useSNI(std::string_view name);

    void associateSocket(qsox::SockFd handle);
    void dissociateSocket();

    virtual ~TlsSessionBase() = default;
    TlsSessionBase(WOLFSSL* ssl) : m_ssl(ssl) {}
    TlsSessionBase(TlsSessionBase&&) = default;
    TlsSessionBase& operator=(TlsSessionBase&&) = default;
};

template <typename Ctx = TlsContext>
class TlsSession : public TlsSessionBase {
public:
    using Context = Ctx;

    virtual ~TlsSession() = default;
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    TlsSession(TlsSession&&) = default;
    TlsSession& operator=(TlsSession&&) = default;

protected:
    std::shared_ptr<Context> m_context;

    TlsSession(std::shared_ptr<Context> context, WOLFSSL* ssl)
        : TlsSessionBase(ssl), m_context(std::move(context)) {}
};

}

#endif
