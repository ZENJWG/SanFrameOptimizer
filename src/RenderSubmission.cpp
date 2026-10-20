#include "mws/ProperShadersCompatibility.hpp"
#include "mws/OptimizationReport.hpp"
#include "mws/RenderSubmission.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/D3D9StateCache.hpp"
#include "mws/Logger.hpp"
#include "mws/InstanceFog.hpp"
#include "mws/SubmissionDepth.hpp"
#include "mws/UploadRing.hpp"
#include "SanFrameInstanceVS.hpp"
#include "SanFrameInstancePS.hpp"
#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <tuple>
#include <type_traits>
#include <vector>
#include <sstream>

namespace {
// Keep transient CPU/GPU immediate-mode staging bounded. 512 KiB is large
// enough for busy effect bursts while avoiding a permanently resident 2 MiB
// CPU+GPU pair from the previous 1 MiB staging size. Larger submissions simply
// take the stock path; queued batches flush when this arena fills.
constexpr UINT kUploadBytes = 512 * 1024;
constexpr UINT kMaxInstances = 128;
constexpr UINT kMatrixRingInstances = kMaxInstances * 16; // 128 KiB, 16 batches before wrap
IDirect3DDevice9* g_device{};
DWORD g_thread{};
bool g_internal{}, g_recording{}, g_immediate{}, g_instancing{}, g_failed{};
bool g_profileIndexedDraws{};
// Native D3D9 methods may call other methods through the patched vtables
// while holding runtime locks. Those calls must not query state or replay
// deferred work back into the runtime. Keep this separate from g_internal:
// explicit flushes from EndScene/Present callbacks still have to run.
thread_local unsigned g_forwardDepth{};
struct ForwardCall {
    ForwardCall() { ++g_forwardDepth; }
    ~ForwardCall() { --g_forwardDepth; }
};
unsigned g_staticDepth{}, g_staticAllowed{}, g_logInterval{}, g_presents{};
std::array<bool, 64> g_staticScopes{};
UINT g_maxPrimitives{};
DWORD g_rasterCaps{};
mws::InstanceFog::State g_instanceFog;
std::uint64_t g_fogCandidates{}, g_fogSaved{}, g_rejectFog{}, g_rejectLighting{}, g_rejectShader{};
std::array<std::byte, kUploadBytes> g_vertices;
UINT g_bytes{}, g_stride{}, g_upDraws{}, g_cursor{}, g_matrixCursor{};
IDirect3DVertexBuffer9 *g_upload{}, *g_instances{};
IDirect3DVertexShader9* g_vs{};
IDirect3DPixelShader9* g_ps{};
IDirect3DStateBlock9* g_restore{};
struct Layout { IDirect3DVertexDeclaration9 *source{}, *instanced{}; };
std::array<Layout, 16> g_layouts;
UINT g_nextLayout{};
// Bounded weak-identity cache: immutable D3D9 descriptors cannot change.
// Resource Release=0 hooks invalidate generations before the next lookup.
// No COM references are pinned: streaming/reset resource lifetimes stay stock.
// Bindings are still queried because state blocks and foreign hooks can change
// them without passing through SanFrame's setter hooks.
struct BufferDescEntry {
    IDirect3DVertexBuffer9* vb{};
    IDirect3DIndexBuffer9* ib{};
    bool vbStatic{}, ibStatic{};
    UINT lastUsed{};
};
std::array<BufferDescEntry, 64> g_bufferDescs{};
UINT g_descClock{};
std::uint64_t g_descHits{}, g_descMisses{};
std::atomic<std::uint64_t> g_resourceGeneration{1};
std::uint64_t g_seenResourceGeneration{1};
struct TextureGuardEntry {
    IDirect3DTexture9* texture{};
    std::uint64_t generation{};
};
std::array<TextureGuardEntry, 64> g_textureGuards{};
enum class FlushReason : unsigned { Mutation, Capacity, Frame, Scope, External, Reset, Count };
std::array<std::uint64_t, static_cast<unsigned>(FlushReason::Count)> g_flushReasons{};
LARGE_INTEGER g_batchFrequency{};
struct BatchPolicy {
    bool configured{}, bypass{};
    unsigned window{600}, probe{240}, frames{}, bypassFrames{};
    std::uint64_t draws{}, saved{}, flushes{}, singletonBatches{}, samples{}, ticks{};
    void record(UINT input, UINT avoided, UINT singleton, std::uint64_t cost) {
        draws += input; saved += avoided; singletonBatches += singleton;
        ++flushes;
        if (cost) { ++samples; ticks += cost; }
    }
    void present(const char* name) {
        if (!configured) return;
        if (bypass) {
            if (++bypassFrames < probe) return;
            bypass = false; bypassFrames = frames = 0;
            draws = saved = flushes = singletonBatches = samples = ticks = 0;
            mws::Logger::instance().info(std::string("Adaptive batch ") + name + ": probe re-enabled");
            return;
        }
        if (++frames < window) return;
        frames = 0;
        // An isolated draw is never worth staging. Avoid changing policy on
        // sparse scenes; observed CPU replay time is required as corroboration.
        const double avgFlushUs = samples && g_batchFrequency.QuadPart
            ? (1e6 * double(ticks) / double(g_batchFrequency.QuadPart) / double(samples)) : 0.0;
        const bool inefficient = draws >= 128 && flushes >= 32 && samples >= 2 &&
            avgFlushUs > 3.0 && saved * 20 < draws &&
            singletonBatches * 4 >= flushes * 3;
        if (inefficient) {
            bypass = true;
            mws::Logger::instance().info(std::string("Adaptive batch ") + name +
                ": bypass (few merged draws, mostly singleton flushes, sampled avg flush " +
                std::to_string(avgFlushUs) + "us); next probe in " +
                std::to_string(probe) + " frames");
        }
        draws = saved = flushes = singletonBatches = samples = ticks = 0;
    }
};
BatchPolicy g_upPolicy, g_instancePolicy;
struct Draw {
    D3DPRIMITIVETYPE type{};
    INT base{};
    UINT min{}, vertices{}, start{}, primitives{};
    bool operator==(const Draw&) const = default;
};
constexpr UINT kMaxInstanceBatches = 32;
struct InstanceBatch {
    Draw draw{};
    std::array<D3DMATRIX, kMaxInstances> worlds{};
    UINT count{};
    int depthBin{};
    IDirect3DVertexBuffer9* vertexBuffer{};
    IDirect3DIndexBuffer9* indexBuffer{};
    IDirect3DBaseTexture9* texture{};
    IDirect3DVertexDeclaration9* sourceLayout{};
    IDirect3DVertexDeclaration9* instancedLayout{};
    UINT offset{};
    UINT stride{};
};
std::array<InstanceBatch, kMaxInstanceBatches> g_instanceBatches{};
UINT g_instanceBatchCount{};
UINT g_count{}; // total queued instances across all opaque buckets
std::uint64_t g_upSubmitted{}, g_upSaved{}, g_instanceSubmitted{}, g_instanceSaved{}, g_fallbacks{};
std::uint64_t g_drawPrimitiveCalls{}, g_drawIndexedPrimitiveCalls{}, g_drawPrimitiveUPCalls{}, g_drawIndexedPrimitiveUPCalls{}, g_staticIndexedCalls{};
bool g_submissionRouteLogged{};
bool g_genericIndexedInstancing{};
std::uint64_t g_genericIndexedCandidates{};
// Low-overhead indexed-draw profile: sample one of every 256 DIP calls.
std::uint64_t g_dipProfileSamples{}, g_dipVsBound{}, g_dipPsBound{}, g_dipTexture0Bound{};
std::uint64_t g_dipDeclBound{}, g_dipFvfBound{}, g_dipVbBound{}, g_dipIbBound{}, g_dipWorldChanged{};
std::array<std::uint64_t, 5> g_dipPrimitiveBuckets{};
D3DMATRIX g_lastProfileWorld{};
bool g_haveLastProfileWorld{};
std::uint64_t g_genericRejectState{}, g_genericRejectTexture{}, g_genericRejectBuffers{};
std::uint64_t g_genericRejectLayout{}, g_genericRejectTransform{}, g_genericRejectShape{};
// Disjoint rejection gates counted before expensive per-draw state queries.
// Exposes why a large indexed draw volume can yield zero instancing candidates.
std::uint64_t g_dipGateNoScope{}, g_dipGateIneligibleStaticScope{};
std::uint64_t g_dipGateDisabled{}, g_dipGateRecordingOrFailed{}, g_dipGateEntered{};
std::uint64_t g_dipGateExplicitStatic{}, g_dipGateGenericIndexed{};

void profileIndexedDraw(const Draw& draw) {
    if (!g_profileIndexedDraws || (g_drawIndexedPrimitiveCalls & 0xFFu) != 0 || !g_device) return;
    ++g_dipProfileSamples;
    if (draw.primitives <= 2) ++g_dipPrimitiveBuckets[0];
    else if (draw.primitives <= 8) ++g_dipPrimitiveBuckets[1];
    else if (draw.primitives <= 32) ++g_dipPrimitiveBuckets[2];
    else if (draw.primitives <= 128) ++g_dipPrimitiveBuckets[3];
    else ++g_dipPrimitiveBuckets[4];

    IDirect3DVertexShader9* vs{};
    IDirect3DPixelShader9* ps{};
    IDirect3DBaseTexture9* tex{};
    IDirect3DVertexDeclaration9* decl{};
    IDirect3DVertexBuffer9* vb{};
    IDirect3DIndexBuffer9* ib{};
    UINT offset{}, stride{};
    DWORD fvf{};
    D3DMATRIX world{};
    if (SUCCEEDED(g_device->GetVertexShader(&vs)) && vs) ++g_dipVsBound;
    if (SUCCEEDED(g_device->GetPixelShader(&ps)) && ps) ++g_dipPsBound;
    if (SUCCEEDED(g_device->GetTexture(0, &tex)) && tex) ++g_dipTexture0Bound;
    if (SUCCEEDED(g_device->GetVertexDeclaration(&decl)) && decl) ++g_dipDeclBound;
    if (SUCCEEDED(g_device->GetFVF(&fvf)) && fvf) ++g_dipFvfBound;
    if (SUCCEEDED(g_device->GetStreamSource(0, &vb, &offset, &stride)) && vb) ++g_dipVbBound;
    if (SUCCEEDED(g_device->GetIndices(&ib)) && ib) ++g_dipIbBound;
    if (SUCCEEDED(g_device->GetTransform(D3DTS_WORLD, &world))) {
        if (g_haveLastProfileWorld && std::memcmp(&world, &g_lastProfileWorld, sizeof(world)) != 0)
            ++g_dipWorldChanged;
        g_lastProfileWorld = world;
        g_haveLastProfileWorld = true;
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (tex) tex->Release();
    if (decl) decl->Release();
    if (vb) vb->Release();
    if (ib) ib->Release();
}
void failure(const char* message);
constexpr UINT kMaxRestoreHooks = 96;
std::array<void(*)(), kMaxRestoreHooks> g_restoreHooks{};
UINT g_restoreHookCount{};
UINT g_peakUpBytes{}, g_peakInstanceBatches{}, g_peakQueuedInstances{};
bool addRestoreHook(void(*fn)()) {
    if (!fn) return false;
    for (UINT i = 0; i < g_restoreHookCount; ++i)
        if (g_restoreHooks[i] == fn) return true;
    if (g_restoreHookCount < g_restoreHooks.size()) {
        g_restoreHooks[g_restoreHookCount++] = fn;
        return true;
    }
    failure("Submission optimizer disabled: restore-hook capacity exhausted.");
    return false;
}
void flushQueue(FlushReason reason = FlushReason::Mutation);
void releaseResources();
bool enqueueUP(D3DPRIMITIVETYPE, UINT, const void*, UINT, const void*, D3DFORMAT, UINT, UINT);
bool enqueueInstances(const Draw&);

template<class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
void clearBufferDescs() {
    for (auto& e : g_bufferDescs) e = {};
    g_descClock = 0;
    g_seenResourceGeneration = g_resourceGeneration.load(std::memory_order_acquire);
}
void notifyResourceDestroyed() {
    // May run on another resource-owner thread. No array mutation here.
    g_resourceGeneration.fetch_add(1, std::memory_order_release);
}
template<class Buffer, class Desc>
bool immutableBuffer(Buffer* ptr) {
    // A runtime boolean is not sufficient here: both template branches must
    // type-check, but vertex/index COM buffer pointers are unrelated types.
    static_assert(
        (std::is_same_v<Buffer, IDirect3DVertexBuffer9> &&
         std::is_same_v<Desc, D3DVERTEXBUFFER_DESC>) ||
        (std::is_same_v<Buffer, IDirect3DIndexBuffer9> &&
         std::is_same_v<Desc, D3DINDEXBUFFER_DESC>),
        "immutableBuffer requires a matching D3D9 buffer and descriptor");
    constexpr bool vertex = std::is_same_v<Buffer, IDirect3DVertexBuffer9>;

    if (!ptr) return false;
    if (g_seenResourceGeneration != g_resourceGeneration.load(std::memory_order_acquire))
        clearBufferDescs();
    const UINT now = ++g_descClock;
    for (auto& e : g_bufferDescs) {
        bool hit{};
        if constexpr (vertex) hit = e.vb == ptr;
        else hit = e.ib == ptr;
        if (hit) {
            // A concurrent destruction must not let a reused address inherit
            // stale immutable metadata.
            if (g_seenResourceGeneration != g_resourceGeneration.load(std::memory_order_acquire)) {
                clearBufferDescs();
                break;
            }
            e.lastUsed = now;
            ++g_descHits;
            if constexpr (vertex) return e.vbStatic;
            else return e.ibStatic;
        }
    }
    ++g_descMisses;
    Desc desc{};
    if (FAILED(ptr->GetDesc(&desc))) return false;
    const bool safe = (desc.Usage & D3DUSAGE_DYNAMIC) == 0;
    auto* victim = &g_bufferDescs[0];
    for (auto& e : g_bufferDescs) {
        if (vertex ? !e.vb : !e.ib) { victim = &e; break; }
        if (e.lastUsed < victim->lastUsed) victim = &e;
    }
    if constexpr (vertex) { victim->vb = ptr; victim->vbStatic = safe; }
    else { victim->ib = ptr; victim->ibStatic = safe; }
    victim->lastUsed = now;
    return safe;
}
void clearInstanceBatches() {
    for (UINT i = 0; i < g_instanceBatchCount; ++i) {
        auto& b = g_instanceBatches[i];
        release(b.vertexBuffer);
        release(b.indexBuffer);
        release(b.texture);
        release(b.sourceLayout);
        release(b.instancedLayout);
        // All metadata is overwritten on reuse; only live matrices are read.
        // Avoid zeroing 128 matrices for every flushed bucket.
        b.count = 0;
    }
    g_instanceBatchCount = 0;
    g_count = 0;
}
struct Internal {
    bool previous{g_internal};
    Internal() { g_internal = true; }
    ~Internal() { g_internal = previous; }
};
bool ours(IDirect3DDevice9* d) { return d == g_device && !g_internal && !g_forwardDepth && GetCurrentThreadId() == g_thread; }
void failure(const char* message) {
    if (!g_failed) mws::Logger::instance().warn(message);
    g_failed = true;
}
bool exchange(void** slot, void* value) {
    DWORD old{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

// A typed vtable adapter: signatures come from the SDK, including x86 stdcall.
// Resource barriers are installed on each encountered implementation's vtable.
template<int Slot, class Signature> struct Hook;
template<int Slot, class T, class R, class... Args>
struct Hook<Slot, R(__stdcall T::*)(Args...)> {
    using Fn = R(STDMETHODCALLTYPE*)(T*, Args...);
    struct Patch { void** table{}; Fn original{}; T* owner{}; };
    static inline std::array<Patch, 32> patches{};
    static inline std::atomic<unsigned> count{};
    static bool install(T* self) {
        auto** table = *reinterpret_cast<void***>(self);
        const auto used = count.load(std::memory_order_acquire);
        for (unsigned i = 0; i < used; ++i) if (patches[i].table == table) return true;
        if (used == patches.size()) return false;
        auto fn = reinterpret_cast<Fn>(table[Slot]);
        if (used == 0 && !addRestoreHook(&restore)) return false;
        // Resource Release can arrive on another thread immediately after
        // replacement. Publish its original-function chain first.
        patches[used] = {table, fn, self};
        count.store(used + 1, std::memory_order_release);
        if (!exchange(&table[Slot], reinterpret_cast<void*>(&invoke))) {
            count.store(used, std::memory_order_release);
            return false;
        }
        return true;
    }
    static void restore() {
        const auto used = count.load(std::memory_order_acquire);
        if constexpr (std::is_base_of_v<IDirect3DDevice9, T>) {
            // A renderer may replace this device's table with a copy of our
            // patched table. Restore the live table as well as the installed
            // one; otherwise uninstall would leave calls into this adapter.
            if (g_device) for (unsigned i = 0; i < used; ++i) {
                const auto& p = patches[i];
                auto** live = *reinterpret_cast<void***>(p.owner);
                if (live[Slot] == reinterpret_cast<void*>(&invoke))
                    exchange(&live[Slot], reinterpret_cast<void*>(p.original));
            }
        }
        for (unsigned i = 0; i < used; ++i) {
            auto& p = patches[i];
            if (p.table[Slot] == reinterpret_cast<void*>(&invoke))
                exchange(&p.table[Slot], reinterpret_cast<void*>(p.original));
        }
        count = 0;
    }
    static R STDMETHODCALLTYPE invoke(T* self, Args... args);
};
template<int Slot, class T, class R, class... Args>
struct Hook<Slot, R(__stdcall T::*)(Args...) noexcept> : Hook<Slot, R(__stdcall T::*)(Args...)> {};
#define H(type, slot, method) Hook<slot, decltype(&type::method)>

template<int Slot, class... Args>
bool unchanged(IDirect3DDevice9* d, Args... args) {
    const auto a = std::tuple{args...};
    if constexpr (Slot == 44) {
        // Every instance snapshots the currently visible WORLD transform.
        // Keeping the actual device up-to-date also preserves GetTransform.
        if (std::get<0>(a) == D3DTS_WORLD && g_count) return true;
        D3DMATRIX value{};
        return std::get<1>(a) && SUCCEEDED(d->GetTransform(std::get<0>(a), &value)) &&
            !std::memcmp(&value, std::get<1>(a), sizeof(value));
    } else if constexpr (Slot == 57) {
        DWORD v{}; return SUCCEEDED(d->GetRenderState(std::get<0>(a), &v)) && v == std::get<1>(a);
    } else if constexpr (Slot == 65) {
        IDirect3DBaseTexture9* v{};
        const bool same = SUCCEEDED(d->GetTexture(std::get<0>(a), &v)) && v == std::get<1>(a);
        release(v); return same;
    } else if constexpr (Slot == 67) {
        DWORD v{}; return SUCCEEDED(d->GetTextureStageState(std::get<0>(a), std::get<1>(a), &v)) && v == std::get<2>(a);
    } else if constexpr (Slot == 69) {
        DWORD v{}; return SUCCEEDED(d->GetSamplerState(std::get<0>(a), std::get<1>(a), &v)) && v == std::get<2>(a);
    } else if constexpr (Slot == 87) {
        IDirect3DVertexDeclaration9* v{};
        const bool same = SUCCEEDED(d->GetVertexDeclaration(&v)) && v == std::get<0>(a);
        release(v); return same;
    } else if constexpr (Slot == 89) {
        DWORD v{}; return SUCCEEDED(d->GetFVF(&v)) && v == std::get<0>(a);
    } else if constexpr (Slot == 92) {
        IDirect3DVertexShader9* v{};
        const bool same = SUCCEEDED(d->GetVertexShader(&v)) && v == std::get<0>(a);
        release(v); return same;
    } else if constexpr (Slot == 107) {
        IDirect3DPixelShader9* v{};
        const bool same = SUCCEEDED(d->GetPixelShader(&v)) && v == std::get<0>(a);
        release(v); return same;
    } else if constexpr (Slot == 100) {
        IDirect3DVertexBuffer9* v{}; UINT offset{}, stride{};
        const bool same = SUCCEEDED(d->GetStreamSource(std::get<0>(a), &v, &offset, &stride)) &&
            v == std::get<1>(a) && offset == std::get<2>(a) && stride == std::get<3>(a);
        release(v); return same;
    } else if constexpr (Slot == 102) {
        UINT v{}; return SUCCEEDED(d->GetStreamSourceFreq(std::get<0>(a), &v)) && v == std::get<1>(a);
    } else if constexpr (Slot == 104) {
        IDirect3DIndexBuffer9* v{};
        const bool same = SUCCEEDED(d->GetIndices(&v)) && v == std::get<0>(a);
        release(v); return same;
    } else if constexpr (Slot == 49) {
        D3DMATERIAL9 v{}; return std::get<0>(a) && SUCCEEDED(d->GetMaterial(&v)) && !std::memcmp(&v, std::get<0>(a), sizeof(v));
    } else if constexpr (Slot == 47) {
        D3DVIEWPORT9 v{}; return std::get<0>(a) && SUCCEEDED(d->GetViewport(&v)) && !std::memcmp(&v, std::get<0>(a), sizeof(v));
    } else if constexpr (Slot == 75) {
        RECT v{}; return std::get<0>(a) && SUCCEEDED(d->GetScissorRect(&v)) && !std::memcmp(&v, std::get<0>(a), sizeof(v));
    }
    return false;
}

template<int Slot, class T, class R, class... Args>
R STDMETHODCALLTYPE Hook<Slot, R(STDMETHODCALLTYPE T::*)(Args...)>::invoke(T* self, Args... args) {
    Fn original{};
    auto** table = *reinterpret_cast<void***>(self);
    const auto used = count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < used; ++i) if (patches[i].table == table) { original = patches[i].original; break; }
    if constexpr (std::is_base_of_v<IDirect3DDevice9, T>) {
        // Mods can clone a device vtable after installation. The copied hook
        // still belongs to the same live COM object and must keep its original
        // chain, rather than fabricating INVALIDCALL (notably for BeginScene).
        // Restrict the identity fallback to devices: resource addresses are
        // weak identities and can be reused after Release reaches zero.
        if (!original) for (unsigned i = 0; i < used; ++i)
            if (patches[i].owner == self) { original = patches[i].original; break; }
    }
    if (!original) return static_cast<R>(D3DERR_INVALIDCALL);
    const auto a = std::tuple{args...};
    if constexpr (std::is_base_of_v<IDirect3DDevice9, T>) {
        if (!ours(self)) {
            ForwardCall forwarding;
            return original(self, args...);
        }
        if constexpr (Slot == 81) ++g_drawPrimitiveCalls;
        if constexpr (Slot == 82) {
            ++g_drawIndexedPrimitiveCalls;
            if (g_staticDepth && g_staticAllowed) ++g_staticIndexedCalls;
        }
        if constexpr (Slot == 83) ++g_drawPrimitiveUPCalls;
        if constexpr (Slot == 84) ++g_drawIndexedPrimitiveUPCalls;
        if constexpr (Slot == 81 || Slot == 82 || Slot == 83 || Slot == 84) {
            if (!g_submissionRouteLogged) {
                g_submissionRouteLogged = true;
                mws::Logger::instance().info(
                    "Submission path probe: first live IDirect3DDevice9 draw reached SanFrame's device hook.");
            }
        }
        if constexpr (Slot == 83) {
            if (enqueueUP(std::get<0>(a), std::get<1>(a), std::get<2>(a), std::get<3>(a), nullptr, D3DFMT_UNKNOWN, 0, 0)) return D3D_OK;
        } else if constexpr (Slot == 84) {
            // A null index pointer is invalid for indexed UP draws. Do not
            // reinterpret it as the nonindexed form accepted by enqueueUP.
            if (std::get<4>(a) && enqueueUP(std::get<0>(a), std::get<3>(a), std::get<6>(a), std::get<7>(a), std::get<4>(a), std::get<5>(a), std::get<1>(a), std::get<2>(a))) return D3D_OK;
        } else if constexpr (Slot == 82) {
            const Draw draw{std::get<0>(a), std::get<1>(a), std::get<2>(a), std::get<3>(a), std::get<4>(a), std::get<5>(a)};
            profileIndexedDraw(draw);
            if (enqueueInstances(draw)) return D3D_OK;
        }
        if (g_bytes || g_count) {
            bool bucketSafeMutation = false;
            if constexpr (Slot == 65) // SetTexture
                bucketSafeMutation = g_count && std::get<0>(a) == 0;
            else if constexpr (Slot == 87) // SetVertexDeclaration
                bucketSafeMutation = g_count;
            else if constexpr (Slot == 100) // SetStreamSource
                bucketSafeMutation = g_count && std::get<0>(a) == 0;
            else if constexpr (Slot == 104) // SetIndices
                bucketSafeMutation = g_count;
            if (!bucketSafeMutation && !unchanged<Slot>(self, args...))
                flushQueue();
        }
        if constexpr (Slot == 132) mws::RenderSubmission::onReset();
        const R result = [&] {
            ForwardCall forwarding;
            return original(self, args...);
        }();
        if constexpr (Slot == 60) { if (SUCCEEDED(result)) g_recording = true; }
        if constexpr (Slot == 61) { if (SUCCEEDED(result)) g_recording = false; }
        if constexpr (Slot == 59 || Slot == 61) {
            auto out = std::get<sizeof...(Args) - 1>(a);
            if (SUCCEEDED(result) && out && *out) {
                if (!H(IDirect3DStateBlock9, 4, Capture)::install(*out) || !H(IDirect3DStateBlock9, 5, Apply)::install(*out))
                    failure("Submission optimizer disabled: state-block barrier unavailable.");
            }
        }
        if constexpr (Slot == 118) {
            auto out = std::get<1>(a);
            if (SUCCEEDED(result) && out && *out) {
                if (!H(IDirect3DQuery9, 6, Issue)::install(*out) || !H(IDirect3DQuery9, 7, GetData)::install(*out))
                    failure("Submission optimizer disabled: query barrier unavailable.");
            }
        }
        return result;
    } else {
        if constexpr (Slot == 2) {
            // COM Release may be declared on inherited IUnknown, not on the
            // concrete VB/IB type. Only vtables explicitly patched above
            // reach this slot-2 adapter.
            ForwardCall forwarding;
            const R remaining = original(self, args...);
            if (!remaining) notifyResourceDestroyed();
            return remaining;
        }
        if (!g_internal && !g_forwardDepth && GetCurrentThreadId() == g_thread && (g_bytes || g_count)) {
            IDirect3DDevice9* owner{};
            if (SUCCEEDED(self->GetDevice(&owner)) && owner == g_device) flushQueue();
            release(owner);
        }
        ForwardCall forwarding;
        return original(self, args...);
    }
}

bool fixedFunction() {
    IDirect3DVertexShader9* vs{}; IDirect3DPixelShader9* ps{};
    bool ok = SUCCEEDED(g_device->GetVertexShader(&vs)) && !vs;
    ok &= SUCCEEDED(g_device->GetPixelShader(&ps)) && !ps;
    release(vs); release(ps);
    return ok;
}
bool rs(D3DRENDERSTATETYPE type, DWORD expected) {
    DWORD v{}; return SUCCEEDED(g_device->GetRenderState(type, &v)) && v == expected;
}
bool ts(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD expected) {
    DWORD v{}; return SUCCEEDED(g_device->GetTextureStageState(stage, type, &v)) && v == expected;
}
bool guardTextures() {
    // Both supported paths use only the first fixed-function texture stage.
    if (!ts(1, D3DTSS_COLOROP, D3DTOP_DISABLE)) return false;
    IDirect3DBaseTexture9* base{};
    if (FAILED(g_device->GetTexture(0, &base))) return false;
    if (!base) return true;
    bool ok = base->GetType() == D3DRTYPE_TEXTURE;
    if (ok) {
        auto* texture = static_cast<IDirect3DTexture9*>(base);
        const auto generation = g_resourceGeneration.load(std::memory_order_acquire);
        auto& cached = g_textureGuards[(reinterpret_cast<std::uintptr_t>(texture) >> 4) & 63u];
        // Texture mip surfaces are stable for the lifetime of a D3D9 texture.
        // Once their mutation barriers are installed, rewalking every level
        // on every draw adds COM traffic without improving protection.
        if (cached.texture == texture && cached.generation == generation) {
            release(base);
            return true;
        }
        D3DSURFACE_DESC desc{};
        // Same-pointer rebinding can implicitly regenerate autogen mipmaps.
        ok = SUCCEEDED(texture->GetLevelDesc(0, &desc)) && !(desc.Usage & D3DUSAGE_AUTOGENMIPMAP) &&
            H(IDirect3DTexture9, 2, Release)::install(texture) &&
            H(IDirect3DTexture9, 19, LockRect)::install(texture) && H(IDirect3DTexture9, 16, GenerateMipSubLevels)::install(texture);
        const UINT levels = ok ? texture->GetLevelCount() : 0;
        for (UINT level = 0; ok && level < levels; ++level) {
            IDirect3DSurface9* surface{};
            ok = SUCCEEDED(texture->GetSurfaceLevel(level, &surface));
            if (ok) ok = H(IDirect3DSurface9, 13, LockRect)::install(surface) && H(IDirect3DSurface9, 15, GetDC)::install(surface);
            release(surface);
        }
        if (ok) cached = {texture, generation};
    }
    release(base);
    return ok;
}
bool readyShaders() {
    if (g_vs && g_ps) return true;
    bool ok = SUCCEEDED(g_device->CreateVertexShader(reinterpret_cast<const DWORD*>(SanFrameInstanceVS), &g_vs));
    if (ok) ok = SUCCEEDED(g_device->CreatePixelShader(reinterpret_cast<const DWORD*>(SanFrameInstancePS), &g_ps));
    if (!ok) { release(g_vs); release(g_ps); }
    return ok;
}

IDirect3DVertexDeclaration9* instanceLayout(IDirect3DVertexDeclaration9* declaration) {
    // Caller already queried this binding; a second GetVertexDeclaration()
    // used to occur for every candidate indexed draw.
    if (!declaration) return nullptr;
    for (auto& layout : g_layouts) if (layout.source == declaration) return layout.instanced;
    std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH + 1> elements{};
    UINT count = static_cast<UINT>(elements.size());
    bool ok = SUCCEEDED(declaration->GetDeclaration(elements.data(), &count));
    std::array<D3DVERTEXELEMENT9, 8> output{};
    unsigned used = 0, found = 0;
    for (UINT i = 0; ok && i < count && elements[i].Stream != 0xff; ++i) {
        const auto& e = elements[i];
        if (e.Stream != 0 || e.Method != D3DDECLMETHOD_DEFAULT || e.UsageIndex != 0) { ok = false; break; }
        unsigned flag = 0;
        if (e.Usage == D3DDECLUSAGE_POSITION && e.Type == D3DDECLTYPE_FLOAT3) flag = 1;
        else if (e.Usage == D3DDECLUSAGE_COLOR && e.Type == D3DDECLTYPE_D3DCOLOR) flag = 2;
        else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.Type == D3DDECLTYPE_FLOAT2) flag = 4;
        else if (e.Usage == D3DDECLUSAGE_NORMAL && e.Type == D3DDECLTYPE_FLOAT3) continue;
        else { ok = false; break; }
        if (found & flag) { ok = false; break; }
        found |= flag;
        output[used++] = e;
    }
    if (!ok || found != 7) return nullptr;
    for (WORD row = 0; row != 4; ++row)
        output[used++] = {1, static_cast<WORD>(row * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(4 + row)};
    output[used] = D3DDECL_END();
    IDirect3DVertexDeclaration9* created{};
    if (FAILED(g_device->CreateVertexDeclaration(output.data(), &created))) return nullptr;
    auto& entry = g_layouts[g_nextLayout++ % g_layouts.size()];
    release(entry.source); release(entry.instanced);
    declaration->AddRef(); // one source reference owned by the bounded layout cache
    entry = {declaration, created};
    return created;
}

bool enqueueUP(D3DPRIMITIVETYPE type, UINT primitives, const void* vertices, UINT stride,
               const void* indices, D3DFORMAT format, UINT min, UINT num) {
    if (!g_immediate || g_upPolicy.bypass || g_failed || g_recording || !vertices || !primitives || !stride || stride > 256 ||
        primitives > g_maxPrimitives || primitives > kUploadBytes / (3 * stride) ||
        (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLEFAN)) return false;
    if (indices && (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32)) return false;
    if (indices && (min > 65535 || !num || num > 65536 - min)) return false;
    if (g_count || (g_bytes && (g_stride != stride || g_bytes + primitives * 3 * stride > kUploadBytes ||
        g_bytes / (3 * stride) + primitives > g_maxPrimitives))) flushQueue(FlushReason::Capacity);
    if (g_failed) return false;
    if (!g_bytes) {
        UINT frequency{};
        if (!fixedFunction() || !rs(D3DRS_SHADEMODE, D3DSHADE_GOURAUD) ||
            FAILED(g_device->GetStreamSourceFreq(0, &frequency)) || frequency != 1 || !guardTextures()) return false;
        // UP accepts only stream zero. Support both screen-space Im2D and
        // world-space Im3D declarations; state barriers preserve their matrices.
        IDirect3DVertexDeclaration9* declaration{};
        if (FAILED(g_device->GetVertexDeclaration(&declaration)) || !declaration) return false;
        std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH + 1> elements{};
        UINT count = static_cast<UINT>(elements.size());
        bool valid = SUCCEEDED(declaration->GetDeclaration(elements.data(), &count));
        release(declaration);
        bool position = false;
        for (UINT i = 0; valid && i < count && elements[i].Stream != 0xff; ++i) {
            const auto& e = elements[i];
            valid &= e.Stream == 0 && e.Method == D3DDECLMETHOD_DEFAULT;
            valid &= e.Usage != D3DDECLUSAGE_BLENDINDICES && e.Usage != D3DDECLUSAGE_BLENDWEIGHT;
            position |= (e.Usage == D3DDECLUSAGE_POSITION && e.Type == D3DDECLTYPE_FLOAT3) ||
                        (e.Usage == D3DDECLUSAGE_POSITIONT && e.Type == D3DDECLTYPE_FLOAT4);
        }
        if (!valid || !position) return false;
    }
    auto indexAt = [&](UINT i) -> UINT {
        if (!indices) return i;
        return format == D3DFMT_INDEX16 ? static_cast<const WORD*>(indices)[i] : static_cast<const DWORD*>(indices)[i];
    };
    const UINT indexCount = type == D3DPT_TRIANGLELIST ? primitives * 3 : primitives + 2;
    // Validate the entire input before appending anything to a pending batch.
    if (indices) for (UINT i = 0; i < indexCount; ++i) {
        const UINT index = indexAt(i);
        if (index < min || index - min >= num) return false;
    }
    if (!indices && type == D3DPT_TRIANGLELIST) {
        // Already in final order: copy the complete contiguous vertex range.
        // Capacity and primitive-count bounds were checked before any append.
        const UINT bytes = primitives * 3 * stride;
        std::memcpy(g_vertices.data() + g_bytes, vertices, bytes);
        g_bytes += bytes;
    } else for (UINT triangle = 0; triangle < primitives; ++triangle) {
        UINT source[3];
        if (type == D3DPT_TRIANGLELIST) { source[0] = triangle * 3; source[1] = source[0] + 1; source[2] = source[0] + 2; }
        else if (type == D3DPT_TRIANGLEFAN) { source[0] = 0; source[1] = triangle + 1; source[2] = triangle + 2; }
        else { source[0] = triangle; source[1] = triangle + 1; source[2] = triangle + 2; if (triangle & 1) std::swap(source[0], source[1]); }
        for (UINT i : source) {
            std::memcpy(g_vertices.data() + g_bytes, static_cast<const std::byte*>(vertices) + static_cast<std::size_t>(indexAt(i)) * stride, stride);
            g_bytes += stride;
        }
    }
    g_peakUpBytes = std::max(g_peakUpBytes, g_bytes);
    g_stride = stride;
    ++g_upDraws;
    ++g_upSubmitted;
    // Preserve the documented UP side effects immediately, including getters
    // and RW's own cached assumptions between two deferred submissions.
    Internal internal;
    g_device->SetStreamSource(0, nullptr, 0, 0);
    if (indices) g_device->SetIndices(nullptr);
    return true;
}

bool enqueueInstances(const Draw& draw) {
    const bool explicitStaticScope = g_staticDepth && g_staticAllowed;
    const bool genericIndexedScope = g_genericIndexedInstancing && g_staticDepth == 0;
    // Reset releases the instancing shaders. Keep the first recovering frame
    // on the stock path until onPresent recreates them; deferred replay cannot
    // succeed without them and would permanently disable the optimizer.
    if (!g_instancing || g_instancePolicy.bypass || !g_vs || !g_ps) {
        ++g_dipGateDisabled;
        return false;
    }
    if (g_failed || g_recording) {
        ++g_dipGateRecordingOrFailed;
        return false;
    }
    if (!explicitStaticScope && !genericIndexedScope) {
        if (g_staticDepth) ++g_dipGateIneligibleStaticScope;
        else ++g_dipGateNoScope;
        return false;
    }
    ++g_dipGateEntered;
    if (explicitStaticScope) ++g_dipGateExplicitStatic;
    else ++g_dipGateGenericIndexed;
    if (draw.type != D3DPT_TRIANGLELIST || !draw.primitives || draw.primitives > g_maxPrimitives) {
        if (genericIndexedScope) ++g_genericRejectShape;
        return false;
    }
    if (g_bytes) flushQueue(FlushReason::Capacity);
    if (g_failed) return false;

    // Common opaque/fixed-function state is a hard barrier. Geometry bindings,
    // texture 0 and WORLD transforms are captured per bucket and may vary.
    if (!g_count) {
        Internal internal;
        if (!fixedFunction()) {
            ++g_rejectShader;
            if (genericIndexedScope) ++g_genericRejectState;
            return false;
        }
        if (!rs(D3DRS_LIGHTING, FALSE)) {
            ++g_rejectLighting;
            if (genericIndexedScope) ++g_genericRejectState;
            return false;
        }
        // Non-adjacent grouping is only order-independent for ordinary
        // depth-writing opaque geometry.  AlphaBlend=FALSE by itself is not
        // enough: depth-disabled/depth-read-only or stencil-writing passes can
        // produce different results when reordered.
        DWORD zEnable{}, zFunc{};
        if (FAILED(g_device->GetRenderState(D3DRS_ZENABLE, &zEnable)) ||
            FAILED(g_device->GetRenderState(D3DRS_ZFUNC, &zFunc)) ||
            zEnable != D3DZB_TRUE ||
            (zFunc != D3DCMP_LESS && zFunc != D3DCMP_LESSEQUAL) ||
            !rs(D3DRS_ZWRITEENABLE, TRUE) ||
            !rs(D3DRS_STENCILENABLE, FALSE) ||
            !rs(D3DRS_ALPHABLENDENABLE, FALSE) || !rs(D3DRS_SPECULARENABLE, FALSE) ||
            !rs(D3DRS_SHADEMODE, D3DSHADE_GOURAUD) || !rs(D3DRS_WRAP0, 0) ||
            !rs(D3DRS_VERTEXBLEND, D3DVBF_DISABLE) || !rs(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE) ||
            !rs(D3DRS_CLIPPLANEENABLE, 0) || !rs(D3DRS_POINTSPRITEENABLE, FALSE) ||
            !ts(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE) || !ts(0, D3DTSS_TEXCOORDINDEX, 0) ||
            !ts(0, D3DTSS_COLOROP, D3DTOP_MODULATE) || !ts(0, D3DTSS_COLORARG1, D3DTA_TEXTURE) ||
            !ts(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE) || !ts(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE) ||
            !ts(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE) || !ts(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE) ||
            !ts(0, D3DTSS_RESULTARG, D3DTA_CURRENT)) {
            if (genericIndexedScope) ++g_genericRejectState;
            return false;
        }
        if (!mws::InstanceFog::capture(g_device, g_rasterCaps, g_instanceFog)) {
            ++g_rejectFog;
            if (genericIndexedScope) ++g_genericRejectState;
            return false;
        }
    }

    if (!guardTextures()) {
        if (genericIndexedScope) ++g_genericRejectTexture;
        return false;
    }

    IDirect3DBaseTexture9* texture{};
    IDirect3DVertexBuffer9* vb{};
    IDirect3DIndexBuffer9* ib{};
    IDirect3DVertexDeclaration9* sourceLayout{};
    UINT offset{}, stride{}, frequency{};
    bool ok = SUCCEEDED(g_device->GetTexture(0, &texture)) && texture &&
        texture->GetType() == D3DRTYPE_TEXTURE &&
        SUCCEEDED(g_device->GetStreamSource(0, &vb, &offset, &stride)) && vb &&
        H(IDirect3DVertexBuffer9, 2, Release)::install(vb) &&
        immutableBuffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(vb) &&
        SUCCEEDED(g_device->GetIndices(&ib)) && ib &&
        H(IDirect3DIndexBuffer9, 2, Release)::install(ib) &&
        immutableBuffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(ib) &&
        SUCCEEDED(g_device->GetVertexDeclaration(&sourceLayout)) && sourceLayout &&
        SUCCEEDED(g_device->GetStreamSourceFreq(0, &frequency)) && frequency == 1 &&
        H(IDirect3DVertexBuffer9, 11, Lock)::install(vb) &&
        H(IDirect3DIndexBuffer9, 11, Lock)::install(ib) &&
        g_device->GetNPatchMode() == 0.0f;

    IDirect3DVertexDeclaration9* instancedLayout = ok ? instanceLayout(sourceLayout) : nullptr;
    ok = ok && instancedLayout;
    if (instancedLayout) instancedLayout->AddRef();

    D3DMATRIX world{}, view{};
    if (ok) ok = SUCCEEDED(g_device->GetTransform(D3DTS_WORLD, &world)) &&
                 SUCCEEDED(g_device->GetTransform(D3DTS_VIEW, &view));

    if (!ok) {
        release(texture); release(vb); release(ib); release(sourceLayout); release(instancedLayout);
        if (genericIndexedScope) ++g_genericRejectBuffers;
        return false;
    }

    // View-space translation gives a stable coarse depth bucket. The 64-unit
    // slab is intentionally broad: it limits opaque reordering while still
    // allowing repeated street props separated by unrelated submissions to meet.
    const float viewZ =
        world._41 * view._13 + world._42 * view._23 +
        world._43 * view._33 + view._43;
    const auto checkedDepthBin = mws::submissionDepthBin(viewZ);
    if (!checkedDepthBin) {
        release(texture); release(vb); release(ib); release(sourceLayout); release(instancedLayout);
        if (genericIndexedScope) ++g_genericRejectTransform;
        return false;
    }
    const int depthBin = *checkedDepthBin;

    InstanceBatch* batch = nullptr;
    for (UINT i = 0; i < g_instanceBatchCount; ++i) {
        auto& candidate = g_instanceBatches[i];
        if (candidate.count < kMaxInstances &&
            candidate.draw == draw &&
            candidate.depthBin == depthBin &&
            candidate.vertexBuffer == vb &&
            candidate.indexBuffer == ib &&
            candidate.texture == texture &&
            candidate.sourceLayout == sourceLayout &&
            candidate.offset == offset &&
            candidate.stride == stride) {
            batch = &candidate;
            break;
        }
    }

    if (!batch && g_instanceBatchCount == kMaxInstanceBatches) {
        release(texture); release(vb); release(ib); release(sourceLayout); release(instancedLayout);
        flushQueue(FlushReason::Capacity);
        return enqueueInstances(draw);
    }

    if (!batch) {
        batch = &g_instanceBatches[g_instanceBatchCount++];
        batch->draw = draw;
        batch->depthBin = depthBin;
        batch->vertexBuffer = vb;
        batch->indexBuffer = ib;
        batch->texture = texture;
        batch->sourceLayout = sourceLayout;
        batch->instancedLayout = instancedLayout;
        batch->offset = offset;
        batch->stride = stride;
    } else {
        // Get* calls returned temporary references; the existing bucket owns
        // its own stable references until the queue is flushed.
        release(texture); release(vb); release(ib); release(sourceLayout); release(instancedLayout);
    }

    batch->worlds[batch->count++] = world;
    ++g_count;
    g_peakInstanceBatches = std::max(g_peakInstanceBatches, g_instanceBatchCount);
    g_peakQueuedInstances = std::max(g_peakQueuedInstances, g_count);
    ++g_instanceSubmitted;
    if (g_instanceFog.control[0] != 0) ++g_fogCandidates;
    if (genericIndexedScope) ++g_genericIndexedCandidates;
    return true;
}

void flushUP() {
    if (!g_bytes) return;
    const UINT inputDraws = g_upDraws;
    LARGE_INTEGER cpuStart{}, cpuEnd{};
    const bool sample = g_upPolicy.configured && g_batchFrequency.QuadPart && ((g_upPolicy.flushes & 15u) == 0);
    if (sample) QueryPerformanceCounter(&cpuStart);
    const UINT primitives = g_bytes / (3 * g_stride);
    bool ok = true;
    if (!g_upload) ok = SUCCEEDED(g_device->CreateVertexBuffer(kUploadBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_upload, nullptr));
    UINT offset = ((g_cursor + g_stride - 1) / g_stride) * g_stride;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (!g_cursor || offset + g_bytes > kUploadBytes) { offset = 0; flags = D3DLOCK_DISCARD; }
    void* data{};
    if (ok) ok = SUCCEEDED(g_upload->Lock(offset, g_bytes, &data, flags));
    if (ok) {
        std::memcpy(data, g_vertices.data(), g_bytes);
        ok = SUCCEEDED(g_upload->Unlock());
    }
    if (ok) ok = SUCCEEDED(g_device->SetStreamSource(0, g_upload, 0, g_stride));
    HRESULT hr;
    if (ok) {
        hr = g_device->DrawPrimitive(D3DPT_TRIANGLELIST, offset / g_stride, primitives);
        g_cursor = offset + g_bytes;
    } else {
        ++g_fallbacks;
        hr = g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, primitives, g_vertices.data(), g_stride);
    }
    g_device->SetStreamSource(0, nullptr, 0, 0);
    if (SUCCEEDED(hr)) {
        const auto savedNow = g_upDraws > 0 ? g_upDraws - 1 : 0;
        const bool firstSaved = g_upSaved == 0 && savedNow != 0;
        g_upSaved += savedNow;
        if (firstSaved)
            mws::Diagnostics::instance().effective(
                "Optimization.ImmediateBatching",
                "live Draw*UP submissions were coalesced and redundant draw calls were removed");
    } else failure("Submission optimizer disabled: deferred UP draw failed.");
    if (sample) QueryPerformanceCounter(&cpuEnd);
    g_upPolicy.record(inputDraws, SUCCEEDED(hr) && inputDraws ? inputDraws - 1 : 0,
        inputDraws <= 1 ? 1u : 0u,
        sample ? static_cast<std::uint64_t>(cpuEnd.QuadPart - cpuStart.QuadPart) : 0);
    g_bytes = g_upDraws = 0;
}
void flushInstances() {
    if (!g_count) return;
    const UINT queued = g_count, batches = g_instanceBatchCount;
    UINT singletonBatches = 0;
    for (UINT i = 0; i < batches; ++i)
        singletonBatches += g_instanceBatches[i].count == 1;
    LARGE_INTEGER cpuStart{}, cpuEnd{};
    const bool sample = g_instancePolicy.configured && g_batchFrequency.QuadPart && ((g_instancePolicy.flushes & 15u) == 0);
    if (sample) QueryPerformanceCounter(&cpuStart);

    D3DMATRIX view{}, projection{}, combined{};
    DWORD originalFogEnable = FALSE;
    bool captured = false;
    bool allOk = true;
    UINT savedNow = 0;

    if (!g_restore) g_device->CreateStateBlock(D3DSBT_ALL, &g_restore);
    if (!g_instances) g_device->CreateVertexBuffer(
        kMatrixRingInstances * sizeof(D3DMATRIX),
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        0, D3DPOOL_DEFAULT, &g_instances, nullptr);

    bool ok = g_restore && g_instances && g_vs && g_ps &&
        SUCCEEDED(g_restore->Capture());
    captured = ok;
    if (ok) ok = SUCCEEDED(g_device->GetTransform(D3DTS_VIEW, &view)) &&
                 SUCCEEDED(g_device->GetTransform(D3DTS_PROJECTION, &projection)) &&
                 SUCCEEDED(g_device->GetRenderState(D3DRS_FOGENABLE, &originalFogEnable));
    if (ok) for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
        for (unsigned k = 0; k < 4; ++k)
            combined.m[r][c] += view.m[r][k] * projection.m[k][c];

    for (UINT bi = 0; ok && bi < g_instanceBatchCount; ++bi) {
        auto& batch = g_instanceBatches[bi];

        if (batch.count > 1) {
            // Stream several consecutive batches through one dynamic VB. NEVER
            // overwrite data referenced by an earlier queued GPU draw.
            const auto upload = mws::reserveUploadSpan(g_matrixCursor, batch.count,
                kMatrixRingInstances);
            const bool wrapped = upload.discard;
            const UINT start = upload.offset;
            void* data{};
            ok = SUCCEEDED(g_instances->Lock(
                start * sizeof(D3DMATRIX), batch.count * sizeof(D3DMATRIX),
                &data, wrapped ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE));
            if (ok) {
                std::memcpy(data, batch.worlds.data(),
                    batch.count * sizeof(D3DMATRIX));
                ok = SUCCEEDED(g_instances->Unlock());
            }
            if (ok) ok =
                SUCCEEDED(g_device->SetTexture(0, batch.texture)) &&
                SUCCEEDED(g_device->SetVertexDeclaration(batch.instancedLayout)) &&
                SUCCEEDED(g_device->SetStreamSource(
                    0, batch.vertexBuffer, batch.offset, batch.stride)) &&
                SUCCEEDED(g_device->SetIndices(batch.indexBuffer)) &&
                SUCCEEDED(g_device->SetVertexShader(g_vs)) &&
                SUCCEEDED(g_device->SetPixelShader(g_ps)) &&
                SUCCEEDED(g_device->SetVertexShaderConstantF(
                    0, &combined.m[0][0], 4)) &&
                SUCCEEDED(g_device->SetVertexShaderConstantF(
                    4, &view.m[0][0], 4)) &&
                mws::InstanceFog::upload(g_device, g_instanceFog) &&
                SUCCEEDED(g_device->SetRenderState(D3DRS_FOGENABLE, FALSE)) &&
                SUCCEEDED(g_device->SetStreamSource(
                    1, g_instances, start * sizeof(D3DMATRIX), sizeof(D3DMATRIX))) &&
                SUCCEEDED(g_device->SetStreamSourceFreq(
                    0, D3DSTREAMSOURCE_INDEXEDDATA | batch.count)) &&
                SUCCEEDED(g_device->SetStreamSourceFreq(
                    1, static_cast<UINT>(D3DSTREAMSOURCE_INSTANCEDATA) | 1u));

            if (ok) {
                const HRESULT hr = g_device->DrawIndexedPrimitive(
                    batch.draw.type, batch.draw.base, batch.draw.min,
                    batch.draw.vertices, batch.draw.start,
                    batch.draw.primitives);
                ok = SUCCEEDED(hr);
            }
            if (ok) {
                g_matrixCursor = upload.next;
                savedNow += batch.count - 1;
            }
        } else {
            // A singleton still has to be replayed because it may have been
            // deferred while a later compatible draw joined another bucket.
            ok =
                SUCCEEDED(g_device->SetTexture(0, batch.texture)) &&
                SUCCEEDED(g_device->SetVertexDeclaration(batch.sourceLayout)) &&
                SUCCEEDED(g_device->SetStreamSource(
                    0, batch.vertexBuffer, batch.offset, batch.stride)) &&
                SUCCEEDED(g_device->SetIndices(batch.indexBuffer)) &&
                SUCCEEDED(g_device->SetVertexShader(nullptr)) &&
                SUCCEEDED(g_device->SetPixelShader(nullptr)) &&
                SUCCEEDED(g_device->SetRenderState(D3DRS_FOGENABLE, originalFogEnable)) &&
                SUCCEEDED(g_device->SetStreamSourceFreq(0, 1)) &&
                SUCCEEDED(g_device->SetStreamSourceFreq(1, 1)) &&
                SUCCEEDED(g_device->SetTransform(D3DTS_WORLD, &batch.worlds[0]));
            if (ok) {
                const HRESULT hr = g_device->DrawIndexedPrimitive(
                    batch.draw.type, batch.draw.base, batch.draw.min,
                    batch.draw.vertices, batch.draw.start,
                    batch.draw.primitives);
                ok = SUCCEEDED(hr);
            }
        }
    }
    allOk = ok;

    if (captured && FAILED(g_restore->Apply())) {
        failure("Submission optimizer disabled: instance state restoration failed.");
        clearInstanceBatches();
        return;
    }

    if (allOk) {
        const bool firstSaved = g_instanceSaved == 0 && savedNow != 0;
        g_instanceSaved += savedNow;
        if (g_instanceFog.control[0] != 0) g_fogSaved += savedNow;
        if (firstSaved) {
            mws::Diagnostics::instance().effective(
                "Optimization.StaticPropInstancing",
                "non-adjacent compatible opaque draws were grouped by geometry/material state and coarse depth, then collapsed into hardware-instanced draws");
        }
    } else {
        ++g_fallbacks;
        failure("Submission optimizer disabled: grouped opaque instance replay failed.");
    }

    if (sample) QueryPerformanceCounter(&cpuEnd);
    // Flush CPU timing is recorded independently from GPU execution timing.
    g_instancePolicy.record(queued, allOk ? savedNow : 0, singletonBatches,
        sample ? static_cast<std::uint64_t>(cpuEnd.QuadPart - cpuStart.QuadPart) : 0);
    clearInstanceBatches();
}

void flushQueue(FlushReason reason) {
    if (!g_device || g_internal || (!g_bytes && !g_count)) return;
    ++g_flushReasons[static_cast<unsigned>(reason)];
    Internal internal;
    flushUP();
    flushInstances();
    mws::D3D9StateCache::instance().invalidate();
}
void releaseResources() {
    Internal internal;
    clearInstanceBatches();
    clearBufferDescs();
    g_textureGuards = {};
    release(g_restore); release(g_upload); release(g_instances);
    release(g_vs); release(g_ps);
    for (auto& layout : g_layouts) { release(layout.source); release(layout.instanced); }
    g_cursor = g_matrixCursor = 0;
}

bool patchDevice() {
    bool ok = true;
#define PATCH(slot, name) ok = H(IDirect3DDevice9, slot, name)::install(g_device) && ok
    PATCH(13, CreateAdditionalSwapChain); PATCH(16, Reset); PATCH(17, Present);
    PATCH(30, UpdateSurface); PATCH(31, UpdateTexture); PATCH(32, GetRenderTargetData);
    PATCH(33, GetFrontBufferData); PATCH(34, StretchRect); PATCH(35, ColorFill);
    PATCH(37, SetRenderTarget); PATCH(39, SetDepthStencilSurface);
    PATCH(41, BeginScene); PATCH(42, EndScene); PATCH(43, Clear);
    PATCH(44, SetTransform); PATCH(46, MultiplyTransform); PATCH(47, SetViewport);
    PATCH(49, SetMaterial); PATCH(51, SetLight); PATCH(53, LightEnable); PATCH(55, SetClipPlane);
    PATCH(57, SetRenderState); PATCH(59, CreateStateBlock); PATCH(60, BeginStateBlock); PATCH(61, EndStateBlock);
    PATCH(62, SetClipStatus); PATCH(65, SetTexture); PATCH(67, SetTextureStageState); PATCH(69, SetSamplerState);
    PATCH(70, ValidateDevice); PATCH(71, SetPaletteEntries); PATCH(73, SetCurrentTexturePalette);
    PATCH(75, SetScissorRect); PATCH(77, SetSoftwareVertexProcessing); PATCH(79, SetNPatchMode);
    PATCH(81, DrawPrimitive); PATCH(82, DrawIndexedPrimitive); PATCH(83, DrawPrimitiveUP); PATCH(84, DrawIndexedPrimitiveUP);
    PATCH(85, ProcessVertices); PATCH(87, SetVertexDeclaration); PATCH(89, SetFVF);
    PATCH(92, SetVertexShader); PATCH(94, SetVertexShaderConstantF); PATCH(96, SetVertexShaderConstantI); PATCH(98, SetVertexShaderConstantB);
    PATCH(100, SetStreamSource); PATCH(102, SetStreamSourceFreq); PATCH(104, SetIndices);
    PATCH(107, SetPixelShader); PATCH(109, SetPixelShaderConstantF); PATCH(111, SetPixelShaderConstantI); PATCH(113, SetPixelShaderConstantB);
    PATCH(115, DrawRectPatch); PATCH(116, DrawTriPatch); PATCH(117, DeletePatch); PATCH(118, CreateQuery);
#undef PATCH
    IDirect3DDevice9Ex* ex{};
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex)))) {
        ok = H(IDirect3DDevice9Ex, 119, SetConvolutionMonoKernel)::install(ex) && ok;
        ok = H(IDirect3DDevice9Ex, 120, ComposeRects)::install(ex) && ok;
        ok = H(IDirect3DDevice9Ex, 121, PresentEx)::install(ex) && ok;
        ok = H(IDirect3DDevice9Ex, 124, WaitForVBlank)::install(ex) && ok;
        ok = H(IDirect3DDevice9Ex, 132, ResetEx)::install(ex) && ok;
        release(ex);
    }
    return ok;
}
}

