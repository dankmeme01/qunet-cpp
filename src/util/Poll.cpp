#include <qunet/util/Poll.hpp>

namespace qn {

void MultiPoller::addSocket(qsox::BaseSocket& socket, qsox::PollType interest) {
    return this->addSocket(socket.handle(), interest);
}

void MultiPoller::addSocket(qsox::SockFd fd, qsox::PollType interest) {
    auto _lock = m_mtx.lock();

    this->addHandle(HandleMeta { HandleMeta::Type::Socket }, fd, interest);
}

void MultiPoller::removeSocket(qsox::BaseSocket& socket) {
    return this->removeSocket(socket.handle());
}

void MultiPoller::removeSocket(qsox::SockFd fd) {
    auto _lock = m_mtx.lock();

    this->removeByIdx(this->findHandle(fd));
}

void MultiPoller::removeByIdx(size_t idx) {
    if (idx == -1) return;

    this->runCleanupFor(idx);

    m_metas.erase(m_metas.begin() + idx);
    m_handles.erase(m_handles.begin() + idx);
}

void MultiPoller::clear() {
    auto _lock = m_mtx.lock();

    while (!m_handles.empty()) {
        this->removeByIdx(m_handles.size() - 1);
    }
}

}