#pragma once

#include <qunet/util/Poll.hpp>
#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/QuicTransport.hpp>
#include "../socket/transport/quic/QuicConnection.hpp"
#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>

#include <sys/poll.h>

namespace qn {

class MultiPoller::Impl {
public:
    void addSocket(qsox::BaseSocket& socket, qsox::PollType interest) {
        this->addFd(socket.handle(), interest);
    }

    void removeSocket(qsox::BaseSocket& socket) {
        this->removeEvent(socket.handle());
    }

    void addPipe(const PollPipe& pipe, qsox::PollType interest) {
        this->addFd(pipe.readFd(), interest);
    }

    void removePipe(const PollPipe& pipe) {
        this->removeEvent(pipe.readFd());
    }

    void addQSocket(qn::Socket& socket, qsox::PollType interest) {
        auto trans = socket.transport();

        qsox::SockFd fd = -1;
        if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(trans)) {
            auto rpipe = quic->connection().m_readablePipe.lock();
            QN_ASSERT(!rpipe->has_value() && "Readable pipe already exists for QUIC connection");

            rpipe->emplace(PollPipe{});
            fd = rpipe->value().readFd();
        } else if (auto tcp = std::dynamic_pointer_cast<qn::TcpTransport>(trans)) {
            fd = tcp->m_socket.handle();
        } else if (auto udp = std::dynamic_pointer_cast<qn::UdpTransport>(trans)) {
            fd = udp->m_socket.handle();
        }

        QN_ASSERT(fd != -1 && "Unknown transport type or socket handle in MultiPoller::addQSocket");

        this->addFd(fd, interest);
    }

    void removeQSocket(qn::Socket& socket) {
        auto _lock = m_mtx.lock();

        if (auto quic = std::dynamic_pointer_cast<qn::QuicTransport>(socket.transport())) {
            auto rpipe = quic->connection().m_readablePipe.lock();
            if (rpipe->has_value()) {
                this->removeEvent(rpipe->value().readFd());
                rpipe->reset(); // clear the pipe
            }
        } else if (auto tcp = std::dynamic_pointer_cast<qn::TcpTransport>(socket.transport())) {
            this->removeEvent(tcp->m_socket.handle());
        } else if (auto udp = std::dynamic_pointer_cast<qn::UdpTransport>(socket.transport())) {
            this->removeEvent(udp->m_socket.handle());
        }
    }

    std::optional<PollResult> poll(const std::optional<asp::time::Duration>& timeout, MultiPoller& poller) {
        PollResult res {0, poller};

        auto _lock = m_mtx.lock();
        if (m_fds.empty()) {
            return std::nullopt;
        }

        ::poll(m_fds.data(), m_fds.size(), timeout.has_value() ? timeout->millis() : -1);

        for (size_t i = 0; i < m_fds.size(); i++) {
            auto& pfd = m_fds[i];

            if (pfd.revents & (POLLIN | POLLPRI)) {
                res.which = i;
                return res;
            }
        }

        return std::nullopt;
    }

    size_t trackedCount() const {
        return m_fds.size();
    }

    // Fortunately, on sane systems we don't need to do anything here.
    void clearReadiness(qsox::BaseSocket& socket) {}
    void clearReadiness(qn::Socket& socket) {}

private:
    friend class MultiPoller::PollResult;
    asp::Mutex<void, true> m_mtx;
    std::vector<struct pollfd> m_fds;

    void addFd(qsox::SockFd fd, qsox::PollType interest) {
        auto _lock = m_mtx.lock();

        m_fds.emplace_back(pollfd {
            .fd = fd,
            .events = (short)((interest == qsox::PollType::Read) ? POLLIN : POLLOUT),
            .revents = 0
        });
    }

    void removeEvent(qsox::SockFd fd) {
        auto _lock = m_mtx.lock();

        for (auto it = m_fds.begin(); it != m_fds.end(); ++it) {
            if (it->fd == fd) {
                m_fds.erase(it);
                return;
            }
        }
    }
};

inline qsox::SockFd MultiPoller::PollResult::fd() const {
    return poller.m_impl->m_fds[which].fd;
}

inline bool MultiPoller::PollResult::isSocket(const qsox::BaseSocket& socket) const {
    return this->fd() == socket.handle();
}

inline bool MultiPoller::PollResult::isPipe(const PollPipe& pipe) const {
    return this->fd() == pipe.readFd() ||
            this->fd() == pipe.writeFd();
}

inline bool MultiPoller::PollResult::isQSocket(const qn::Socket& socket) const {
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