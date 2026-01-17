#include <qunet/Connection.hpp>
#include <qunet/dns/Resolver.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>
#include <qunet/util/assert.hpp>
#include "UrlParser.hpp"

#include <arc/future/Select.hpp>
#include <arc/future/Join.hpp>
#include <arc/time/Sleep.hpp>
#include <asp/time/Duration.hpp>
#include <asp/time/Instant.hpp>
#include <asp/time/sleep.hpp>

#include <algorithm>

using enum std::memory_order;
using qsox::SocketAddress;
using namespace asp::time;
using namespace arc;

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

static void sortAddresses(std::vector<SocketAddress>& addrs, bool preferIpv6) {
    std::sort(addrs.begin(), addrs.end(), [&](const auto& a, const auto& b) {
        if (a.isV6() && !b.isV6()) {
            return preferIpv6;
        } else if (!a.isV6() && b.isV6()) {
            return !preferIpv6;
        } else {
            // same family, does not really matter
            auto aip = a.ip();
            auto bip = b.ip();
            if (a.isV4()) {
                return aip.asV4().toBits() < bip.asV4().toBits();
            } else {
                auto octetsA = aip.asV6().octets();
                auto octetsB = bip.asV6().octets();

                return octetsA < octetsB;
            }
        }
    });
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

bool ConnectionError::isAllAddressesFailed() const {
    return std::holds_alternative<AllAddressesFailed>(m_err);
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

const AllAddressesFailed& ConnectionError::asAllAddressesFailed() const {
    return std::get<AllAddressesFailed>(m_err);
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

std::string_view AllAddressesFailed::message() const {
    return "Failed to connect to all resolved addresses";
}

bool AllAddressesFailed::operator==(const AllAddressesFailed& other) const {
    return this->addresses == other.addresses;
}

bool AllAddressesFailed::operator!=(const AllAddressesFailed& other) const {
    return !(*this == other);
}

std::string ConnectionError::message() const {
    if (this->isTransportError()) {
        return this->asTransportError().message();
    } else if (this->isServerClosedError()) {
        return std::string{this->asServerClosedError().message()};
    } else if (this->isAllAddressesFailed()) {
        return std::string{this->asAllAddressesFailed().message()};
    } else switch (this->asOtherError()) {
        case Code::Success: return "Success";
        case Code::InvalidProtocol: return "Invalid protocol specified";
        case Code::InvalidPort: return "Invalid port specified";
        case Code::InProgress: return "Connection is already in progress";
        case Code::NotConnected: return "Not connected to a server";
        case Code::AlreadyConnected: return "Already connected to a server";
        case Code::AlreadyClosing: return "Connection is already closing";
        case Code::ProtocolDisabled: return "The used protocol (IPv4/IPv6) is disabled, or both are disabled, connection cannot proceed";
        case Code::NoConnectionTypeFound: return "Failed to determine a suitable protocol for the connection";
        case Code::NoAddresses: return "No addresses were given, cannot connect";
        case Code::InternalError: return "An internal error occurred";
        case Code::TlsInitFailed: return "Failed to initialize TLS context";
        case Code::DnsResolutionFailed: return "DNS resolution failed (resolver error, see logs for more details)";
        case Code::DomainNotFound: return "Could not resolve the requested hostname";
    }

    qn::unreachable();
}

// Connection

Future<std::shared_ptr<Connection>> Connection::create() {
    auto [cTx, cRx] = arc::mpsc::channel<std::string>(std::nullopt);
    auto [mTx, mRx] = arc::mpsc::channel<OutgoingMessage>(128);

    WorkerThreadState wts{
        .connectChan = std::move(cRx),
        .msgChan = std::move(mRx)
    };

    auto conn = std::make_shared<Connection>(
        arc::Runtime::current(),
        std::move(cTx),
        std::move(mTx),
        std::move(wts)
    );

    conn->m_workerTask = arc::spawn([](auto conn) -> arc::Future<> {
        bool running = true;

        while (running) {
            co_await arc::select(
                arc::selectee(
                    conn->m_cancel.waitCancelled(),
                    [&] { running = false; }
                ),

                arc::selectee(conn->workerThreadLoop())
            );
        }
    }(conn));

    co_return conn;
}

Connection::Connection(
    arc::Runtime* runtime,
    mpsc::Sender<std::string> connectChan,
    mpsc::Sender<OutgoingMessage> msgChan,
    WorkerThreadState wts
)
    : m_connectChan(std::move(connectChan)),
      m_msgChan(std::move(msgChan)),
      m_wts(std::move(wts)),
      m_runtime(runtime)
{}

Connection::~Connection() {
    m_cancel.cancel();

    log::debug("Connection object destroyed");

    // in practice, the worker task should have already finished running if the connection is being destroyed
    if (m_workerTask) {
        m_workerTask.reset();
    }

    // close the socket if applicable
    if (m_socket) {
        (void) m_socket->closeSync();
    }
}

void Connection::destroy() {
    m_cancel.cancel();

    if (m_workerTask) {
        m_workerTask->abort();
        m_workerTask.reset();
    }
}

Future<> Connection::workerThreadLoop() {
    ARC_FRAME();

    switch (this->state()) {
        case ConnectionState::Disconnected: {
            // wait for connect request
            auto rres = co_await m_wts.connectChan.recv();

            // this should never fail?
            QN_DEBUG_ASSERT(rres.isOk());

            std::string url = rres.unwrap();
            co_await arc::select(
                arc::selectee(
                    this->threadConnect(std::move(url)),
                    [&](auto res) {
                        if (res) {
                            this->setState(ConnectionState::Connected);
                            m_lastError.lock() = ConnectionError::Success;
                        } else {
                            this->onFatalError(res.unwrapErr());
                        }

                        m_wts.msgChan.drain();
                    }
                ),

                arc::selectee(
                    m_disconnectNotify.notified(),
                    [&] -> arc::Future<> {
                        co_await this->threadCancelConnection();
                    }
                )
            );
        } break;

        // These are intermediary states and should never occur here
        case ConnectionState::DnsResolving:
        case ConnectionState::Pinging:
        case ConnectionState::Connecting: {
            QN_DEBUG_ASSERT(false && "Invalid connection state in worker thread loop");
            this->setState(ConnectionState::Disconnected);
        } break;

        case ConnectionState::Connected: {
            bool disconnectRequested = m_disconnectReq.load(acquire);
            if (disconnectRequested) {
                this->setState(ConnectionState::Closing);
                co_return;
            }

            auto untilExpiry = m_socket->untilTimerExpiry();

            // wait for either of the events to occur:
            co_await arc::select(
                // New messages to send
                arc::selectee(
                    m_wts.msgChan.recv(),
                    [&](auto recvRes) -> arc::Future<> {
                        if (!recvRes) co_return; // channel closed?

                        auto msg = std::move(recvRes).unwrap();
                        auto res = co_await m_socket->sendMessage(std::move(msg));

                        if (!res) {
                            this->onConnectionError(res.unwrapErr());
                        }
                    }
                ),

                // Timer expiry
                arc::selectee(
                    arc::sleep(untilExpiry),
                    [&] -> arc::Future<> {
                        auto res = co_await m_socket->handleTimerExpiry();
                        if (!res) {
                            this->onConnectionError(res.unwrapErr());
                        }
                    }
                ),

                // Disconnect request
                arc::selectee(
                    m_disconnectNotify.notified(),
                    [&] {
                        this->setState(ConnectionState::Closing);
                    }
                ),

                // Incoming messages
                arc::selectee(
                    m_socket->receiveMessage(),
                    [&](auto res) -> arc::Future<> {
                        if (!res) {
                            log::debug("Error receiving message");
                            this->onConnectionError(res.unwrapErr());
                            co_return;
                        }

                        auto msg = std::move(res).unwrap();
                        this->threadHandleIncomingMessage(std::move(msg));
                    }
                ),

                // Allow to periodically wake the task, in case connection state changed
                arc::selectee(m_taskWakeNotify.notified())
            );
        } break;

        case ConnectionState::Reconnecting: {
            bool disconnectRequested = m_disconnectReq.load(acquire);
            if (disconnectRequested) {
                co_await this->threadCancelConnection();
                co_return;
            }

            uint16_t attempt;
            bool tryStateless;
            {
                auto data = m_data.lock();
                attempt = ++data->m_reconnectAttempt;
                tryStateless = data->m_tryStatelessReconnect;
            }

            if (attempt >= 7) {
                log::warn("Too many reconnect attempts, giving up");
                co_await this->threadCancelConnection();
                co_return;
            }

            log::debug("Reconnecting (attempt {})", attempt);

            // initially 5 seconds, then jump to 10
            auto timeout = Duration::fromSecs(attempt < 3 ? 5 : 10);

            auto reconnectFut = [](auto self, auto timeout, bool stateless) -> arc::Future<> {
                auto res = co_await self->threadTryReconnect(stateless);

                if (res) {
                    log::info("Reconnected successfully!");
                    self->setState(ConnectionState::Connected);
                    co_return;
                }

                auto err = res.unwrapErr();

                if (err == TransportError::ReconnectFailed && !stateless) {
                    // instead of waiting, attempt a stateless reconnect next time
                    log::warn("Reconnect failed, will try stateless reconnect next time");
                    self->m_data.lock()->m_tryStatelessReconnect = true;
                    co_return;
                }

                log::warn("Reconnect attempt failed: {}", err);

                // failed, sleep for a bit and try again
                co_await arc::sleep(timeout);
            };

            // wait until either reconnect completes or a disconnect is requested
            co_await arc::select(
                arc::selectee(
                    reconnectFut(this, timeout, tryStateless)
                ),

                arc::selectee(
                    m_disconnectNotify.notified(),
                    [&] -> arc::Future<> {
                        co_await this->threadCancelConnection();
                    }
                )
            );
        } break;

        case ConnectionState::Closing: {
            if (!m_socket) co_return;

            // send close message
            (void) co_await m_socket->sendMessage(OutgoingMessage{
                .message = ClientCloseMessage{},
            });

            auto res = co_await m_socket->close();
            if (!res) {
                log::warn("Failed to close socket: {}", res.unwrapErr());
                this->onFatalError(res.unwrapErr());
                co_return;
            }

            while (!m_socket->isClosed()) {
                // wait for the socket to close
                co_await arc::sleep(Duration::fromMillis(10));
            }

            log::debug("Connection closed cleanly");
            this->resetConnectionState();
            this->setState(ConnectionState::Disconnected);
        } break;
    }

    co_return;
}

void Connection::resetConnectionState() {
    *m_data.lock() = ConnectionData{};
    m_disconnectReq.store(false, release);
    m_socket.reset();
}

static bool isFatalError(const ConnectionError& err) {
    if (err.isServerClosedError()) {
        return true;
    }

    if (err.isTransportError()) {
        auto& terr = err.asTransportError();
        if (std::holds_alternative<qsox::Error>(terr.m_kind)) {
            auto err = std::get<qsox::Error>(terr.m_kind);
            // pretty much all of them are fatal
            return err != qsox::Error::Success;
        }

        return (
            terr == TransportError::ConnectionTimedOut
            || terr == TransportError::TooUnreliable
            || terr == TransportError::TimedOut
            || terr == TransportError::Closed
            || terr == TransportError::Cancelled
        );
    }

    if (err.isOtherError()) {
        auto code = err.asOtherError();
        return code != ConnectionError::Code::Success;
    }

    return false;
}

void Connection::onConnectionError(const ConnectionError& err) {
    if (isFatalError(err)) {
        this->onFatalError(err);
        return;
    }

    log::warn("Connection error: {}", err);
    m_lastError.lock() = err;
}

void Connection::onFatalError(const ConnectionError& err) {
    log::error("Fatal connection error: {}", err);
    m_lastError.lock() = err;

    auto state = this->state();

    if (state == ConnectionState::Connected) {
        // attempt to reconnect
        m_wts.msgChan.drain();
        this->setState(ConnectionState::Reconnecting);
    } else {
        this->resetConnectionState();
        this->setState(ConnectionState::Disconnected);
    }
}

Future<ConnectionResult<>> Connection::threadConnect(std::string url) {
    ARC_FRAME();

    UrlParser parser{url};
    auto parseRes = parser.result();

    switch (parseRes) {
        case UrlParseError::Success: break;
        case UrlParseError::InvalidProtocol:
            co_return Err(ConnectionError::InvalidProtocol);
        case UrlParseError::InvalidPort:
            co_return Err(ConnectionError::InvalidPort);
    }

    this->resetConnectionState();
    auto type = parser.protocol().value();

    if (parser.isIpWithPort()) {
        co_return co_await this->threadConnectIp(parser.asIpWithPort(), type);
    } else if (parser.isIp()) {
        co_return co_await this->threadConnectIp(
            qsox::SocketAddress{parser.asIp(), DEFAULT_PORT},
            type
        );
    } else if (parser.isDomainWithPort()) {
        auto& [domain, port] = parser.asDomainWithPort();
        co_return co_await this->threadConnectDomain(domain, port, type);
    } else if (parser.isDomain()) {
        co_return co_await this->threadConnectDomain(parser.asDomain(), std::nullopt, type);
    } else {
        QN_ASSERT(false && "Invalid urlparser outcome");
    }
}

Future<ConnectionResult<>> Connection::threadConnectIp(qsox::SocketAddress addr, ConnectionType type) {
    bool preferIpv6 = m_settings.lock()->m_preferIpv6;
    m_wts.connectHostname.clear();

    return this->threadConnectWithIps(
        std::vector<SocketAddress>{addr},
        type,
        preferIpv6
    );
}

static Future<ConnectionResult<>> fetchIps(
    std::vector<SocketAddress>& out,
    const std::string& hostname,
    uint16_t port,
    bool ipv4,
    bool ipv6
) {
    ARC_FRAME();

    auto& resolver = Resolver::get();

    auto fut1 = [&](this auto self) -> Future<ResolverResult<>> {
        if (!ipv4) co_return Ok();

        auto res = ARC_CO_UNWRAP(co_await resolver.asyncQueryA(hostname));
        for (auto& addr : res.addresses) {
            out.push_back(SocketAddress{addr, port});
        }
        co_return Ok();
    }();

    auto fut2 = [&](this auto self) -> Future<ResolverResult<>> {
        if (!ipv6) co_return Ok();

        auto res = ARC_CO_UNWRAP(co_await resolver.asyncQueryAAAA(hostname));
        for (auto& addr : res.addresses) {
            out.push_back(SocketAddress{addr, port});
        }
        co_return Ok();
    }();

    auto res = co_await arc::joinAll(
        std::move(fut1),
        std::move(fut2)
    );

    for (size_t i = 0; i < res.size(); i++) {
        auto& r = res[i];
        if (!r) {
            using qsox::resolver::Error;
            auto err = r.unwrapErr();
            if (err != Error::NoData && err != Error::UnknownHost) {
                log::warn("Failed to resolve {} record for {}: {}", (i == 0 ? "A" : "AAAA"), hostname, err.message());
            }
        }
    }

    log::debug("Resolved {} addresses for {}", out.size(), hostname);

    co_return Ok();
}

Future<ConnectionResult<>> Connection::threadConnectDomain(std::string_view hostnameSv, std::optional<uint16_t> port, ConnectionType type) {
    this->setState(ConnectionState::DnsResolving);
    std::string hostname{hostnameSv};
    auto& resolver = Resolver::get();

    auto settings = m_settings.lock();
    auto srvName = fmt::format("{}.{}.{}", settings->m_srvPrefix, getSrvProto(type), hostname);
    bool ipv4Allowed = settings->m_ipv4Enabled;
    bool ipv6Allowed = settings->m_ipv6Enabled;
    bool preferIpv6 = settings->m_preferIpv6;
    settings.unlock();

    std::vector<SocketAddress> addrs;

    // If a port number is provided, skip the SRV query and just resolve A/AAAA records
    if (port.has_value()) {
        ARC_CO_UNWRAP(co_await fetchIps(
            addrs, hostname, *port, ipv4Allowed, ipv6Allowed
        ));
    } else {
        auto res = co_await resolver.asyncQuerySRV(srvName);

        if (res) {
            // resolver guarantees that at least 1 endpoint is present
            auto& endpoints = res.unwrap().endpoints;
            auto& ep = endpoints[0];
            ARC_CO_UNWRAP(co_await fetchIps(
                addrs,
                ep.target,
                endpoints[0].port ?: DEFAULT_PORT,
                ipv4Allowed,
                ipv6Allowed
            ));
        } else {
            auto err = res.unwrapErr();

            if (err == ResolverError::NoData || err == ResolverError::UnknownHost) {
                log::debug("No SRV record found for {}, trying A/AAAA records", srvName);
            } else {
                log::warn("Failed to resolve SRV record for {}: {}", srvName, err);
            }

            ARC_CO_UNWRAP(co_await fetchIps(
                addrs,
                hostname,
                DEFAULT_PORT,
                ipv4Allowed,
                ipv6Allowed
            ));
        }
    }

    if (addrs.empty()) {
        co_return Err(ConnectionError::DomainNotFound);
    }

    m_wts.connectHostname = hostname;

    co_return co_await this->threadConnectWithIps(
        std::move(addrs),
        type,
        preferIpv6
    );
}

Future<ConnectionResult<>> Connection::threadConnectWithIps(std::vector<SocketAddress> addrs, ConnectionType type, bool preferIpv6) {
    if (addrs.empty()) {
        co_return Err(ConnectionError::NoAddresses);
    }

    sortAddresses(addrs, preferIpv6);

    // if we know the concrete connection type, connect instead of pinging
    if (type == ConnectionType::Unknown) {
        co_return co_await this->threadPingCandidates(std::move(addrs));
    } else {
        std::vector<std::pair<SocketAddress, ConnectionType>> finalAddrs;
        for (auto& addr : addrs) {
            finalAddrs.push_back({ addr, type });
        }

        co_return co_await this->threadFinalConnect(std::move(finalAddrs));
    }
}

Future<ConnectionResult<>> Connection::threadPingCandidates(std::vector<SocketAddress> addrs) {
    this->setState(ConnectionState::Pinging);

    auto startedAt = Instant::now();

    auto [tx, rx] = mpsc::channel<std::pair<size_t, PingResult>>(8);

    for (size_t i = 0; i < addrs.size(); i++) {
        auto& addr = addrs[i];
        log::debug("Sending ping to {}", addr.toString());

        Pinger::get().ping(addr, [tx, i](const PingResult& result) mutable {
            (void) tx.trySend({i, result});
        });
    }

    std::vector<std::pair<SocketAddress, Duration>> pingResults;
    std::vector<SupportedProtocol> protocols;
    std::vector<std::pair<SocketAddress, ConnectionType>> finalAddrs;
    size_t anyResults = 0;

    bool waiting = true;
    while (anyResults < addrs.size() && waiting) {
        auto now = Instant::now();
        // if any arrived already, terminate soon; otherwise wait longer
        auto deadline = startedAt + (pingResults.size() > 0 ? Duration::fromMillis(50) : Duration::fromSecs(5));

        if (now >= deadline) {
            break;
        }

        co_await arc::select(
            arc::selectee(arc::sleepUntil(deadline), [&] { waiting = false; }),

            arc::selectee(rx.recv(), [&](auto res) {
                if (!res) {
                    // channel closed
                    waiting = false;
                    return;
                }

                anyResults++;

                auto [i, result] = std::move(res).unwrap();
                if (result.timedOut || result.errored || result.protocols.empty()) {
                    return;
                }

                pingResults.push_back({ addrs[i], result.responseTime });

                // add supported protocols
                for (auto& proto : result.protocols) {
                    if (std::find_if(protocols.begin(), protocols.end(),
                        [&proto](const auto& res) { return res.protocolId == proto.protocolId; }) == protocols.end())
                    {
                        protocols.push_back(proto);
                    }
                }
            })
        );
    }

    if (pingResults.empty()) {
        log::warn("No pings arrived after {}, will try all the addresses in order",
            startedAt.elapsed().toString()
        );

        for (auto& addr : addrs) {
            finalAddrs.push_back({ addr, ConnectionType::Udp });
            finalAddrs.push_back({ addr, ConnectionType::Tcp });
            finalAddrs.push_back({ addr, ConnectionType::Quic });
        }
    } else {
        log::debug("Pinging finished in {}, arrived pings: {} / {}, fastest address: {} ({})",
            startedAt.elapsed().toString(),
            pingResults.size(),
            addrs.size(),
            pingResults[0].first.toString(),
            pingResults[0].second.toString()
        );

        bool preferIpv6 = m_settings.lock()->m_preferIpv6;

        // sort ping results
        std::sort(pingResults.begin(), pingResults.end(), [&](const auto& a, const auto& b) {
            auto timeDiff = a.second.absDiff(b.second);

            // if time difference is over 25ms or they are the same family, sort by time
            if (timeDiff > Duration::fromMillis(25) || a.first.family() == b.first.family()) {
                return a.second < b.second;
            }

            // otherwise, see the preferred address family
            if (a.first.isV6() && !b.first.isV6()) {
                return preferIpv6;
            } else {
                return !preferIpv6;
            }
        });

        // prepare final addresses with connection types
        for (auto [addr, _] : pingResults) {
            for (auto ty : protocols) {
                ConnectionType ctype;
                switch (ty.protocolId) {
                    case PROTO_UDP: ctype = ConnectionType::Udp; break;
                    case PROTO_TCP: ctype = ConnectionType::Tcp; break;
                    case PROTO_QUIC: ctype = ConnectionType::Quic; break;
                    default:
                        log::warn("Unknown protocol ID {} in supported protocols, skipping", ty.protocolId);
                        continue;
                }

                addr.setPort(ty.port);
                finalAddrs.push_back({ addr, ctype });
            }
        }
    }

    co_return co_await this->threadFinalConnect(std::move(finalAddrs));
}

Future<ConnectionResult<>> Connection::threadFinalConnect(std::vector<std::pair<SocketAddress, ConnectionType>> addrs) {
    ARC_FRAME();
    ARC_ASSERT(!addrs.empty());
    this->setState(ConnectionState::Connecting);

    auto preferred = m_settings.lock()->m_preferredConnType;

    // sort by connection type
    std::sort(addrs.begin(), addrs.end(), [&](const auto& a, const auto& b) {
        // By default sort in the order Tcp > Quic > Udp
        auto toNum = [](ConnectionType t) {
            switch (t) {
                case ConnectionType::Tcp: return 10;
                case ConnectionType::Quic: return 20;
                case ConnectionType::Udp: return 30;
                default: QN_ASSERT(false && "ConnectionType must not be unknown in therSortConnTypes");
            };
        };

        if (a.second == preferred) {
            return true;
        } else if (b.second == preferred) {
            return false;
        }

        return toNum(a.second) < toNum(b.second);
    });

    // finally, try connecting in order

    bool connected = false;

    std::vector<std::tuple<SocketAddress, ConnectionType, ConnectionError>> errors;

    for (auto& [addr, type] : addrs) {
        auto res = co_await this->threadEstablishConn(addr, type);

        if (!res) {
            log::info("Failed to connect to {} ({}): {}", addr.toString(), connTypeToString(type), res.unwrapErr());
            errors.push_back({ addr, type, std::move(res).unwrapErr() });
            continue;
        }

        log::debug("Connected to {} ({})", addr.toString(), connTypeToString(type));
        connected = true;

        auto data = m_data.lock();
        data->m_address = addr;
        data->m_connType = type;

        break;
    }

    if (!connected) {
        // we have tried all ips with all connection types, fail
        log::warn("Tried all IP and connection type combinations, could not establish a connection.");
        log::warn("Attempted addresses:");
        for (auto& [addr, type] : addrs) {
            log::warn(" - {} ({})", addr.toString(), connTypeToString(type));
        }

        ARC_ASSERT(!errors.empty());
        co_return Err(AllAddressesFailed{ std::move(errors) });
    }

    // connected!
    co_return Ok();
}

Future<ConnectionResult<>> Connection::threadEstablishConn(SocketAddress addr, ConnectionType type) {
    ARC_FRAME();

    log::debug("Trying to connect to {} ({})", addr.toString(), connTypeToString(type));

    // if this connection requires TLS, check if we have a TLS context
#ifdef QUNET_QUIC_SUPPORT
    if (type == ConnectionType::Quic && !m_tlsContext) {
        bool certVerification = m_settings.lock()->m_tlsCertVerification;
        auto tlsres = ClientTlsContext::create(!certVerification);
        if (!tlsres) {
            log::warn("Failed to create TLS context: {}", tlsres.unwrapErr());
            co_return Err(ConnectionError::TlsInitFailed);
        }

        m_tlsContext = std::move(tlsres).unwrap();
    }
#endif

    co_return co_await this->threadConnectSocket(addr, type, false);
}

TransportOptions Connection::makeOptions(SocketAddress addr, ConnectionType type, bool reconnect) {
    auto settings = m_settings.lock();

    return TransportOptions {
        .address = addr,
        .type = type,
        .timeout = settings->m_connTimeout,
        .hostname = m_wts.connectHostname,
        .connOptions = &settings->m_connOptions,
#ifdef QUNET_TLS_SUPPORT
        .tlsContext = m_tlsContext ? &*m_tlsContext : nullptr,
#endif
        .reconnecting = reconnect,
    };
}

Future<ConnectionResult<>> Connection::threadConnectSocket(SocketAddress addr, ConnectionType type, bool reconnect) {
    ARC_FRAME();

    TransportOptions opts = makeOptions(addr, type, reconnect);
    auto qdbDir = m_settings.lock()->m_qdbFolder;

    auto res = co_await (reconnect
        ? Socket::reconnect(opts, *m_socket)
        : Socket::connect(opts, qdbDir));

    m_socket = ARC_CO_UNWRAP(std::move(res));
    co_return Ok();
}

Future<ConnectionResult<>> Connection::threadTryReconnect(bool stateless) {
    ARC_FRAME();

    auto data = m_data.lock();
    auto addr = data->m_address;
    auto type = data->m_connType;
    data.unlock();

    QN_ASSERT(addr && type != ConnectionType::Unknown);

    log::info("Attempting to reconnect to {} ({}, stateless: {})", addr->toString(), connTypeToString(type), stateless);

    ARC_CO_UNWRAP(co_await this->threadConnectSocket(*addr, type, !stateless));

    // success! notify the user in case this was a stateless reconnect
    if (stateless) {
        auto callbacks = m_callbacks.lock();

        if (callbacks->m_stateResetCallback) {
            callbacks->m_stateResetCallback();
        }
    }

    co_return Ok();
}

Future<> Connection::threadCancelConnection() {
    this->resetConnectionState();
    this->setState(ConnectionState::Disconnected);
    co_return;
}

void Connection::threadHandleIncomingMessage(QunetMessage message) {
    log::debug("Received message: {}", message.typeStr());

    if (message.is<DataMessage>()) {
        auto& msg = message.as<DataMessage>();

        // do not forward 0 byte messages, they are used for internal purposes
        if (msg.data.empty()) {
            return;
        }

        auto callbacks = m_callbacks.lock();

        if (callbacks->m_dataCallback) {
            callbacks->m_dataCallback(std::move(msg.data));
        }
        return;
    }

    if (message.is<ConnectionErrorMessage>()) {
        auto& errMsg = message.as<ConnectionErrorMessage>();
        log::warn("Received connection error message: {}", errMsg.message());
        return;
    } else if (message.is<ServerCloseMessage>()) {
        auto& closeMsg = message.as<ServerCloseMessage>();
        log::warn("Received server close message: {}", closeMsg.message());
        this->onFatalError(ServerClosedError{ std::string{closeMsg.message()} });
    } else if (message.is<KeepaliveResponseMessage>()) {
        // do nothing!
        // TODO: actually forward the data to the client
    } else {
        log::warn("Don't know how to handle this message: {}", message.typeStr());
    }
}

ConnectionResult<> Connection::connect(std::string_view destination) {
    auto state = this->state();
    if (state == ConnectionState::Connected) {
        return Err(ConnectionError::AlreadyConnected);
    } else if (state != ConnectionState::Disconnected) {
        return Err(ConnectionError::InProgress);
    }

    bool res = m_connectChan.trySend(std::string(destination)).isOk();

    if (res) {
        return Ok();
    } else {
        return Err(ConnectionError::InProgress);
    }
}

Future<ConnectionResult<>> Connection::connectWait(std::string_view destination) {
    ARC_FRAME();

    // set up a connection state callback
    arc::Notify notify;

    this->setConnectionStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected || state == ConnectionState::Disconnected) {
            notify.notifyOne();
        }
    });

    auto res = this->connect(destination);
    if (!res) {
        this->setConnectionStateCallback(nullptr);
        co_return res;
    }

    co_await notify.notified();

    this->setConnectionStateCallback(nullptr);

    auto state = this->state();
    if (state == ConnectionState::Connected) {
        co_return Ok();
    } else {
        co_return Err(this->lastError());
    }
}

