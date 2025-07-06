#include <qunet/dns/Resolver.hpp>
#include <qunet/util/Platform.hpp>
#include <qunet/Log.hpp>

#include <qsox/BaseSocket.hpp>
#include <ares.h>
#include <algorithm>

namespace qn {

Resolver& Resolver::get() {
    static Resolver instance;
    return instance;
}

Resolver::Resolver() {
    // we must do wsastartup manually, ares does not do it
    auto sres = qsox::initSockets();
    if (!sres) {
        log::error("Failed to initialize sockets, error: {}", sres.unwrapErr().message());
        return;
    }

    int res = ares_library_init(ARES_LIB_INIT_ALL);
    if (res != ARES_SUCCESS) {
        log::error("ares_library_init failed with code {}: {}", res, ares_strerror(res));
        return;
    }

    // TODO: apparently we need to call ares_library_init_jvm and ares_library_init_android here on android

    int optmask = ARES_OPT_TIMEOUTMS | ARES_OPT_EVENT_THREAD;
    ares_options options = {};
    options.evsys = ARES_EVSYS_DEFAULT;
    options.timeout = 1500;

    if (qn::isWine()) {
        this->wineWorkaround();
    }

    res = ares_init_options((ares_channel_t**)&m_channel, &options, optmask);
    if (res != ARES_SUCCESS) {
        log::error("ares_init_options failed with code {}: {}", res, ares_strerror(res));
        m_channel = nullptr;
        return;
    }
}

void Resolver::wineWorkaround() {
#ifdef _WIN32
    // the registry keys here don't exist in wine for some reason, so we need to create them
    // https://github.com/c-ares/c-ares/blob/2a3e30361c180adc6a622fe7955a2235f18e0436/src/lib/event/ares_event_configchg.c#L324

    auto createIfNeeded = [](const wchar_t* name) {
        HKEY hKey;
        DWORD disp;

        LONG res = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            name,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_NOTIFY,
            nullptr,
            &hKey,
            &disp
        );

        if (res == ERROR_SUCCESS) {
            log::debug("Created registry key!");
            RegCloseKey(hKey);
        } else {
            log::warn("Failed to create registry key: {}", res);
        }
    };

