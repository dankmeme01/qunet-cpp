#include <netinet/in.h>
#include <qunet/Connection.hpp>
#include <qunet/dns/Resolver.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>
#include <qunet/util/assert.hpp>

#include <asp/time/Duration.hpp>
#include <asp/time/sleep.hpp>

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

bool ConnectionError::isSocketError() const {
    return std::holds_alternative<qsox::Error>(m_err);

}
bool ConnectionError::isOtherError() const {
    return std::holds_alternative<Code>(m_err);
}

qsox::Error ConnectionError::asSocketError() const {
    return std::get<qsox::Error>(m_err);
}

ConnectionError::Code ConnectionError::asOtherError() const {
    return std::get<Code>(m_err);
}

bool ConnectionError::operator==(Code code) const {
    return std::holds_alternative<Code>(m_err) && std::get<Code>(m_err) == code;
}

bool ConnectionError::operator!=(Code code) const {
    return !(*this == code);
}

std::string ConnectionError::message() const {
    if (this->isSocketError()) {
        return this->asSocketError().message();
    } else switch (this->asOtherError()) {
        case Code::Success: return "Success";
        case Code::InvalidProtocol: return "Invalid protocol specified";
        case Code::InvalidPort: return "Invalid port specified";
        case Code::InProgress: return "Connection is already in progress";
        case Code::AlreadyConnected: return "Already connected to a server";
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
                // TODO: yeah this could be more efficient
                asp::time::sleep(Duration::fromMillis(50));
            } break;

            case ConnectionState::DnsResolving: {
                if (!m_resolvingIp) {
                    asp::time::sleep(Duration::fromMillis(25));
                    break;
                }

                auto _lock = m_internalMutex.lock();

                size_t remaining = m_dnsRequests - m_dnsResponses;

                if (remaining == 0) {
                    // all responses, we are done and can try connecting
                    this->doneResolving();
                } else if (m_dnsResponses > m_thrLastDnsResponseCount) {
                    m_thrLastDnsResponseCount = m_dnsResponses;
                    m_thrFirstDnsResponseTime = SystemTime::now();

                    _lock.unlock();
                    asp::time::sleep(Duration::fromMillis(10));
                } else if (m_dnsResponses > 0) {
                    // we did not get any new responses, check how long has it been
                    auto elapsed = m_thrFirstDnsResponseTime.elapsed();
                    if (elapsed > Duration::fromMillis(100) && !m_usedIps.empty()) {
                        // one of the queries is taking 100ms more than the other, we can skip it and use addresses we got from the first
                        // if the query arrives eventually, the ips will be added to the vector and could be used too
                        this->doneResolving();
                    } else {
                        _lock.unlock();
                        asp::time::sleep(Duration::fromMillis(10));
                    }
                } else {
                    // no responses at all this far, keep waiting
                    auto elapsed = m_startedResolvingIpAt.elapsed();
                    if (elapsed > Duration::fromMillis(2500)) {
                        // something really went wrong
                        log::warn("DNS resolution took too long (no responses after {}), aborting", elapsed.toString());
                        this->onFatalConnectionError(ConnectionError::DnsResolutionFailed);
                        return;
                    }

                    _lock.unlock();
                    asp::time::sleep(Duration::fromMillis(10));
                }
            } break;

            case ConnectionState::Pinging: {
                // In the Pinging state, we send pings to all known addresses at the same time,
                // compare the response times, and choose the best one (prefer IPv6 by default unless overriden)
                auto _lock = m_internalMutex.lock();

                if (m_thrStartedPingingAt.timeSinceEpoch().isZero()) {
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
                auto elapsed = m_thrStartedPingingAt.elapsed();
                auto sinceLastPing = m_thrLastArrivedPing.elapsed();

                bool finishNow = remainingPings == 0 || (anyArrived && sinceLastPing > Duration::fromMillis(50)) || (elapsed > Duration::fromSecs(3));

                if (!finishNow)  {
                    _lock.unlock();
                    asp::time::sleep(Duration::fromMillis(5));
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

                m_connState = ConnectionState::Connecting;
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

            case ConnectionState::Connected: {

            } break;
        }
    });
    m_thread.start();
}

