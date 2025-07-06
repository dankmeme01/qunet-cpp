#pragma once

#include <asp/time/Duration.hpp>
#include <asp/sync/Mutex.hpp>
#include <qsox/BaseSocket.hpp>
#include <qsox/Poll.hpp>
#include <vector>

#ifndef _WIN32
# include <sys/poll.h>
#endif

namespace qn {

/// A simple synchronization primitive that allows one thread to wait for an event,
/// and another thread to signal that event.
class PollPipe {
public:
    PollPipe();
    ~PollPipe();

    PollPipe(const PollPipe&) = delete;
    PollPipe& operator=(const PollPipe&) = delete;
    PollPipe(PollPipe&&) noexcept = delete;
    PollPipe& operator=(PollPipe&&) noexcept = delete;

    void notify();

    // Blocks until notify has been called.
    void consume();

private:
    friend class MultiPoller;

#ifdef _WIN32
    void* m_event;
#else
    int m_readFd;
    int m_writeFd;
#endif
};

class MultiPoller {
public:
    void addSocket(qsox::BaseSocket& socket, qsox::PollType interest);
    void addSocket(qsox::BaseSocket::SockFd socket, qsox::PollType interest);
    void removeSocket(qsox::BaseSocket& socket);
    void removeSocket(qsox::BaseSocket::SockFd fd);

    void addPipe(const PollPipe& pipe, qsox::PollType interest);

    void clear();

    struct PollResult {
        size_t which;
        MultiPoller& poller;

        bool isSocket(const qsox::BaseSocket& socket) const;
        bool isPipe(const PollPipe& pipe) const;
    };

    std::optional<PollResult> poll(const std::optional<asp::time::Duration>& timeout);

private:
    asp::Mutex<void, true> m_mtx;

    struct HandleMeta {
        enum class Type {
            Socket,
            Pipe,
        } type;
    };

    std::vector<HandleMeta> m_metas;

#ifdef _WIN32
    std::vector<void*> m_handles;
#else
    std::vector<struct pollfd> m_handles;
#endif

    size_t findHandle(qsox::BaseSocket::SockFd fd) const;
    void addHandle(HandleMeta meta, qsox::BaseSocket::SockFd fd, qsox::PollType interest);
    void removeByIdx(size_t idx);
    void runCleanupFor(size_t idx);
};

}