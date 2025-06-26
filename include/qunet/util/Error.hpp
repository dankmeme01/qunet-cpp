#pragma once

#include <Geode/Result.hpp>
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
    }

namespace qn {

using geode::Ok;
using geode::Err;

[[noreturn]] inline void unreachable() {
#if defined __clang__ || defined __GNUC__
    __builtin_unreachable();
#elif defined _MSC_VER
    __assume(0);
#endif
}

}