namespace mws::RenderSubmission {
bool install(IDirect3DDevice9* device, const Config& config) {
    if (ProperShadersCompatibility::loaded()) {
        ProperShadersCompatibility::report(config);
        return false;
    }
    if (g_device) return g_device == device;
    if (!device || (!config.immediateBatching && !config.staticPropInstancing)) return false;
    D3DDEVICE_CREATION_PARAMETERS creation{}; D3DCAPS9 caps{};
    const HRESULT creationHr = device->GetCreationParameters(&creation);
    const HRESULT capsHr = device->GetDeviceCaps(&caps);
    if (FAILED(creationHr) || FAILED(capsHr)) {
        Logger::instance().warn("Submission optimizer unavailable: device creation parameters/caps query failed.");
        if (config.immediateBatching)
            Diagnostics::instance().failed("Optimization.ImmediateBatching", "device creation parameters/caps query failed");
        if (config.staticPropInstancing)
            Diagnostics::instance().failed("Optimization.StaticPropInstancing", "device creation parameters/caps query failed");
        return false;
    }
    {
        std::ostringstream os;
        os << "Submission device: behaviorFlags=0x" << std::hex << std::uppercase << creation.BehaviorFlags
           << std::dec << ", VS=" << ((caps.VertexShaderVersion >> 8) & 0xff) << "." << (caps.VertexShaderVersion & 0xff)
           << ", PS=" << ((caps.PixelShaderVersion >> 8) & 0xff) << "." << (caps.PixelShaderVersion & 0xff)
           << ", MaxStreams=" << caps.MaxStreams
           << ", multithreaded=" << ((creation.BehaviorFlags & D3DCREATE_MULTITHREADED) ? 1 : 0)
           << ", pure=" << ((creation.BehaviorFlags & D3DCREATE_PUREDEVICE) ? 1 : 0);
        Logger::instance().info(os.str());
    }
    // D3D's per-call lock does not protect our deferred queues or the interval
    // between state queries and replay. Passing another thread's calls through
    // can change state/resources before queued draws execute.
    if (creation.BehaviorFlags & D3DCREATE_MULTITHREADED) {
        Logger::instance().warn("Submission optimizer unavailable: multithreaded device cannot safely defer draws.");
        if (config.immediateBatching)
            Diagnostics::instance().skipped("Optimization.ImmediateBatching", "multithreaded device requires immediate draw ordering");
        if (config.staticPropInstancing)
            Diagnostics::instance().skipped("Optimization.StaticPropInstancing", "multithreaded device requires immediate draw ordering");
        return false;
    }
    // Pure devices do not expose the state queries used by batching.
    if (creation.BehaviorFlags & D3DCREATE_PUREDEVICE) {
        Logger::instance().warn("Submission optimizer unavailable: D3DCREATE_PUREDEVICE prevents required state queries.");
        if (config.immediateBatching)
            Diagnostics::instance().skipped("Optimization.ImmediateBatching", "pure D3D9 device does not expose required Get* state");
        if (config.staticPropInstancing)
            Diagnostics::instance().skipped("Optimization.StaticPropInstancing", "pure D3D9 device does not expose required Get* state");
        return false;
    }
    g_device = device;
    g_rasterCaps = caps.RasterCaps;
    g_thread = GetCurrentThreadId();
    g_immediate = config.immediateBatching;
    const bool skyGfxLoaded =
        GetModuleHandleA("skygfx.asi") != nullptr ||
        GetModuleHandleA("SkyGFX.asi") != nullptr;
    g_genericIndexedInstancing = config.staticPropInstancing && skyGfxLoaded;
    if (g_genericIndexedInstancing)
        Logger::instance().info(
            "Submission optimizer: SkyGFX indexed fallback enabled; only strict opaque/fixed-function identical DIP geometry may be coalesced across WORLD transforms.");
    g_instancing = config.staticPropInstancing && caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) &&
        caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) && caps.MaxStreams >= 2 &&
        (creation.BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING);
    g_maxPrimitives = std::min(caps.MaxPrimitiveCount, DWORD{65535});
    g_logInterval = config.optimizationLogInterval;
    g_profileIndexedDraws = config.passTiming;
    QueryPerformanceFrequency(&g_batchFrequency);
    g_upPolicy = g_instancePolicy = {};
    g_upPolicy.configured = g_instancePolicy.configured = config.adaptiveBatching;
    g_upPolicy.window = g_instancePolicy.window = config.adaptiveBatchWindow;
    g_upPolicy.probe = g_instancePolicy.probe = config.adaptiveBatchProbe;
    if (g_instancing && !readyShaders()) {
        g_instancing = false;
        Logger::instance().warn("Static prop instancing unavailable: shader creation failed.");
    }
    if (!patchDevice()) {
        uninstall();
        Logger::instance().warn("Submission optimizer skipped: incomplete device barriers.");
        if (config.immediateBatching)
            Diagnostics::instance().skipped("Optimization.ImmediateBatching", "incomplete device barriers");
        if (config.staticPropInstancing)
            Diagnostics::instance().skipped("Optimization.StaticPropInstancing", "incomplete device barriers");
        return false;
    }
    Logger::instance().info(std::string("Submission optimizer ACTIVE: UP batching=") + (g_immediate ? "1" : "0") + "; restricted static instancing=" + (g_instancing ? "1" : "0"));
    Logger::instance().info("Static prop instancing: fixed-function prelit geometry with linear/exp/exp2 vertex or table fog; unsupported states retain original draws.");
    Logger::instance().info("Submission profiler wiring: stage6-direct-totals");
    Logger::instance().info(
        std::string("Indexed instancing route: PC immutable-building scope required; generic indexed fallback=") +
        (g_genericIndexedInstancing ? "ENABLED (SkyGFX detected)" : "OFF (SkyGFX not detected)") +
        "; day/night callback does not grant the PC-only immutable scope.");
    if (g_immediate)
        Diagnostics::instance().active("Optimization.ImmediateBatching", "submission optimizer installed");
    if (config.staticPropInstancing) {
        if (g_instancing)
            Diagnostics::instance().active("Optimization.StaticPropInstancing", "instancing path installed");
        else
            Diagnostics::instance().skipped("Optimization.StaticPropInstancing", "required shader/capability path unavailable");
    }
    return true;
}
void flush() { flushQueue(FlushReason::External); }
void onReset() {
    // Lost-device work cannot be replayed. Discard that incomplete frame
    // instead of treating the expected draw failure as a permanent fault.
    if (g_device && g_device->TestCooperativeLevel() == D3D_OK)
        flushQueue(FlushReason::Reset);
    else
        g_bytes = g_upDraws = 0;
    releaseResources();
    g_recording = false;
    // Shader/declaration resources are recreated lazily after a successful reset.
}

