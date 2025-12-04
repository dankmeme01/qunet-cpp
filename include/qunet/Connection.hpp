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
        DnsResolutionFailed,
        AllAddressesFailed,
        ProtocolDisabled,
        NoConnectionTypeFound,
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
    move_only_function<void(ConnectionState)> m_connStateCallback;
    move_only_function<void(std::vector<uint8_t>)> m_dataCallback;
};

struct WorkerThreadState;
struct ConnectionData {};

// Connection is a class that is a manager for connecting to a specific endpoint.
// It handles DNS resolution, happy eyeballs (choosing ipv4 or ipv6),
// choosing the best transport (TCP, UDP, QUIC, ...), and reconnection.
// It is completely asynchronous and uses threads internally.
class Connection {
public:
    static arc::Future<std::shared_ptr<Connection>> create();
    /// Do not use this. Use create() instead.
    Connection(
        arc::mpsc::Sender<std::string> connectChan
    );
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

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

    ConnectionState state() const;
private:
    std::optional<arc::TaskHandle<void>> m_workerTask;
    std::atomic<ConnectionState> m_connState{ConnectionState::Disconnected};
    arc::CancellationToken m_cancel;

    arc::mpsc::Sender<std::string> m_connectChan;

    asp::Mutex<ConnectionSettings> m_settings;
    ConnectionData m_data;

    void setState(ConnectionState state);

    arc::Future<> workerThreadLoop(WorkerThreadState& wts);
    arc::Future<ConnectionResult<>> threadConnect(std::string url);
    arc::Future<ConnectionResult<>> threadConnectIp(qsox::SocketAddress addr, ConnectionType type);
    arc::Future<ConnectionResult<>> threadConnectDomain(std::string_view hostname, std::optional<uint16_t> port, ConnectionType type);
    arc::Future<ConnectionResult<>> threadConnectWithIps(std::vector<qsox::SocketAddress> addrs, ConnectionType type);
};

}