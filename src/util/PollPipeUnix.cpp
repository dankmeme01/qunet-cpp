#include <qunet/util/PollPipe.hpp>

#ifndef _WIN32

#include <unistd.h>
#include <fcntl.h>

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

}

#endif // _WIN32
