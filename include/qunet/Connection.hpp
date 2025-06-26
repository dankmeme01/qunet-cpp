#pragma once

#include "socket/Socket.hpp"
#include "Pinger.hpp"

#include <asp/sync/Channel.hpp>
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
        AlreadyConnected,
        DnsResolutionFailed,
        AllAddressesFailed,
        ProtocolDisabled,
        NoConnectionTypeFound,
    } Code;

    constexpr inline ConnectionError(Code code) : m_err(code) {}
    constexpr inline ConnectionError(qsox::Error err) : m_err(err) {}

    bool isSocketError() const;
    bool isOtherError() const;

    qsox::Error asSocketError() const;
    Code asOtherError() const;

    bool operator==(const ConnectionError& other) const = default;
    bool operator!=(const ConnectionError& other) const = default;
    bool operator==(Code code) const;
    bool operator!=(Code code) const;

    std::string message() const;

private:
    std::variant<Code, qsox::Error> m_err;
};

template <typename T = void>
using ConnectionResult = geode::Result<T, ConnectionError>;

enum class ConnectionState {
    Disconnected,
    DnsResolving,
    Pinging,
    Connecting,
    Connected,
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

    // Set the connection timeout. This is applied per a specific connection attempt to a single IP address with a single protocol.
    // If multiple IPv4/IPv6 addresses and protocols are available, the full connection attempt may take way longer than this.
    // Default is 5 seconds.
    void setConnectTimeout(asp::time::Duration dur);

    bool connecting() const;
    bool connected() const;
    ConnectionState state() const;

    // Returns the last error that occurred during the connection process.
    ConnectionError lastError() const;

private:
    // vvv settings vvv
    std::string m_srvPrefix = "_qunet";
    ConnectionType m_preferredConnType = ConnectionType::Tcp;
    bool m_preferIpv6 = true;
    bool m_ipv4Enabled = true;
    bool m_ipv6Enabled = true;
    asp::time::Duration m_connTimeout = asp::time::Duration::fromSecs(5);

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
    size_t m_dnsRequests = 0;
    size_t m_dnsResponses = 0;
    ConnectionType m_chosenConnType = ConnectionType::Unknown;
    std::vector<qsox::IpAddress> m_usedIps;
    uint16_t m_usedPort;

    asp::Mutex<void, true> m_internalMutex; // Guards certain fields above
    asp::Thread<> m_thread;

    // vvv these are internal fields used by the thread vvv
    asp::time::SystemTime m_thrFirstDnsResponseTime;
    asp::time::SystemTime m_thrStartedPingingAt;
    asp::time::SystemTime m_thrLastArrivedPing;
    size_t m_thrLastDnsResponseCount = 0;
    size_t m_thrArrivedPings = 0;
    std::vector<std::pair<qsox::SocketAddress, asp::time::Duration>> m_thrPingResults;
    std::vector<SupportedProtocol> m_thrPingerSupportedProtocols;
    size_t m_thrConnIpIndex = 0;
    size_t m_thrConnTypeIndex = 0;

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
    void finishCancellation();
    void clearLastError();
    void resetConnectionState();
    void sortUsedIps();

    // Call when DNS resolution is over, we can proceed to either pinging or connecting to the destination
    void doneResolving();

    // vvv thread functions, do not call outside of the thread vvv
    void thrTryConnectNext();
    void thrTryConnectWith(const qsox::SocketAddress& addr, ConnectionType type);
    bool thrConnecting();
    void thrNewPingResult(const PingResult& result, const qsox::SocketAddress& addr);
    void thrSortPingResults();
    void thrSortConnTypes();
};

}
