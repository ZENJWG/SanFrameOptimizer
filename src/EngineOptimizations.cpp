#include "mws/ProperShadersCompatibility.hpp"
#include "mws/OptimizationReport.hpp"
#include "mws/EngineOptimizations.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/StockHook.hpp"
#include "mws/Logger.hpp"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <string>

namespace {
template<class T> T& at(std::uintptr_t address) { return *reinterpret_cast<T*>(address); }
template<class T> T field(const void* p, std::size_t offset) {
    T result;
    std::memcpy(&result, static_cast<const std::byte*>(p) + offset, sizeof(T));
    return result;
}
struct Vec { float x, y, z; };
struct Sphere { Vec center; float radius; };
struct BoundsKey {
    void* rwObject{};
    void* model{};
    void* collision{};
    void* matrix{};
    Sphere sphere{};
    std::array<std::byte, 64> pose{};
};
struct BoundsEntry { void* entity{}; unsigned epoch{}; BoundsKey key{}; Vec center{}; };
struct Policy {
    std::atomic<void*> entity{};
    std::atomic<unsigned> flags{}, visibleUntil{};
};
std::array<BoundsEntry, 4096> g_bounds;
std::array<Policy, 4096> g_policy;
std::atomic<unsigned> g_epoch{1}, g_frame{1};
std::uint64_t g_boundHits{}, g_boundMisses{};
bool g_visibilityTiming{};
std::uint64_t g_visibilityEligible{}, g_visibilityOptimizedSamples{}, g_visibilityStockSamples{}, g_visibilitySampledHits{};
std::uint64_t g_visibilityOptimizedTicks{}, g_visibilityStockTicks{};
LARGE_INTEGER g_visibilityFrequency{};
struct VisibilitySample {
    bool active{}, hit{};
    LARGE_INTEGER start{};
    ~VisibilitySample() {
        if (!active) return;
        LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
        ++g_visibilityOptimizedSamples;
        g_visibilityOptimizedTicks += end.QuadPart - start.QuadPart;
        g_visibilitySampledHits += hit;
    }
};
std::uint64_t g_visibilityHookCalls{}, g_visibilityWrongThread{}, g_visibilityNullEntity{};
std::uint64_t g_visibilityNonBuilding{}, g_visibilityBadModel{}, g_visibilityNoCollision{};
unsigned g_logInterval{}, g_sinceLog{};
DWORD g_thread{};
using ScreenFn = bool(__thiscall*)(void*);
ScreenFn g_screen{};
std::uintptr_t g_modelInfoBase{0xA9B0C8};

std::size_t slot(void* entity) { return (reinterpret_cast<std::uintptr_t>(entity) >> 4) % g_bounds.size(); }

std::uintptr_t discoverModelInfoBase() {
    // Limit adjusters commonly relocate CModelInfo::ms_modelInfoPtrs and patch
    // both stock helper functions. Find the common absolute operand referenced
    // by those helpers instead of insisting on the vanilla 0xA9B0C8 address.
    constexpr std::uintptr_t funcs[] = {0x534250, 0x534290};
    constexpr std::size_t lengths[] = {0x40, 0x20};
    // Never dereference fixed GTA addresses until the executable mapping has
    // been validated. This keeps unsupported/rebased builds on the safe
    // "feature unavailable" path instead of risking an access violation.
    if (!mws::gta::addressIsReadable(funcs[0], lengths[0]) ||
        !mws::gta::addressIsReadable(funcs[1], lengths[1]))
        return 0;
    for (std::size_t a = 0; a < lengths[0]; ++a) {
        if (a + sizeof(std::uint32_t) > lengths[0]) break;
        std::uint32_t candidate{};
        std::memcpy(&candidate, reinterpret_cast<const void*>(funcs[0] + a), sizeof(candidate));
        if ((candidate & 3u) || candidate < 0x10000 ||
            !mws::gta::addressIsReadable(candidate, 20000u * sizeof(void*)))
            continue;
        for (std::size_t b = 0; b + sizeof(std::uint32_t) <= lengths[1]; ++b) {
            std::uint32_t other{};
            std::memcpy(&other, reinterpret_cast<const void*>(funcs[1] + b), sizeof(other));
            if (other == candidate)
                return candidate;
        }
    }
    const bool vanillaReadable = mws::gta::addressIsReadable(0xA9B0C8, 20000u * sizeof(void*));
    if (!vanillaReadable) {
        // Diagnostic only: never select an unvalidated model-info table.
        auto probe = [](const char* name, std::uintptr_t address, std::size_t bytes) {
            MEMORY_BASIC_INFORMATION mbi{};
            const auto queried = VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi));
            std::ostringstream os;
            os << "Visibility address probe " << name << ": address=0x" << std::hex << address
               << " bytes=" << std::dec << bytes << " query=" << (queried != 0);
            if (queried) {
                os << " regionBase=0x" << std::hex << reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)
                   << " regionSize=0x" << mbi.RegionSize << " protect=0x" << mbi.Protect
                   << " state=0x" << mbi.State << std::dec;
            }
            mws::Logger::instance().info(os.str());
        };
        probe("vanilla model table", 0xA9B0C8, 20000u * sizeof(void*));
        probe("helper A", funcs[0], lengths[0]);
        probe("helper B", funcs[1], lengths[1]);
        mws::Logger::instance().info(
            "Visibility model-info resolution: no common validated helper operand and vanilla table range unreadable.");
    }
    return vanillaReadable ? 0xA9B0C8 : 0;
}