Connection::~Connection() {
    // TODO: disconnect
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

    auto sock = Socket::connect(addr, type, m_connTimeout);
    if (!sock) {
        // try next..
        log::debug("Failed to connect to {} ({}): {}", addr.toString(), connTypeToString(type), sock.unwrapErr().message());
        return;
    }

    log::debug("Connected to {} ({})", addr.toString(), connTypeToString(type));

    m_socket = std::move(sock.unwrap());

    this->thrConnected();
}

void Connection::thrConnected() {
    m_connState = ConnectionState::Connected;
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
    auto protoEnd = destination.find("://");
    ConnectionType type = ConnectionType::Unknown; // Unknown is qunet

    if (protoEnd != std::string::npos) {
        std::string_view proto = destination.substr(0, protoEnd);
        if (proto == "udp") {
            type = ConnectionType::Udp;
        } else if (proto == "tcp") {
            type = ConnectionType::Tcp;
        } else if (proto == "quic") {
            type = ConnectionType::Quic;
        } else if (proto == "qunet") {
            type = ConnectionType::Unknown;
        } else {
            return Err(ConnectionError::InvalidProtocol);
        }

        destination = destination.substr(protoEnd + 3); // skip the protocol path
    }

    this->resetConnectionState();

    // if there's a trailing slash, remove it
    while (!destination.empty() && destination.back() == '/') {
        destination.remove_suffix(1);
    }

    // check if it's an IP+port / IP / domain+port / domain
    if (auto addressResult = qsox::SocketAddress::parse(destination)) {
        return connectIp(addressResult.unwrap(), type);
    } else if (auto ipResult = qsox::IpAddress::parse(std::string(destination))) {
        return connectIp(qsox::SocketAddress{ipResult.unwrap(), DEFAULT_PORT}, type);
    } else if (destination.find(':') != std::string::npos) {
        auto portStr = destination.substr(destination.find_last_of(':') + 1);
        uint16_t port;

        if (std::from_chars(&*portStr.begin(), &*portStr.end(), port).ec != std::errc()) {
            return Err(ConnectionError::InvalidPort);
        }

        destination.remove_suffix(portStr.size() + 1); // remove the port part

        return connectDomain(destination, port, type);
    } else {
        return connectDomain(destination, std::nullopt, type);
    }
}

void Connection::cancelConnection() {
    auto _lock = m_internalMutex.lock();

    if (this->connecting()) {
        m_cancelling = true;
    }
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

void Connection::setConnectTimeout(Duration dur) {
    m_connTimeout = dur;
}

bool Connection::connecting() const {
    return m_connState != ConnectionState::Disconnected && m_connState != ConnectionState::Connected;
}

bool Connection::connected() const {
    return m_connState == ConnectionState::Connected;
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

    m_connState = ConnectionState::DnsResolving;

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

void Connection::onFatalConnectionError(const ConnectionError& err) {
    m_lastError = err;
    m_connState = ConnectionState::Disconnected;
}

void Connection::finishCancellation() {
    auto _lock = m_internalMutex.lock();

    log::debug("Connection cancelled");

    this->resetConnectionState();
}

void Connection::resetConnectionState() {
    auto _lock = m_internalMutex.lock();

    m_cancelling = false;
    m_resolvingIp = false;
    m_connState = ConnectionState::Disconnected;
    m_lastError = ConnectionError::Success;
    m_waitingForA = false;
    m_waitingForAAAA = false;
    m_dnsASuccess = false;
    m_dnsAAAASuccess = false;
    m_dnsRequests = 0;
    m_dnsResponses = 0;
    m_chosenConnType = ConnectionType::Unknown;
    m_usedIps.clear();
    m_usedPort = 0;
    m_thrConnIpIndex = 0;
    m_thrConnTypeIndex = 0;
    m_socket = std::nullopt;
    m_usedConnTypes.clear();
    m_thrPingResults.clear();
    m_thrPingerSupportedProtocols.clear();
    m_thrLastDnsResponseCount = 0;
    m_connStartedAt = SystemTime::now();
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
        m_connState = ConnectionState::Connecting;
    } else {
        m_connState = ConnectionState::Pinging;
    }
}

}