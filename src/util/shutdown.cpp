#include <qunet/util/shutdown.hpp>
#ifdef _WIN32
# include <windows.h>
#endif

namespace qn {

bool isCleanupUnsafe() {
#ifdef _WIN32
    // if a debugger is attached, always cleanup
    if (IsDebuggerPresent()) {
        return false;
    }

    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto shutdownInProgress = (BOOL(WINAPI*)())GetProcAddress(ntdll, "RtlDllShutdownInProgress");
    return shutdownInProgress && shutdownInProgress();
#else
    return false;
#endif
}

}