    createIfNeeded(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces");
    createIfNeeded(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces");
#endif
}

Resolver::~Resolver() {
    ares_queue_wait_empty((ares_channel_t*)m_channel, -1); // wait for all queries to finish
    ares_destroy((ares_channel_t*)m_channel);
    ares_library_cleanup();
}

static ResolverError errorFromStatus(ares_status_t status) {
    switch (status) {
        case ARES_SUCCESS: return ResolverError::Success;
        case ARES_ENODATA: return ResolverError::NoData;
        case ARES_ESERVFAIL: return ResolverError::TemporaryFailure;
        case ARES_ENOTFOUND: return ResolverError::UnknownHost;
        case ARES_ENOMEM: return ResolverError::OutOfMemory;
        case ARES_ETIMEOUT: return ResolverError::TemporaryFailure;
        default: return ResolverError::Other;
    }
}

template <typename T>
static ResolverResult<T> parseQuery(const ares_dns_record_t* result);

template <typename T>
static void queryCallback(void* arg, ares_status_t status, size_t timeouts, const ares_dns_record_t* result) {
    auto& callback = *reinterpret_cast<ResolverCallback<T>*>(arg);
    ares_srv_reply* a;

    switch (status) {
        case ARES_SUCCESS: {
            auto res = parseQuery<T>(result);
            callback(std::move(res));
        } break;

        default: {
            log::warn("DNS query failed with status {}: {}", (int) status, ares_strerror(status));
            callback(Err(errorFromStatus(status)));
        } break;
    }

    delete &callback; // clean up the callback
}

ResolverResult<> Resolver::queryA(const std::string& name, ResolverCallback<DNSRecordA> callback) {
    log::debug("(Resolver) queryA for {}", name);

    auto res = ares_query_dnsrec(
        (ares_channel_t*)m_channel,
        name.c_str(),
        ARES_CLASS_IN,
        ARES_REC_TYPE_A,
        &queryCallback<DNSRecordA>,
        new auto {std::move(callback)},
        nullptr
    );

    if (res != ARES_SUCCESS) {
        log::warn("queryA failed with code {}: {}", (int) res, ares_strerror(res));
        return Err(errorFromStatus(res));
    }

    return Ok();
}

ResolverResult<> Resolver::queryAAAA(const std::string& name, ResolverCallback<DNSRecordAAAA> callback) {
    log::debug("(Resolver) queryAAAA for {}", name);

    auto res = ares_query_dnsrec(
        (ares_channel_t*)m_channel,
        name.c_str(),
        ARES_CLASS_IN,
        ARES_REC_TYPE_AAAA,
        &queryCallback<DNSRecordAAAA>,
        new auto {std::move(callback)},
        nullptr
    );

    if (res != ARES_SUCCESS) {
        log::warn("queryAAAA failed with code {}: {}", (int) res, ares_strerror(res));
        return Err(errorFromStatus(res));
    }

    return Ok();
}

ResolverResult<> Resolver::querySRV(const std::string& name, ResolverCallback<DNSRecordSRV> callback) {
    log::debug("(Resolver) querySRV for {}", name);

    auto res = ares_query_dnsrec(
        (ares_channel_t*)m_channel,
        name.c_str(),
        ARES_CLASS_IN,
        ARES_REC_TYPE_SRV,
        &queryCallback<DNSRecordSRV>,
        new auto {std::move(callback)},
        nullptr
    );

    if (res != ARES_SUCCESS) {
        log::warn("querySRV failed with code {}: {}", (int) res, ares_strerror(res));
        return Err(errorFromStatus(res));
    }

    return Ok();
}

template <>
ResolverResult<DNSRecordA> parseQuery(const ares_dns_record_t* result) {
    DNSRecordA record;

    for (size_t i = 0; i < ares_dns_record_rr_cnt(result, ARES_SECTION_ANSWER); i++) {
        auto rr = ares_dns_record_rr_get_const(result, ARES_SECTION_ANSWER, i);
        auto recordType = ares_dns_rr_get_type(rr);

        if (recordType != ARES_REC_TYPE_A) {
            continue;
        }

        auto addr = ares_dns_rr_get_addr(rr, ARES_RR_A_ADDR);

        if (addr) {
            record.addresses.emplace_back(qsox::Ipv4Address::fromInAddr(*addr));
        }
    }

    if (record.addresses.empty()) {
        return Err(ResolverError::NoData);
    }

    return Ok(std::move(record));
}

template <>
ResolverResult<DNSRecordAAAA> parseQuery(const ares_dns_record_t* result) {
    DNSRecordAAAA record;

    for (size_t i = 0; i < ares_dns_record_rr_cnt(result, ARES_SECTION_ANSWER); i++) {
        auto rr = ares_dns_record_rr_get_const(result, ARES_SECTION_ANSWER, i);
        auto recordType = ares_dns_rr_get_type(rr);

        if (recordType != ARES_REC_TYPE_AAAA) {
            continue;
        }

        auto addr = (const in6_addr*) ares_dns_rr_get_addr6(rr, ARES_RR_AAAA_ADDR);

        if (addr) {
            record.addresses.emplace_back(qsox::Ipv6Address::fromInAddr(*addr));
        }
    }

    if (record.addresses.empty()) {
        return Err(ResolverError::NoData);
    }

    return Ok(std::move(record));
}

template <>
ResolverResult<DNSRecordSRV> parseQuery(const ares_dns_record_t* result) {
    DNSRecordSRV record;

    for (size_t i = 0; i < ares_dns_record_rr_cnt(result, ARES_SECTION_ANSWER); i++) {
        auto rr = ares_dns_record_rr_get_const(result, ARES_SECTION_ANSWER, i);
        auto recordType = ares_dns_rr_get_type(rr);

        if (recordType != ARES_REC_TYPE_SRV) {
            continue;
        }

        auto target = ares_dns_rr_get_str(rr, ARES_RR_SRV_TARGET);
        if (!target) {
            log::warn("SRV record without target, skipping");
            continue;
        }

        SRVEndpoint endp;
        endp.target = target;
        endp.port = ares_dns_rr_get_u16(rr, ARES_RR_SRV_PORT);
        endp.priority = ares_dns_rr_get_u16(rr, ARES_RR_SRV_PRIORITY);
        endp.weight = ares_dns_rr_get_u16(rr, ARES_RR_SRV_WEIGHT);

        record.endpoints.push_back(std::move(endp));
    }

    if (record.endpoints.empty()) {
        return Err(ResolverError::NoData);
    }

    // Sort endpoints by priority and weight
    std::sort(record.endpoints.begin(), record.endpoints.end(), [](const SRVEndpoint& a, const SRVEndpoint& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.weight < b.weight;
    });

    return Ok(std::move(record));
}

}