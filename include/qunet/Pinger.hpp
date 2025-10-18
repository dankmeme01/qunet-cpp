#pragma once

#include <qunet/buffers/ByteReader.hpp>
#include <qunet/util/compat.hpp>

#include <qsox/UdpSocket.hpp>
#include <asp/thread/Thread.hpp>
#include <asp/sync/Channel.hpp>
#include <asp/sync/Mutex.hpp>
#include <asp/sync/Notify.hpp>
#include <asp/time/Duration.hpp>

namespace qn {

struct SupportedProtocol {
    uint8_t protocolId;
    uint16_t port;
};

struct PingResult {
    uint32_t pingId;
    asp::time::Duration responseTime;
    std::vector<SupportedProtocol> protocols;
    std::vector<uint8_t> extraData;
    bool timedOut = false;
};

class Pinger {
public:
    Pinger(const Pinger&) = delete;
    Pinger& operator=(const Pinger&) = delete;
    Pinger(Pinger&&) = delete;
    Pinger& operator=(Pinger&&) = delete;
    ~Pinger();

    static Pinger& get();

    using Callback = move_only_function<void(const PingResult&)>;

    void ping(const qsox::SocketAddress& address, Callback callback);

    /// Pings a url rather than an address. This is slightly more limited, it will not resolve SRV records but will resolve A/AAAA records.
    /// The callback may never be invoked if DNS resolution fails, but timeouts will still be handled.
    geode::Result<> pingUrl(const std::string& url, Callback callback);

private:
    Pinger();

    struct OutgoingPing {
        asp::time::SystemTime sentAt;
        Callback callback;
        uint32_t pingId;
    };

    struct CachedPing {
        asp::time::Duration responseTime;
        std::vector<SupportedProtocol> protocols;
    };

    asp::Thread<> m_pingThread, m_recvThread;
    asp::Channel<std::pair<qsox::SocketAddress, Callback>> m_channel;

    std::optional<qsox::UdpSocket> m_socket;
    asp::Notify m_socketNotify;

    asp::Mutex<std::vector<OutgoingPing>> m_outgoingPings;
    uint32_t m_nextPingId = 0;

    asp::Mutex<std::unordered_map<qsox::SocketAddress, CachedPing>> m_cache;
    asp::Mutex<std::unordered_map<std::string, qsox::IpAddress>> m_dnsCache;

    void thrDoPing(const qsox::SocketAddress& address, Callback callback);
    ByteReader::Result<PingResult> thrParsePingResponse(const uint8_t* data, size_t size);
    void thrDispatchResult(PingResult& result, const qsox::SocketAddress& address);

    bool isCached(const qsox::SocketAddress& address);

    geode::Result<> resolveAndPing(std::string_view domain, uint16_t port, Callback callback);
};

}