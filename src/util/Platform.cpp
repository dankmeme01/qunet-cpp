#include <qunet/util/Platform.hpp>

#include <Windows.h>

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