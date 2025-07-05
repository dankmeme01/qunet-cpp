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