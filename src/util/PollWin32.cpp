#include <qunet/util/Poll.hpp>

#ifdef _WIN32

#include <qunet/util/assert.hpp>
#include <WinSock2.h>
#include <Windows.h>

using namespace asp::time;

namespace qn {

PollPipe::PollPipe() {
    m_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    QN_ASSERT(m_event != nullptr && "Failed to create event for PollPipe");
}

PollPipe::~PollPipe() {
    CloseHandle(m_event);
}

void PollPipe::notify() {
    SetEvent(m_event);
}

void PollPipe::consume() {
    ResetEvent(m_event);
}

void MultiPoller::addPipe(const PollPipe& pipe, qsox::PollType interest) {
    auto _lock = m_mtx.lock();

    this->addHandle(
        HandleMeta { HandleMeta::Type::Pipe },
        (qsox::SockFd) pipe.m_event,
        interest
    );
}

void MultiPoller::addHandle(HandleMeta meta, qsox::SockFd fd, qsox::PollType interest) {
    // disallow duplicates
    size_t idx = this->findHandle(fd);
    if (idx != -1) {
        QN_ASSERT(false && "Handle already exists in poller");
    }

    if (meta.type == HandleMeta::Type::Socket) {
        meta.origFd = fd; // we need this later

        WSAEVENT ev = WSACreateEvent();
        QN_ASSERT(ev != WSA_INVALID_EVENT);

        long flags = 0;
        if (interest == qsox::PollType::Read) {
            flags = FD_READ | FD_CLOSE;
        } else if (interest == qsox::PollType::Write) {
            flags = FD_WRITE | FD_CLOSE;
        }

        int r = WSAEventSelect(fd, ev, flags);
        QN_ASSERT(r == 0 && "Failed to associate event with socket");

        m_handles.push_back(ev);
    } else if (meta.type == HandleMeta::Type::Pipe) {
        m_handles.push_back((void*) fd);
    } else {
        QN_ASSERT(false && "Unknown handle type");
    }

    m_metas.push_back(meta);
}

size_t MultiPoller::findHandle(qsox::SockFd fd) const {
    for (size_t i = 0; i < m_handles.size(); ++i) {
        auto& meta = m_metas[i];
        if (meta.type == HandleMeta::Type::Socket && meta.origFd == fd) {
            return i;
        }
    }

    return -1; // not found
}

void MultiPoller::runCleanupFor(size_t idx) {
    auto _lock = m_mtx.lock();

    auto& meta = m_metas[idx];
    if (meta.type == HandleMeta::Type::Socket) {
        // cleanup the event
        WSAEVENT ev = (WSAEVENT) m_handles[idx];

        // i'm not sure if we need to disassociate the event first here, by calling WSAEventSelect with 0 flags
        // WSAEventSelect(meta.origFd, ev, 0);

        WSACloseEvent(ev);
    }
}

bool MultiPoller::PollResult::isSocket(const qsox::BaseSocket& socket) const {
    if (poller.m_metas[which].type != HandleMeta::Type::Socket) {
        return false; // not a socket handle
    }

    return poller.m_metas[which].origFd == socket.handle();
}

bool MultiPoller::PollResult::isPipe(const PollPipe& pipe) const {
    return poller.m_handles[which] == pipe.m_event;
}

std::optional<MultiPoller::PollResult> MultiPoller::poll(const std::optional<Duration>& timeout) {
    PollResult res {0, *this};

    auto _lock = m_mtx.lock();

    if (m_handles.empty()) {
        return std::nullopt; // nothing to poll
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

void MultiPoller::clearReadiness(qsox::BaseSocket& socket) {
    // https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaeventselect
    // > With these semantics, an application need not read all available data in response to an FD_READ networkevent—
    // > a single recv in response to each FD_READ network event is appropriate.

    // we have to reset the event for the socket, otherwise next call to poll() will immediately return.
    // when the application eventually reads data from the socket, the event will be set again if data is available.

    auto _lock = m_mtx.lock();

    size_t idx = this->findHandle(socket.handle());

    if (idx == -1) {
        QN_ASSERT(false && "Socket not found in poller");
        return;
    }

    auto& meta = m_metas[idx];
    QN_ASSERT(meta.type == HandleMeta::Type::Socket && "Handle is not a socket");

    WSAResetEvent(m_handles[idx]);
}

}

#endif
