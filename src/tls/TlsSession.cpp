#include <qunet/tls/TlsSession.hpp>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace qn {

WOLFSSL* TlsSessionBase::handle() const {
    return m_ssl.get();
}

TlsResult<> TlsSessionBase::useSNI(std::string_view name) {
    if (name.empty()) return Ok();

    return tlsWrap(wolfSSL_UseSNI(this->handle(), WOLFSSL_SNI_HOST_NAME, name.data(), name.size()));
}

void TlsSessionBase::associateSocket(qsox::SockFd handle) {
    wolfSSL_set_fd(this->handle(), (int)handle);
}

void TlsSessionBase::dissociateSocket() {
    wolfSSL_set_fd(this->handle(), -1);
}

TlsError TlsSessionBase::myLastError(int rcode) const {
    return TlsError(wolfSSL_get_error(this->handle(), rcode));
}

TlsResult<> TlsSessionBase::myWrapTls(int rcode) const {
    auto err = this->myLastError(rcode);
    if (err.ok()) {
        return Ok();
    } else {
        return Err(err);
    }
}

}
