#pragma once

namespace qn {

/// Returns whether any kind of cleanup is currently unsafe.
/// If this returns `true`, it means we are on Windows and the DLL is being unloaded.
/// This means, mutex locks and many other operations may crash or hang and cleanup should be avoided.
bool isCleanupUnsafe();

}