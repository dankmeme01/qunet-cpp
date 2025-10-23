#include <qunet/dns/Resolver.hpp>

namespace qn {

Resolver& Resolver::get() {
    static Resolver instance;
    return instance;
}

}