#include "mws/ProperShadersCompatibility.hpp"
#include "mws/FxpOptimizer.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/ExecutableInfo.hpp"
#include "mws/GtaSa10US.hpp"
#include "mws/Logger.hpp"
#include "mws/NativeVulkan.hpp"
#include "mws/RenderSubmission.hpp"
#include "fxp/Core.h"
#include "fxp/runtime/Engine.h"
#include "fxp/runtime/OperatorPlans.h"
#include "fxp/runtime/ProcessLifetime.h"
#include "fxp/renderer/RenderStream.h"
#include <safetyhook.hpp>
#include "fxp/GtaFxAbi.h"
#include <atomic>
#include <memory>
#include <sstream>

namespace {
enum { Update, Render, Matrix, Destroy, Unload, Count };
auto& hooks = fxp::processLifetime<std::array<SafetyHookInline, Count>>();
struct Entry { std::uintptr_t address; const char* signature; std::size_t length; };
constexpr Entry entries[] = {
    {0x4A9A80, "\x8B\x44\x24\x04\x53\x55\x56\x50\x8B\xD9", 10},
    {0x4A92A0, "\x51\xD9\x05\x24\x86\x85\x00\x56", 8},
    {0x4AA8C0, "\x8B\x41\x0C\x85\xC0\x75\x15\x56", 8},
    {0x4A9810, "\x51\x53\x57\x8B\x7C\x24\x10\x8B\x47\x08", 10},
    {0x4A9AE0, "\x6A\xFF\x68\xEB\x98\x83\x00\x64", 8}
};
struct SystemState { void* blueprint{}; fxp::MatrixMemo<RwMatrix> matrix; };
std::unique_ptr<fxp::FixedTable<SystemState, 1024>> systems;
std::atomic<DWORD> owner{};
bool installed{}, matrixEnabled{}, profile{};
ULONGLONG nextReport{};
unsigned reportMs{};
std::uint64_t epoch{}, hits{}, misses{}, copies{};

bool onGameThread() { return owner.load(std::memory_order_relaxed) == GetCurrentThreadId(); }
void log(const char* message) {
    mws::Logger::instance().info(std::string("[FXP] ") + message);
    // First execution messages come from successful real work, independently
    // of optional timing. Installation alone never claims effectiveness.
    if (std::strstr(message, "EXECUTING:")) {
        const char* feature = std::strstr(message, "operator cache") ? "FXP.OperatorCache" :
            std::strstr(message, "operator plan") ? "FXP.OperatorPlans" : std::strstr(message, "curve") ? "FXP.CompiledCurves" :
            std::strstr(message, "draw") ? "FXP.InstancedSubmission" : "FXP.ReplaceSimulation";
        mws::Diagnostics::instance().effective(feature, message);
    }
}
void report(bool force=false) {
    const auto now = GetTickCount64();
    if (!force && (!reportMs || now < nextReport)) return;
    nextReport = now + reportMs;
    const auto c = fxp::engine::TakeCounters();
    const auto o = fxp::operators::TakeStats();
    auto& diagnostics=mws::Diagnostics::instance();
    diagnostics.addActivity("FXP.ReplaceSimulation",c.particles,c.particles);
    diagnostics.addActivity("FXP.CompiledCurves",c.curves+c.curveFallbacks,c.curves,0,c.curveFallbacks);
    diagnostics.addActivity("FXP.InstancedSubmission",c.draws+c.renderFallbacks,c.draws,0,c.renderFallbacks);
    diagnostics.addActivity("FXP.MatrixCache",hits+misses+copies,hits+misses+copies,hits+copies);
    diagnostics.addActivity("FXP.OperatorCache",o.cacheCalls,o.cacheCalls,o.cacheHits);
    diagnostics.addActivity("FXP.OperatorPlans",o.planCalls+o.fallbacks,o.planCalls,0,o.fallbacks);
    std::ostringstream s;
    s << "activity: particles=" << c.particles << " SIMD-packets=" << c.packets
      << " curves=" << c.curves << " curve-fallbacks=" << c.curveFallbacks
      << " instanced-triangles=" << c.triangles << " instanced-draws=" << c.draws
      << " render-fallbacks=" << c.renderFallbacks << " matrix-hits=" << hits
      << " matrix-misses=" << misses << " direct-copies=" << copies;
    s << " operator-cache-calls=" << o.cacheCalls << " operator-cache-hits=" << o.cacheHits
      << " operator-plan-calls=" << o.planCalls << " operator-plan-hits=" << o.planHits << " operator-fallbacks=" << o.fallbacks;
    if (profile) s << " GPU-samples=" << c.gpu.samples
      << " GPU-mean-ms=" << (c.gpu.samples ? c.gpu.milliseconds / double(c.gpu.samples) : 0.0)
      << " GPU-dropped=" << c.gpu.dropped;
    log(s.str().c_str());
    hits = misses = copies = 0;
}
void __fastcall updateHook(FxManager_c* manager, void*, RwCamera* camera, float delta) {
    DWORD expected{};
    owner.compare_exchange_strong(expected, GetCurrentThreadId(), std::memory_order_relaxed);
    if (!onGameThread()) { hooks[Update].unsafe_thiscall<void>(manager, camera, delta); return; }
    fxp::engine::SetGameThread(GetCurrentThreadId());
    ++epoch;
    fxp::operators::BeginFrame();
    hooks[Update].unsafe_thiscall<void>(manager, camera, delta);
    report();
}
void __fastcall renderHook(FxManager_c* manager, void*, RwCamera* camera, unsigned char haze) {
    if (!onGameThread()) { hooks[Render].unsafe_thiscall<void>(manager, camera, haze); return; }
    if (mws::ProperShadersCompatibility::loaded()) {
        hooks[Render].unsafe_thiscall<void>(manager, camera, haze);
        return;
    }
    mws::RenderSubmission::flush();
    fxp::engine::BeginRender(profile);
    hooks[Render].unsafe_thiscall<void>(manager, camera, haze);
    fxp::engine::EndRender();
}
void __fastcall matrixHook(FxSystem_c* system, void*, RwMatrix* out) {
    if (!matrixEnabled || !onGameThread() || !system || !out) {
        hooks[Matrix].unsafe_thiscall<void>(system, out); return;
    }
    if (!system->m_pParentMatrix) { *out = system->m_localMatrix; ++copies; return; }
    auto* state = systems->get(reinterpret_cast<std::uintptr_t>(system));
    if (!state) { hooks[Matrix].unsafe_thiscall<void>(system, out); return; }
    if (state->blueprint != system->m_pBlueprint) { *state = {}; state->blueprint = system->m_pBlueprint; }
    if (state->matrix.matches(epoch, system->m_localMatrix, system->m_pParentMatrix)) {
        *out = state->matrix.composite;
        if (++hits == 1) mws::Diagnostics::instance().effective("FXP.MatrixCache", "live FX matrix input reuse");
        return;
    }
    state->matrix.inputs(epoch, system->m_localMatrix, system->m_pParentMatrix);
    hooks[Matrix].unsafe_thiscall<void>(system, out);
    state->matrix.composite = *out; state->matrix.valid = true; ++misses;
}
void __fastcall destroyHook(FxManager_c* manager, void*, FxSystem_c* system) {
    if (onGameThread() && systems) systems->erase(reinterpret_cast<std::uintptr_t>(system));
    hooks[Destroy].unsafe_thiscall<void>(manager, system);
}
void __fastcall unloadHook(FxManager_c* manager, void*) {
    if (onGameThread()) {
        fxp::engine::ClearProject();
        if (systems) systems->clear();
        ++epoch;
    }
    hooks[Unload].unsafe_thiscall<void>(manager);
}
}

