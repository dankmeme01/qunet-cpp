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
};

class Socket;

class MultiPoller {
public:
    void addSocket(qsox::BaseSocket& socket, qsox::PollType interest);
    void removeSocket(qsox::BaseSocket& socket);

    void addPipe(const PollPipe& pipe, qsox::PollType interest);
    void removePipe(const PollPipe& pipe);

    void addQSocket(qn::Socket& socket, qsox::PollType interest);
    void removeQSocket(qn::Socket& socket);

    void clear();

    struct PollResult {
        size_t which;
        MultiPoller& poller;

        bool isSocket(const qsox::BaseSocket& socket) const;
        bool isPipe(const PollPipe& pipe) const;
        bool isQSocket(const qn::Socket& socket) const;
    };

    /// Polls all the registered handles. Note: **this does not clear readiness.**
    /// Depending on the platform, this function may either return immediately if a handle is ready, or it may block until another event occurs.
    /// Even if you read data from a socket, this function will still consider it ready during the next call.
    /// You must manually clear the readiness. For pipes, call `consume()` on the pipe.
    /// For sockets, call `clearReadiness()` on this poller with the socket, **before** receiving data.
    std::optional<PollResult> poll(const std::optional<asp::time::Duration>& timeout);

    void clearReadiness(qsox::BaseSocket& socket);
    void clearReadiness(qn::Socket& socket);

private:
    asp::Mutex<void, true> m_mtx;

    struct HandleMeta {
        enum class Type {
            Socket,
            Pipe,
            QSocket,
        } type;

        union {
            qsox::SockFd origFd = -1; // used on windows for sockets
            qn::Socket* qsocket; // used for qn::Socket
        };
    };

    std::vector<HandleMeta> m_metas;

#ifdef _WIN32
    std::vector<void*> m_handles;
#else
    std::vector<struct pollfd> m_handles;
#endif

    size_t findHandle(const qsox::BaseSocket& fd) const;
    size_t findHandle(const PollPipe& fd) const;
    size_t findHandle(const qn::Socket& fd) const;

    qsox::SockFd readFdForQSocket(const qn::Socket& socket) const;

    void addHandle(HandleMeta meta, qsox::SockFd fd, qsox::PollType interest);
    void removeByIdx(size_t idx);
    void runCleanupFor(size_t idx);
    void cleanupQSocket(size_t idx);
};

}