void Connection::disconnect() {
    m_disconnectReq.store(true, release);
    m_disconnectNotify.notifyAll();
}

void Connection::setConnectionStateCallback(move_only_function<void(ConnectionState)> callback) {
    auto callbacks = m_callbacks.lock();
    callbacks->m_connStateCallback = std::move(callback);
}

void Connection::setDataCallback(move_only_function<void(std::vector<uint8_t>)> callback) {
    auto callbacks = m_callbacks.lock();
    callbacks->m_dataCallback = std::move(callback);
}

void Connection::setStateResetCallback(move_only_function<void()> callback) {
    auto callbacks = m_callbacks.lock();
    callbacks->m_stateResetCallback = std::move(callback);
}

void Connection::setSrvPrefix(std::string_view pfx) {
    auto settings = m_settings.lock();
    settings->m_srvPrefix = pfx;
}

void Connection::setPreferIpv6(bool preferIpv6) {
    auto settings = m_settings.lock();
    settings->m_preferIpv6 = preferIpv6;
}

void Connection::setPreferProtocol(ConnectionType type) {
    auto settings = m_settings.lock();
    settings->m_preferredConnType = type;
}

void Connection::setIpv4Enabled(bool enabled) {
    auto settings = m_settings.lock();
    settings->m_ipv4Enabled = enabled;
}

