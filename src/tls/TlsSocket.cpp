#include <qunet/tls/TlsSocket.hpp>
#include <qunet/Log.hpp>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

using namespace qsox;
using arc::Future, arc::Interest;

namespace qn {

TlsSocket::TlsSocket(
    qsox::TcpStream stream,
    arc::Registration io,
    TcpTlsSession tls
) : EventIoBase(std::move(io)), m_stream(std::move(stream)), m_tls(std::move(tls)) {
    m_tls.associateSocket(m_stream.handle());
}

TlsSocket::~TlsSocket() {
    // deregister io before destroying the socket
    this->unregister();

    m_tls.dissociateSocket();
}

Future<TransportResult<TlsSocket>> TlsSocket::connect(
    SocketAddress address,
    std::string_view serverName
) {
    ARC_FRAME();
    auto ctx = ARC_CO_UNWRAP(TcpTlsContext::create());
    co_return co_await TlsSocket::connect(address, ctx, serverName);
}

// Creates a new TLS stream, connecting to the given address and using the given TLS context.
Future<TransportResult<TlsSocket>> TlsSocket::connect(
    SocketAddress address,
    std::shared_ptr<TcpTlsContext> tlsContext,
    std::string_view serverName
) {
    ARC_FRAME();

    ARC_CO_UNWRAP_INTO(auto stream, qsox::TcpStream::connectNonBlocking(address));
    ARC_CO_UNWRAP(stream.setNonBlocking(false));

    // create a TlsSocket immediately for raii
    auto rio = arc::ctx().runtime()->ioDriver().registerIo(stream.handle(), arc::Interest::ReadWrite);
    auto tls = ARC_CO_UNWRAP(TcpTlsSession::create(tlsContext, address, serverName));

    TlsSocket out {
        std::move(stream),
        std::move(rio),
        std::move(tls),
    };

    // wait until writable
    ARC_CO_UNWRAP(co_await out.pollWritable());

    auto err = out.m_stream.getSocketError();
    if (err != Error::Success) {
        co_return Err(err);
    }

    // do TLS handshake
    log::debug("starting TLS handshake");
    ARC_CO_UNWRAP(co_await out.handshake());
    log::debug("TLS handshake complete!");

    co_return Ok(std::move(out));
}

Future<TransportResult<void>> TlsSocket::handshake() {
    return this->rioPoll([this](uint64_t& id) {
        return this->pollHandshake(id);
    });
}

Future<NetResult<void>> TlsSocket::shutdown(ShutdownMode mode) {
    ARC_CO_UNWRAP(m_stream.shutdown(mode));
    co_return Ok();
}

NetResult<void> TlsSocket::setNoDelay(bool noDelay) {
    return m_stream.setNoDelay(noDelay);
}

Future<TransportResult<size_t>> TlsSocket::send(const void* data, size_t size) {
    return this->rioPoll([this, data, size](uint64_t& id) {
        return this->pollWrite(data, size, id);
    });
}

Future<TransportResult<void>> TlsSocket::sendAll(const void* datav, size_t size) {
    const char* data = reinterpret_cast<const char*>(datav);
    size_t remaining = size;

    uint64_t id = 0;

    TransportResult<void> result = Ok();
    while (remaining > 0) {
        auto res = co_await arc::pollFunc([&] {
            return this->pollWrite(data, remaining, id);
        });

        if (!res) {
            result = Err(res.unwrapErr());
            break;
        }

        auto n = res.unwrap();
        data += n;
        remaining -= n;
    }

    if (id != 0) {
        m_io.unregister(id);
    }

    co_return result;
}

Future<TransportResult<size_t>> TlsSocket::receive(void* buffer, size_t size) {
    return this->rioPoll([this, buffer, size](uint64_t& id) {
        return this->pollRead(buffer, size, id);
    });
}

Future<TransportResult<void>> TlsSocket::receiveExact(void* buffer, size_t size) {
    char* buf = reinterpret_cast<char*>(buffer);
    size_t remaining = size;

    uint64_t id = 0;

    TransportResult<void> result = Ok();
    while (remaining > 0) {
        auto res = co_await arc::pollFunc([&] {
            return this->pollRead(buf, remaining, id);
        });

        if (!res) {
            result = Err(res.unwrapErr());
            break;
        }

        auto n = res.unwrap();
        buf += n;
        remaining -= n;
    }

    if (id != 0) {
        m_io.unregister(id);
    }

    co_return result;
}

NetResult<qsox::SocketAddress> TlsSocket::localAddress() const {
    return m_stream.localAddress();
}

NetResult<qsox::SocketAddress> TlsSocket::remoteAddress() const {
    return m_stream.remoteAddress();
}

std::optional<TransportResult<size_t>> TlsSocket::pollWrite(const void* data, size_t size, uint64_t& id) {
    size_t written = 0;

    return this->pollTls(id, Interest::ReadWrite, [&] {
        int bytes = wolfSSL_write(m_tls.handle(), data, (int)size);
        if (bytes <= 0) return -1;
        written += (size_t)bytes;
        return (int)WOLFSSL_SUCCESS;
    }).transform([&](auto&& res) { return res.map([&] { return written; }); });
}

std::optional<TransportResult<size_t>> TlsSocket::pollRead(void* buf, size_t size, uint64_t& id, bool peek) {
    size_t read = 0;

    return this->pollTls(id, Interest::ReadWrite, [&] {
        int bytes = wolfSSL_read(m_tls.handle(), buf, (int)size);
        if (bytes <= 0) return -1;
        read += (size_t)bytes;
        return (int)WOLFSSL_SUCCESS;
    }).transform([&](auto&& res) { return res.map([&] { return read; }); });
}

std::optional<TransportResult<>> TlsSocket::pollHandshake(uint64_t& id) {
    return this->pollTls(id, Interest::ReadWrite, [&] {
        return wolfSSL_connect(m_tls.handle());
    });
}

/// This function is needed because wolfSSL calls can return both WANT_READ or WANT_WRITE,
/// even if we are strictly wanting one of those at a time. It's silly, but it is what it is.
std::optional<TransportResult<>> TlsSocket::pollTls(
    uint64_t& id,
    Interest interest,
    std23::function_ref<int()> fn
) {
    while (true) {
        auto ready = m_io.pollReady(interest | Interest::Error, id);
        if (ready == 0) {
            return std::nullopt;
        } else if (ready & Interest::Error) {
            if (auto err = this->takeOrClearError()) {
                return Err(*err);
            } else {
                continue;
            }
        }

        auto res = fn();
        if (res == WOLFSSL_SUCCESS) {
            return Ok();
        }

        auto err = lastTlsError();
        if (err == SSL_ERROR_WANT_READ) {
            interest = Interest::Readable;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            interest = Interest::Writable;
        } else {
            return Err(err);
        }

        // clear readiness since we know it's not ready
        m_io.clearReadiness(interest);
    }
}

}
