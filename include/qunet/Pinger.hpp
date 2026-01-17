#pragma once

#include <qunet/buffers/ByteReader.hpp>
#include <qunet/util/compat.hpp>

#include <arc/sync/mpsc.hpp>
#include <arc/sync/Mutex.hpp>
#include <arc/net/UdpSocket.hpp>
#include <arc/util/ManuallyDrop.hpp>
#include <asp/time/Instant.hpp>

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
    bool errored = false;
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

    /// Pings a url rather than an address. This is slightly more limited, it will not resolve SRV records but will resolve an A record.
    /// The callback may never be invoked if DNS resolution fails, but timeouts will still be handled.
    geode::Result<> pingUrl(const std::string& url, Callback callback);

private:
    friend struct PingerHolder;
    friend struct arc::ManuallyDrop<Pinger>;
    Pinger();

    struct OutgoingPing {
        asp::time::Instant sentAt;
        Callback callback;
        uint32_t pingId;
    };

    struct CachedPing {
        asp::time::Duration responseTime;
        std::vector<SupportedProtocol> protocols;
    };

    std::optional<arc::TaskHandle<void>> m_workerTask;
    std::optional<arc::mpsc::Sender<std::pair<qsox::SocketAddress, Callback>>> m_pingTx;
    std::optional<arc::UdpSocket> m_socket;

    std::vector<OutgoingPing> m_outgoingPings;
    uint32_t m_nextPingId = 0;

    asp::Mutex<std::unordered_map<qsox::SocketAddress, CachedPing>> m_cache;

    arc::Future<> workerLoop(arc::mpsc::Receiver<std::pair<qsox::SocketAddress, Callback>>);

    arc::Future<> thrDoPing(const qsox::SocketAddress& address, Callback callback);
    ByteReader::Result<PingResult> thrParsePingResponse(const uint8_t* data, size_t size);
    void thrDispatchResult(PingResult& result, const qsox::SocketAddress& address);
    void thrRemoveTimedOutPings();
    arc::Future<> recreateSocket();

    bool isCached(const qsox::SocketAddress& address);

    geode::Result<> resolveAndPing(std::string_view domain, uint16_t port, Callback callback);
};

}