void Connection::setIpv6Enabled(bool enabled) {
    auto settings = m_settings.lock();
    settings->m_ipv6Enabled = enabled;
}

void Connection::setTlsCertVerification(bool verify) {
    auto settings = m_settings.lock();
    settings->m_tlsCertVerification = verify;
}

void Connection::setQdbFolder(const std::filesystem::path& folder) {
    auto settings = m_settings.lock();
    settings->m_qdbFolder = folder;
}

void Connection::setDebugOptions(const ConnectionDebugOptions& opts) {
    auto settings = m_settings.lock();
    settings->m_connOptions.debug = opts;
}

void Connection::setConnectTimeout(asp::time::Duration dur) {
    auto settings = m_settings.lock();
    settings->m_connTimeout = dur;
}

void Connection::setActiveKeepaliveInterval(std::optional<asp::time::Duration> interval) {
    auto settings = m_settings.lock();
    settings->m_connOptions.activeKeepaliveInterval = interval;
}

bool Connection::connecting() const {
    auto state = this->state();
    return state == ConnectionState::Connecting || state == ConnectionState::DnsResolving || state == ConnectionState::Pinging;
}

bool Connection::connected() const {
    return this->state() == ConnectionState::Connected;
}

bool Connection::disconnected() const {
    return this->state() == ConnectionState::Disconnected;
}

