#pragma once
#include <string_view>

#define QN_ASSERT(condition) \
    do { \
        if (!(condition)) [[unlikely]] { \
            ::qn::_assertionFail(#condition, __FILE__, __LINE__); \
        } \
    } while (false)

#if defined (QUNET_DEBUG)
# define QN_DEBUG_ASSERT(condition) QN_ASSERT(condition)
#else
# define QN_DEBUG_ASSERT(condition) (void)0
#endif

namespace qn {
    [[noreturn]] void _assertionFail(std::string_view what, std::string_view file, int line);
}
