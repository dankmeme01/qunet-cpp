#pragma once

#include "socket/Socket.hpp"
#include "socket/transport/tls/ClientTlsContext.hpp"
#include "util/Poll.hpp"
#include "Pinger.hpp"

#include <asp/sync/Channel.hpp>
#include <asp/sync/Notify.hpp>
#include <asp/thread/Thread.hpp>
#include <optional>

namespace qn {

class ConnectionError {
public:
    typedef enum {
        Success,
        InvalidProtocol,
        InvalidPort,
        InProgress,
        NotConnected,
        AlreadyConnected,
        AlreadyClosing,
        DnsResolutionFailed,
        AllAddressesFailed,
        ProtocolDisabled,
        NoConnectionTypeFound,
        ServerClosed,
    } Code;

    constexpr inline ConnectionError(Code code) : m_err(code) {}
    inline ConnectionError(TransportError err) : m_err(err) {}

    bool isOtherError() const;
    bool isTransportError() const;

    const TransportError& asTransportError() const;
    Code asOtherError() const;

    bool operator==(const ConnectionError& other) const = default;
    bool operator!=(const ConnectionError& other) const = default;
    bool operator==(Code code) const;
    bool operator!=(Code code) const;
    bool operator==(const TransportError& err) const;
    bool operator!=(const TransportError& err) const;

    std::string message() const;

private:
    std::variant<Code, TransportError> m_err;
};

template <typename T = void>
using ConnectionResult = geode::Result<T, ConnectionError>;

enum class ConnectionState {
    Disconnected,
    DnsResolving,
    Pinging,
    Connecting,
    Connected,
    Closing,
    Reconnecting,
};

// Various Debug options for the connection. Note that some of those do nothing in Release builds.
struct ConnectionDebugOptions {
    // Print verbose wolfSSL debug output (QUIC)
    bool verboseSsl = false;
    // Print verbose ngtcp2 debug output (QUIC)
    bool verboseQuic = false;
    // Simulate packet loss, 0.0f means no packet loss, 1.0f means 100% packet loss (QUIC, UDP)
    float packetLossSimulation = 0.0f;
};

struct ConnectionOptions {
    ConnectionDebugOptions debug;
    std::optional<asp::time::Duration> activeKeepaliveInterval;
};

// Connection is a class that is a manager for connecting to a specific endpoint.
// It handles DNS resolution, happy eyeballs (choosing ipv4 or ipv6),
// choosing the best transport (TCP, UDP, QUIC, ...), and reconnection.
// It is completely asynchronous and uses threads internally.
class Connection {
public:
    Connection();
    ~Connection();

    // Connect to an IP address or a domain name. This function supports many different protocols.
    // If a domain name is provided without a port number, _qunet.<proto>.<domain> SRV record is fetched (`_qunet` part can be configured),
    // if one is not found, then a regular A/AAAA record is fetched and port number 4340 is used. If an IP address is provided, it is used directly.
    // <proto> can be: `_udp` (default), `_tcp` or `_quic`
    //
    // If a port number is specified, no SRV query is performed. In this case, only an A/AAAA query is performed if a domain name is also provided.
    //
    // If the protocol is not specified (valid protocols: qunet, udp, tcp, quic), `qunet` is assumed. Meaning of the protocol:
    // - qunet - make no assumptions, try to ping the destination and then connect using the preferred transport
    // - udp - try connecting only using UDP. `proto` in the SRV query is `_udp`
    // - tcp - try connecting only using TCP. `proto` in the SRV query is `_tcp`
    // - quic - try connecting only using QUIC. `proto` in the SRV query is `_quic`
    //
    // for example:
    // - qunet://example.com or example.com - fetches SRV record for _qunet._udp.example.com (on failure fetches A/AAAA records and uses port 4340),
    // pings the destination (with UDP) to discover available transports, then tries to connect using the preferred one.
    // - qunet://example.com:1234 - fetches an A/AAAA record and uses the specified port to ping the destination, the rest is the same as above.
    // - udp://example.com - same as qunet, but does not ping the destination, just tries to connect using UDP.
    // - tcp://example.com - fetches SRV record for _qunet._tcp.example.com (on failure fetches A/AAAA records and uses port 4340),
    // does not ping the destination, just tries to connect using TCP.
    // - quic://example.com - fetches SRV record for _qunet._quic.example.com (on failure fetches A/AAAA records and uses port 4340),
    // does not ping the destination, just tries to connect using QUIC.
    // - quic://example.com:1234 - fetches an A/AAAA record and uses the specified port to connect using QUIC.
    ConnectionResult<> connect(std::string_view destination);

