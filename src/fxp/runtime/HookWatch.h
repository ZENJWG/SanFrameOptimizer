#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace fxp {
struct HookWatch {
    uintptr_t address{};
    std::array<unsigned char, 16> installed{};
    bool reported{};
    void capture(uintptr_t value) { address = value; std::memcpy(installed.data(), reinterpret_cast<void*>(value), installed.size()); reported = false; }
    void check(void(*log)(const char*)) {
        if (!address || reported || !log || std::memcmp(installed.data(), reinterpret_cast<void*>(address), installed.size()) == 0) return;
        reported = true;
        auto* bytes = reinterpret_cast<const unsigned char*>(address);
        uintptr_t target{};
        if (bytes[0] == 0xE9) { int32_t displacement{}; std::memcpy(&displacement, bytes + 1, 4); target = address + 5 + displacement; }
        HMODULE module{}; char name[512] = "unknown";
        if (target && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(target), &module)) {
            wchar_t path[512]{};
            if (GetModuleFileNameW(module, path, 512)) WideCharToMultiByte(CP_UTF8, 0, path, -1, name, 512, nullptr, nullptr);
        }
        char message[768];
        std::snprintf(message, sizeof(message), "HOOK OVERWRITTEN address=0x%08lX target=0x%08lX module=%s. Installed replacement entry changed after initialization; this path may be bypassed.",
            static_cast<unsigned long>(address), static_cast<unsigned long>(target), name);
        log(message);
    }
};
}
