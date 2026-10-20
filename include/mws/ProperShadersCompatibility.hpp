#pragma once
#include "mws/Config.hpp"
#include "mws/Diagnostics.hpp"
#include <Windows.h>
#include <atomic>

namespace mws::ProperShadersCompatibility {
inline std::atomic<bool> detected{false};
inline bool active() noexcept { return detected.load(std::memory_order_relaxed); }
// Loaded modules only: an installed but inactive ASI must not disable features.
inline bool loaded() noexcept {
    if (active()) return true;
    const bool found = GetModuleHandleW(L"ProperShaders.asi") ||
           GetModuleHandleW(L"ProperShaders.dll") ||
           GetModuleHandleW(L"proper_shaders.asi") ||
           GetModuleHandleW(L"proper_shaders.dll");
    if (found) detected.store(true, std::memory_order_relaxed);
    return found;
}

inline void report(const Config& c) {
    detected.store(true, std::memory_order_relaxed);
    auto& d = Diagnostics::instance();
    struct Feature { const char* name; bool requested; const char* reason; };
    const Feature features[] = {
        {"Optimization.D3DStateDedup", c.d3dStateDeduplication,
         "Proper Shaders proxy tracks setter side effects and rewrites depth state; state suppression disabled"},
        {"Optimization.SkygfxShaderBindings", c.skygfxShaderBindingOptimization,
         "Proper Shaders owns shader bindings and proxy change tracking"},
        {"Optimization.SkygfxConstants", c.skygfxShaderConstantOptimization,
         "Proper Shaders owns shader constants and proxy change tracking"},
        {"Optimization.ImmediateBatching", c.immediateBatching,
         "Proper Shaders owns draw batching and multipass submission"},
        {"Optimization.StaticPropInstancing", c.staticPropInstancing,
         "Proper Shaders replacement pipelines require their own shaders and draw order"},
        {"Optimization.RenderWareBatching", c.renderWareBatching,
         "Proper Shaders replaces building pipelines"},
        {"Optimization.RenderWareDayNightBatching", c.renderWareDayNightBatching,
         "Proper Shaders replaces building pipelines"},
        {"Optimization.RenderWarePlanCache", c.renderWarePlanCache,
         "Proper Shaders replacement pipelines invalidate stock render plans"},
        {"Optimization.VisibilityBoundsCache", c.visibilityBoundsCache,
         "Proper Shaders uses visibility helpers for additional render passes; legacy opt-in overridden"},
        {"Optimization.AlphaListIndex", c.alphaListIndex,
         "Proper Shaders intercepts fading/transparent rendering; stock list assumptions unverified"},
        {"Optimization.ShadowGroupIndex", c.shadowGroupIndex,
         "Proper Shaders owns shadow passes; stock grouping assumptions unverified"},
        {"FXP.InstancedSubmission", c.fxpEnabled && c.fxpInstancedSubmission,
         "Proper Shaders wraps FxManager render and supplies soft-particle shading"},
    };
    for (const auto& f : features) {
        if (!f.requested) continue;
        d.ensureFeature(f.name, true);
        d.autoDisable(f.name, f.reason);
    }
}
} // namespace mws::ProperShadersCompatibility