    // Cancel the current connection attempt. The actual cancellation might take some time, and this function does not block.
    // Cancellation is complete when `connecting()` returns false.
    void cancelConnection();

    // Disconnect from the server. Errors if not connected, or if already disconnecting.
    ConnectionResult<> disconnect();

    // Set the callback that is called when the connection state changes.
    void setConnectionStateCallback(std::function<void(ConnectionState)> callback);

    // Set the callback that is called when a data message is received.
    void setDataCallback(std::function<void(std::vector<uint8_t>)> callback);

    // Set the SRV query name prefix, by default it is `_qunet`.
    void setSrvPrefix(std::string_view pfx);

    // Set the preferred address family (`true` for IPv6, `false` for IPv4). By default, IPv6 is preferred.
    void setPreferIpv6(bool preferIpv6);

    // Set the preferred connection protocol. By default, it is `ConnectionType::Tcp`.
    // Set `ConnectionType::Unknown` to prefer no specific protocol.
    void setPreferProtocol(ConnectionType type);

    // Set whether to enable IPv4 connections. If `false`, only IPv6 connections will be attempted.
    void setIpv4Enabled(bool enabled);
    // Set whether to enable IPv6 connections. If `false`, only IPv4 connections will be attempted.
    void setIpv6Enabled(bool enabled);

    // Set whether to verify TLS certificates. If `false`, TLS (QUIC) connections will be established without certificate verification.
    // This will do nothing if a connection is established, otherwise it will invalidate the current TLS context.
    void setTlsCertVerification(bool verify);

    // Set the debug options for the connection.
    void setDebugOptions(const ConnectionDebugOptions& opts);

    // Set the connection timeout. This is applied per a specific connection attempt to a single IP address with a single protocol.
    // If multiple IPv4/IPv6 addresses and protocols are available, the full connection attempt may take way longer than this.
    // Default is 5 seconds.
    void setConnectTimeout(asp::time::Duration dur);

    // Set whether to enable "active" keepalives, and the interval between them.
    // By default, keepalives are only sent when the connection is idle for more than 30 seconds (45 for TCP).
    // With this option, they will always be sent at the given interval, even if the connection is not idle.
    // This can be useful for automatic measurement of the connection latency.
    void setActiveKeepaliveInterval(std::optional<asp::time::Duration> interval);

    // Returns whether a connection is currently in progress.
    bool connecting() const;

    // Returns whether a connection is established.
    bool connected() const;

    // Returns whether there is no connection established and no connection attempt is in progress.
    bool disconnected() const;

    // Returns the average latency of the connection, zero if not connected or if there's not enough ping data.
    asp::time::Duration getLatency() const;

    ConnectionState state() const;

    // Returns the last error that occurred during the connection process.
    ConnectionError lastError() const;

    // Sends a keepalive message to the server. Does nothing if not connected.
    void sendKeepalive();
    // Sends a data message to the server. Does nothing if not connected.
    void sendData(std::vector<uint8_t> data, bool reliable = true);

private:
    // vvv settings vvv
    std::string m_srvPrefix = "_qunet";
    ConnectionType m_preferredConnType = ConnectionType::Tcp;
    ConnectionOptions m_connOptions{};
    bool m_preferIpv6 = true;
    bool m_ipv4Enabled = true;
    bool m_ipv6Enabled = true;
    bool m_tlsCertVerification = true;
    asp::time::Duration m_connTimeout = asp::time::Duration::fromSecs(5);
    std::function<void(ConnectionState)> m_connStateCallback;
    std::function<void(std::vector<uint8_t>)> m_dataCallback;