bool __fastcall screenHook(void* entity, void*) {
    ++g_visibilityHookCalls;
    // Only immutable building bounds are cached. Visibility itself is always
    // evaluated by GTA, including the mirror camera, near plane and far plane.
    if (GetCurrentThreadId() != g_thread) {
        ++g_visibilityWrongThread;
        return g_screen(entity);
    }
    if (!entity) {
        ++g_visibilityNullEntity;
        return g_screen(entity);
    }
    if ((field<unsigned char>(entity, 0x36) & 7) != 1) {
        ++g_visibilityNonBuilding;
        return g_screen(entity);
    }
    const auto& policy = g_policy[slot(entity)];
    if (policy.entity.load(std::memory_order_acquire) == entity) {
        const auto until = policy.visibleUntil.load(std::memory_order_relaxed);
        if (until && static_cast<std::int32_t>(until - g_frame.load()) > 0) return true;
        if (policy.flags.load(std::memory_order_relaxed) != 0) return g_screen(entity);
    }
    VisibilitySample sampled;
    if (g_visibilityTiming && (++g_visibilityEligible & 255u) == 0) {
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
        // Alternate stock/optimized paths. Never do both in one callback:
        // stock visibility can have stateful side effects.
        if ((g_visibilityEligible & 256u) != 0) {
            const bool result = g_screen(entity);
            LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
            ++g_visibilityStockSamples;
            g_visibilityStockTicks += end.QuadPart - start.QuadPart;
            return result;
        }
        sampled.active = true;
        sampled.start = start;
    }
    const auto modelId = field<std::uint16_t>(entity, 0x22);
    if (modelId >= 20000) {
        ++g_visibilityBadModel;
        return g_screen(entity);
    }
    BoundsKey key{};
    key.model = at<void*>(g_modelInfoBase + modelId * sizeof(void*));
    if (!key.model) return g_screen(entity);
    key.collision = field<void*>(key.model, 0x14);
    if (!key.collision) {
        ++g_visibilityNoCollision;
        return g_screen(entity);
    }
    key.sphere = field<Sphere>(key.collision, 0x18);
    if (!std::isfinite(key.sphere.radius) || key.sphere.radius < 0) return g_screen(entity);
    key.rwObject = field<void*>(entity, 0x18);
    key.matrix = field<void*>(entity, 0x14);
    if (key.matrix) std::memcpy(key.pose.data(), key.matrix, key.pose.size());
    else std::memcpy(key.pose.data(), static_cast<std::byte*>(entity) + 4, 16);
    auto& cached = g_bounds[slot(entity)];
    const auto epoch = g_epoch.load(std::memory_order_relaxed);
    if (cached.entity != entity || cached.epoch != epoch ||
        std::memcmp(&cached.key, &key, sizeof(key)) != 0) {
        // Use the game's own transform arithmetic, including matrix-less placement.
        reinterpret_cast<void(__thiscall*)(void*, Vec&)>(0x534290)(entity, cached.center);
        cached.key = key;
        cached.entity = entity;
        cached.epoch = epoch;
        ++g_boundMisses;
    } else {
        sampled.hit = true;
        if (++g_boundHits == 1)
            mws::Diagnostics::instance().effective(
                "Optimization.VisibilityBoundsCache",
                "live immutable-building bounds were reused from cache");
    }
    return reinterpret_cast<bool(__thiscall*)(void*, const Vec&, float)>(0x420D40)(
        reinterpret_cast<void*>(0xB6F028), cached.center, key.sphere.radius);
}

} // namespace

