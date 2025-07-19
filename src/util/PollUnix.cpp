#include <qunet/util/Poll.hpp>

#ifndef _WIN32

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

using namespace asp::time;

namespace qn {

PollPipe::PollPipe() {
    int fds[2];
    ::pipe(fds);
    m_readFd = fds[0];
    m_writeFd = fds[1];
}

PollPipe::PollPipe(PollPipe&& other) noexcept {
    m_readFd = other.m_readFd;
    m_writeFd = other.m_writeFd;
    other.m_readFd = -1;
    other.m_writeFd = -1;
}

PollPipe& PollPipe::operator=(PollPipe&& other) noexcept {
    if (this != &other) {
        if (m_readFd != -1) {
            ::close(m_readFd);
        }
        if (m_writeFd != -1) {
            ::close(m_writeFd);
        }

        m_readFd = other.m_readFd;
        m_writeFd = other.m_writeFd;
        other.m_readFd = -1;
        other.m_writeFd = -1;
    }
    return *this;
}

PollPipe::~PollPipe() {
    if (m_readFd != -1) {
        ::close(m_readFd);
    }

    if (m_writeFd != -1) {
        ::close(m_writeFd);
    }
}

void PollPipe::notify() {
    if (m_writeFd != -1) {
        ::write(m_writeFd, "x", 1);
    }
}

void PollPipe::consume() {
    if (m_readFd == -1) {
        return; // nothing to consume
    }

    char buf[32];
    ::read(m_readFd, buf, sizeof(buf));
}

void PollPipe::clear() {
    if (m_readFd == -1) {
        return;
    }

    int oldFlags = fcntl(m_readFd, F_GETFL, 0);

    fcntl(m_readFd, F_SETFL, oldFlags | O_NONBLOCK);

    while (true) {
        char buf[32];
        ssize_t bytesRead = ::read(m_readFd, buf, sizeof(buf));
        if (bytesRead <= 0) {
            break;
        }
    }

    fcntl(m_readFd, F_SETFL, oldFlags);
}

qsox::SockFd PollPipe::readFd() const {
    return (qsox::SockFd) m_readFd;
}

qsox::SockFd PollPipe::writeFd() const {
    return (qsox::SockFd) m_writeFd;
}

void MultiPoller::addPipe(const PollPipe& pipe, qsox::PollType interest) {
    auto _lock = m_mtx.lock();

    this->addHandle(
        HandleMeta { HandleMeta::Type::Pipe },
        interest == qsox::PollType::Read ? pipe.m_readFd : pipe.m_writeFd,
        interest
    );
}

void MultiPoller::addHandle(HandleMeta meta, qsox::SockFd fd, qsox::PollType interest) {
    m_metas.push_back(meta);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = (interest == qsox::PollType::Read) ? POLLIN : POLLOUT;
    pfd.revents = 0;
    m_handles.push_back(pfd);
}

size_t MultiPoller::findHandle(const qsox::BaseSocket& s) const {
    for (size_t i = 0; i < m_handles.size(); ++i) {
        if (m_handles[i].fd == s.handle()) {
            return i;
        }
    }

    return -1; // not found
}

size_t MultiPoller::findHandle(const PollPipe& fd) const {
    for (size_t i = 0; i < m_handles.size(); ++i) {
        if (m_handles[i].fd == fd.m_readFd || m_handles[i].fd == fd.m_writeFd) {
            return i;
        }
    }

    return -1; // not found
}

size_t MultiPoller::findHandle(const qn::Socket& sock) const {
    for (size_t i = 0; i < m_handles.size(); ++i) {
        if (m_metas[i].type == HandleMeta::Type::QSocket && m_metas[i].qsocket == &sock) {
            return i;
        }
    }

    return -1; // not found
}

void MultiPoller::runCleanupFor(size_t idx) {
    auto _lock = m_mtx.lock();

    switch (m_metas[idx].type) {
        case HandleMeta::Type::Socket: break;
        case HandleMeta::Type::Pipe: break;
        case HandleMeta::Type::QSocket: {
            this->cleanupQSocket(idx);
        } break;
    }
}

bool MultiPoller::PollResult::isSocket(const qsox::BaseSocket& socket) const {
    return poller.m_handles[which].fd == socket.handle();
}

bool MultiPoller::PollResult::isPipe(const PollPipe& pipe) const {
    return (poller.m_metas[which].type == HandleMeta::Type::Pipe) &&
           (poller.m_handles[which].fd == pipe.m_readFd || poller.m_handles[which].fd == pipe.m_writeFd);
}

bool MultiPoller::PollResult::isQSocket(const qn::Socket& socket) const {
    return (poller.m_metas[which].type == HandleMeta::Type::QSocket) &&
           (poller.m_metas[which].qsocket == &socket);
}

std::optional<MultiPoller::PollResult> MultiPoller::poll(const std::optional<Duration>& timeout) {
    PollResult res {0, *this};

    auto _lock = m_mtx.lock();

    if (m_handles.empty()) {
        return std::nullopt; // nothing to poll
    }

    ::poll(m_handles.data(), m_handles.size(), timeout.has_value() ? timeout->millis() : -1);

    for (size_t i = 0; i < m_handles.size(); ++i) {
        auto& meta = m_metas[i];
        auto& handle = m_handles[i];

        if (handle.revents & (POLLIN | POLLOUT)) {
            res.which = i;
            return res;
        }
    }

    return std::nullopt;
}

void MultiPoller::clearReadiness(qsox::BaseSocket& socket) {
    // Fortunately, on sane systems we don't need to do anything here.
}

}

#endif
