#include <qunet/util/PollPipe.hpp>

#ifdef _WIN32

#include <qunet/util/assert.hpp>
#include <Windows.h>

namespace qn {

PollPipe::PollPipe() {
    m_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    QN_ASSERT(m_event != nullptr && "Failed to create event for PollPipe");
}

PollPipe::PollPipe(PollPipe&& other) noexcept {
    m_event = other.m_event;
    other.m_event = nullptr;
}

PollPipe& PollPipe::operator=(PollPipe&& other) noexcept {
    if (this != &other) {
        if (m_event) CloseHandle(m_event);
        m_event = other.m_event;
        other.m_event = nullptr;
    }

    return *this;
}

PollPipe::~PollPipe() {
    if (m_event) CloseHandle(m_event);
}

void PollPipe::notify() {
    if (m_event) SetEvent(m_event);
}

void PollPipe::consume() {
    if (m_event) ResetEvent(m_event);
}

void PollPipe::clear() {
    this->consume();
}

qsox::SockFd PollPipe::readFd() const {
    return (qsox::SockFd) m_event;
}

qsox::SockFd PollPipe::writeFd() const {
    return (qsox::SockFd) m_event;
}

}

#endif // _WIN32