namespace mws::EngineOptimizations {
void install(const Config& config) {
    if (ProperShadersCompatibility::loaded()) {
        ProperShadersCompatibility::report(config);
        return;
    }
    if (g_screen) return;
    g_thread = GetCurrentThreadId();
    g_logInterval = config.optimizationLogInterval;
    g_visibilityTiming = config.passTiming && config.optimizationsEnabled;
    QueryPerformanceFrequency(&g_visibilityFrequency);
    if (config.visibilityBoundsCache) {
        g_modelInfoBase = discoverModelInfoBase();
        const bool hookTargetOk = stock::matches(0x534540, 0x60, {});
        const bool helperOk = gta::addressIsExecutable(0x534290) && gta::addressIsExecutable(0x420D40);
        if (g_modelInfoBase && hookTargetOk && helperOk &&
            stock::hook(0x534540, &screenHook, g_screen)) {
            std::ostringstream os;
            os << "Visibility bounds cache ACTIVE: relocation-aware model-info base=0x"
               << std::hex << std::uppercase << g_modelInfoBase
               << "; native camera/mirror tests retained.";
            Logger::instance().info(os.str());
            Diagnostics::instance().active(
                "Optimization.VisibilityBoundsCache",
                "relocation-aware visibility hook installed; awaiting live cache reuse");
        } else {
            std::ostringstream os;
            os << "Visibility bounds cache unavailable: modelBase=0x" << std::hex << std::uppercase
               << g_modelInfoBase << std::dec << ", hookTarget=" << hookTargetOk
               << ", helper=" << helperOk << ".";
            Logger::instance().warn(os.str());
            Diagnostics::instance().skipped(
                "Optimization.VisibilityBoundsCache",
                "no safe executable visibility hook/model-info base was available");
        }
    }

}

void appendOptimizerReport(OptimizationReport::Rows& rows, const Config& c) {
  rows.push_back({"bounds-recomputations", "VisibilityBoundsCache", "VisibilityBoundsCache", "lookups", g_boundHits + g_boundMisses, g_boundHits, c.visibilityBoundsCache, g_screen != nullptr});

}
void onPresent() {
    ++g_frame;
    if (!g_logInterval || ++g_sinceLog < g_logInterval) return;
    g_sinceLog = 0;
    std::ostringstream s;
    s << "Engine optimization totals: bounds hits=" << g_boundHits << " misses=" << g_boundMisses
      << "; visibilityHookCalls=" << g_visibilityHookCalls
      << " rejects[thread,null,type,model,collision]="
      << g_visibilityWrongThread << "," << g_visibilityNullEntity << ","
      << g_visibilityNonBuilding << "," << g_visibilityBadModel << "," << g_visibilityNoCollision
      << "; profiler=visibility-bounds"
      << " sampledCPU[optimized,stock]us="
      << (g_visibilityOptimizedSamples && g_visibilityFrequency.QuadPart ?
          1e6 * double(g_visibilityOptimizedTicks) / double(g_visibilityFrequency.QuadPart) /
              double(g_visibilityOptimizedSamples) : 0.0)
      << ","
      << (g_visibilityStockSamples && g_visibilityFrequency.QuadPart ?
          1e6 * double(g_visibilityStockTicks) / double(g_visibilityFrequency.QuadPart) /
              double(g_visibilityStockSamples) : 0.0)
      << " samples[optimized,stock,hit]=" << g_visibilityOptimizedSamples
      << "," << g_visibilityStockSamples << "," << g_visibilitySampledHits
      << "; sampled CPU timing only, scenes may differ";
    Logger::instance().info(s.str());
    if (g_screen && g_visibilityHookCalls == 0)
        Logger::instance().warn("Visibility route probe: installed stock visibility hook received zero calls; the active mod stack is bypassing/replacing this visibility route.");
}
void uninstall() {
    stock::unhook(0x534540, g_screen);
    ++g_epoch;
}
bool registerEntity(void* entity, std::uint32_t flags) {
    if (!entity) return false;
    auto& p = g_policy[slot(entity)];
    void* expected = nullptr;
    if (!p.entity.compare_exchange_strong(expected, entity) && expected != entity) return false;
    p.flags.store(flags);
    ++g_epoch;
    return true;
}
void unregisterEntity(void* entity) {
    if (!entity) return;
    auto& p = g_policy[slot(entity)];
    if (p.entity.load() == entity) {
        p.flags.store(0);
        p.visibleUntil.store(0);
        p.entity.store(nullptr);
    }
    ++g_epoch;
}
void invalidateEntity(void*) { ++g_epoch; }
void forceVisible(void* entity, std::uint32_t frames) {
    if (!entity) return;
    auto& p = g_policy[slot(entity)];
    if (p.entity.load() != entity && !registerEntity(entity, 0)) return;
    p.visibleUntil.store(g_frame.load() + std::min(frames, 1000000u));
    ++g_epoch;
}
}
