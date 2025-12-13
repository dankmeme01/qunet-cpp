#pragma once

#include "socket/Socket.hpp"
#include "socket/transport/tls/ClientTlsContext.hpp"
#include "util/compat.hpp"
#include "Pinger.hpp"

#include <arc/task/CancellationToken.hpp>
#include <arc/sync/Mutex.hpp>
#include <arc/sync/mpsc.hpp>
#include <asp/sync/Channel.hpp>
#include <asp/sync/Notify.hpp>
#include <asp/thread/Thread.hpp>
#include <optional>

namespace qn {


struct ServerClosedError {
    std::string reason;

    std::string_view message() const;

    bool operator==(const ServerClosedError& other) const = default;
    bool operator!=(const ServerClosedError& other) const = default;
};

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
        AllAddressesFailed,
        ProtocolDisabled,
        NoConnectionTypeFound,
        NoAddresses,
        InternalError,
        TlsInitFailed,
        /// dns resolution did not succeed due to a resolver failure
        DnsResolutionFailed,
        /// dns resolution succeeded but the domain was not found
        DomainNotFound,
    } Code;

    constexpr inline ConnectionError(Code code) : m_err(code) {}
    inline ConnectionError(TransportError err) : m_err(std::move(err)) {}
    inline ConnectionError(ServerClosedError err) : m_err(std::move(err)) {}

    bool isOtherError() const;
    bool isTransportError() const;
    bool isServerClosedError() const;

    const TransportError& asTransportError() const;
    const ServerClosedError& asServerClosedError() const;
    Code asOtherError() const;

    bool operator==(const ConnectionError& other) const = default;
    bool operator!=(const ConnectionError& other) const = default;
    bool operator==(Code code) const;
    bool operator!=(Code code) const;
    bool operator==(const TransportError& err) const;
    bool operator!=(const TransportError& err) const;
    bool operator==(const ServerClosedError& err) const;
    bool operator!=(const ServerClosedError& err) const;

    std::string message() const;

private:
    std::variant<Code, TransportError, ServerClosedError> m_err;
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
    // Record metadata of all packets and messages
    bool recordStats = false;
    // Simulate packet loss, 0.0f means no packet loss, 1.0f means 100% packet loss (QUIC, UDP)
    float packetLossSimulation = 0.0f;
};

struct ConnectionOptions {
    ConnectionDebugOptions debug;
    std::optional<asp::time::Duration> activeKeepaliveInterval;
};

struct ConnectionSettings {
    std::string m_srvPrefix = "_qunet";
    ConnectionType m_preferredConnType = ConnectionType::Tcp;
    ConnectionOptions m_connOptions{};
    bool m_preferIpv6 = true;
    bool m_ipv4Enabled = true;
    bool m_ipv6Enabled = true;
    bool m_tlsCertVerification = true;
    asp::time::Duration m_connTimeout = asp::time::Duration::fromSecs(5);
    std::optional<std::filesystem::path> m_qdbFolder;
};

struct ConnectionCallbacks {
    move_only_function<void(ConnectionState)> m_connStateCallback;
    move_only_function<void(std::vector<uint8_t>)> m_dataCallback;
    move_only_function<void()> m_stateResetCallback;
};

struct WorkerThreadState;

struct ConnectionData {
    std::optional<qsox::SocketAddress> m_address;
    ConnectionType m_connType{ConnectionType::Unknown};

    uint16_t m_reconnectAttempt = 0;
    bool m_tryStatelessReconnect = false;
};

struct ChannelMsg {
    QunetMessage message;
    bool reliable = false;
    bool uncompressed = false;
};

struct WorkerThreadState {
    arc::mpsc::Receiver<std::string> connectChan;
    arc::mpsc::Receiver<ChannelMsg> msgChan;
    std::string connectHostname;
};

// Connection is a class that is a manager for connecting to a specific endpoint.
// It handles DNS resolution, happy eyeballs (choosing ipv4 or ipv6),
// choosing the best transport (TCP, UDP, QUIC, ...), and reconnection.
// It is completely asynchronous and uses threads internally.
class Connection {
public:
    static arc::Future<std::shared_ptr<Connection>> create();
    /// Do not use this. Use create() instead.
    Connection(
        arc::Runtime* runtime,
        arc::mpsc::Sender<std::string> connectChan,
        arc::mpsc::Sender<ChannelMsg> msgChan,
        WorkerThreadState wts
    );
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    // Destroys the connection, terminating the worker task.
    void destroy();

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

