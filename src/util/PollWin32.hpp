#pragma once

#include <qunet/util/Poll.hpp>
#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/QuicTransport.hpp>
#include "../socket/transport/quic/QuicConnection.hpp"
#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>

#include <Ws2tcpip.h>

static void* createWsaEvent(qsox::SockFd socket, qsox::PollType interest) {
    WSAEVENT ev = WSACreateEvent();
    QN_ASSERT(ev != WSA_INVALID_EVENT);

    long flags = 0;
    if (interest == qsox::PollType::Read) {
        flags = FD_READ | FD_CLOSE;
    } else if (interest == qsox::PollType::Write) {
        flags = FD_WRITE | FD_CLOSE;
    }

    int r = WSAEventSelect(socket, ev, flags);
    QN_ASSERT(r == 0 && "Failed to associate event with socket");

    return (void*) ev;
}

namespace qn {

class MultiPoller::Impl {
public:
    void addSocket(qsox::BaseSocket& socket, qsox::PollType interest) {
        auto event = createWsaEvent(socket.handle(), interest);
        this->addHandle(event, socket.handle());
    }

    void removeSocket(qsox::BaseSocket& socket) {
        this->removeEventForSocket(socket.handle());
    }

    void addPipe(const PollPipe& pipe, qsox::PollType interest) {
        this->addHandle((void*) pipe.readFd(), pipe.readFd());
    }

    void removePipe(const PollPipe& pipe) {
        this->removeHandle((void*) pipe.readFd());
    }

    void addQSocket(qn::Socket& socket, qsox::PollType interest) {
        auto trans = socket.transport();

        if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(trans)) {
            auto rpipe = quic->connection().m_readablePipe.lock();
            QN_ASSERT(!rpipe->has_value() && "Readable pipe already exists for QUIC connection");

            rpipe->emplace(PollPipe{});
            this->addPipe(rpipe->value(), interest);
        } else if (auto tcp = std::dynamic_pointer_cast<qn::TcpTransport>(trans)) {
            auto event = createWsaEvent(tcp->m_socket.handle(), interest);
            this->addHandle(event, tcp->m_socket.handle());
        } else if (auto udp = std::dynamic_pointer_cast<qn::UdpTransport>(trans)) {
            auto event = createWsaEvent(udp->m_socket.handle(), interest);
            this->addHandle(event, udp->m_socket.handle());
        } else {
            QN_ASSERT(false && "Unknown transport type in MultiPoller::addQSocket");
        }
    }

    void removeQSocket(qn::Socket& socket) {
        auto _lock = m_mtx.lock();

        if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(socket.transport())) {
            auto rpipe = quic->connection().m_readablePipe.lock();
            QN_ASSERT(rpipe->has_value() && "Readable pipe does not exist for QUIC connection");

            this->removePipe(rpipe->value());
            rpipe->reset(); // clear the pipe
        } else if (auto tcp = std::dynamic_pointer_cast<qn::TcpTransport>(socket.transport())) {
            this->removeEventForSocket(tcp->m_socket.handle());
        } else if (auto udp = std::dynamic_pointer_cast<qn::UdpTransport>(socket.transport())) {
            this->removeEventForSocket(udp->m_socket.handle());
        }
    }

    std::optional<PollResult> poll(const std::optional<asp::time::Duration>& timeout, MultiPoller& poller) {
        PollResult res {0, poller};

        auto _lock = m_mtx.lock();
        if (m_handles.empty()) {
            return std::nullopt;
        }

        DWORD waitTime = timeout.has_value() ? static_cast<DWORD>(timeout->millis()) : INFINITE;
        DWORD result = WaitForMultipleObjects(
            static_cast<DWORD>(m_handles.size()),
            m_handles.data(),
            FALSE,
            waitTime
        );

        if (result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + m_handles.size()) {
            return std::nullopt; // no events or error
        }

        res.which = result - WAIT_OBJECT_0;

        return res;
    }

    void clearReadiness(qsox::BaseSocket& socket) {
        // https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaeventselect
        // > With these semantics, an application need not read all available data in response to an FD_READ networkevent—
        // > a single recv in response to each FD_READ network event is appropriate.

        // we have to reset the event for the socket, otherwise next call to poll() will immediately return.
        // when the application eventually reads data from the socket, the event will be set again if data is available.

        auto _lock = m_mtx.lock();
        size_t idx = this->findEventForSocket(socket.handle());

        if (idx == -1) {
            QN_ASSERT(false && "Socket not found in poller");
            return;
        }

        WSAResetEvent(m_handles[idx]);
    }

    void clearReadiness(qn::Socket& socket) {
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

private:
    friend class MultiPoller::PollResult;
    asp::Mutex<void, true> m_mtx;

    std::vector<void*> m_handles;
    std::vector<qsox::SockFd> m_associations;

    void addHandle(void* fd, qsox::SockFd associated) {
        auto _lock = m_mtx.lock();
        m_handles.emplace_back(fd);
        m_associations.emplace_back(associated);
    }

    void removeEventForSocket(qsox::SockFd associated) {
        auto _lock = m_mtx.lock();
        auto idx = this->findEventForSocket(associated);

        QN_DEBUG_ASSERT(idx != -1 && "Tried to remove an event for a socket that was not registered in the poller");

        WSACloseEvent(m_handles[idx]);
        m_handles.erase(m_handles.begin() + idx);
        m_associations.erase(m_associations.begin() + idx);
    }

    size_t findEventForSocket(qsox::SockFd associated) {
        auto _lock = m_mtx.lock();

        for (size_t i = 0; i < m_associations.size(); i++) {
            if (m_associations[i] == associated) {
                return i;
            }
        }

        return -1; // not found
    }

    void removeHandle(void* handle) {
        auto _lock = m_mtx.lock();

        for (auto it = m_handles.begin(); it != m_handles.end(); ++it) {
            if (*it == handle) {
                m_handles.erase(it);
                return;
            }
        }

        QN_DEBUG_ASSERT(false && "Tried to remove a handle that was not registered in the poller");
    }
};

qsox::SockFd MultiPoller::PollResult::fd() const {
    return (qsox::SockFd) poller.m_impl->m_associations[which];
}

bool MultiPoller::PollResult::isSocket(const qsox::BaseSocket& socket) const {
    return this->fd() == socket.handle();
}

bool MultiPoller::PollResult::isPipe(const PollPipe& pipe) const {
    return this->fd() == pipe.readFd() || this->fd() == pipe.writeFd();
}

bool MultiPoller::PollResult::isQSocket(const qn::Socket& socket) const {
    auto trans = socket.transport();

    if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(trans)) {
        auto rpipe = quic->connection().m_readablePipe.lock();

        if (*rpipe) {
            return this->fd() == rpipe->value().readFd();
        } else {
            return false;
        }
    } else if (auto tcp = std::dynamic_pointer_cast<qn::TcpTransport>(trans)) {
        return this->fd() == tcp->m_socket.handle();
    } else if (auto udp = std::dynamic_pointer_cast<qn::UdpTransport>(trans)) {
        return this->fd() == udp->m_socket.handle();
    } else {
        QN_ASSERT(false && "unknown transport type");
    }

    return false;
}

}