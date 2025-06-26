#include <qunet/Log.hpp>

namespace qn::log {

void setLogFunction(LogFunction func) {
    getLogFunction() = std::move(func);
}

LogFunction& getLogFunction() {
    // default function
    static LogFunction function = [](Level level, const std::string& message) {
        switch (level) {
            case Level::Debug: {
                fmt::println("[Qunet] [DEBUG] {}", message);
            } break;

            case Level::Info: {
                fmt::println("[Qunet] [INFO] {}", message);
            } break;

            case Level::Warning: {
                fmt::println("[Qunet] [WARNING] {}", message);
            } break;

            case Level::Error: {
                fmt::println("[Qunet] [ERROR] {}", message);
            } break;
        }
    };

    return function;
}

}