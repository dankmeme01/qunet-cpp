#pragma once
#include <qunet/util/Error.hpp>
#include <qunet/util/compat.hpp>
#include <qsox/Resolver.hpp> // we don't use qsox resolver, but we use its error type
#include <arc/future/Future.hpp>
#include <asp/sync/Mutex.hpp>

#include <string>

namespace qn {

using ResolverError = qsox::resolver::Error;
template <typename T = void>
using ResolverResult = geode::Result<T, ResolverError>;

enum class QueryType {
    A,
    AAAA,
    Address,
    SRV,
    TXT,
};

struct DNSRecordA {
    std::vector<qsox::Ipv4Address> addresses;
};

struct DNSRecordAAAA {
    std::vector<qsox::Ipv6Address> addresses;
};

struct SRVEndpoint {
    std::string target;
    uint16_t port = 0;
    uint16_t priority = 0;
    uint16_t weight = 0;
};

struct DNSRecordSRV {
    std::vector<SRVEndpoint> endpoints;
};

enum class DNSTransport {
    Udp,
    Tcp,
    Tls,
    Https,
};

template <typename R>
using ResolverCallback = move_only_function<void(ResolverResult<R>)>;

class Resolver {
public:
    static Resolver& get();
    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;
    Resolver(Resolver&&) = delete;
    Resolver& operator=(Resolver&&) = delete;

    ResolverResult<> queryA(const std::string& name, ResolverCallback<DNSRecordA> callback);
    ResolverResult<> queryAAAA(const std::string& name, ResolverCallback<DNSRecordAAAA> callback);
    ResolverResult<> querySRV(const std::string& name, ResolverCallback<DNSRecordSRV> callback);

    arc::Future<ResolverResult<DNSRecordA>> asyncQueryA(const std::string& name);
    arc::Future<ResolverResult<DNSRecordAAAA>> asyncQueryAAAA(const std::string& name);
    arc::Future<ResolverResult<DNSRecordSRV>> asyncQuerySRV(const std::string& name);

    void setCustomDnsServer(qsox::IpAddress addr);
    void setCustomDnsServers(std::optional<qsox::IpAddress> primary, std::optional<qsox::IpAddress> secondary = std::nullopt);
    geode::Result<> setDnsTransport(DNSTransport transport);

#ifdef QUNET_ADVANCED_DNS
    template <typename T>
    void cacheRecord(const std::string& name, const T& record);
#endif

private:
#ifdef QUNET_ADVANCED_DNS
    void* m_channel = nullptr; // ares_channel_t
    std::optional<qsox::IpAddress> m_primaryNs, m_secondaryNs;
    DNSTransport m_transport = DNSTransport::Udp;
    asp::Mutex<std::unordered_map<std::string, DNSRecordA>> m_aCache;
    asp::Mutex<std::unordered_map<std::string, DNSRecordAAAA>> m_aaaaCache;
    asp::Mutex<std::unordered_map<std::string, DNSRecordSRV>> m_srvCache;

    void reloadServers();
    std::optional<DNSRecordA> getCachedA(const std::string& name);
    std::optional<DNSRecordAAAA> getCachedAAAA(const std::string& name);
    std::optional<DNSRecordSRV> getCachedSRV(const std::string& name);
#else
    friend class ResolverData;
    void* m_data = nullptr;
#endif

    Resolver();
    ~Resolver();
    void wineWorkaround();
};

}