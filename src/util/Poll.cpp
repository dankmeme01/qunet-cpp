#include <qunet/util/Poll.hpp>
#include <qunet/util/assert.hpp>
#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/QuicTransport.hpp>
#include "../socket/transport/quic/QuicConnection.hpp"
#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>

namespace qn {

void MultiPoller::addSocket(qsox::BaseSocket& socket, qsox::PollType interest) {
    auto _lock = m_mtx.lock();

    return this->addHandle(HandleMeta { HandleMeta::Type::Socket, }, socket.handle(), interest);
}

void MultiPoller::removeSocket(qsox::BaseSocket& socket) {
    auto _lock = m_mtx.lock();

    this->removeByIdx(this->findHandle(socket));
}

void MultiPoller::removePipe(const PollPipe& pipe) {
    auto _lock = m_mtx.lock();

    this->removeByIdx(this->findHandle(pipe));
}

qsox::SockFd MultiPoller::readFdForQSocket(const qn::Socket& socket) const {
    auto trans = socket.transport();

    qsox::SockFd fd = -1;

    if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(trans)) {
        auto rpipe = quic->connection().m_readablePipe.lock();

        QN_ASSERT(!rpipe->has_value() && "Readable pipe already exists for QUIC connection");

        rpipe->emplace(PollPipe{});
        fd = rpipe->value().m_readFd;
    } else if (auto tcp = std::dynamic_pointer_cast<qn::TcpTransport>(trans)) {
        fd = tcp->m_socket.handle();
    } else if (auto udp = std::dynamic_pointer_cast<qn::UdpTransport>(trans)) {
        fd = udp->m_socket.handle();
    }

    return fd;
}

void MultiPoller::addQSocket(qn::Socket& socket, qsox::PollType interest) {
    auto fd = this->readFdForQSocket(socket);

    QN_ASSERT(fd != -1 && "Unknown transport type or socket handle in MultiPoller::addQSocket");

    HandleMeta meta;
    meta.type = HandleMeta::Type::QSocket;
    meta.qsocket = &socket;

    this->addHandle(meta, fd, interest);
}

void MultiPoller::removeQSocket(qn::Socket& socket) {
    auto _lock = m_mtx.lock();

    this->removeByIdx(this->findHandle(socket));
}

void MultiPoller::removeByIdx(size_t idx) {
    if (idx == -1) return;

    this->runCleanupFor(idx);

    m_metas.erase(m_metas.begin() + idx);
    m_handles.erase(m_handles.begin() + idx);
}

void MultiPoller::cleanupQSocket(size_t idx) {
    auto& socket = *m_metas[idx].qsocket;
    auto trans = socket.transport();

    if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(trans)) {
        auto rpipe = quic->connection().m_readablePipe.lock();

        QN_ASSERT(rpipe->has_value() && "Readable pipe must exist for QUIC connection when cleaning up");

        rpipe->reset();
    }
}

void MultiPoller::clear() {
    auto _lock = m_mtx.lock();

    while (!m_handles.empty()) {
        this->removeByIdx(m_handles.size() - 1);
    }
}

void MultiPoller::clearReadiness(qn::Socket& socket) {
    auto trans = socket.transport();

    if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(trans)) {
        // We don't need to clear the pipe, QuicTransport handles it internally
    } else if (auto tcp = std::dynamic_pointer_cast<qn::TcpTransport>(trans)) {
        this->clearReadiness(tcp->m_socket);
    } else if (auto udp = std::dynamic_pointer_cast<qn::UdpTransport>(trans)) {
        this->clearReadiness(udp->m_socket);
    } else {
        QN_ASSERT(false && "Unknown transport type in MultiPoller::clearReadiness for qn::Socket");
    }
}

}