    // vvv long lived fields vvv
    std::optional<ClientTlsContext> m_tlsContext;

    // vvv semi-public fields vvv
    ConnectionState m_connState = ConnectionState::Disconnected;
    ConnectionError m_lastError = ConnectionError::Success;
    asp::AtomicBool m_cancelling = false;

    // vvv these fields are temporary fields for async dns resolution vvv
    asp::time::SystemTime m_startedResolvingIpAt;
    asp::AtomicBool m_resolvingIp = false;
    bool m_waitingForA = false;
    bool m_waitingForAAAA = false;
    bool m_dnsASuccess = false;
    bool m_dnsAAAASuccess = false;
    std::atomic_size_t m_dnsRequests = 0;
    std::atomic_size_t m_dnsResponses = 0;
    ConnectionType m_chosenConnType = ConnectionType::Unknown;
    std::vector<qsox::IpAddress> m_usedIps;
    uint16_t m_usedPort;

    asp::Mutex<void, true> m_internalMutex; // Guards certain fields above
    asp::Thread<> m_thread;

    // vvv notifications, messages for the thread vvv
    MultiPoller m_poller;
    asp::Mutex<std::queue<std::pair<QunetMessage, bool>>> m_msgChannel;
    PollPipe m_msgPipe;
    PollPipe m_disconnectPipe;
    asp::Notify m_connStartedNotify;
    asp::Notify m_resolvingIpNotify;
    asp::Notify m_dnsResponseNotify;
    asp::Notify m_pingArrivedNotify;

    // vvv these are internal fields used by the thread vvv
    std::optional<asp::time::SystemTime> m_thrStartedPingingAt;
    asp::time::SystemTime m_thrLastArrivedPing;
    size_t m_thrArrivedPings = 0;
    std::vector<std::pair<qsox::SocketAddress, asp::time::Duration>> m_thrPingResults;
    std::vector<SupportedProtocol> m_thrPingerSupportedProtocols;
    size_t m_thrConnIpIndex = 0;
    size_t m_thrConnTypeIndex = 0;
    std::optional<std::pair<qsox::SocketAddress, ConnectionType>> m_thrSuccessfulPair;
    size_t m_thrReconnectAttempt = 0;

    // vvv actual connection fields vvv
    std::optional<Socket> m_socket;
    asp::AtomicSizeT m_connCounter = 0; // connection counter that increments with every connection attempt
    asp::time::SystemTime m_connStartedAt;
    std::vector<std::pair<ConnectionType, uint16_t>> m_usedConnTypes;

    ConnectionResult<> connectIp(const qsox::SocketAddress& address, ConnectionType type);
    ConnectionResult<> connectDomain(std::string_view hostname, std::optional<uint16_t> port, ConnectionType type);

    // Does not actually do anything, pushes the task to the thread
    ConnectionResult<> fetchIpAndConnect(const std::string& hostname, uint16_t port, ConnectionType type);

    void onFatalConnectionError(const ConnectionError& err);
    void onConnectionError(const ConnectionError& err);
    void finishCancellation();
    void clearLastError();
    void resetConnectionState();
    void sortUsedIps();

    void doSend(QunetMessage&& message, bool reliable = true);

    // Call when DNS resolution is over, we can proceed to either pinging or connecting to the destination
    void doneResolving();

    void onUnexpectedClosure();

    // vvv thread functions, do not call outside of the thread vvv
    void thrTryConnectNext();
    void thrTryConnectWith(const qsox::SocketAddress& addr, ConnectionType type);
    TransportResult<> thrTryReconnect();
    TransportResult<Socket> thrConnectSocket(const qsox::SocketAddress& addr, ConnectionType type, bool reconnecting = false);
    bool thrConnecting();
    void thrConnected();
    void thrNewPingResult(const PingResult& result, const qsox::SocketAddress& addr);
    void thrSortPingResults();
    void thrSortConnTypes();
    void thrHandleIncomingMessage(QunetMessage&& message);

    void setConnState(ConnectionState state);
};

}
