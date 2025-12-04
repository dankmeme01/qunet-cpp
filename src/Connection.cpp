#include <qunet/Connection.hpp>
#include <qunet/dns/Resolver.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>
#include <qunet/util/assert.hpp>
#include "UrlParser.hpp"

#include <arc/future/Select.hpp>
#include <arc/future/Join.hpp>
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

struct WorkerThreadState {
    mpsc::Receiver<std::string> connectChan;
};

Future<std::shared_ptr<Connection>> Connection::create() {
    auto [cTx, cRx] = arc::mpsc::channel<std::string>(1);
    auto conn = std::make_shared<Connection>(std::move(cTx));
    WorkerThreadState wts{
        .connectChan = std::move(cRx)
    };

    conn->m_workerTask = arc::spawn([](auto conn, auto wts) -> arc::Future<> {
        bool running = true;

        while (running) {
            co_await arc::select(
                arc::selectee(
                    conn->m_cancel.waitCancelled(),
                    [&] { running = false; }
                ),

                arc::selectee(conn->workerThreadLoop(wts))
            );
        }
    }(conn, std::move(wts)));

    co_return conn;
}

Connection::Connection(
    mpsc::Sender<std::string> connectChan
)
    : m_connectChan(std::move(connectChan))
{}

Connection::~Connection() {
    m_cancel.cancel();

    if (m_workerTask) {
        m_workerTask->abort();
        // TODO: wait for task to finish?
    }
}

Future<> Connection::workerThreadLoop(WorkerThreadState& wts) {
    switch (this->state()) {
        case ConnectionState::Disconnected: {
            // wait for connect request
            auto rres = co_await wts.connectChan.recv();

            // this should never fail?
            QN_DEBUG_ASSERT(rres.isOk());

            std::string url = rres.unwrap();
            auto res = co_await this->threadConnect(std::move(url));

            // TODO: handle?
            if (!res) {
                this->setState(ConnectionState::Disconnected);
            }
        } break;
    }

    co_return;
}

Future<ConnectionResult<>> Connection::threadConnect(std::string url) {
    UrlParser parser{url};
    auto parseRes = parser.result();

    switch (parseRes) {
        case UrlParseError::Success: break;
        case UrlParseError::InvalidProtocol:
            co_return Err(ConnectionError::InvalidProtocol);
        case UrlParseError::InvalidPort:
            co_return Err(ConnectionError::InvalidPort);
    }

    auto type = parser.protocol().value();
    m_data = {};

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
    return this->threadConnectWithIps(
        std::vector<SocketAddress>{addr},
        type
    );
}

static Future<ConnectionResult<>> fetchIps(
    std::vector<SocketAddress>& out,
    const std::string& hostname,
    uint16_t port
) {
    auto& resolver = Resolver::get();

    auto fut1 = [&](this auto self) -> Future<ResolverResult<>> {
        auto res = ARC_CO_UNWRAP(co_await resolver.asyncQueryA(hostname));
        for (auto& addr : res.addresses) {
            out.push_back(SocketAddress{addr, port});
        }
        co_return Ok();
    }();

    auto fut2 = [&](this auto self) -> Future<ResolverResult<>> {
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
    auto settings = m_settings.lock();
    auto& resolver = Resolver::get();
    std::string hostname{hostnameSv};

    std::vector<SocketAddress> addrs;

    // If a port number is provided, skip the SRV query and just resolve A/AAAA records
    if (port.has_value()) {
        ARC_CO_UNWRAP(co_await fetchIps(
            addrs,
            hostname,
            *port
        ));
    } else {
        auto srvName = fmt::format("{}.{}.{}", settings->m_srvPrefix, getSrvProto(type), hostname);
        auto res = co_await resolver.asyncQuerySRV(srvName);

        if (res) {
            // resolver guarantees that at least 1 endpoint is present
            auto& endpoints = res.unwrap().endpoints;
            auto& ep = endpoints[0];
            ARC_CO_UNWRAP(co_await fetchIps(
                addrs,
                ep.target,
                endpoints[0].port ?: DEFAULT_PORT
            ));
        } else {
            auto err = res.unwrapErr();

            if (err == ResolverError::NoData || err == ResolverError::UnknownHost) {
                log::debug("No SRV record found for {}, trying A/AAAA records", srvName);
            } else {
                log::warn("Failed to resolve SRV record for {}: {}", srvName, err.message());
            }

            ARC_CO_UNWRAP(co_await fetchIps(
                addrs,
                hostname,
                DEFAULT_PORT
            ));
        }
    }

    co_return co_await this->threadConnectWithIps(
        std::move(addrs),
        type
    );
}

Future<ConnectionResult<>> Connection::threadConnectWithIps(std::vector<SocketAddress> addrs, ConnectionType type) {
    if (addrs.empty()) {
        co_return Err(ConnectionError::DnsResolutionFailed);
    }

    sortAddresses(
        addrs,
        m_settings.lock()->m_preferIpv6
    );

    // if we know the concrete connection type, connect instead of pinging
    if (type != ConnectionType::Unknown) {
        this->setState(ConnectionState::Connecting);
    } else {
        this->setState(ConnectionState::Pinging);
    }

    co_return Ok();
}

ConnectionResult<> Connection::connect(std::string_view destination) {
    auto state = this->state();
    if (state == ConnectionState::Connected) {
        return Err(ConnectionError::AlreadyConnected);
    } else if (state != ConnectionState::Disconnected) {
        return Err(ConnectionError::InProgress);
    }

    // TODO
    // if (!m_ipv4Enabled && !m_ipv6Enabled) {
    //     return Err(ConnectionError::ProtocolDisabled); // no protocols enabled
    // }

    bool res = m_connectChan.trySend(std::string(destination)).isOk();

    if (res) {
        return Ok();
    } else {
        return Err(ConnectionError::InProgress);
    }
}

ConnectionState Connection::state() const {
    return m_connState.load(acquire);
}

void Connection::setState(ConnectionState state) {
    m_connState.store(state, release);
}

}
