#include <qunet/Connection.hpp>
#include <qunet/dns/Resolver.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>
#include <qunet/util/assert.hpp>
#include <qunet/util/Poll.hpp>
#include "UrlParser.hpp"

#include <asp/time/Duration.hpp>
#include <asp/time/Instant.hpp>
#include <asp/time/sleep.hpp>

#include <algorithm>

using namespace asp::time;

static std::string_view getSrvProto(qn::ConnectionType type) {
    using enum qn::ConnectionType;

    switch (type) {
        case Udp: return "_udp";
        case Tcp: return "_tcp";
        case Quic: return "_quic";
        default: return "_udp";
    }
}

static std::string_view connTypeToString(qn::ConnectionType type) {
    using enum qn::ConnectionType;

    switch (type) {
        case Udp: return "udp";
        case Tcp: return "tcp";
        case Quic: return "quic";
        default: return "unknown";
    }
}

namespace qn {

// ConnectionError

bool ConnectionError::isTransportError() const {
    return std::holds_alternative<TransportError>(m_err);

}

bool ConnectionError::isOtherError() const {
    return std::holds_alternative<Code>(m_err);
}

bool ConnectionError::isServerClosedError() const {
    return std::holds_alternative<ServerClosedError>(m_err);
}

const TransportError& ConnectionError::asTransportError() const {
    return std::get<TransportError>(m_err);
}

const ServerClosedError& ConnectionError::asServerClosedError() const {
    return std::get<ServerClosedError>(m_err);
}

ConnectionError::Code ConnectionError::asOtherError() const {
    return std::get<Code>(m_err);
}

bool ConnectionError::operator==(Code code) const {
    return this->isOtherError() && this->asOtherError() == code;
}

bool ConnectionError::operator!=(Code code) const {
    return !(*this == code);
}

bool ConnectionError::operator==(const TransportError& err) const {
    return this->isTransportError() && this->asTransportError() == err;
}

bool ConnectionError::operator!=(const TransportError& err) const {
    return !(*this == err);
}

bool ConnectionError::operator==(const ServerClosedError& err) const {
    return this->isServerClosedError() && this->asServerClosedError().reason == err.reason;
}

bool ConnectionError::operator!=(const ServerClosedError& err) const {
    return !(*this == err);
}

std::string_view ServerClosedError::message() const {
    return reason.empty() ? "Server closed the connection" : std::string_view{reason};
}

std::string ConnectionError::message() const {
    if (this->isTransportError()) {
        return this->asTransportError().message();
    } else if (this->isServerClosedError()) {
        return std::string{this->asServerClosedError().message()};
    } else switch (this->asOtherError()) {
        case Code::Success: return "Success";
        case Code::InvalidProtocol: return "Invalid protocol specified";
        case Code::InvalidPort: return "Invalid port specified";
        case Code::InProgress: return "Connection is already in progress";
        case Code::NotConnected: return "Not connected to a server";
        case Code::AlreadyConnected: return "Already connected to a server";
        case Code::AlreadyClosing: return "Connection is already closing";
        case Code::DnsResolutionFailed: return "DNS resolution failed";
        case Code::AllAddressesFailed: return "Failed to connect to all possible addresses";
        case Code::ProtocolDisabled: return "The used protocol (IPv4/IPv6) is disabled, or both are disabled, connection cannot proceed";
        case Code::NoConnectionTypeFound: return "Failed to determine a suitable protocol for the connection";
    }

    qn::unreachable();
}

// Connection

Connection::Connection() {
    m_thread.setLoopFunction([this](auto& stopToken) {
        switch (m_connState) {
            case ConnectionState::Disconnected: {
                m_connStartedNotify.wait(Duration::fromMillis(200), [&] {
                    return m_connState != ConnectionState::Disconnected;
                });
            } break;

            case ConnectionState::DnsResolving: {
                // wait until we are resolving IPs, not SRV queries
                bool timedOut = !m_resolvingIpNotify.wait(Duration::fromMillis(1000), [&] {
                    return m_resolvingIp.load();
                });

                if (timedOut) return;

                constexpr Duration RESOLVE_LIMIT = Duration::fromMillis(2500);

                auto started = Instant::now();

                size_t lastResponseCount = m_dnsResponses.load();

                while (lastResponseCount < m_dnsRequests) {
                    auto timeout = RESOLVE_LIMIT - started.elapsed();
                    if (timeout.isZero()) {
                        break;
                    }

                    // If at least one response arrived, clamp max response time to 75ms.
                    // the other query may still arrive eventually, and their addresses will be considered too
                    if (lastResponseCount > 0) {
                        timeout = std::min(timeout, Duration::fromMillis(75));
                    }

                    bool timedOut = !m_dnsResponseNotify.wait(timeout, [&] {
                        return m_dnsResponses.load() >= lastResponseCount;
                    });

                    if (timedOut) {
                        break;
                    }

                    lastResponseCount = m_dnsResponses.load();
                }

                if (m_dnsResponses == 0) {
                    log::warn("DNS resolution took too long (no responses after {}), aborting", started.elapsed().toString());
                    this->onFatalConnectionError(ConnectionError::DnsResolutionFailed);
                    return;
                }

                this->doneResolving();
            } break;

            case ConnectionState::Pinging: {
                // In the Pinging state, we send pings to all known addresses at the same time,
                // compare the response times, and choose the best one (prefer IPv6 by default unless overriden)
                auto _lock = m_internalMutex.lock();

                if (!m_thrStartedPingingAt) {
                    m_thrStartedPingingAt = SystemTime::now();
                    m_thrArrivedPings = 0;
                    m_thrPingResults.clear();

                    for (auto& ip : m_usedIps) {
                        // do note the callback runs on another thread
                        qsox::SocketAddress addr{ip, m_usedPort};
                        log::debug("Sending ping to {}", addr.toString());

                        Pinger::get().ping(addr, [this, addr, cid = m_connCounter.load()](const PingResult& res) {
                            // if these are attempts for the previous connection, ignore them
                            if (m_connCounter.load() != cid) {
                                return;
                            }

                            auto _lock = m_internalMutex.lock();

                            m_thrArrivedPings++;
                            m_pingArrivedNotify.notifyOne();

                            if (res.timedOut) {
                                log::debug("Ping to {} timed out", addr.toString());
                                return;
                            }

                            log::debug("Ping to {} arrived, response time: {}", addr.toString(), res.responseTime.toString());

                            m_thrLastArrivedPing = SystemTime::now();

                            this->thrNewPingResult(res, addr);
                        });
                    }
                }

                bool remainingPings = m_thrArrivedPings < m_usedIps.size();
                bool anyArrived = m_thrArrivedPings > 0;
                auto elapsed = m_thrStartedPingingAt->elapsed();
                auto sinceLastPing = m_thrLastArrivedPing.elapsed();

                bool finishNow = remainingPings == 0 || (anyArrived && sinceLastPing > Duration::fromMillis(50)) || (elapsed > Duration::fromSecs(3));

                if (!finishNow)  {
                    _lock.unlock();
                    m_pingArrivedNotify.wait(Duration::fromMillis(100));
                    break;
                }

                // if no pings succeeded, try to connect to the first address
                if (!anyArrived || m_thrPingResults.empty()) {
                    log::warn("No pings arrived after {}, trying all the addresses in order",
                        elapsed.toString()
                    );
                } else {
                    log::debug("Pinging finished in {}, arrived pings: {} / {}, fastest address: {} ({})",
                        elapsed.toString(),
                        m_thrArrivedPings,
                        m_usedIps.size(),
                        m_thrPingResults[0].first.toString(),
                        m_thrPingResults[0].second.toString()
                    );

                    // pinging finished, sort addresses in thrPingResults, and then add them to m_usedIps
                    this->thrSortPingResults();
                    m_usedIps.clear();

                    for (auto& [addr, _] : m_thrPingResults) {
                        m_usedIps.push_back(addr.ip());
                    }
                }

                this->setConnState(ConnectionState::Connecting);
            } break;

            case ConnectionState::Connecting: {
                auto _lock = m_internalMutex.lock();

                QN_ASSERT(m_usedIps.size() > 0);

                // Set connection types
                if (m_usedConnTypes.empty()) {
                    if (m_chosenConnType == ConnectionType::Unknown) {
                        // read from protocols
                        for (auto& proto : m_thrPingerSupportedProtocols) {
                            switch (proto.protocolId) {
                                case PROTO_UDP: m_usedConnTypes.push_back({ConnectionType::Udp, m_usedPort}); break;
                                case PROTO_TCP: m_usedConnTypes.push_back({ConnectionType::Tcp, m_usedPort}); break;
                                case PROTO_QUIC: m_usedConnTypes.push_back({ConnectionType::Quic, m_usedPort}); break;
                                default: {
                                    log::warn("Unknown protocol ID {} in supported protocols, skipping", proto.protocolId);
                                    continue;
                                }
                            }
                        }
                    } else {
                        m_usedConnTypes.push_back({m_chosenConnType, m_usedPort});
                    }

                    this->thrSortConnTypes();
                }

                // if still empty, womp womp
                if (m_usedConnTypes.empty()) {
                    log::warn("No connection types available, cannot connect");
                    this->onFatalConnectionError(ConnectionError::NoConnectionTypeFound);
                    return;
                }

                this->thrTryConnectNext();
            } break;

            case ConnectionState::Reconnecting: {
                if (m_thrReconnectAttempt >= 7) {
                    log::debug("Too many reconnect attempts, disconnecting");
                    this->setConnState(ConnectionState::Disconnected);
                    return;
                }

                log::debug("Reconnecting (attempt {})", m_thrReconnectAttempt + 1);

                auto _lock = m_internalMutex.lock();
                auto res = this->thrTryReconnect();

                if (res) {
                    m_thrReconnectAttempt = 0;
                } else if (*res.err() == TransportError::Cancelled) {
                    // cancelled, do nothing
                } else if (*res.err() == TransportError::ReconnectFailed) {
                    // server did not want to accept us, likely the server just got restarted, simply disconnect
                    this->finishCancellation();
                } else {
                    // failed, sleep for a bit and try again
                    // initially 5 seconds, then jump to 10
                    auto timeout = Duration::fromSecs(m_thrReconnectAttempt < 3 ? 5 : 10);

                    log::debug("Reconnect attempt failed, sleeping for {} before trying again", timeout.toString());

                    MultiPoller poller;
                    poller.addPipe(m_disconnectPipe, qsox::PollType::Read);

                    _lock.unlock();
                    auto pRes = poller.poll(timeout);
                    _lock.relock();

                    m_disconnectPipe.consume();
                    poller.removePipe(m_disconnectPipe);

                    if (pRes) {
                        // a disconnect was requested, abort connection
                        this->finishCancellation();
                    } else {
                        m_thrReconnectAttempt++;
                    }
                }
            } break;

            case ConnectionState::Connected: {
                bool disconnect = false, messages = false, qsocket = false, timerExpired = false;

                if (m_msgChannel.lock()->size() > 0) {
                    messages = true;
                }

                if (m_socket->messageAvailable()) {
                    qsocket = true;
                }

                auto untilExpiry = m_socket->untilTimerExpiry();
                if (untilExpiry.isZero()) {
                    timerExpired = true;
                }

                bool shouldPoll = !disconnect && !messages && !qsocket && !timerExpired;

                if (shouldPoll) {
                    auto pollTime = std::clamp(untilExpiry, Duration::fromMillis(1), Duration::fromSecs(1));
                    // log::debug("polling for {}", pollTime.toString());

                    auto result = m_poller.poll(pollTime);

                    if (!result) {
                        if (m_socket->untilTimerExpiry().isZero()) {
                            timerExpired = true;
                        } else {
                            // timed out
                            return;
                        }
                    } else {
                        disconnect = result->isPipe(m_disconnectPipe);
                        messages = result->isPipe(m_msgPipe);
                        qsocket = result->isQSocket(*m_socket);
                    }

                    if (messages) m_msgPipe.consume();
                    if (disconnect) m_disconnectPipe.consume();
                    if (qsocket) m_poller.clearReadiness(*m_socket);
                }

                // log::debug("poll finished, messages: {}, disconnect: {}, qsocket: {}", messages, disconnect, qsocket);

                // check which events are ready
                if (disconnect) {
                    this->setConnState(ConnectionState::Closing);
                    return;
                }

                if (timerExpired) {
                    auto res = m_socket->handleTimerExpiry();
                    if (!res) {
                        this->onConnectionError(res.unwrapErr());
                        return;
                    }
                }

                if (qsocket) {
                    // always process data!
                    auto res = m_socket->processIncomingData();
                    if (!res) {
                        this->onConnectionError(res.unwrapErr());
                        return;
                    }

                    auto avail = res.unwrap();

                    while (avail) {
                        // receive a message
                        auto msgRes = m_socket->receiveMessage(std::nullopt);
                        if (!msgRes) {
                            auto err = msgRes.unwrapErr();
                            this->onConnectionError(err);
                            return;
                        }

                        auto msg = std::move(msgRes).unwrap();
                        this->thrHandleIncomingMessage(std::move(msg));

                        // check if another message is available
                        avail = m_socket->messageAvailable();
                    }
                }

                if (messages) {
                    auto chan = m_msgChannel.lock();

                    while (!chan->empty()) {
                        auto cmsg = std::move(chan->front());
                        chan->pop();

                        chan.unlock();

                        auto res = m_socket->sendMessage(std::move(cmsg.message), cmsg.reliable, cmsg.uncompressed);
                        if (!res) {
                            auto err = res.unwrapErr();
                            this->onConnectionError(err);
                        }

                        chan.relock();
                    }
                }
            } break;

            case ConnectionState::Closing: {
                auto _lock = m_internalMutex.lock();

                // TODO
                // m_socket->sendMessage(ClientCloseMessage{});

                auto res = m_socket->close();

                if (!res) {
                    log::warn("Failed to close socket: {}", res.unwrapErr().message());
                    this->onFatalConnectionError(res.unwrapErr());
                    m_socket.reset();
                    return;
                }

                while (!m_socket->isClosed()) {
                    // wait for the socket to close
                    _lock.unlock();
                    asp::time::sleep(Duration::fromMillis(10));
                    _lock.relock();
                }

                // done!
                log::debug("Connection closed cleanly");
                this->resetConnectionState();
            } break;
        }
    });
    m_thread.start();
}

Connection::~Connection() {
    // Let's try to avoid any mutex locks in the destructor

    if (this->connected()) {
        m_disconnectPipe.notify();
    }

    m_thread.stopAndWait();
}

void Connection::thrTryConnectNext() {
    auto _lock = m_internalMutex.lock();

    if (m_thrConnIpIndex >= m_usedIps.size()) {
        // we have tried all ips with all connection types, fail
        log::warn("Tried all IPs ({}) with all connection types ({}), connection failed",
            m_usedIps.size(),
            m_usedConnTypes.size()
        );

        this->onFatalConnectionError(ConnectionError::AllAddressesFailed);
        return;
    }

    auto ip = m_usedIps[m_thrConnIpIndex];
    auto& [connType, port] = m_usedConnTypes[m_thrConnTypeIndex];

    if (!port) {
        port = m_usedPort ? m_usedPort : DEFAULT_PORT;
    }

    // increment to the next connection type / ip
    m_thrConnTypeIndex++;
    if (m_thrConnTypeIndex >= m_usedConnTypes.size()) {
        m_thrConnTypeIndex = 0;
        m_thrConnIpIndex++;
    }

    // try connecting
    qsox::SocketAddress address{ip, port};

    this->thrTryConnectWith(address, connType);
}

void Connection::thrTryConnectWith(const qsox::SocketAddress& addr, ConnectionType type) {
    log::debug("Trying to connect to {} ({})", addr.toString(), connTypeToString(type));

    auto _lock = m_internalMutex.lock();

    if (m_cancelling) {
        this->finishCancellation();
        return;
    }

    // if this connection requires TLS, check if we have a TLS context
#ifdef QUNET_QUIC_SUPPORT
    if (type == ConnectionType::Quic && !m_tlsContext) {
        auto tlsres = ClientTlsContext::create(!m_tlsCertVerification);
        if (!tlsres) {
            log::warn("Failed to create TLS context: {}", tlsres.unwrapErr().message());
            return;
        }

        m_tlsContext = std::move(tlsres).unwrap();
    }
#endif

    auto sock = this->thrConnectSocket(addr, type);

    if (!sock) {
        // try next..
        log::info("Failed to connect to {} ({}): {}", addr.toString(), connTypeToString(type), sock.unwrapErr().message());
        return;
    }

    log::debug("Connected to {} ({})", addr.toString(), connTypeToString(type));

    m_socket = std::move(sock.unwrap());
    m_thrSuccessfulPair = std::make_pair(addr, type);

    this->thrConnected();
}

TransportResult<> Connection::thrTryReconnect() {
    if (m_cancelling) {
        this->finishCancellation();
        return Err(TransportError::Cancelled);
    }

    QN_ASSERT(m_socket && "m_socket must be nonnull when reconnecting");

    auto& addr = m_thrSuccessfulPair->first;
    auto type = m_thrSuccessfulPair->second;

    auto res = this->thrConnectSocket(addr, type, true);
    if (!res) {
        log::info("Failed to reconnect to {} ({}): {}", addr.toString(), connTypeToString(type), res.unwrapErr().message());
        return Err(std::move(res).unwrapErr());
    }

    log::debug("Reconnected to {}", addr.toString());

    m_socket = std::move(res).unwrap();

    this->thrConnected();

    return Ok();
}

TransportResult<Socket> Connection::thrConnectSocket(const qsox::SocketAddress& addr, ConnectionType type, bool reconnecting) {
    TransportOptions opts {
        .address = addr,
        .type = type,
        .timeout = m_connTimeout,
        .connOptions = &m_connOptions,
#ifdef QUNET_TLS_SUPPORT
        .tlsContext = m_tlsContext ? &*m_tlsContext : nullptr,
#endif
        .reconnecting = reconnecting,
    };

    return reconnecting ? Socket::reconnect(opts, m_socket.value()) : Socket::connect(opts);
}

void Connection::thrConnected() {
    this->setConnState(ConnectionState::Connected);

    QN_ASSERT(m_socket.has_value());

    // Setup poller
    m_poller.addPipe(m_msgPipe, qsox::PollType::Read);
    m_poller.addPipe(m_disconnectPipe, qsox::PollType::Read);
    m_poller.addQSocket(*m_socket, qsox::PollType::Read);

    QN_ASSERT(m_poller.trackedCount() == 3 && "thrConnected: invalid count of tracked fds in poller");
}

void Connection::thrHandleIncomingMessage(QunetMessage&& message) {
    log::debug("Received message: {}", message.typeStr());

    if (message.is<DataMessage>() && m_dataCallback) {
        auto& msg = message.as<DataMessage>();
        m_dataCallback(std::move(msg.data));
        return;
    }

    // TODO: handle other messages
    if (message.is<ConnectionErrorMessage>()) {
        auto& errMsg = message.as<ConnectionErrorMessage>();
        log::warn("Received connection error message: {}", errMsg.message());
        return;
    } else if (message.is<ServerCloseMessage>()) {
        auto& closeMsg = message.as<ServerCloseMessage>();
        log::warn("Received server close message: {}", closeMsg.message());
        this->onFatalConnectionError(ServerClosedError{ std::string{closeMsg.message()} });
    } else if (message.is<KeepaliveResponseMessage>()) {
        // do nothing!
        // TODO: actually forward the data to the client
    } else {
        log::warn("Don't know how to handle this message: {}", message.typeStr());
    }
}

void Connection::thrNewPingResult(const PingResult& result, const qsox::SocketAddress& addr) {
    auto _lock = m_internalMutex.lock();
    m_thrPingResults.push_back({addr, result.responseTime});

    for (auto& proto : result.protocols) {
        if (std::find_if(m_thrPingerSupportedProtocols.begin(), m_thrPingerSupportedProtocols.end(),
            [&proto](const auto& res) { return res.protocolId == proto.protocolId; }) != m_thrPingerSupportedProtocols.end()) {
            // already have this protocol, skip
            continue;
        }

        m_thrPingerSupportedProtocols.push_back(proto);
    }
}

void Connection::thrSortPingResults() {
    std::sort(m_thrPingResults.begin(), m_thrPingResults.end(), [this](const auto& a, const auto& b) {
        auto timeDiff = a.second.absDiff(b.second);

        // if time difference is over 30ms or they are the same family, sort by time
        if (timeDiff > Duration::fromMillis(30) || a.first.family() == b.first.family()) {
            return a.second < b.second;
        }

        // otherwise, see the preferred address family
        if (a.first.isV6() && !b.first.isV6()) {
            return m_preferIpv6;
        } else {
            return !m_preferIpv6;
        }
    });
}

void Connection::thrSortConnTypes() {
    if (m_usedConnTypes.size() <= 1) {
        return;
    }

    std::sort(m_usedConnTypes.begin(), m_usedConnTypes.end(), [this](const auto& a, const auto& b) {
        ConnectionType protoA = a.first;
        ConnectionType protoB = b.first;

        if (protoA == m_preferredConnType) {
            return true;
        } else if (protoB == m_preferredConnType) {
            return false;
        }

        // Sort in the order Tcp > Quic > Udp
        auto toNum = [](ConnectionType t) {
            switch (t) {
                case ConnectionType::Tcp: return 10;
                case ConnectionType::Quic: return 20;
                case ConnectionType::Udp: return 30;
                default: QN_ASSERT(false && "ConnectionType must not be unknown in therSortConnTypes");
            };
        };

        return toNum(protoA) < toNum(protoB);
    });
}

bool Connection::thrConnecting() {
    return false;
}

ConnectionResult<> Connection::connect(std::string_view destination) {
    auto _lock = m_internalMutex.lock();

    if (this->connecting()) {
        return Err(ConnectionError::InProgress);
    } else if (this->connected()) {
        // TODO: maybe allow this
        return Err(ConnectionError::AlreadyConnected);
    }

    if (!m_ipv4Enabled && !m_ipv6Enabled) {
        return Err(ConnectionError::ProtocolDisabled); // no protocols enabled
    }

    m_connCounter = m_connCounter + 1;

    // first parse the destination
    UrlParser parser{destination};
    auto parseRes = parser.result();

    switch (parseRes) {
        case UrlParseError::Success: break;
        case UrlParseError::InvalidProtocol: return Err(ConnectionError::InvalidProtocol);
        case UrlParseError::InvalidPort: return Err(ConnectionError::InvalidPort);
    }

    auto type = parser.protocol().value();

    this->resetConnectionState();

    // check if it's an IP+port / IP / domain+port / domain
    if (parser.isIpWithPort()) {
        return connectIp(parser.asIpWithPort(), type);
    } else if (parser.isIp()) {
        return connectIp(qsox::SocketAddress{parser.asIp(), DEFAULT_PORT}, type);
    } else if (parser.isDomainWithPort()) {
        auto& [domain, port] = parser.asDomainWithPort();
        return connectDomain(domain, port, type);
    } else if (parser.isDomain()) {
        return connectDomain(parser.asDomain(), std::nullopt, type);
    } else {
        QN_ASSERT(false && "Invalid urlparser outcome");
    }
}

void Connection::cancelConnection() {
    auto _lock = m_internalMutex.lock();

    if (this->connecting()) {
        m_cancelling = true;
        m_disconnectPipe.notify();
    }
}

ConnectionResult<> Connection::disconnect() {
    auto _lock = m_internalMutex.lock();

    if (m_connState == ConnectionState::Closing) {
        return Err(ConnectionError::AlreadyClosing);
    } else if (m_connState != ConnectionState::Connected) {
        return Err(ConnectionError::NotConnected);
    }

    m_disconnectPipe.notify();

    return Ok();
}

void Connection::setConnectionStateCallback(move_only_function<void(ConnectionState)> callback) {
    auto _lock = m_internalMutex.lock();

    m_connStateCallback = std::move(callback);
}

void Connection::setDataCallback(move_only_function<void(std::vector<uint8_t>)> callback) {
    auto _lock = m_internalMutex.lock();

    m_dataCallback = std::move(callback);
}

void Connection::setSrvPrefix(std::string_view pfx) {
    auto _lock = m_internalMutex.lock();

    m_srvPrefix = std::string(pfx);
}

void Connection::setPreferIpv6(bool preferIpv6) {
    auto _lock = m_internalMutex.lock();

    m_preferIpv6 = preferIpv6;
}

void Connection::setIpv4Enabled(bool enabled) {
    auto _lock = m_internalMutex.lock();

    m_ipv4Enabled = enabled;
}

void Connection::setIpv6Enabled(bool enabled) {
    auto _lock = m_internalMutex.lock();

    m_ipv6Enabled = enabled;
}

void Connection::setTlsCertVerification(bool verify) {
    auto _lock = m_internalMutex.lock();

    if (!this->disconnected()) return;

    m_tlsCertVerification = verify;
#ifdef QUNET_TLS_SUPPORT
    m_tlsContext.reset();
#endif
}

void Connection::setDebugOptions(const ConnectionDebugOptions& opts) {
    auto _lock = m_internalMutex.lock();

    m_connOptions.debug = opts;
}

void Connection::setConnectTimeout(Duration dur) {
    auto _lock = m_internalMutex.lock();

    m_connTimeout = dur;
}

void Connection::setActiveKeepaliveInterval(std::optional<asp::time::Duration> interval) {
    auto _lock = m_internalMutex.lock();

    m_connOptions.activeKeepaliveInterval = interval;
}

bool Connection::connecting() const {
    return m_connState != ConnectionState::Disconnected && m_connState != ConnectionState::Connected;
}

bool Connection::connected() const {
    return m_connState == ConnectionState::Connected;
}

bool Connection::disconnected() const {
    return m_connState == ConnectionState::Disconnected;
}

Duration Connection::getLatency() const {
    return m_socket ? m_socket->getLatency() : Duration::zero();
}

ConnectionState Connection::state() const {
    return m_connState;
}

ConnectionError Connection::lastError() const {
    return m_lastError;
}

void Connection::clearLastError() {
    m_lastError = ConnectionError::Success;
}

ConnectionResult<> Connection::connectDomain(std::string_view hostname, std::optional<uint16_t> port, ConnectionType type) {
    auto srvName = fmt::format("{}.{}.{}", m_srvPrefix, getSrvProto(type), hostname);

    this->setConnState(ConnectionState::DnsResolving);
    m_connStartedNotify.notifyOne();

    auto& resolver = Resolver::get();

    // If a port number is provided, skip the SRV query and just resolve A/AAAA records
    if (port.has_value()) {
        return this->fetchIpAndConnect(std::string(hostname), *port, type);
    } else {
        auto res = resolver.querySRV(srvName, [this, srvName, hostname, type](ResolverResult<DNSRecordSRV> record) {
            // if connection was cancelled, do nothing
            if (m_cancelling) {
                this->finishCancellation();
                return;
            }

            if (record) {
                // resolver guarantees that at least 1 endpoint is present
                auto& endpoints = record.unwrap().endpoints;

                auto res = this->fetchIpAndConnect(endpoints[0].target, endpoints[0].port ? endpoints[0].port : DEFAULT_PORT, type);
                if (!res) {
                    this->onFatalConnectionError(res.unwrapErr());
                }
            } else {
                auto err = record.unwrapErr();

                if (err == ResolverError::NoData || err == ResolverError::UnknownHost) {
                    log::debug("No SRV record found for {}, trying A/AAAA records", srvName);
                } else {
                    log::warn("Failed to resolve SRV record for {}: {}", srvName, err.message());
                }

                auto res = this->fetchIpAndConnect(std::string(hostname), DEFAULT_PORT, type);
                if (!res) {
                    this->onFatalConnectionError(res.unwrapErr());
                }
            }
        });

        if (!res) {
            log::warn("Failed to query SRV record for {}: {}", srvName, res.unwrapErr().message());
            this->onFatalConnectionError(ConnectionError::DnsResolutionFailed);
            return Err(ConnectionError::DnsResolutionFailed);
        }
    }

    return Ok();
}

ConnectionResult<> Connection::fetchIpAndConnect(const std::string& hostname, uint16_t port, ConnectionType type) {
    auto _lock = m_internalMutex.lock();

    auto& resolver = Resolver::get();

    m_resolvingIp = true;
    m_startedResolvingIpAt = SystemTime::now();
    m_usedPort = port;
    m_chosenConnType = type;

    // Query A and AAAA at the same time
    if (m_ipv4Enabled) {
        m_waitingForA = true;
        m_dnsRequests++;

        auto res1 = resolver.queryA(hostname, [this](ResolverResult<DNSRecordA> record) {
            auto _lock = m_internalMutex.lock();

            m_waitingForA = false;
            m_dnsResponses++;
            m_dnsResponseNotify.notifyOne();

            if (record) {
                m_dnsASuccess = true;

                for (auto& addr : record.unwrap().addresses) {
                    m_usedIps.push_back(addr);
                }

                this->sortUsedIps();
            } else {
                log::warn("Failed to resolve A record: {}", record.unwrapErr().message());
            }
        });

        if (!res1) {
            m_waitingForA = false;
            log::warn("Failed to query A record for {}: {}", hostname, res1.unwrapErr().message());
        }
    }

    if (m_ipv6Enabled) {
        m_waitingForAAAA = true;
        m_dnsRequests++;

        auto res2 = resolver.queryAAAA(hostname, [this](ResolverResult<DNSRecordAAAA> record) {
            auto _lock = m_internalMutex.lock();

            m_waitingForA = false;
            m_dnsResponses++;
            m_dnsResponseNotify.notifyOne();

            if (record) {
                m_dnsAAAASuccess = true;

                for (auto& addr : record.unwrap().addresses) {
                    m_usedIps.push_back(addr);
                }

                this->sortUsedIps();
            } else {
                log::warn("Failed to resolve AAAA record: {}", record.unwrapErr().message());
            }
        });

        if (!res2) {
            m_waitingForAAAA = false;
            log::warn("Failed to query AAAA record for {}: {}", hostname, res2.unwrapErr().message());
        }
    }

    // only error if both queries failed
    if (!m_waitingForA && !m_waitingForAAAA && !(m_dnsASuccess || m_dnsAAAASuccess)) {
        this->onFatalConnectionError(ConnectionError::DnsResolutionFailed);
        return Err(ConnectionError::DnsResolutionFailed);
    }

    // the worker thread will wait for dns results.
    m_resolvingIpNotify.notifyOne();

    return Ok();
}

ConnectionResult<> Connection::connectIp(const qsox::SocketAddress& address, ConnectionType type) {
    auto _lock = m_internalMutex.lock();

    if ((address.isV4() && !m_ipv4Enabled) || (address.isV6() && !m_ipv6Enabled)) {
        return Err(ConnectionError::ProtocolDisabled);
    }

    m_usedIps.clear();
    m_usedIps.push_back(address.ip());
    m_usedPort = address.port();
    m_chosenConnType = type;

    this->doneResolving();

    // the thread will handle the connection

    return Ok();
}

void Connection::sendKeepalive() {
    return this->doSend(KeepaliveMessage{});
}

void Connection::sendData(std::vector<uint8_t> data, bool reliable, bool uncompressed) {
    return this->doSend(DataMessage{std::move(data)}, reliable, uncompressed);
}

void Connection::doSend(QunetMessage&& message, bool reliable, bool uncompressed) {
    auto _lock = m_internalMutex.lock();

    if (!this->connected()) {
        return;
    }

    m_msgChannel.lock()->push(ChannelMsg {
        std::move(message),
        reliable,
        uncompressed
    });
    m_msgPipe.notify();
}

void Connection::onFatalConnectionError(const ConnectionError& err) {
    log::error("Fatal connection error: {}", err.message());

    m_lastError = err;
    switch (m_connState) {
        case ConnectionState::Connected: {
            *m_msgChannel.lock() = {};
            m_msgPipe.consume();
            m_disconnectPipe.consume();
            m_poller.removeQSocket(*m_socket);
            m_poller.removePipe(m_msgPipe);
            m_poller.removePipe(m_disconnectPipe);

            QN_ASSERT(m_poller.trackedCount() == 0 && "onFatalConnectionError: invalid count of tracked fds in poller");

            // attempt to reconnect
            this->setConnState(ConnectionState::Reconnecting);
        } break;

        default: {
            this->setConnState(ConnectionState::Disconnected);
        } break;
    }
}

void Connection::onConnectionError(const ConnectionError& err) {
    bool isMinor =
        err == ConnectionError::Success
        || err == ConnectionError::InProgress
        || err == TransportError::CongestionLimited;

    // TODO: maybe add more minor errors

    if (isMinor) {
        m_lastError = err;
        log::warn("Connection error: {}", err.message());
    } else {
        this->onFatalConnectionError(err);
    }
}

void Connection::finishCancellation() {
    auto _lock = m_internalMutex.lock();

    log::debug("Connection cancelled");

    this->resetConnectionState();
}

void Connection::resetConnectionState() {
    auto _lock = m_internalMutex.lock();

    m_lastError = ConnectionError::Success;
    this->setConnState(ConnectionState::Disconnected);
    m_cancelling = false;
    m_resolvingIp = false;
    m_waitingForA = false;
    m_waitingForAAAA = false;
    m_dnsASuccess = false;
    m_dnsAAAASuccess = false;
    m_dnsRequests = 0;
    m_dnsResponses = 0;
    m_chosenConnType = ConnectionType::Unknown;
    m_usedIps.clear();
    m_usedPort = 0;
    m_thrStartedPingingAt.reset();
    m_thrArrivedPings = 0;
    m_thrPingResults.clear();
    m_thrPingerSupportedProtocols.clear();
    m_thrConnIpIndex = 0;
    m_thrConnTypeIndex = 0;
    m_thrSuccessfulPair.reset();
    m_thrReconnectAttempt = 0;
    m_connStartedAt = SystemTime::now();
    m_usedConnTypes.clear();
    *m_msgChannel.lock() = {};

    m_poller.removePipe(m_msgPipe);
    m_poller.removePipe(m_disconnectPipe);
    if (m_socket) m_poller.removeQSocket(*m_socket);

    QN_ASSERT(m_poller.trackedCount() == 0 && "resetConnectionState: invalid count of tracked fds in poller");

    m_msgPipe.clear();
    m_disconnectPipe.clear();
    m_socket = std::nullopt;
}

void Connection::sortUsedIps() {
    auto _lock = m_internalMutex.lock();

    std::sort(m_usedIps.begin(), m_usedIps.end(), [&](const qsox::IpAddress& a, const qsox::IpAddress& b) {
        if (a.isV6() && !b.isV6()) {
            return m_preferIpv6;
        } else if (!a.isV6() && b.isV6()) {
            return !m_preferIpv6;
        } else {
            // same family, does not really matter
            if (a.isV4()) {
                return a.asV4().toBits() < b.asV4().toBits();
            } else {
                auto octetsA = a.asV6().octets();
                auto octetsB = b.asV6().octets();

                return octetsA < octetsB;
            }
        }
    });
}

void Connection::onUnexpectedClosure() {
    m_disconnectPipe.notify();
}

void Connection::doneResolving() {
    auto _lock = m_internalMutex.lock();

    if (m_cancelling) {
        this->finishCancellation();
        return;
    }

    if (m_usedIps.empty()) {
        log::warn("No IPs resolved, connection aborted");
        this->onFatalConnectionError(ConnectionError::DnsResolutionFailed);
        return;
    }

    // if we know concrete connection type, set the state to connecting, otherwise set it to pinging
    if (m_chosenConnType != ConnectionType::Unknown) {
        this->setConnState(ConnectionState::Connecting);
    } else {
        this->setConnState(ConnectionState::Pinging);
    }

    m_connStartedNotify.notifyOne();
}

void Connection::setConnState(ConnectionState state) {
    if (m_connState == state) {
        return; // no change
    }

    m_connState = state;

    if (m_connStateCallback) {
        m_connStateCallback(m_connState);
    }
}

}
