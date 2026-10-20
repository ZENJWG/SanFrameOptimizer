#pragma once
#include <Windows.h>
#include <imgui.h>
#include <limits>
#include <cstdio>

namespace mws::OverlayFont {
inline HMODULE containingModule(const void* address) noexcept {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}

inline ImFont* addEmbedded(ImFontAtlas& atlas, HMODULE module, int resourceId, float pixels) {
    if (!module) return nullptr;
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10));
    if (!resource) return nullptr;
    const auto size = SizeofResource(module, resource);
    const auto loaded = LoadResource(module, resource);
    auto* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size <= 100 || size > static_cast<DWORD>((std::numeric_limits<int>::max)()))
        return nullptr;
    ImFontConfig config;
    // The ASI is pinned for its lifetime, so its read-only resource remains
    // valid for every lazy glyph bake. Avoid a second retained copy of the TTF.
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 4;
    config.OversampleV = 4;
    config.PixelSnapH = false;
    std::snprintf(config.Name, sizeof(config.Name), "BankGothic Bold (embedded)");
    return atlas.AddFontFromMemoryTTF(data, static_cast<int>(size), pixels, &config);
}

inline ImFont* addFallback(ImFontAtlas& atlas, float pixels) {
    ImFontConfig config;
    config.SizePixels = pixels;
    return atlas.AddFontDefaultVector(&config);
}
}
