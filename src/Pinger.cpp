#include <qunet/Pinger.hpp>
#include <qunet/dns/Resolver.hpp>
#include <qunet/Log.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/util/shutdown.hpp>
#include "UrlParser.hpp"

#include <arc/future/Select.hpp>
#include <arc/time/Sleep.hpp>

using namespace arc;
using namespace asp::time;

constexpr Duration PING_TIMEOUT = Duration::fromMillis(1500);

namespace qn {

struct PingerHolder {
    static Pinger& get() {
        static PingerHolder holder;
        return holder.instance.get();
    }

    PingerHolder() : instance() {}
    ~PingerHolder() {
        if (!isCleanupUnsafe()) {
            instance.drop();
        }
    }
private:
    ManuallyDrop<Pinger> instance;
};

static auto toV6(const qsox::SocketAddress& addr) {
    if (addr.isV6()) {
        return addr.toV6();
    }

    return qsox::SocketAddressV6{
        qsox::Ipv6Address::fromIpv4Mapped(addr.toV4().address()),
        addr.port()
    };
}

Pinger::Pinger() {
    auto rt = arc::Runtime::current();
    if (!rt) {
        throw std::runtime_error("Pinger must be created within an arc runtime");
    }

    auto [tx, rx] = arc::mpsc::channel<std::pair<qsox::SocketAddress, Callback>>(128);
    m_pingTx = std::move(tx);

    m_workerTask = rt->spawn(this->workerLoop(std::move(rx)));
    m_workerTask->setName("qn::Pinger worker");

    auto cache = m_cache.lock();
    cache->setMaxEntries(128);
    cache->setTimeToLive(Duration::fromMinutes(15));
    cache->setWorkInterval(asp::Duration::fromMinutes(1));
}

Pinger::~Pinger() {
    if (m_workerTask) m_workerTask->abort();
}

Pinger& Pinger::get() {
    return PingerHolder::get();
}

Future<> Pinger::workerLoop(arc::mpsc::Receiver<std::pair<qsox::SocketAddress, Callback>> rx) {
    while (true) {
        if (!m_socket) {
            // keep trying to recreate it
            co_await this->recreateSocket();

            if (!m_socket) {
                co_await arc::sleep(Duration::fromSecs(5));
                continue;
            }
        }

        qsox::SocketAddress src = qsox::SocketAddress::any();
        uint8_t response[256];

        Instant nextTimeout = Instant::farFuture();

        for (auto& ping : m_outgoingPings) {
            auto pingTimeout = ping.sentAt + PING_TIMEOUT;
            nextTimeout = std::min(nextTimeout, pingTimeout);
        }

        co_await arc::select(
            // wait for a new ping request
            arc::selectee(
                rx.recv(),
                [this](auto res) -> arc::Future<> {
                    if (!res) co_return; // channel closed

                    auto [address, callback] = std::move(res).unwrap();
                    co_await this->thrDoPing(address, std::move(callback));
                }
            ),

            // wait to receive a ping response
            arc::selectee(
                m_socket->recvFrom(response, sizeof(response), src),
                [&](auto res) -> arc::Future<> {
                    if (!res) {
                        auto err = res.unwrapErr();

                        // windows kinda decides to spam connection reset error whenever a send fails due to icmp error
                        if (err == qsox::Error::ConnectionReset) {
                            co_return;
                        }

                        log::warn("Failed to receive ping response: {}", err);

                        // recreate the socket
                        m_socket.reset();

                        co_return;
                    }

                    size_t bytes = res.unwrap();
                    auto pres = this->thrParsePingResponse(response, bytes);
                    if (!pres) {
                        log::warn("Failed to parse ping response: {}", pres.unwrapErr());
                        co_return;
                    }

                    auto result = std::move(pres).unwrap();
                    this->thrDispatchResult(result, src);
                }
            ),

            // wait for the next timeout
            arc::selectee(
                arc::sleepUntil(nextTimeout),
                [this] {
                    // check for timed out pings
                    this->thrRemoveTimedOutPings();
                }
            )
        );
    }
}

Future<> Pinger::recreateSocket() {
    auto res = co_await arc::UdpSocket::bindAny();
    if (!res) {
        log::error("Failed to create UDP socket for pinger: {}", res.unwrapErr());
        co_return;
    }

    m_socket = std::move(res).unwrap();
}

void Pinger::thrRemoveTimedOutPings() {
    Instant now = Instant::now();

    for (size_t i = 0; i < m_outgoingPings.size(); ) {
        auto& ping = m_outgoingPings[i];
        if (now.durationSince(ping.sentAt) > PING_TIMEOUT) {
            PingResult result {
                .pingId = ping.pingId,
                .timedOut = true,
            };
            ping.callback(result);

            m_outgoingPings.erase(m_outgoingPings.begin() + i);
        } else {
            ++i;
        }
    }
}

void Pinger::ping(const qsox::SocketAddress& address, Callback callback) {
    (void) m_pingTx->trySend(std::make_pair(address, std::move(callback)));
}

geode::Result<> Pinger::pingUrl(const std::string& url, Callback callback) {
    // parse and resolve the url
    UrlParser parser{url};
    auto parseRes = parser.result();

    switch (parseRes) {
        case UrlParseError::Success: break;
        case UrlParseError::InvalidProtocol: return Err("Invalid protocol in URL");
        case UrlParseError::InvalidPort: return Err("Invalid port in URL");
    }

    if (parser.isIpWithPort()) {
        this->ping(parser.asIpWithPort(), std::move(callback));
        return Ok();
    } else if (parser.isIp()) {
        this->ping(qsox::SocketAddress{parser.asIp(), defaultPort(ConnectionType::Udp)}, std::move(callback));
        return Ok();
    } else if (parser.isDomainWithPort()) {
        auto& [domain, port] = parser.asDomainWithPort();
        return this->resolveAndPing(domain, port, std::move(callback));
    } else if (parser.isDomain()) {
        return this->resolveAndPing(parser.asDomain(), defaultPort(ConnectionType::Udp), std::move(callback));
    } else {
        QN_ASSERT(false && "Invalid urlparser outcome");
    }
}

geode::Result<> Pinger::resolveAndPing(std::string_view domain, uint16_t port, Callback callback) {
    // not cached, resolve the domain
    auto& resolver = Resolver::get();
    auto res = resolver.queryA(std::string(domain), [
        this,
        domain = std::string(domain),
        port,
        callback = std::move(callback)
    ](ResolverResult<DNSRecordA> record) mutable {
        if (!record) {
            log::warn("Failed to resolve A record for '{}': {}", domain, record.unwrapErr());
            return;
        }

        auto& addrs = record.unwrap().addresses;

        QN_ASSERT(!addrs.empty() && "DNS A record should not be empty");

        // ping the first address
        this->ping(qsox::SocketAddress{addrs[0], port}, std::move(callback));
    });

    if (!res) {
        return Err(fmt::format("Failed to query A record for '{}': {}", domain, res.unwrapErr()));
    }

    return Ok();
}

Future<> Pinger::thrDoPing(const qsox::SocketAddress& address, Callback callback) {
    uint32_t pingId = ++m_nextPingId;

    HeapByteWriter writer;
    writer.writeU8(MSG_PING);
    writer.writeU32(pingId);
    writer.writeU8(this->isCached(address) ? 1 : 0); // flags

    auto res = co_await m_socket->sendTo(writer.written().data(), writer.written().size(), address);

    if (!res) {
        log::error("Failed to send ping to {}: {}", address.toString(), res.unwrapErr());
        callback(PingResult{
            .pingId = pingId,
            .errored = true,
        });

        co_return;
    }

    m_outgoingPings.push_back({
        .sentAt = Instant::now(),
        .callback = std::move(callback),
        .pingId = pingId,
    });
}

ByteReader::Result<PingResult> Pinger::thrParsePingResponse(const uint8_t* data, size_t size) {
    ByteReader reader(data, size);

    auto code = reader.readU8().unwrapOr(0);
    if (code != MSG_PONG) {
        log::warn("Invalid ping response received, expected MSG_PONG, got {}", code);
        return Err(ByteReaderError::OutOfBoundsRead); // eh
    }

    uint32_t pingId = GEODE_UNWRAP(reader.readU32());
    uint8_t protocolCount = GEODE_UNWRAP(reader.readU8());

    PingResult result {
        .pingId = pingId,
    };

    for (uint8_t i = 0; i < protocolCount; ++i) {
        SupportedProtocol proto;
        proto.protocolId = GEODE_UNWRAP(reader.readU8());
        proto.port = GEODE_UNWRAP(reader.readU16());
        result.protocols.push_back(proto);
    }

    uint16_t extraDataSize = GEODE_UNWRAP(reader.readU16());
    if (extraDataSize > 0) {
        result.extraData.resize(extraDataSize);
        GEODE_UNWRAP(reader.readBytes(result.extraData.data(), extraDataSize));
    }

    return Ok(result);
}

void Pinger::thrDispatchResult(PingResult& result, const qsox::SocketAddress& address) {
    auto& pings = m_outgoingPings;
    auto it = std::find_if(pings.begin(), pings.end(), [&result](const auto& ping) {
        return ping.pingId == result.pingId;
    });

    if (it == pings.end()) {
        log::warn("Received pong with unknown ID: {}", result.pingId);
        return;
    }

    Duration responseTime = it->sentAt.elapsed();
    result.responseTime = responseTime;

    // store ipv6 mapped addresses in cache, since that is what recvFrom may return
    auto v6 = toV6(address);
    auto cache = m_cache.lock();
    auto cacheEntry = cache->get(v6);

    if (result.protocols.empty() && cacheEntry) {
        // if we have cached protocols, use them
        result.protocols = cacheEntry->protocols;
    }

    if (!cacheEntry) {
        // if we don't have a cache entry, create one
        cache->insert(v6, CachedPing{
            .responseTime = result.responseTime,
            .protocols = result.protocols,
        });
    } else {
        // update the existing cache entry
        cacheEntry->responseTime = result.responseTime;
        cacheEntry->protocols = result.protocols;
    }

    it->callback(result);

    // remove the ping from the list
    pings.erase(it);
}

bool Pinger::isCached(const qsox::SocketAddress& address) {
    return this->getCached(address).has_value();
}

std::optional<PingResult> Pinger::getCached(const qsox::SocketAddress& address) {
    auto cache = m_cache.lock();
    auto entry = cache->get(toV6(address));
    if (!entry) return std::nullopt;

    PingResult out{};
    out.responseTime = entry->responseTime;
    out.protocols = entry->protocols;
    return out;
}

}