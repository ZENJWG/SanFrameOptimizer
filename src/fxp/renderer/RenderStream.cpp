#include "RenderStream.h"
#include "../runtime/HookWatch.h"
#include "../runtime/ProcessLifetime.h"
#include <windows.h>
#include <d3d9.h>
#include "SanFrameFxpVS.hpp"
#include "SanFrameFxpPS.hpp"
#include "mws/UploadRing.hpp"
#if (!defined(FXP_RENDER_TEST) || defined(FXP_INTEGRATION_TEST)) && !defined(FXP_REFERENCE_TEST)
#include "mws/NativeVulkan.hpp"
#include "mws/RenderSubmission.hpp"
#include "mws/D3D9StateCache.hpp"
#endif
#include <safetyhook.hpp>
#include "../GtaFxAbi.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>

namespace fxp::render {
namespace {
// RenderAddTri's 27 stack arguments, verified at 0x4A1410 in the supplied EXE.
static_assert(sizeof(TriangleArgs) == 108);
struct Instance { float p[9], uv[6]; uint32_t color[3]; };
static_assert(sizeof(Instance) == 72);
enum { Begin, Triangle, End, Count };
auto& hooks = processLifetime<std::array<SafetyHookInline, Count>>();
auto& resetHook = processLifetime<SafetyHookInline>();
std::array<HookWatch, Count> watches;
engine::Logger log{};
engine::Counters counters;
auto& gpuTimer = processLifetime<gpu::Timer>();
uint32_t loggedFallbacks{};
bool firstInstanced{}, gpuStatusLogged{};
IDirect3DDevice9* device{};
IDirect3DDevice9* failedDevice{};
IDirect3DVertexBuffer9* corners{};
IDirect3DVertexBuffer9* instances{};
IDirect3DIndexBuffer9* indices{};
IDirect3DVertexDeclaration9* declaration{};
IDirect3DVertexShader9* vertexShader{};
IDirect3DPixelShader9* pixelShader{};
IDirect3DStateBlock9* savedState{};
constexpr unsigned Capacity = 8192;
constexpr unsigned RingCapacity = Capacity * 4;
unsigned instanceCursor{};
std::vector<TriangleArgs> queue;
unsigned scope{};
bool active{}, restored{true};
D3DMATRIX world;
DWORD rasterCaps{};
bool clampVertexFog{};
#ifdef FXP_RENDER_TEST
IDirect3DDevice9* testDevice{};
void TestLog(const char* message) { OutputDebugStringA(message); }
#endif
template<class T> void Release(T*& value) { if (value) { value->Release(); value = nullptr; } }
void ReleaseBuffers() { Release(corners); Release(instances); Release(indices); instanceCursor = 0; }
void ReleaseAll() { gpuTimer.reset(); Release(savedState); ReleaseBuffers(); Release(declaration); Release(vertexShader); Release(pixelShader); }
bool Failure(unsigned reason, const char* message, HRESULT error = S_OK) {
    const uint32_t bit = 1u << reason;
    if (!(loggedFallbacks & bit)) {
        loggedFallbacks |= bit;
        char line[512]; snprintf(line, sizeof(line), "RENDER FALLBACK reason=%u HRESULT=0x%08lX: %s", reason, static_cast<unsigned long>(error), message);
        if (log) log(line);
    }
    return false;
}



HRESULT __stdcall ResetHook(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* parameters) {
    BeforeReset();
    return resetHook.unsafe_stdcall<HRESULT>(d, parameters);
}
bool CreateBuffers() {
    if (corners && instances && indices) return true;
    ReleaseBuffers();
    if (FAILED(device->CreateVertexBuffer(3 * sizeof(float), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &corners, nullptr)) ||
        FAILED(device->CreateIndexBuffer(3 * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indices, nullptr)) ||
        FAILED(device->CreateVertexBuffer(RingCapacity * sizeof(Instance), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &instances, nullptr))) {
        ReleaseBuffers(); return false;
    }
    void* mapped{};
    if (FAILED(corners->Lock(0, 0, &mapped, 0))) { ReleaseBuffers(); return false; }
    const float vertices[] = {0, 1, 2}; memcpy(mapped, vertices, sizeof(vertices));
    if (FAILED(corners->Unlock())) { ReleaseBuffers(); return false; }
    if (FAILED(indices->Lock(0, 0, &mapped, 0))) { ReleaseBuffers(); return false; }
    const uint16_t index[] = {0, 1, 2}; memcpy(mapped, index, sizeof(index));
    if (FAILED(indices->Unlock())) { ReleaseBuffers(); return false; }
    return true;
}
bool Ready() {
#ifdef FXP_RENDER_TEST
    auto* current = testDevice;
#else
#ifndef FXP_REFERENCE_TEST
    if (mws::NativeVulkan::isNativeBackend()) return false;
#endif
    auto* current = reinterpret_cast<IDirect3DDevice9*(__cdecl*)()>(0x7F9D50)();
#endif
    if (!current || current == failedDevice) return false;
    if (current != device) {
        resetHook.reset(); ReleaseAll(); device = current; restored = true;
        D3DDEVICE_CREATION_PARAMETERS creation{};
        if (FAILED(device->GetCreationParameters(&creation)) ||
            (creation.BehaviorFlags & (D3DCREATE_MULTITHREADED | D3DCREATE_PUREDEVICE))) {
            failedDevice = device;
            log("FX instancing skipped on multithreaded/pure device; original submission retained.");
            return false;
        }
        D3DCAPS9 caps{};
        if (FAILED(device->GetDeviceCaps(&caps)) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0) ||
            caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.MaxStreams < 2 ||
            caps.MaxStreamStride < sizeof(Instance) || caps.MaxPrimitiveCount < Capacity || device->GetSoftwareVertexProcessing()) {
            failedDevice = device; log("Instancing unavailable on this device; original submission retained."); return false;
        }
        rasterCaps = caps.RasterCaps;
        // DXVK's public Vulkan interop IID, from src/d3d9/d3d9_interfaces.h:
        // https://github.com/doitsujin/dxvk/blob/master/src/d3d9/d3d9_interfaces.h
        // Query only IUnknown; no Vulkan calls or dependency are required.
        constexpr GUID dxvkInterop = {0x2eaa4b89, 0x0107, 0x4bdb, {0x87,0xf7,0x0f,0x54,0x1c,0x49,0x3c,0xe0}};
        IUnknown* interop{};
        clampVertexFog = SUCCEEDED(device->QueryInterface(dxvkInterop, reinterpret_cast<void**>(&interop)));
        Release(interop);
        log(clampVertexFog ? "Fog backend: DXVK Vulkan interop; vertex factors clamped before interpolation."
                           : "Fog backend: native D3D9; vertex factors clamped after interpolation.");
        const D3DVERTEXELEMENT9 elements[] = {
            {0,0,D3DDECLTYPE_FLOAT1,0,D3DDECLUSAGE_POSITION,0},
            {1,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_TEXCOORD,0},
            {1,12,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_TEXCOORD,1},
            {1,24,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_TEXCOORD,2},
            {1,36,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_TEXCOORD,3},
            {1,52,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,4},
            {1,60,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_TEXCOORD,5},
            {1,64,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_TEXCOORD,6},
            {1,68,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_TEXCOORD,7}, D3DDECL_END()
        };
        HRESULT result = device->CreateVertexDeclaration(elements, &declaration);
        if (SUCCEEDED(result)) result = device->CreateVertexShader(reinterpret_cast<const DWORD*>(SanFrameFxpVS), &vertexShader);
        if (SUCCEEDED(result)) result = device->CreatePixelShader(reinterpret_cast<const DWORD*>(SanFrameFxpPS), &pixelShader);
#ifdef FXP_RENDER_TEST
        if (SUCCEEDED(result)) {
            auto** vtable = *reinterpret_cast<void***>(device);
            auto h = safetyhook::InlineHook::create(vtable[16], reinterpret_cast<void*>(&ResetHook));
            if (h) resetHook = std::move(*h); else result = E_FAIL;
        }
#endif
        if (FAILED(result)) { ReleaseAll(); failedDevice = device; log("Instancing initialization failed; original submission retained."); return false; }
        log("D3D9 SM3 instancing ready: 72-byte triangle instances, dynamic DISCARD buffer; embedded shaders prepared.");
    }
    return restored && CreateBuffers();
}
bool stateReadFailed{};
DWORD State(D3DRENDERSTATETYPE state) {
    DWORD v{}; if (FAILED(device->GetRenderState(state, &v))) stateReadFailed = true;
    return v;
}
float FloatState(D3DRENDERSTATETYPE state) { DWORD v = State(state); float result; memcpy(&result, &v, 4); return result; }
int TextureMode(D3DTEXTURESTAGESTATETYPE operation, D3DTEXTURESTAGESTATETYPE argument1, D3DTEXTURESTAGESTATETYPE argument2) {
    DWORD op{}, a{}, b{};
    if (FAILED(device->GetTextureStageState(0, operation, &op)) || FAILED(device->GetTextureStageState(0, argument1, &a)) || FAILED(device->GetTextureStageState(0, argument2, &b))) return -1;
    if (op == D3DTOP_MODULATE && ((a == D3DTA_TEXTURE && b == D3DTA_DIFFUSE) || (b == D3DTA_TEXTURE && a == D3DTA_DIFFUSE))) return 2;
    DWORD selected = op == D3DTOP_SELECTARG1 ? a : op == D3DTOP_SELECTARG2 ? b : ~0u;
    return selected == D3DTA_DIFFUSE ? 0 : selected == D3DTA_TEXTURE ? 1 : -1;
}
bool Draw() {
    stateReadFailed = false;
#if (!defined(FXP_RENDER_TEST) || defined(FXP_INTEGRATION_TEST)) && !defined(FXP_REFERENCE_TEST)
    mws::RenderSubmission::flush();
    mws::D3D9StateCache::instance().invalidate();
#endif
    if (!Ready()) return Failure(0, "Device, buffers or shader backend unavailable.");
#if !defined(FXP_RENDER_TEST) && !defined(FXP_REFERENCE_TEST)
    // RW render-state setters are deferred. Commit pending blend/depth/texture
    // state before inspecting it or snapshotting the actual D3D9 device.
    reinterpret_cast<void(__cdecl*)()>(0x7FC200)();
#endif
    DWORD stage1{};
    IDirect3DVertexShader9* previousVS{};
    IDirect3DPixelShader9* previousPS{};
    if (FAILED(device->GetVertexShader(&previousVS)) || FAILED(device->GetPixelShader(&previousPS))) {
        Release(previousVS); Release(previousPS); return Failure(1, "Unable to inspect bound shader state.");
    }
    const bool programmable = previousVS || previousPS;
    Release(previousVS); Release(previousPS);
    // Other graphics mods may install custom material programs. Let them draw it.
    if (programmable) return Failure(2, "A vertex/pixel shader is already bound. Original submission retained.");
    if (FAILED(device->GetTextureStageState(1, D3DTSS_COLOROP, &stage1)) || stage1 != D3DTOP_DISABLE) return Failure(3, "An additional texture stage is active or unreadable.");
    const int rgbMode = TextureMode(D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2);
    const int alphaMode = TextureMode(D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2);
    if (rgbMode < 0 || alphaMode < 0) return Failure(4, "Stage-zero color/alpha operation is unsupported.");
    DWORD lighting{}, specular{}, shade{}, clip{}, tween{}, blend{};
    if (FAILED(device->GetRenderState(D3DRS_LIGHTING, &lighting)) || lighting ||
        FAILED(device->GetRenderState(D3DRS_SPECULARENABLE, &specular)) || specular ||
        FAILED(device->GetRenderState(D3DRS_SHADEMODE, &shade)) || shade != D3DSHADE_GOURAUD ||
        FAILED(device->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip)) || clip ||
        FAILED(device->GetRenderState(D3DRS_VERTEXBLEND, &blend)) || blend != D3DVBF_DISABLE ||
        FAILED(device->GetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, &tween)) || tween)
        return Failure(10, "Unsupported fixed-function lighting, shading, clipping or vertex blending.");
    DWORD coordinates{}, textureTransform{};
    if (FAILED(device->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &coordinates)) || coordinates != 0 ||
        FAILED(device->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &textureTransform)) || textureTransform != D3DTTFF_DISABLE)
        return Failure(11, "Generated texture coordinates or texture transform requires original submission.");
    IDirect3DBaseTexture9* texture{};
    if (FAILED(device->GetTexture(0, &texture))) return Failure(11, "Unable to inspect texture type.");
    const bool textureSupported = !texture || texture->GetType() == D3DRTYPE_TEXTURE;
    Release(texture);
    if (!textureSupported) return Failure(11, "Volume/cube textures require original submission.");
    // Reuse the state-block object; Capture refreshes every saved binding.
    // Release it before Reset/project unload to avoid retaining game resources.
    HRESULT capture = savedState ? savedState->Capture() : device->CreateStateBlock(D3DSBT_ALL, &savedState);
    if (FAILED(capture)) return Failure(5, "Unable to capture device state.", capture);
    UINT frequency0{}, frequency1{};
    if (FAILED(device->GetStreamSourceFreq(0, &frequency0)) || FAILED(device->GetStreamSourceFreq(1, &frequency1)) || frequency0 != 1 || frequency1 != 1)
        return Failure(6, "Unsupported or unreadable stream frequencies.");
    D3DMATRIX view{}, projection{};
    HRESULT result = device->GetTransform(D3DTS_VIEW, &view);
    if (SUCCEEDED(result)) result = device->GetTransform(D3DTS_PROJECTION, &projection);
    float fogMode{};
    if (State(D3DRS_FOGENABLE)) {
        DWORD mode = State(D3DRS_FOGTABLEMODE); if (!mode) mode = State(D3DRS_FOGVERTEXMODE);
        fogMode = mode == D3DFOG_LINEAR ? 1.0f : mode == D3DFOG_EXP ? 2.0f : mode == D3DFOG_EXP2 ? 3.0f : 0.0f;
    }
    const bool tableFog = fogMode != 0 && State(D3DRS_FOGTABLEMODE) != D3DFOG_NONE;
    const bool affineProjection = projection._14 == 0 && projection._24 == 0 && projection._34 == 0 && projection._44 == 1;
    const bool perspectiveProjection = projection._14 == 0 && projection._24 == 0 && projection._34 == 1 && projection._44 == 0;
    if (tableFog && !affineProjection && !perspectiveProjection) return Failure(7, "Table fog uses a noncanonical projection matrix.");
    const float fog[] = {FloatState(D3DRS_FOGSTART), FloatState(D3DRS_FOGEND), FloatState(D3DRS_FOGDENSITY), fogMode};
    if (!std::isfinite(fog[0]) || !std::isfinite(fog[1]) || !std::isfinite(fog[2]) || (fogMode == 1 && fog[0] == fog[1]))
        return Failure(8, "Invalid or degenerate fog parameters.");
    const float options[] = {float(!tableFog && (rasterCaps & D3DPRASTERCAPS_FOGRANGE) && State(D3DRS_RANGEFOGENABLE) != 0), float(clampVertexFog), 0, 0};
    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetViewport(&viewport))) return Failure(7, "Unable to inspect fog depth viewport.");
    const float pixelOptions[] = {float(tableFog), float(perspectiveProjection && (rasterCaps & D3DPRASTERCAPS_WFOG)), viewport.MinZ, viewport.MaxZ - viewport.MinZ};
    DWORD packedFog = State(D3DRS_FOGCOLOR);
    const float color[] = {float((packedFog >> 16) & 255) / 255, float((packedFog >> 8) & 255) / 255, float(packedFog & 255) / 255, 1};
    const float textureModes[] = {float(rgbMode), float(alphaMode), 0, 0};
    if (stateReadFailed || FAILED(result)) return Failure(12, "Unable to inspect fog or transform state.");
    void* mapped{};
    const auto upload = mws::reserveUploadSpan(instanceCursor,
        static_cast<unsigned>(queue.size()), RingCapacity);
    const bool ringWrap = upload.discard;
    const UINT instanceStart = upload.offset;
    if (SUCCEEDED(result)) result = instances->Lock(
        instanceStart * sizeof(Instance), UINT(queue.size() * sizeof(Instance)),
        &mapped, ringWrap ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
    if (SUCCEEDED(result)) {
        auto* output = static_cast<Instance*>(mapped);
        for (size_t i = 0; i < queue.size(); ++i) {
            memcpy(output[i].p, queue[i].p, sizeof(output[i].p));
            memcpy(output[i].uv, queue[i].uv, sizeof(output[i].uv));
            for (unsigned c = 0; c < 3; ++c) {
                auto* rgba = queue[i].rgba + c * 4;
                output[i].color[c] = (rgba[3] << 24) | (rgba[0] << 16) | (rgba[1] << 8) | rgba[2];
            }
        }
        result = instances->Unlock();
    }
    auto apply = [&](HRESULT next) { if (SUCCEEDED(result)) result = next; };
    if (SUCCEEDED(result)) {
        apply(device->SetVertexDeclaration(declaration)); apply(device->SetVertexShader(vertexShader)); apply(device->SetPixelShader(pixelShader));
        apply(device->SetVertexShaderConstantF(0, &world._11, 4)); apply(device->SetVertexShaderConstantF(4, &view._11, 4)); apply(device->SetVertexShaderConstantF(8, &projection._11, 4));
        apply(device->SetVertexShaderConstantF(12, fog, 1)); apply(device->SetVertexShaderConstantF(13, options, 1));
        apply(device->SetPixelShaderConstantF(0, color, 1)); apply(device->SetPixelShaderConstantF(1, textureModes, 1));
        apply(device->SetPixelShaderConstantF(2, fog, 1)); apply(device->SetPixelShaderConstantF(3, pixelOptions, 1));
        apply(device->SetRenderState(D3DRS_FOGENABLE, FALSE));
        apply(device->SetStreamSource(0, corners, 0, sizeof(float))); apply(device->SetStreamSource(1, instances, instanceStart * sizeof(Instance), sizeof(Instance))); apply(device->SetIndices(indices));
        apply(device->SetStreamSourceFreq(0, DWORD(D3DSTREAMSOURCE_INDEXEDDATA) | UINT(queue.size()))); apply(device->SetStreamSourceFreq(1, DWORD(D3DSTREAMSOURCE_INSTANCEDATA) | 1u));
        if (SUCCEEDED(result)) result = device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
    }
    const bool drew = SUCCEEDED(result);
    // Restore actual device state to the unchanged RenderWare cache values.
    restored = SUCCEEDED(savedState->Apply());
    restored = SUCCEEDED(device->SetStreamSourceFreq(0, frequency0)) && restored;
    restored = SUCCEEDED(device->SetStreamSourceFreq(1, frequency1)) && restored;
#if (!defined(FXP_RENDER_TEST) || defined(FXP_INTEGRATION_TEST)) && !defined(FXP_REFERENCE_TEST)
    mws::D3D9StateCache::instance().invalidate();
#endif
    if (!restored) log("D3D9 state restore failed; instancing suspended until device replacement.");
    if (drew) {
        instanceCursor = upload.next;
        ++counters.draws; counters.triangles += queue.size();
        if (!firstInstanced) { firstInstanced = true; log("EXECUTING: first successful hardware-instanced FX draw. The replacement renderer is drawing actual effect triangles."); }
    } else Failure(9, "Transform, buffer upload, shader/stream setup or draw API failed.", result);
    return drew;
}
void Flush() {
    if (queue.empty()) return;
    if (!Draw()) {
        active = false; ++counters.renderFallbacks;
        for (const auto& triangle : queue) hooks[Triangle].unsafe_ccall<void>(triangle);
    }
    queue.clear();
}
void __cdecl BeginHook(RwRaster* raster, RwMatrix* transform, unsigned flags) {
    if (!queue.empty()) Flush();
    hooks[Begin].unsafe_ccall<void>(raster, transform, flags);
    active = scope != 0;
    world = {}; world._11 = world._22 = world._33 = world._44 = 1;
    if (transform) {
        world._11=transform->right.x; world._12=transform->right.y; world._13=transform->right.z;
        world._21=transform->up.x; world._22=transform->up.y; world._23=transform->up.z;
        world._31=transform->at.x; world._32=transform->at.y; world._33=transform->at.z;
        world._41=transform->pos.x; world._42=transform->pos.y; world._43=transform->pos.z;
    }
}
void __cdecl TriangleHook(TriangleArgs triangle) {
    if (!active) { hooks[Triangle].unsafe_ccall<void>(triangle); return; }
    if (queue.size() == Capacity) Flush();
    if (!active) { hooks[Triangle].unsafe_ccall<void>(triangle); return; }
    queue.push_back(triangle);
}
void __cdecl EndHook() { Flush(); hooks[End].unsafe_ccall<void>(); active = false; }
}
bool Install(engine::Logger logger) {
    log = logger; queue.reserve(Capacity);
    const uintptr_t addresses[] = {0x4A13B0, 0x4A1410, 0x4A1600};
    const char* signatures[] = {"\x8B\x44\x24\x08\x8B\x4C\x24\x0C", "\xA1\x6C\xAE\xA9\x00\xD9\x44\x24\x04", "\xE9\xD2\x43\xF6\xFF"};
    const size_t sizes[] = {8, 9, 5};
    void* destinations[] = {reinterpret_cast<void*>(&BeginHook), reinterpret_cast<void*>(&TriangleHook), reinterpret_cast<void*>(&EndHook)};
    for (unsigned i = 0; i < Count; ++i) {
        const bool compactEnd = i == End && memcmp(reinterpret_cast<void*>(addresses[i]), "\xA1\x5C\xAE\xA9\x00", 5) == 0;
        if (!compactEnd && memcmp(reinterpret_cast<void*>(addresses[i]), signatures[i], sizes[i]) != 0) {
            log("Renderer entry conflict; instancing not installed. Original submission retained."); Uninstall(); return false;
        }
        auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(addresses[i]), destinations[i], safetyhook::InlineHook::StartDisabled);
        if (!h) { Uninstall(); return false; }
        hooks[i] = std::move(*h);
    }
    for (auto& h : hooks) if (!h.enable()) { Uninstall(); return false; }
    for (unsigned i = 0; i < Count; ++i) watches[i].capture(addresses[i]);
    log("Replacement render submission armed; preserves triangle order, stock textures and blend/depth state. Device validation happens at first FX draw.");
    return true;
}
void Uninstall() { for (auto& h : hooks) h.reset(); watches = {}; resetHook.reset(); ReleaseAll(); device = failedDevice = nullptr; queue.clear(); active = false; scope = 0; }
bool IsInstalled() { return bool(hooks[Begin]); }
void BeforeReset() {
    queue.clear(); active = false; gpuTimer.reset(); gpuStatusLogged = false;
    Release(savedState); ReleaseBuffers(); failedDevice = nullptr; restored = true;
}
void ClearProject() { queue.clear(); active = false; Release(savedState); }
void BeginScope(bool profile, bool explosion) {
    ++scope;
    // Prepare at the first manager render, including an empty loading-frame pass.
    // This avoids compiling shaders for the first explosion itself.
    if (scope != 1 || !log || !hooks[Begin] || !Ready() || !profile) return;
    const bool supported = gpuTimer.initialize(device);
    if (!gpuStatusLogged) { gpuStatusLogged = true; log(supported ? "GPU timing: asynchronous timestamp queries active; whole FX manager pass measured, including fallback draws." : "GPU timing unavailable: device does not support the timestamp query set."); }
    if (supported) gpuTimer.begin(explosion);
}
void EndScope() { Flush(); active = false; if (scope == 1) gpuTimer.end(); if (scope) --scope; }
engine::Counters TakeCounters() { for (auto& watch : watches) watch.check(log); auto result = counters; counters = {}; result.gpu = gpuTimer.take(); return result; }
#ifdef FXP_RENDER_TEST
bool TestDraw(void* d, const TriangleArgs* triangles, unsigned count, const float* matrix) {
    log = &TestLog; testDevice = static_cast<IDirect3DDevice9*>(d);
    memcpy(&world, matrix, sizeof(world)); queue.assign(triangles, triangles + count);
    const bool result = Draw(); queue.clear(); return result;
}
void TestRelease() { Uninstall(); testDevice = nullptr; }
#endif
}
