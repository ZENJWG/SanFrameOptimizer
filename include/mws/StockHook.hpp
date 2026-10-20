#pragma once
#include "mws/GtaSa10US.hpp"
#include "mws/ExecutableInfo.hpp"
#include <MinHook.h>
#include <cstring>
#include <initializer_list>

namespace mws::stock {
inline bool references(std::uintptr_t address, std::size_t length, std::uint32_t value) {
    if (!gta::addressIsReadable(address, length)) return false;
    for (std::size_t i = 0; i + sizeof(value) <= length; ++i) {
        std::uint32_t operand;
        std::memcpy(&operand, reinterpret_cast<const void*>(address + i), sizeof(operand));
        if (operand == value) return true;
    }
    return false;
}
// Structural US 1.0 guard, not a checksum. Never chain an unknown entry detour.
inline bool matches(std::uintptr_t address, std::size_t length,
                    std::initializer_list<std::uintptr_t> calls) {
    const auto exe = ExecutableInfo::current();
    if (!exe.validPe || reinterpret_cast<std::uintptr_t>(exe.base) != 0x400000 ||
        address < 0x400000 || address - 0x400000 + length > exe.imageSize ||
        !gta::addressIsExecutable(address) || !gta::addressIsReadable(address, length))
        return false;
    auto* bytes = reinterpret_cast<const unsigned char*>(address);
    if (bytes[0] == 0xE9 || bytes[0] == 0xEB || bytes[0] == 0xFF ||
        bytes[0] == 0x68 || bytes[0] == 0xB8 || bytes[0] == 0xCC)
        return false;
    for (auto target : calls) {
        bool found = false;
        for (std::size_t i = 0; i + 5 <= length; ++i) {
            if (bytes[i] != 0xE8) continue;
            std::int32_t delta;
            std::memcpy(&delta, bytes + i + 1, sizeof(delta));
            found |= static_cast<std::int64_t>(address + i + 5) + delta == target;
        }
        if (!found) return false;
    }
    return true;
}
template<class F, class H> bool hook(std::uintptr_t address, H replacement, F& original) {
    auto* p = reinterpret_cast<void*>(address);
    if (MH_CreateHook(p, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original)) != MH_OK)
        return false;
    if (MH_EnableHook(p) == MH_OK) return true;
    MH_RemoveHook(p);
    original = nullptr;
    return false;
}
template<class F> void unhook(std::uintptr_t address, F& original) {
    if (!original) return;
    MH_DisableHook(reinterpret_cast<void*>(address));
    MH_RemoveHook(reinterpret_cast<void*>(address));
    original = nullptr;
}
}
