#ifdef _WIN32
# include "PollWin32.hpp"
#else
# include "PollUnix.hpp"
#endif

namespace qn {

MultiPoller::MultiPoller() : m_impl(std::make_unique<Impl>()) {}
MultiPoller::~MultiPoller() {}

void MultiPoller::addSocket(qsox::BaseSocket& socket, qsox::PollType interest) {
    m_impl->addSocket(socket, interest);
}

void MultiPoller::removeSocket(qsox::BaseSocket& socket) {
    m_impl->removeSocket(socket);
}

void MultiPoller::addPipe(const PollPipe& pipe, qsox::PollType interest) {
    m_impl->addPipe(pipe, interest);
}

void MultiPoller::removePipe(const PollPipe& pipe) {
    m_impl->removePipe(pipe);
}

void MultiPoller::addQSocket(qn::Socket& socket, qsox::PollType interest) {
    m_impl->addQSocket(socket, interest);
}

void MultiPoller::removeQSocket(qn::Socket& socket) {
    m_impl->removeQSocket(socket);
}

std::optional<MultiPoller::PollResult> MultiPoller::poll(const std::optional<asp::time::Duration>& timeout) {
    return m_impl->poll(timeout, *this);
}

void MultiPoller::clearReadiness(qsox::BaseSocket& socket) {
    m_impl->clearReadiness(socket);
}

void MultiPoller::clearReadiness(qn::Socket& socket) {
    m_impl->clearReadiness(socket);
}

}