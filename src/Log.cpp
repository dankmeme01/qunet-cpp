#include <qunet/Log.hpp>

namespace qn::log {

void setLogFunction(LogFunction func) {
    getLogFunction() = std::move(func);
}

LogFunction& getLogFunction() {
    // default function
    static LogFunction function = [](Level level, const std::string& message) {
        fmt::println("[Qunet] [{}] {}", levelToString(level), message);
    };

    return function;
}

}