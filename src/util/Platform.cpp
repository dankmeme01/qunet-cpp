#include <qunet/util/Platform.hpp>

#ifdef _WIN32
# include <Windows.h>
#endif

namespace qn {

#ifdef _WIN32
static bool _checkWine() {
    return GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version");
}
#else
static bool _checkWine() { return false; }
#endif

bool isWine() {
    static bool wine = _checkWine();
    return wine;
}

}