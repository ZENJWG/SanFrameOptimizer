#include "Engine.h"
#include "OperatorPlans.h"
#include "ParticleMath.h"
#include "HookWatch.h"
#include "ProcessLifetime.h"
#include "../compiler/Curve.h"
#include "../renderer/RenderStream.h"
#include <windows.h>
#include <safetyhook.hpp>
#include "../GtaFxAbi.h"
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <atomic>
#include <unordered_map>

namespace fxp::engine {
namespace {
struct Particle {
    void* prev; Particle* next;
    float life, age;
    Vec3 position, velocity;
    FxSystem_c* system;
    uint32_t color;
    uint8_t size, rotationMultiplier, randomR, randomG, randomB, brightness;
    int8_t rotZ; bool local;
    float rotation;
    void* secondaryPrev; void* secondaryNext;
};
static_assert(sizeof(Particle) == 0x44);
static_assert(offsetof(Particle, rotation) == 0x38);
static_assert(offsetof(Particle, randomR) == 0x32);
struct Movement { Vec3 position, velocity; float rotation[4]; bool floating, underwater; char pad[2]; };
static_assert(sizeof(Movement) == 0x2C);
struct Interp {
    void* vtable; bool loop; int8_t keys, channels; uint8_t pad;
    const uint16_t* times; const void* const* values;
};
static_assert(sizeof(Interp) == 0x10);
struct Compiled { Curve curve; Interp source; int format; bool valid; };
std::unordered_map<const Interp*, Compiled> curves;
// Curve evaluations repeatedly revisit a small set of effect operators.
// unordered_map references survive rehash; this bounded front cache avoids
// hashing/bucket traversal while metadata checks still detect changed inputs.
struct CurveSlot { const Interp* source{}; Compiled* compiled{}; };
std::array<CurveSlot, 256> curveSlots{};
enum { ParticleTick, EmitterTick, FloatCurve, UnsignedCurve, Signed32Curve, Signed255Curve, HookCount };
auto& hooks = processLifetime<std::array<SafetyHookInline, HookCount>>();
std::array<HookWatch, HookCount> watches;
Counters counters;
std::atomic<DWORD> owner{};
Logger log{};
bool firstParticle{}, firstPacket{}, firstCurve{};
bool OwnThread() {
    return owner.load(std::memory_order_relaxed) == GetCurrentThreadId();
}

bool Finish(FxEmitterBP_c* bp, Particle* p, float dt) {
    Movement m{p->position, p->velocity, {}, false, false, {}};
    const auto* system = p->system;
    const float length = *reinterpret_cast<const float*>(static_cast<const char*>(system->m_pBlueprint) + 0xC);
    using Process = void(__thiscall*)(void*, float, float, float, float, bool, Movement*);
    // World queries and unusual movement operators stay on GTA's main thread.
    // Their interpolation calls use our compiled curve evaluator below.
    reinterpret_cast<Process>(0x4A4A10)(reinterpret_cast<char*>(bp) + 0x2C,
        std::bit_cast<float>(system->field_54), p->age / p->life, dt, length, false, &m);
    p->position = m.position; p->velocity = m.velocity;
    if (m.floating || m.underwater) {
        float level{};
        using Water = bool(__cdecl*)(float, float, float, float*, bool, Vec3*);
        bool found = reinterpret_cast<Water>(0x6EB690)(p->position.x, p->position.y, p->position.z, &level, true, nullptr);
        if (m.floating && found && p->position.z < level) p->position.z = level;
        if (m.underwater && (!found || p->position.z > level)) return true;
    }
    p->rotation = advanceRotation(p->rotation, m.rotation, p->randomR, p->rotationMultiplier, dt);
    return false;
}
bool __fastcall ParticleHook(FxEmitterBP_c* bp, void*, float delta, Particle* p) {
    if (!OwnThread()) return hooks[ParticleTick].unsafe_thiscall<bool>(bp, delta, p);
    ++counters.particles;
    if (!firstParticle) { firstParticle = true; log("EXECUTING: replacement scalar particle integration (manual/spawn path)."); }
    const float dt = delta * (p->system->m_nTimeMult * 0.001f);
    p->age += dt;
    if (p->age >= p->life) return true;
    p->position.x += p->velocity.x * dt;
    p->position.y += p->velocity.y * dt;
    p->position.z += p->velocity.z * dt;
    return Finish(bp, p, dt);
}
void __fastcall EmitterHook(FxEmitterBP_c* bp, void*, float delta) {
    if (!OwnThread()) { hooks[EmitterTick].unsafe_thiscall<void>(bp, delta); return; }
    auto* list = reinterpret_cast<char*>(bp) + 0x20;
    auto* cursor = *reinterpret_cast<Particle**>(list);
    while (cursor) {
        ParticlePacket packet{ParticlePacket::Uninitialized{}};
        std::array<Particle*, 4> lanes{};
        unsigned n{};
        while (cursor && n < 4) {
            auto* p = cursor; cursor = p->next;
            if (p->system->m_nKillStatus == 3) p->system->m_nKillStatus = 2;
            if (p->system->m_nPlayStatus == 2) continue;
            lanes[n] = p;
            packet.age[n] = p->age; packet.life[n] = p->life;
            packet.dt[n] = delta * (p->system->m_nTimeMult * 0.001f);
            packet.x[n] = p->position.x; packet.y[n] = p->position.y; packet.z[n] = p->position.z;
            packet.vx[n] = p->velocity.x; packet.vy[n] = p->velocity.y; packet.vz[n] = p->velocity.z;
            ++n;
        }
        if (!n) continue;
        packet.clearTail(n);
        const unsigned dead = packet.integrate();
        ++counters.packets; counters.particles += n;
        if (!firstPacket) { firstPacket = true; log("EXECUTING: replacement SSE2 emitter simulation."); }
        for (unsigned i = 0; i < n; ++i) {
            auto* p = lanes[i];
            p->age = packet.age[i];
            bool remove = (dead & (1u << i)) != 0;
            if (!remove) {
                p->position = {packet.x[i], packet.y[i], packet.z[i]};
                remove = Finish(bp, p, packet.dt[i]);
            }
            if (remove) {
                using Remove = void(__thiscall*)(void*, Particle*);
                reinterpret_cast<Remove>(0x4A8E30)(list, p);
                reinterpret_cast<Remove>(0x4A93B0)(reinterpret_cast<void*>(0xA9AE80), p);
            }
        }
    }
}
bool Evaluate(Interp* src, float* output, float t, int format) {
    if (!OwnThread() || !src || !output || !std::isfinite(t)) return false;
    auto& slot = curveSlots[(reinterpret_cast<std::uintptr_t>(src) >> 4) & 255u];
    Compiled* cached = slot.source == src ? slot.compiled : nullptr;
    if (!cached) {
        const auto found = curves.find(src);
        if (found != curves.end()) cached = &found->second;
    }
    if (!cached || std::memcmp(&cached->source, src, sizeof(Interp)) != 0 || cached->format != format) {
        const auto found = curves.find(src);
        if (found == curves.end() && curves.size() >= 8192) { ++counters.curveFallbacks; return false; }
        Compiled compiled{}; compiled.source = *src; compiled.format = format;
        const int keys = src->keys, channels = src->channels;
        if (keys > 0 && channels > 0 && channels <= 64 && src->times && src->values) {
            std::vector<float> values(size_t(keys) * channels);
            bool valid = true;
            for (int c = 0; c < channels; ++c) {
                if (!src->values[c]) { valid = false; break; }
                for (int k = 0; k < keys; ++k) {
                    float v;
                    if (format == 0) v = static_cast<const float*>(src->values[c])[k];
                    else if (format == 1) v = static_cast<const uint16_t*>(src->values[c])[k] * (1.0f / 256);
                    else v = static_cast<const int16_t*>(src->values[c])[k] * (format == 2 ? 0.001f : 1.0f / 128);
                    values[size_t(c) * keys + k] = v;
                }
            }
            if (valid) compiled.valid = compiled.curve.compile({src->times, size_t(keys)}, values, unsigned(channels), src->loop);
        }
        cached = &curves.insert_or_assign(src, std::move(compiled)).first->second;
    }
    slot = {src, cached};
    if (!cached->valid || !cached->curve.evaluate(t, output)) { ++counters.curveFallbacks; return false; }
    ++counters.curves;
    if (!firstCurve) { firstCurve = true; log("EXECUTING: compiled FXP curve interpolation."); }
    return true;
}
template<int Format> void __fastcall CurveHook(Interp* src, void*, float* output, float t) {
    try { if (Evaluate(src, output, t, Format)) return; }
    catch (...) { ++counters.curveFallbacks; }
    hooks[FloatCurve + Format].unsafe_thiscall<void>(src, output, t);
}
}
bool Install(bool simulation, bool compiled, bool renderer, Logger logger) {
    log = logger;
    struct Entry { uintptr_t address; const char* bytes; size_t length; void* destination; bool needed; };
    Entry entries[] = {
        {0x4A21D0, "\x83\xEC\x2C\x56\x8B\x74\x24\x38\x8B\x56\x28", 11, reinterpret_cast<void*>(&ParticleHook), simulation},
        {0x4A2BC0, "\x51\x53\x8B\xD9\x55\x8D\x6B\x20", 8, reinterpret_cast<void*>(&EmitterHook), simulation},
        {0x4A8470, "\x51\x53\x56\x8B\xF1\x8A\x5E\x05", 8, reinterpret_cast<void*>(&CurveHook<0>), compiled},
        {0x4A8800, "\x51\x53\x56\x8B\xF1\x8A\x5E\x05", 8, reinterpret_cast<void*>(&CurveHook<1>), compiled},
        {0x4A89C0, "\x51\x53\x56\x8B\xF1\x8A\x5E\x05", 8, reinterpret_cast<void*>(&CurveHook<2>), compiled},
        {0x4A8B80, "\x51\x53\x56\x8B\xF1\x8A\x5E\x05", 8, reinterpret_cast<void*>(&CurveHook<3>), compiled}
    };
    for (unsigned i = 0; i < HookCount; ++i) {
        const auto& e = entries[i]; if (!e.needed) continue;
        if (memcmp(reinterpret_cast<void*>(e.address), e.bytes, e.length) != 0) {
            log("Replacement entry conflict: simulation/curve hooks not installed."); Uninstall(); return false;
        }
        auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(e.address), e.destination, safetyhook::InlineHook::StartDisabled);
        if (!h) { Uninstall(); return false; }
        hooks[i] = std::move(*h);
    }
    for (auto& h : hooks) if (h && !h.enable()) { Uninstall(); return false; }
    for (unsigned i = 0; i < HookCount; ++i) if (hooks[i]) watches[i].capture(entries[i].address);
    if (renderer) render::Install(logger);
    if (simulation) log("Replacement simulation: SSE2 four-particle integration; stock emission, movement operators and world queries retained.");
    if (compiled) log("Replacement curves: exact key segments, packed SSE2 channels; unsupported curves use original evaluator.");
    return true;
}
void Uninstall() {
    operators::Uninstall();
    render::Uninstall(); for (auto& h : hooks) h.reset(); watches = {}; curves.clear(); curveSlots = {};
    owner.store(0, std::memory_order_relaxed); counters = {};
    firstParticle = firstPacket = firstCurve = false;
}
void ClearProject() { operators::ClearProject(); curveSlots = {}; curves.clear(); render::ClearProject(); }
void SetGameThread(uint32_t thread) { owner.store(thread, std::memory_order_relaxed); }
void BeginRender(bool profile, bool explosion) { render::BeginScope(profile, explosion); }
void EndRender() { render::EndScope(); }
Counters TakeCounters() { for (auto& watch : watches) watch.check(log); auto result = counters; counters = {}; auto r = render::TakeCounters(); result.triangles = r.triangles; result.draws = r.draws; result.renderFallbacks = r.renderFallbacks; result.gpu = r.gpu; return result; }
}
