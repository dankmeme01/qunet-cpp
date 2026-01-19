#include <qunet/tls/TcpTlsContext.hpp>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace qn {

TcpTlsContext::TcpTlsContext(WOLFSSL_CTX* ctx) : TlsContext(ctx) {}

TlsResult<std::shared_ptr<TcpTlsContext>> TcpTlsContext::create(const TcpTlsOptions& options) {
    auto ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());

    if (!ctx) {
        return Err(wolfSSL_ERR_get_error());
    }

    auto context = std::make_shared<TcpTlsContext>(ctx);
    GEODE_UNWRAP(context->configure(options));

    return Ok(std::move(context));
}

}