void appendOptimizerReport(OptimizationReport::Rows& rows, const Config& c) {
  rows.push_back({"draw-submissions", "ImmediateBatching", "ImmediateBatching", "eligible-draws", g_upSubmitted, g_upSaved, c.immediateBatching, g_device && g_immediate});
  rows.push_back({"draw-submissions", "StaticPropInstancing", "StaticPropInstancing", "eligible-draws", g_instanceSubmitted, g_instanceSaved, c.staticPropInstancing, g_device && g_instancing});
}
void onPresent() {
    flushQueue(FlushReason::Frame);
    g_upPolicy.present("UP");
    g_instancePolicy.present("instances");
    if (g_instancing && !g_vs && !g_failed && g_device->TestCooperativeLevel() == D3D_OK) {
        Internal internal;
        if (!readyShaders()) g_instancing = false;
    }
    if (!g_logInterval || ++g_presents < g_logInterval) return;
    g_presents = 0;
    std::ostringstream s;
    s << "Submission totals: UP submitted=" << g_upSubmitted << " draws saved=" << g_upSaved
      << "; instance candidates=" << g_instanceSubmitted << " draws saved=" << g_instanceSaved
      << "; fog candidates=" << g_fogCandidates << " fog draws saved=" << g_fogSaved
      << " rejects[fog,lighting,shader]=" << g_rejectFog << "," << g_rejectLighting << "," << g_rejectShader
      << "; batchPolicy[UP,instances]=" << (g_upPolicy.bypass ? "bypass" : "probe")
      << "," << (g_instancePolicy.bypass ? "bypass" : "probe")
      << " work[UPdraws,saved,singletons,flushes,cpuUsPerSample]="
      << g_upPolicy.draws << "," << g_upPolicy.saved << ","
      << g_upPolicy.singletonBatches << "," << g_upPolicy.flushes << ","
      << (g_upPolicy.samples && g_batchFrequency.QuadPart ? 
          1e6 * double(g_upPolicy.ticks) / double(g_batchFrequency.QuadPart) / double(g_upPolicy.samples) : 0.0)
      << "; work[instanceDraws,saved,singletonBuckets,flushes,cpuUsPerSample]="
      << g_instancePolicy.draws << "," << g_instancePolicy.saved << ","
      << g_instancePolicy.singletonBatches << "," << g_instancePolicy.flushes << ","
      << (g_instancePolicy.samples && g_batchFrequency.QuadPart ?
          1e6 * double(g_instancePolicy.ticks) / double(g_batchFrequency.QuadPart) / double(g_instancePolicy.samples) : 0.0)
      << "; flushReasons[mutation,capacity,frame,scope,external,reset]="
      << g_flushReasons[0] << "," << g_flushReasons[1] << "," << g_flushReasons[2] << ","
      << g_flushReasons[3] << "," << g_flushReasons[4] << "," << g_flushReasons[5]
      << "; immutableDescCache[hits,misses]=" << g_descHits << "," << g_descMisses
      << "; fallbacks=" << g_fallbacks
      << "; memoryHW[UPKiB,batches,instances]="
      << ((g_peakUpBytes + 1023u) / 1024u) << ","
      << g_peakInstanceBatches << "," << g_peakQueuedInstances
      << "; live draws DP=" << g_drawPrimitiveCalls
      << " DIP=" << g_drawIndexedPrimitiveCalls
      << " DPUP=" << g_drawPrimitiveUPCalls
      << " DIPUP=" << g_drawIndexedPrimitiveUPCalls
      << " staticDIP=" << g_staticIndexedCalls
      << " genericIndexedCandidates=" << g_genericIndexedCandidates
      << "; profileSamples=" << g_dipProfileSamples
      << " VS=" << g_dipVsBound << " PS=" << g_dipPsBound
      << " tex0=" << g_dipTexture0Bound
      << " decl=" << g_dipDeclBound << " FVF=" << g_dipFvfBound
      << " VB=" << g_dipVbBound << " IB=" << g_dipIbBound
      << " worldChanged=" << g_dipWorldChanged
      << "; rejects[shape,state,texture,buffers,layout,transform]="
      << g_genericRejectShape << "," << g_genericRejectState << ","
      << g_genericRejectTexture << "," << g_genericRejectBuffers << ","
      << g_genericRejectLayout << "," << g_genericRejectTransform;
    Logger::instance().info(s.str());

    // Stage-6 proof lines are unconditional. Zero is meaningful: it proves the
    // relevant profiler/eligibility path did not execute instead of silently
    // suppressing the diagnostic.
    std::ostringstream p;
    p << "DIP state profile: samples=" << g_dipProfileSamples
      << " VS=" << g_dipVsBound << " PS=" << g_dipPsBound
      << " tex0=" << g_dipTexture0Bound
      << " decl=" << g_dipDeclBound << " FVF=" << g_dipFvfBound
      << " VB=" << g_dipVbBound << " IB=" << g_dipIbBound
      << " worldChanged=" << g_dipWorldChanged
      << "; primitiveBuckets[<=2,<=8,<=32,<=128,>128]="
      << g_dipPrimitiveBuckets[0] << "," << g_dipPrimitiveBuckets[1] << ","
      << g_dipPrimitiveBuckets[2] << "," << g_dipPrimitiveBuckets[3] << ","
      << g_dipPrimitiveBuckets[4];
    Logger::instance().info(p.str());
    std::ostringstream r;
    r << "Indexed eligibility rejects: shape=" << g_genericRejectShape
      << " state/shader=" << g_genericRejectState
      << " texture=" << g_genericRejectTexture
      << " buffers=" << g_genericRejectBuffers
      << " layout=" << g_genericRejectLayout
      << " transform=" << g_genericRejectTransform;
    Logger::instance().info(r.str());
    std::ostringstream gates;
    gates << "DIP instancing gates cumulative: disabled=" << g_dipGateDisabled
          << " recordingOrFailed=" << g_dipGateRecordingOrFailed
          << " noEligibleScope=" << g_dipGateNoScope
          << " ineligibleStaticScope=" << g_dipGateIneligibleStaticScope
          << " entered=" << g_dipGateEntered
          << " enteredBy[PCstatic,genericIndexed]="
          << g_dipGateExplicitStatic << "," << g_dipGateGenericIndexed
          << " genericFallbackEnabled=" << (g_genericIndexedInstancing ? 1 : 0)
          << "; gate counts are mutually exclusive except enteredBy";
    Logger::instance().info(gates.str());
    if (g_submissionRouteLogged && g_upSubmitted == 0 && g_instanceSubmitted == 0 &&
        (g_drawPrimitiveCalls || g_drawIndexedPrimitiveCalls ||
         g_drawPrimitiveUPCalls || g_drawIndexedPrimitiveUPCalls)) {
        Logger::instance().info(
            "Submission eligibility probe: live D3D draw route is hooked, but no draw matched the current safe UP/strict indexed-instancing eligibility rules.");
    }
}
void enterStaticBuilding(bool eligible) {
    if (!eligible) flushQueue(FlushReason::Scope);
    if (g_staticDepth < g_staticScopes.size()) g_staticScopes[g_staticDepth] = eligible;
    ++g_staticDepth;
    g_staticAllowed = g_staticDepth <= g_staticScopes.size() && eligible;
}
void leaveStaticBuilding() {
    if (g_staticDepth) --g_staticDepth;
    g_staticAllowed = g_staticDepth && g_staticDepth <= g_staticScopes.size() && g_staticScopes[g_staticDepth - 1];
}
std::size_t trackedMemoryBytes() noexcept {
    // Include the permanently-resident CPU staging/bucket storage plus D3D
    // buffers only after they have actually been created. COM object/driver
    // bookkeeping is intentionally excluded because D3D9 does not expose it.
    std::size_t bytes = sizeof(g_vertices) + sizeof(g_instanceBatches) +
                        sizeof(g_layouts) + sizeof(g_restoreHooks) +
                        sizeof(g_bufferDescs) + sizeof(g_textureGuards);
    if (g_upload) bytes += kUploadBytes;
    if (g_instances) bytes += kMatrixRingInstances * sizeof(D3DMATRIX);
    return bytes;
}

