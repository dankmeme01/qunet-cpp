#pragma once

#include <functional> // bug in geode result
#include <Geode/Result.hpp>
#include <cassert>
#include <string_view>

// taken from qsox
#define QN_MAKE_ERROR_STRUCT(name, ...) \
    class name { \
    public: \
        typedef enum {\
            __VA_ARGS__ \
        } Code; \
        constexpr inline name(Code code) : m_code(code) {} \
        constexpr inline name(const name& other) = default; \
        constexpr inline name& operator=(const name& other) = default; \
        constexpr inline Code code() const { return m_code; } \
        constexpr inline bool operator==(const name& other) const { return m_code == other.m_code; } \
        constexpr inline bool operator!=(const name& other) const { return !(*this == other); } \
        std::string_view message() const; \
    private: \
        Code m_code; \
    }; \
    inline auto format_as(const name& err) { return err.message(); }

namespace qn {

using geode::Ok;
using geode::Err;

#ifndef QUNET_DEBUG
[[noreturn]] inline void unreachable() {
#if defined __clang__ || defined __GNUC__
    __builtin_unreachable();
#elif defined _MSC_VER
    __assume(0);
#endif
}

#else

[[noreturn]] inline void unreachable() {
    assert(false && "Unreachable code reached!");
    __builtin_trap();
}

#endif

}
