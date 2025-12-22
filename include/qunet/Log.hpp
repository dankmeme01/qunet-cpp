#pragma once

#include <fmt/format.h>
#include "util/compat.hpp"

#include <string>

namespace qn::log {

enum class Level {
    Debug, Info, Warning, Error
};

using LogFunction = move_only_function<void(Level, const std::string&)>;

void setLogFunction(LogFunction func);
LogFunction& getLogFunction();

template <typename... Args>
void debug(fmt::format_string<Args...> fmt, Args&&... args) {
    getLogFunction()(Level::Debug, fmt::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void info(fmt::format_string<Args...> fmt, Args&&... args) {
    getLogFunction()(Level::Info, fmt::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(fmt::format_string<Args...> fmt, Args&&... args) {
    getLogFunction()(Level::Warning, fmt::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(fmt::format_string<Args...> fmt, Args&&... args) {
    getLogFunction()(Level::Error, fmt::format(fmt, std::forward<Args>(args)...));
}

inline std::string_view levelToString(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error: return "ERROR";
    }
    return "UNKNOWN";
}

}