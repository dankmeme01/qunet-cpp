#pragma once
#ifdef QUNET_TLS_SUPPORT

#include <qunet/socket/transport/Error.hpp>
#include <qsox/SocketAddress.hpp>
#include <filesystem>

struct WOLFSSL_CTX;
struct WOLFSSL;

namespace qn {

struct WolfsslCtxDeleter {
    void operator()(WOLFSSL_CTX* ctx) const;
};
struct WolfsslDeleter {
    void operator()(WOLFSSL* ssl) const;
};

TlsError lastTlsError();

template <typename T = void>
using TlsResult = geode::Result<T, TlsError>;

TlsResult<> tlsWrap(int rcode);

struct TlsOptions {
    /// CA certificate options. Priority: insecure > caCert > caCertPath

    /// Whether to skip certificate verification. Not recommended for production use.
    bool insecure = false;
    /// Path to a custom CA certificate file in PEM format.
    std::filesystem::path caCertPath;
    /// A custom CA certificate string in PEM format.
    std::string caCerts;
};

class TlsContext {
public:
    virtual ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    TlsContext(TlsContext&&) noexcept = default;
    TlsContext& operator=(TlsContext&&) noexcept = default;

    WOLFSSL_CTX* handle() const;

protected:
    TlsContext(WOLFSSL_CTX* ctx);

    std::unique_ptr<WOLFSSL_CTX, WolfsslCtxDeleter> m_ctx;

    TlsResult<> configure(const TlsOptions&);
    TlsResult<> loadCerts(std::string_view certs);
    TlsResult<> loadCerts(const std::filesystem::path& certs);
    TlsResult<> loadSystemCerts();
};

}

#endif