namespace mws::FxpOptimizer {
bool install(const Config& c) {
    if (installed) return true;
    if (!c.enabled || !c.optimizationsEnabled || !c.fxpEnabled) return false;
    if (GetModuleHandleW(L"FXP Optimized.asi") || GetModuleHandleW(L"FxPerfSA.asi")) {
        log("Overlapping standalone FX optimizer loaded; integrated FX hooks skipped."); return false;
    }
    const auto exe = ExecutableInfo::current();
    if (!exe.validPe || reinterpret_cast<std::uintptr_t>(exe.base) != 0x400000 ||
        exe.imageSize < 0x900000 || !gta::addressIsReadable(0x401000, 4)) {
        log("Unsupported executable; FX hooks skipped."); return false;
    }
    std::uint32_t marker{};
    std::memcpy(&marker, reinterpret_cast<const void*>(0x401000), sizeof(marker));
    if (marker != 0x53EC8B55 && marker != 0x16197BE9) {
        log("Unsupported executable marker; FX hooks skipped."); return false;
    }
    matrixEnabled = c.fxpMatrixCache;
    // Pass timing also samples the FX manager's existing asynchronous GPU
    // timer; simulation profiling remains independently controlled by FXP.
    profile = c.fxpProfile || c.passTiming;
    reportMs = c.fxpStatsIntervalSeconds * 1000u;
    const bool properShaders = ProperShadersCompatibility::loaded();
    const bool render = c.fxpInstancedSubmission && !properShaders && !NativeVulkan::isNativeBackend();
    if (properShaders && c.fxpInstancedSubmission)
        Diagnostics::instance().autoDisable("FXP.InstancedSubmission",
            "Proper Shaders wraps particle rendering; original submission and soft-particle shading retained");
    const bool needed[] = {c.fxpReplaceSimulation || c.fxpCompiledCurves || matrixEnabled || render || c.fxpOperatorCache || c.fxpOperatorPlans,
        render, matrixEnabled, matrixEnabled, matrixEnabled || c.fxpCompiledCurves || render || c.fxpOperatorCache || c.fxpOperatorPlans};
    if (!needed[Update]) return false;
    void* replacements[] = {reinterpret_cast<void*>(&updateHook), reinterpret_cast<void*>(&renderHook),
        reinterpret_cast<void*>(&matrixHook), reinterpret_cast<void*>(&destroyHook), reinterpret_cast<void*>(&unloadHook)};
    try {
        for (unsigned i = 0; i < Count; ++i) if (needed[i]) {
            const auto& e = entries[i];
            if (!gta::addressIsReadable(e.address, e.length) || !gta::addressIsExecutable(e.address) ||
                std::memcmp(reinterpret_cast<const void*>(e.address), e.signature, e.length)) {
                log("Manager entry conflict; integrated FX hooks skipped."); uninstall(); return false;
            }
            auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(e.address), replacements[i], safetyhook::InlineHook::StartDisabled);
            if (!hook) { uninstall(); return false; }
            hooks[i] = std::move(*hook);
        }
        if (matrixEnabled) systems = std::make_unique<fxp::FixedTable<SystemState, 1024>>();
        if (!fxp::engine::Install(c.fxpReplaceSimulation, c.fxpCompiledCurves, render, &log)) { uninstall(); return false; }
        const bool operatorsReady = fxp::operators::Install(c.fxpOperatorCache, c.fxpOperatorPlans, &log);
        for (const auto& feature : {std::pair{"FXP.OperatorCache", c.fxpOperatorCache}, std::pair{"FXP.OperatorPlans", c.fxpOperatorPlans}})
            if (feature.second) {
                if (operatorsReady) Diagnostics::instance().active(feature.first, "experimental operator hooks installed; awaiting execution");
                else Diagnostics::instance().skipped(feature.first, "stock operator entry conflict; existing FX runtime retained");
            }
        for (auto& hook : hooks) if (hook && !hook.enable()) { uninstall(); return false; }
        installed = true;
        nextReport = GetTickCount64() + reportMs;
        if (c.fxpReplaceSimulation) Diagnostics::instance().active("FXP.ReplaceSimulation", "hooks installed; awaiting real particle updates");
        if (c.fxpCompiledCurves) Diagnostics::instance().active("FXP.CompiledCurves", "hooks installed; awaiting real curve evaluations");
        if (matrixEnabled) Diagnostics::instance().active("FXP.MatrixCache", "matrix hook installed; awaiting live input reuse");
        if (render && fxp::render::IsInstalled()) Diagnostics::instance().active("FXP.InstancedSubmission", "triangle hooks installed; awaiting successful draws");
        else if (c.fxpInstancedSubmission && !properShaders) Diagnostics::instance().skipped("FXP.InstancedSubmission", "native backend or renderer entry conflict");
        log("Integrated FXP initialized; original assets, emission, world queries and particle counts retained.");
        return true;
    } catch (...) { uninstall(); log("FX initialization failed; hooks rolled back."); return false; }
}
void beforeDeviceReset() { fxp::render::BeforeReset(); }
void captureDiagnostics() {if(installed && Diagnostics::instance().enabled()) report(true);}
void uninstall() {
    captureDiagnostics();
    for (auto& hook : hooks) hook.reset();
    fxp::engine::Uninstall(); systems.reset(); owner.store(0); installed = false;
    hits = misses = copies = epoch = 0;
}
}
