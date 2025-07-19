#pragma once

#include <asp/time/Duration.hpp>
#include <asp/sync/Mutex.hpp>
#include <qsox/BaseSocket.hpp>
#include <qsox/Poll.hpp>
#include <vector>
#include <memory>

#include "PollPipe.hpp"

namespace qn {

class Socket;

class MultiPoller {
public:
    MultiPoller();
    ~MultiPoller();

    void addSocket(qsox::BaseSocket& socket, qsox::PollType interest);
    void removeSocket(qsox::BaseSocket& socket);

    void addPipe(const PollPipe& pipe, qsox::PollType interest);
    void removePipe(const PollPipe& pipe);

    void addQSocket(qn::Socket& socket, qsox::PollType interest);
    void removeQSocket(qn::Socket& socket);

    struct PollResult {
        size_t which;
        MultiPoller& poller;

        qsox::SockFd fd() const;

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
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}