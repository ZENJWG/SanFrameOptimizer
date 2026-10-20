#pragma once

namespace fxp {
// Hook destruction suspends threads and may release D3D resources. The DLL's
// CRT invokes global destructors under the Windows loader lock at process exit.
// Keep these small owners process-lived; explicit Uninstall still resets hooks
// and releases their resources outside DllMain. Windows reclaims the empty
// owner storage on exit, without running hook/driver code under the loader lock.
template<class T> T& processLifetime() {
    static T* value = new T{};
    return *value;
}
}
