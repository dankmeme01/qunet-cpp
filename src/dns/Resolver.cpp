#include <qunet/dns/Resolver.hpp>
#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>
#include <arc/sync/mpsc.hpp>
#include <arc/time/Timeout.hpp>

using namespace arc;
using namespace asp::time;

namespace qn {

Resolver& Resolver::get() {
    static Resolver instance;
    return instance;
}

template <auto Fn, typename R, typename Res = ResolverResult<R>>
static Future<Res> wrapSync(Resolver* self, const std::string& name) {
    auto [tx, rx] = mpsc::channel<Res>(1);
    auto res = (self->*Fn)(name, [tx = std::move(tx)](Res record) mutable {
        (void) tx.trySend(std::move(record));
    });

    if (!res) {
        co_return Err(res.unwrapErr());
    }

    auto tres = co_await arc::timeout(
        Duration::fromSecs(10),
        rx.recv()
    );

    if (!tres.isOk()) {
        // timed out
        log::warn("DNS query for {} timed out", name);
        co_return Err(ResolverError::Other);
    }

    auto rres = std::move(tres).unwrap();
    QN_ASSERT(rres.isOk()); // channel recv() should never fail

    co_return std::move(rres).unwrap();
}

Future<ResolverResult<DNSRecordA>> Resolver::asyncQueryA(const std::string& name) {
    return wrapSync<&Resolver::queryA, DNSRecordA>(this, name);
}

Future<ResolverResult<DNSRecordAAAA>> Resolver::asyncQueryAAAA(const std::string& name) {
    return wrapSync<&Resolver::queryAAAA, DNSRecordAAAA>(this, name);
}

Future<ResolverResult<DNSRecordSRV>> Resolver::asyncQuerySRV(const std::string& name) {
    return wrapSync<&Resolver::querySRV, DNSRecordSRV>(this, name);
}


}