#pragma once

#include <qsox/BaseSocket.hpp>

namespace qn {

/// A simple synchronization primitive that allows one thread to wait for an event,
/// and another thread to signal that event.
class PollPipe {
public:
    PollPipe();
    ~PollPipe();

    PollPipe(const PollPipe&) = delete;
    PollPipe& operator=(const PollPipe&) = delete;
    PollPipe(PollPipe&&) noexcept;
    PollPipe& operator=(PollPipe&&) noexcept;

    void notify();

    // Blocks until notify has been called.
    void consume();

    // Clears the readiness of the pipe without blocking.
    void clear();

private:
    friend class MultiPoller;

#ifdef _WIN32
    void* m_event;
#else
    int m_readFd;
    int m_writeFd;
#endif

    qsox::SockFd readFd() const;
    qsox::SockFd writeFd() const;
};

}