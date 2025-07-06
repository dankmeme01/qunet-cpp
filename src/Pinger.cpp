#include <qunet/Pinger.hpp>
#include <qunet/Log.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <asp/time/sleep.hpp>

using namespace asp::time;

constexpr Duration PING_TIMEOUT = Duration::fromMillis(2000);

namespace qn {

Pinger::Pinger() {
    m_pingThread.setLoopFunction([this](auto& stopToken) {
        // TODO: might refactor this to not use condvars
        auto item = m_channel.popTimeout(std::chrono::milliseconds{1000});

        if (item) {
            this->thrDoPing(item->first, std::move(item->second));
        }
    });
    m_pingThread.start();

    // TODO: maybe make the recv thread sleep longer if there are no outstanding pings
    m_recvThread.setLoopFunction([this](auto& sotpToken) {
        if (!m_socket) {
            // block until we have a socket
            m_socketNotify.wait({}, [&] {
                return m_socket.has_value();
            });

            return;
        }

        qsox::SocketAddress src = qsox::SocketAddress::any();
        uint8_t response[256];

        auto res = m_socket->recvFrom(response, sizeof(response), src);
        if (res) {
            size_t bytes = res.unwrap();
            auto res = this->thrParsePingResponse(response, bytes);
            if (!res) {
                log::warn("Failed to parse ping response: {}", res.unwrapErr().message());
                return;
            }

            auto result = std::move(res).unwrap();

            this->thrDispatchResult(result, src);
        } else {
            auto err = res.unwrapErr();
            if (err != qsox::Error::TimedOut && err != qsox::Error::WouldBlock) {
                log::warn("Failed to receive ping response: {}", err.message());
            }
        }

        // check if any pings have timed out
        auto pings = m_outgoingPings.lock();

        for (size_t i = 0; i < pings->size(); ) {
            auto& ping = (*pings)[i];
            if (ping.sentAt.elapsed() > PING_TIMEOUT) {
                PingResult result {
                    .pingId = ping.pingId,
                    .responseTime = Duration::fromMillis(0), // no response time
                    .timedOut = true,
                };
                ping.callback(result);

                pings->erase(pings->begin() + i);
            } else {
                ++i;
            }
        }
    });
    m_recvThread.start();
}

Pinger::~Pinger() {
    m_pingThread.stop();
    m_recvThread.stop();

    m_pingThread.join();
    m_recvThread.join();
}

Pinger& Pinger::get() {
    static Pinger instance;
    return instance;
}

void Pinger::ping(const qsox::SocketAddress& address, Callback callback) {
    m_channel.push(std::make_pair(address, std::move(callback)));
}

void Pinger::thrDoPing(const qsox::SocketAddress& address, Callback callback) {
    if (!m_socket) {
        auto res = qsox::UdpSocket::bindAny();
        if (!res) {
            log::error("Failed to bind pinger UDP socket: {}", res.unwrapErr().message());
            return;
        }

        m_socket = std::move(res).unwrap();
        m_socketNotify.notifyAll();

        if (auto err = m_socket->setReadTimeout(100).err()) {
            log::error("Failed to set read timeout on pinger UDP socket: {}", err->message());
            return;
        }
    }

    uint32_t pingId = ++m_nextPingId;

    HeapByteWriter writer;
    writer.writeU8(MSG_PING);
    writer.writeU32(pingId);
    writer.writeU8(this->isCached(address) ? 1 : 0); // flags

    auto res = m_socket->sendTo(writer.written().data(), writer.written().size(), address);
    if (!res) {
        log::error("Failed to send ping to {}: {}", address.toString(), res.unwrapErr().message());
        return;
    }

    m_outgoingPings.lock()->push_back({
        .sentAt = SystemTime::now(),
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
    auto pings = m_outgoingPings.lock();
    auto cache = m_cache.lock();

    auto cacheEntry = cache->find(address);

    if (result.protocols.empty() && cacheEntry != cache->end()) {
        // if we have cached protocols, use them
        result.protocols = cacheEntry->second.protocols;
    }

    if (cacheEntry == cache->end()) {
        // if we don't have a cache entry, create one
        cache->emplace(address, CachedPing{
            .responseTime = result.responseTime,
            .protocols = result.protocols,
        });
    } else {
        // update the existing cache entry
        cacheEntry->second.responseTime = result.responseTime;
        cacheEntry->second.protocols = result.protocols;
    }

    for (size_t i = 0; i < pings->size(); i++) {
        auto& ping = (*pings)[i];

        if (result.pingId == ping.pingId) {
            Duration responseTime = ping.sentAt.elapsed();
            result.responseTime = responseTime;
            ping.callback(result);

            // remove the ping from the list
            pings->erase(pings->begin() + i);
            break;
        }
    }
}

bool Pinger::isCached(const qsox::SocketAddress& address) {
    return m_cache.lock()->contains(address);
}

}