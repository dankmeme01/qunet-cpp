#ifndef _WIN32
#include "QuicConnection.hpp"
#include <sys/poll.h>

using namespace asp::time;

namespace qn {

void QuicConnection::thrPlatformSetup() {
    auto makePipe = [](int& readFd, int& writeFd) {
        int fds[2];
        ::pipe(fds);

        readFd = fds[0];
        writeFd = fds[1];
    };

    makePipe(wrbPipeRead, wrbPipeWrite);
    makePipe(ackPipeRead, ackPipeWrite);
    makePipe(recvPipeRead, recvPipeWrite);
}

void QuicConnection::thrPlatformCleanup() {
    auto closePipe = [](int& pipeFd) {
        if (pipeFd != -1) {
            ::close(pipeFd);
            pipeFd = -1;
        }
    };

    closePipe(wrbPipeRead);
    closePipe(wrbPipeWrite);
    closePipe(ackPipeRead);
    closePipe(ackPipeWrite);
    closePipe(recvPipeRead);
    closePipe(recvPipeWrite);
}

static bool pollPipe(int fd, const Duration& timeout) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    int t = timeout.millis();
    if (t == 0) {
        t = -1;
    }

    int res = ::poll(&pfd, 1, t);
    if (res <= 0) {
        return false; // timeout or error
    }

    if (pfd.revents & POLLIN) {
        // Read to clear the pipe
        char buf[16];
        ::read(fd, buf, sizeof(buf));
        return true;
    }

    return false;
}

void QuicConnection::notifyWritable(asp::MutexGuard<void>& lock) {
    // if there are no waiters, do nothing
    if (m_ackPipeWaiters == 0) {
        lock.unlock();
        return;
    }

    lock.unlock();

    // notify waiters
    ::write(ackPipeWrite, "x", 1);
}

bool QuicConnection::waitUntilWritable(const Duration& timeout, asp::MutexGuard<void>& lock) {
    m_ackPipeWaiters++;
    lock.unlock();

    bool result = pollPipe(ackPipeRead, timeout);

    lock.relock();
    m_ackPipeWaiters--;

    return result;
}

void QuicConnection::notifyReadable(asp::MutexGuard<void>& lock) {
    // if there are no waiters, do nothing
    if (m_recvPipeWaiters == 0) {
        lock.unlock();
        return;
    }

    lock.unlock();

    // notify waiters
    ::write(recvPipeWrite, "x", 1);
}

bool QuicConnection::waitUntilReadable(const Duration& timeout, asp::MutexGuard<void>& lock) {
    m_recvPipeWaiters++;
    lock.unlock();

    bool result = pollPipe(recvPipeRead, timeout);

    lock.relock();
    m_recvPipeWaiters--;

    return result;
}

void QuicConnection::notifyDataWritten() {
    ::write(wrbPipeWrite, "x", 1);
}

QuicConnection::ThrPollResult QuicConnection::thrPoll(const asp::time::Duration& timeout) {
    // Wait until the socket is readable or the pipe is readable (meaning data was written to the stream)
    struct pollfd fds[2];
    fds[0].fd = m_socket->handle();
    fds[0].events = POLLIN;
    fds[1].fd = wrbPipeRead;
    fds[1].events = POLLIN;

    ::poll(fds, 2, timeout.millis());

    ThrPollResult res;
    if (fds[0].revents & POLLIN) {
        res.sockReadable = true;
    }

    if (fds[1].revents & POLLIN) {
        res.newDataAvail = true;

        // Read the pipe to clear it
        char buf[16];
        ::read(wrbPipeRead, buf, sizeof(buf));
    }

    return res;
}

}

#endif