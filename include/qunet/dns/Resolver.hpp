#pragma once
#include <qunet/util/Error.hpp>
#include <qsox/Resolver.hpp> // we don't use qsox resolver, but we use its error type

#include <string>
#include <functional>

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

template <typename R>
using ResolverCallback = std::function<void(ResolverResult<R>)>;

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

private:
    void* m_channel = nullptr; // ares_channel_t

    Resolver();
    ~Resolver();
    void wineWorkaround();
};

}