Duration Connection::getLatency() const {
    return m_socket ? m_socket->getLatency() : Duration::zero();
}

bool Connection::sendKeepalive() {
    return this->sendMsgToThread(KeepaliveMessage{});
}

bool Connection::sendData(std::vector<uint8_t> data, bool reliable, bool uncompressed, std::string tag) {
    return this->sendMsgToThread(DataMessage{ std::move(data) }, reliable, uncompressed, std::move(tag));
}

StatSnapshot Connection::statSnapshot(Duration period) const {
    if (m_socket) {
        return m_socket->transport()->_tracker().snapshot(period);
    }

    return {};
}

StatWholeSnapshot Connection::statSnapshotFull() const {
    if (m_socket) {
        return m_socket->transport()->_tracker().snapshotFull();
    }

    return {};
}

void Connection::simulateConnectionDrop() {
    this->onFatalError(ConnectionError::InternalError);
    m_taskWakeNotify.notifyOne();
}

bool Connection::sendMsgToThread(QunetMessage&& message, bool reliable, bool uncompressed, std::string tag) {
    if (!this->connected()) {
        return false;
    }

    return m_msgChan.trySend(OutgoingMessage{
        .message = std::move(message),
        .reliable = reliable,
        .uncompressed = uncompressed,
        .tag = std::move(tag)
    }).isOk();
}

ConnectionState Connection::state() const {
    return m_connState.load(acquire);
}

void Connection::setState(ConnectionState state) {
    auto prev = m_connState.exchange(state, release);

    if (prev != state) {
        auto callbacks = m_callbacks.lock();

        if (callbacks->m_connStateCallback) {
            callbacks->m_connStateCallback(state);
        }

        // clean up some things when connected / disconnected
        if (state == ConnectionState::Connected) {
            auto data = m_data.lock();
            data->m_reconnectAttempt = 0;
            data->m_tryStatelessReconnect = false;
        }
    }
}

ConnectionError Connection::lastError() const {
    return *m_lastError.lock();
}

}
