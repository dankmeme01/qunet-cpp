#ifndef QUNET_ADVANCED_DNS

#include <qunet/dns/Resolver.hpp>
#include <qunet/util/Platform.hpp>
#include <qunet/Log.hpp>

#include <qsox/BaseSocket.hpp>
#include <asp/thread/Thread.hpp>
#include <asp/sync/Channel.hpp>
#include <asp/time.hpp>

using namespace asp::time;

namespace qn {

struct ResolverQuery {
    bool aaaa;
    std::string name;
    union {
        ResolverCallback<DNSRecordA> aCallback;
        ResolverCallback<DNSRecordAAAA> aaaaCallback;
    };

    ResolverQuery(std::string name, ResolverCallback<DNSRecordA> cb)
        : name(std::move(name)), aaaa(false), aCallback(std::move(cb)) {}

    ResolverQuery(std::string name, ResolverCallback<DNSRecordAAAA> cb)
        : name(std::move(name)), aaaa(true), aaaaCallback(std::move(cb)) {}

    ResolverQuery(ResolverQuery&& other) noexcept
        : aaaa(other.aaaa), name(std::move(other.name)) {
        if (aaaa) {
            new (&aaaaCallback) ResolverCallback<DNSRecordAAAA>(std::move(other.aaaaCallback));
        } else {
            new (&aCallback) ResolverCallback<DNSRecordA>(std::move(other.aCallback));
        }
    }

    ~ResolverQuery() {
        if (aaaa) {
            aaaaCallback.~move_only_function();
        } else {
            aCallback.~move_only_function();
        }
    }
};

struct ResolverData {
    asp::Thread<> thread;
    asp::Channel<ResolverQuery> channel;

    static ResolverData& from(Resolver& r) {
        return from(&r);
    }

    static ResolverData& from(Resolver* ptr) {
        return *reinterpret_cast<ResolverData*>(ptr->m_data);
    }

    // static ResolverData& from(void* ptr) {
    //     return *reinterpret_cast<ResolverData*>(ptr);
    // }
};

Resolver::Resolver() {
    auto sres = qsox::initSockets();
    if (!sres) {
        log::error("Failed to initialize sockets, error: {}", sres.unwrapErr().message());
        return;
    }

    auto data = new ResolverData {};
    m_data = data;
    data->thread.setName("DNS Resolver Thread");
    data->thread.setLoopFunction([data](auto& stopToken) {
        auto opt = data->channel.popTimeout(Duration::fromMillis(250));
        if (!opt) return;

        auto& query = opt.value();

        if (query.aaaa) {
            query.aaaaCallback(qsox::resolver::resolveIpv6(query.name).map([](const auto& ip) {
                return DNSRecordAAAA {.addresses = {ip}};
            }));
        } else {
            query.aCallback(qsox::resolver::resolveIpv4(query.name).map([](const auto& ip) {
                return DNSRecordA {.addresses = {ip}};
            }));
        }
    });
    data->thread.start();
}

Resolver::~Resolver() {
    ResolverData::from(this).thread.detach();
}

ResolverResult<> Resolver::queryA(const std::string& name, ResolverCallback<DNSRecordA> callback) {
    log::debug("(Resolver) queryA for {}", name);

    ResolverData::from(this).channel.push(ResolverQuery(name, std::move(callback)));

    return Ok();
}

ResolverResult<> Resolver::queryAAAA(const std::string& name, ResolverCallback<DNSRecordAAAA> callback) {
    log::debug("(Resolver) queryAAAA for {}", name);

    ResolverData::from(this).channel.push(ResolverQuery(name, std::move(callback)));

    return Ok();
}

ResolverResult<> Resolver::querySRV(const std::string& name, ResolverCallback<DNSRecordSRV> callback) {
    log::debug("(Resolver) querySRV for {}", name);
    log::warn("(Resolver) SRV queries not supported with basic DNS resolver!");

    return Err(ResolverError::Other);
}

}

#endif