    /// Like `connect`, but will wait for the connection to be established, or fail.
    /// This will override some callbacks, so if you use custom callbacks, don't use this.
    arc::Future<ConnectionResult<>> connectWait(std::string_view destination);

    // Disconnect from the server, or aborts the current connection attempt.
    // Does nothing if disconnected.
    void disconnect();

    // Set the callback that is called when the connection state changes.
    void setConnectionStateCallback(move_only_function<void(ConnectionState)> callback);

    // Set the callback that is called when a data message is received.
    void setDataCallback(move_only_function<void(std::vector<uint8_t>)> callback);

    // Set the callback that is called when a stateless reconnect occurs.
    // A stateless reconnect is what typically occurs when the server is restarted, the client attempts to reconnect
    // but the server no longer has any state and prior knowledge of the client.
    // Thus, an entirely new connection has to be made, and with it, some connection state has to be reset.
    void setStateResetCallback(move_only_function<void()> callback);

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

    // Set the directory that will be used to save and load QDB files.
    // QDB files are files with compression dictionaries and other metadata,
    // they are already used automatically but caching them may significantly help
    // connection establishment times.
    void setQdbFolder(const std::filesystem::path& folder);

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

    // Get the current connection state.
    ConnectionState state() const;

    // Returns the last error that occurred during the connection process.
    ConnectionError lastError() const;

    // Sends a keepalive message to the server. Returns true on success,
    // false if not connected or message buffer is full.
    bool sendKeepalive();
    // Sends a data message to the server. Returns true on success,
    // false if not connected or message buffer is full.
    bool sendData(std::vector<uint8_t> data, bool reliable = true, bool uncompressed = false);

    /// Get a snapshot of various message data. If `period` is nonzero, will only include stats for that time period.
    /// If `period` is zero (default), will include all-time stats.
    StatSnapshot statSnapshot(asp::time::Duration period = {}) const;

    /// Like `statSnapshot` but includes all-time stats plus extra fields
    StatWholeSnapshot statSnapshotFull() const;

private:
    arc::Runtime* m_runtime;
    std::optional<arc::TaskHandle<void>> m_workerTask;
    WorkerThreadState m_wts;
    std::atomic<ConnectionState> m_connState{ConnectionState::Disconnected};
    arc::CancellationToken m_cancel;

    arc::mpsc::Sender<std::string> m_connectChan;
    arc::mpsc::Sender<ChannelMsg> m_msgChan;

    asp::Mutex<ConnectionSettings> m_settings;
    asp::SpinLock<ConnectionCallbacks> m_callbacks;
    asp::SpinLock<ConnectionData> m_data;
    asp::SpinLock<ConnectionError> m_lastError{ConnectionError::Success};
    std::optional<Socket> m_socket;
#ifdef QUNET_TLS_SUPPORT
    std::optional<ClientTlsContext> m_tlsContext;
#endif

    arc::Notify m_disconnectNotify;
    std::atomic<bool> m_disconnectReq{false};

    void setState(ConnectionState state);

    TransportOptions makeOptions(qsox::SocketAddress addr, ConnectionType type, bool reconnect);

    arc::Future<> workerThreadLoop();
    arc::Future<ConnectionResult<>> threadConnect(std::string url);
    arc::Future<ConnectionResult<>> threadConnectIp(qsox::SocketAddress addr, ConnectionType type);
    arc::Future<ConnectionResult<>> threadConnectDomain(std::string_view hostname, std::optional<uint16_t> port, ConnectionType type);
    arc::Future<ConnectionResult<>> threadConnectWithIps(std::vector<qsox::SocketAddress> addrs, ConnectionType type, bool preferIpv6);
    arc::Future<ConnectionResult<>> threadPingCandidates(std::vector<qsox::SocketAddress> addrs);
    arc::Future<ConnectionResult<>> threadFinalConnect(std::vector<std::pair<qsox::SocketAddress, ConnectionType>> addrs);
    arc::Future<ConnectionResult<>> threadEstablishConn(qsox::SocketAddress addr, ConnectionType type);
    arc::Future<ConnectionResult<>> threadConnectSocket(qsox::SocketAddress addr, ConnectionType type, bool reconnect);

    arc::Future<ConnectionResult<>> threadTryReconnect(bool stateless);

    arc::Future<> threadCancelConnection();

    void threadHandleIncomingMessage(QunetMessage msg);

    void resetConnectionState();
    void onConnectionError(const ConnectionError& err);
    void onFatalError(const ConnectionError& err);

    bool sendMsgToThread(QunetMessage&& message, bool reliable = true, bool uncompressed = false);
};

}