void uninstall() {
    flushQueue();
    releaseResources();
    for (UINT i = g_restoreHookCount; i > 0; --i)
        if (g_restoreHooks[i - 1]) g_restoreHooks[i - 1]();
    g_restoreHooks.fill(nullptr);
    g_restoreHookCount = 0;
    g_device = nullptr;
    g_profileIndexedDraws = false;
    g_bytes = g_upDraws = g_count = g_staticDepth = g_staticAllowed = 0;
    g_drawPrimitiveCalls = g_drawIndexedPrimitiveCalls = 0;
    g_drawPrimitiveUPCalls = g_drawIndexedPrimitiveUPCalls = g_staticIndexedCalls = 0;
    g_genericIndexedCandidates = 0;
    g_fogCandidates = g_fogSaved = g_rejectFog = g_rejectLighting = g_rejectShader = 0;
    g_descHits = g_descMisses = 0;
    g_instanceFog = {};
    g_peakUpBytes = g_peakInstanceBatches = g_peakQueuedInstances = 0;
    g_rasterCaps = 0;
    g_dipProfileSamples = g_dipVsBound = g_dipPsBound = g_dipTexture0Bound = 0;
    g_dipDeclBound = g_dipFvfBound = g_dipVbBound = g_dipIbBound = g_dipWorldChanged = 0;
    g_dipPrimitiveBuckets.fill(0);
    g_haveLastProfileWorld = false;
    g_genericRejectState = g_genericRejectTexture = g_genericRejectBuffers = 0;
    g_genericRejectLayout = g_genericRejectTransform = g_genericRejectShape = 0;
    g_dipGateNoScope = g_dipGateIneligibleStaticScope = 0;
    g_dipGateDisabled = g_dipGateRecordingOrFailed = g_dipGateEntered = 0;
    g_dipGateExplicitStatic = g_dipGateGenericIndexed = 0;
    g_genericIndexedInstancing = false;
    g_upPolicy = g_instancePolicy = {};
    g_flushReasons.fill(0);
    g_submissionRouteLogged = false;
    g_failed = g_recording = false;
}
}
