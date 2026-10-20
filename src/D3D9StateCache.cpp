#include "mws/ProperShadersCompatibility.hpp"
#include "mws/OptimizationReport.hpp"
#include "mws/D3D9StateCache.hpp"
#include "mws/SkygfxCompatibility.hpp"
#include "mws/DrawTelemetry.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/Logger.hpp"

#include <Windows.h>
#include <array>
#include <bitset>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <sstream>

namespace mws {
namespace {

constexpr std::size_t kDeviceVTableSize = 119;


enum AdaptiveCategory : std::uint32_t {
    ADAPT_RS   = 1u << 0,
    ADAPT_TEX  = 1u << 1,
    ADAPT_SS   = 1u << 2,
    ADAPT_TSS  = 1u << 3,
    ADAPT_FVF  = 1u << 4,
    ADAPT_VS   = 1u << 5,
    ADAPT_PS   = 1u << 6,
    ADAPT_DECL = 1u << 7,
    ADAPT_VSC  = 1u << 8,
    ADAPT_PSC  = 1u << 9,
    ADAPT_TRANSFORM = 1u << 10,
    ADAPT_VIEWPORT = 1u << 11,
    ADAPT_SCISSOR = 1u << 12,
    ADAPT_RT = 1u << 13,
    ADAPT_DS = 1u << 14,
    ADAPT_MATERIAL = 1u << 15,
    ADAPT_LIGHT = 1u << 16,
    ADAPT_LIGHT_ENABLE = 1u << 17,
    ADAPT_CLIP = 1u << 18,
    ADAPT_NPATCH = 1u << 19,
};

// IDirect3DDevice9 vtable slots.
enum DeviceSlot : std::size_t {
    SLOT_STRETCH_RECT = 34,
    SLOT_DRAW_PRIMITIVE = 81,
    SLOT_DRAW_INDEXED_PRIMITIVE = 82,
    SLOT_SET_RENDER_TARGET = 37,
    SLOT_SET_DEPTH_STENCIL_SURFACE = 39,
    SLOT_SET_TRANSFORM = 44,
    SLOT_MULTIPLY_TRANSFORM = 46,
    SLOT_SET_VIEWPORT = 47,
    SLOT_SET_MATERIAL = 49,
    SLOT_SET_LIGHT = 51,
    SLOT_LIGHT_ENABLE = 53,
    SLOT_SET_CLIP_PLANE = 55,
    SLOT_SET_RENDER_STATE = 57,
    SLOT_CREATE_STATE_BLOCK = 59,
    SLOT_BEGIN_STATE_BLOCK = 60,
    SLOT_END_STATE_BLOCK = 61,
    SLOT_SET_TEXTURE = 65,
    SLOT_SET_TEXTURE_STAGE_STATE = 67,
    SLOT_SET_SAMPLER_STATE = 69,
    SLOT_SET_SCISSOR_RECT = 75,
    SLOT_SET_NPATCH_MODE = 79,
    SLOT_DRAW_PRIMITIVE_UP = 83,
    SLOT_DRAW_INDEXED_PRIMITIVE_UP = 84,
    SLOT_SET_VERTEX_DECLARATION = 87,
    SLOT_SET_FVF = 89,
    SLOT_SET_VERTEX_SHADER = 92,
    SLOT_SET_VERTEX_SHADER_CONSTANT_F = 94,
    SLOT_SET_STREAM_SOURCE = 100,
    SLOT_SET_STREAM_SOURCE_FREQ = 102,
    SLOT_SET_INDICES = 104,
    SLOT_SET_PIXEL_SHADER = 107,
    SLOT_SET_PIXEL_SHADER_CONSTANT_F = 109,
};

using DrawPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using StretchRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using SetRenderTargetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepthStencilSurfaceFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
using SetTransformFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using DrawPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using SetViewportFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetMaterialFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DMATERIAL9*);
using SetLightFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*);
using LightEnableFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, BOOL);
using SetClipPlaneFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const float*);
using SetScissorRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);
using SetNPatchModeFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, float);
using SetRenderStateFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using CreateStateBlockFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
using BeginStateBlockFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using EndStateBlockFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DStateBlock9**);
using SetTextureFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetTextureStageStateFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
using SetSamplerStateFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using SetVertexDeclarationFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetFVFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);
using SetVertexShaderFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVertexShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetStreamSourceFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
using SetStreamSourceFreqFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT);
using SetIndicesFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DIndexBuffer9*);
using SetPixelShaderFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

using StateBlockCaptureFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
using StateBlockApplyFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
using StateBlockReleaseFn = ULONG (STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);

struct ViewportState {
    D3DVIEWPORT9 viewport{};
    bool valid = false;
};

struct ScissorState {
    RECT rect{};
    bool valid = false;
};

struct MaterialState {
    D3DMATERIAL9 material{};
    bool valid = false;
};

struct LightState {
    D3DLIGHT9 light{};
    bool valid = false;
};

struct ClipPlaneState {
    float values[4]{};
    bool valid = false;
};

struct StreamState {
    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT offset = 0;
    UINT stride = 0;
    bool valid = false;
};

struct StreamFreqState {
    UINT setting = 0;
    bool valid = false;
};

struct StateBlockPatch {
    void** vtable = nullptr;
    StateBlockCaptureFn originalCapture = nullptr;
    StateBlockApplyFn originalApply = nullptr;
    StateBlockReleaseFn originalRelease = nullptr;
};

// Exact delta recorded between BeginStateBlock / EndStateBlock.
// Only state categories SAVO itself tracks are represented here.
// Applying the block can therefore update the cache without a global invalidation.
struct RecordedStateBlock {
    bool reconstructable = false;

    std::array<DWORD, 256> renderStates{};
    std::bitset<256> renderStateValid{};

    std::array<IDirect3DBaseTexture9*, 16> textures{};
    std::bitset<16> textureValid{};

    std::array<std::array<DWORD, 14>, 16> samplerStates{};
    std::array<std::bitset<14>, 16> samplerValid{};

    std::array<std::array<DWORD, 33>, 8> textureStageStates{};
    std::array<std::bitset<33>, 8> textureStageValid{};

    DWORD fvf = 0;
    bool fvfValid = false;

    IDirect3DVertexShader9* vertexShader = nullptr;
    bool vertexShaderValid = false;

    IDirect3DPixelShader9* pixelShader = nullptr;
    bool pixelShaderValid = false;

    IDirect3DVertexDeclaration9* declaration = nullptr;
    bool declarationValid = false;

    std::array<std::array<float, 4>, 256> vsConstants{};
    std::bitset<256> vsConstantValid{};

    std::array<std::array<float, 4>, 224> psConstants{};
    std::bitset<224> psConstantValid{};

    void Clear() {
        // Values are read only through their validity masks. Clearing all
        // matrix/constant/state payloads here needlessly dirties cache lines.
        renderStateValid.reset();
        textureValid.reset();
        for (auto& bits : samplerValid) bits.reset();
        for (auto& bits : textureStageValid) bits.reset();
        fvfValid = false;
        vertexShaderValid = false;
        pixelShaderValid = false;
        declarationValid = false;
        vsConstantValid.reset();
        psConstantValid.reset();
        reconstructable = true;
    }
};


struct OptimizerConfig {
    bool enabled = true;
    bool d3d9Enabled = true;
    bool productionMode = true;
    bool adaptiveEnabled = true;
    std::uint32_t adaptiveWarmupPresents = 1800;
    std::uint32_t adaptiveMinimumCalls = 50000;
    std::uint32_t adaptiveMaxPresents = 12000;
    std::uint32_t adaptiveMinSkipPercent = 5;
    std::uint32_t resampleInterval = 3600;
    std::uint32_t resampleWindow = 300;

    bool optimizeRenderState = true;
    bool optimizeTexture = true;
    bool optimizeSamplerState = true;
    bool optimizeTextureStageState = true;
    bool optimizeFVF = true;
    bool optimizeShaders = true;
    bool optimizeVertexDeclaration = true;
    bool optimizeStreamSource = true;
    bool optimizeIndices = true;
    bool optimizeShaderConstants = true;
    bool optimizeTransform = true;
    bool optimizeViewport = true;
    bool optimizeScissorRect = true;
    bool optimizeRenderTarget = true;
    bool optimizeDepthStencil = true;
    bool optimizeMaterial = true;
    bool optimizeLight = true;
    bool optimizeLightEnable = true;
    bool optimizeClipPlane = true;
    bool optimizeNPatchMode = true;

    // Draw-call profiling is not needed inside SanFrame because Present is
    // already centrally owned by D3D9Hook. Avoid four extra draw detours.
    bool invalidateOnPresent = true;
    bool disableSuppressionOnMultithreadedDevice = true;
    std::uint32_t logEveryNFrames = 600;
};

void Log(const char* format, ...) {
    if (!format)
        return;

    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    mws::Logger::instance().info(buffer);
}


bool ProperShadersLoaded() noexcept {
    return ProperShadersCompatibility::loaded();
}

bool IsProperShadersCriticalRenderState(D3DRENDERSTATETYPE state) noexcept {
    switch (state) {
    case D3DRS_ZENABLE:
    case D3DRS_ZWRITEENABLE:
    case D3DRS_ZFUNC:
    case D3DRS_ALPHABLENDENABLE:
    case D3DRS_SRCBLEND:
    case D3DRS_DESTBLEND:
    case D3DRS_SEPARATEALPHABLENDENABLE:
    case D3DRS_SRCBLENDALPHA:
    case D3DRS_DESTBLENDALPHA:
    case D3DRS_ALPHATESTENABLE:
    case D3DRS_ALPHAFUNC:
    case D3DRS_ALPHAREF:
    case D3DRS_STENCILENABLE:
    case D3DRS_STENCILFUNC:
    case D3DRS_STENCILREF:
    case D3DRS_STENCILMASK:
    case D3DRS_STENCILWRITEMASK:
    case D3DRS_COLORWRITEENABLE:
    case D3DRS_COLORWRITEENABLE1:
    case D3DRS_COLORWRITEENABLE2:
    case D3DRS_COLORWRITEENABLE3:
    case D3DRS_CULLMODE:
        return true;
    default:
        return false;
    }
}

int ReadGtaCurrentArea() noexcept {
    __try {
        return *reinterpret_cast<volatile int*>(0xB72914);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -999;
    }
}

struct Runtime {
    OptimizerConfig config{};
    IDirect3DDevice9* device = nullptr;
    void** deviceVTable = nullptr;
    bool installed = false;
    bool suppressionEnabled = false;
    bool skygfxRequested = false;
    bool skygfxActive = false;
    bool skygfxAvailable = false;
    bool skygfxConstantsRequested = false;
    bool skygfxBindingsRequested = false;
    bool skygfxBindingsActive = false;
    bool skygfxProfileRequested = false;
    bool skygfxProfileActive = false;
    bool baseShaders = false;
    IDirect3DSurface9* profilePrimary = nullptr;
    IDirect3DSurface9* profileTarget = nullptr;
    bool profileTargetKnown = false;
    LARGE_INTEGER profileFrequency{};
    unsigned profileLogInterval = 0;
    D3D9StateCacheStats profileBaseline{};
    DrawPrimitiveFn drawPrimitive = nullptr;
    DrawIndexedPrimitiveFn drawIndexedPrimitive = nullptr;
    StretchRectFn stretchRect = nullptr;
    bool baseShaderConstants = false;
    ULONGLONG skygfxPollMs = 0;
    bool recordingStateBlock = false;
    bool adaptiveEvaluated = false;
    bool resampling = false;
    std::uint64_t sampleStart = 0;
    std::uint64_t nextSample = 0;
    std::uint32_t previousDisabledMask = 0;
    D3D9StateCacheStats sampleBaseline{};
    std::uint32_t adaptiveEvaluatedMask = 0;
    std::uint32_t adaptiveConfiguredMask = 0;
    struct CostSample { std::uint64_t calls[2]{}, ticks[2]{}; };
    std::array<CostSample, 4> costSamples{}; // RS, texture, sampler, texture-stage
    bool costEnabled{}, costRunning{}, costPassthrough{};
    std::uint32_t costDisabledMask{};
    unsigned costWindow{300}, costFrames{}, costNext{};
    std::uint64_t costCalls[4]{};
    LARGE_INTEGER costFrequency{};

    bool effectiveRenderState = true;
    bool effectiveTexture = true;
    bool effectiveSamplerState = true;
    bool effectiveTextureStageState = true;
    bool effectiveFVF = true;
    bool effectiveVertexShader = true;
    bool effectivePixelShader = true;
    bool effectiveVertexDeclaration = true;
    bool effectiveVSConstants = true;
    bool effectivePSConstants = true;
    bool effectiveTransform = true;
    bool effectiveViewport = true;
    bool effectiveScissorRect = true;
    bool effectiveRenderTarget = true;
    bool effectiveDepthStencil = true;
    bool effectiveMaterial = true;
    bool effectiveLight = true;
    bool effectiveLightEnable = true;
    bool effectiveClipPlane = true;
    bool effectiveNPatchMode = true;

    SetRenderTargetFn setRenderTarget = nullptr;
    SetDepthStencilSurfaceFn setDepthStencilSurface = nullptr;
    SetTransformFn setTransform = nullptr;
    SetTransformFn multiplyTransform = nullptr;
    DrawPrimitiveUPFn drawPrimitiveUP = nullptr;
    DrawIndexedPrimitiveUPFn drawIndexedPrimitiveUP = nullptr;
    SetViewportFn setViewport = nullptr;
    SetMaterialFn setMaterial = nullptr;
    SetLightFn setLight = nullptr;
    LightEnableFn lightEnable = nullptr;
    SetClipPlaneFn setClipPlane = nullptr;
    SetScissorRectFn setScissorRect = nullptr;
    SetNPatchModeFn setNPatchMode = nullptr;
    SetRenderStateFn setRenderState = nullptr;
    CreateStateBlockFn createStateBlock = nullptr;
    BeginStateBlockFn beginStateBlock = nullptr;
    EndStateBlockFn endStateBlock = nullptr;
    SetTextureFn setTexture = nullptr;
    SetTextureStageStateFn setTextureStageState = nullptr;
    SetSamplerStateFn setSamplerState = nullptr;
    SetVertexDeclarationFn setVertexDeclaration = nullptr;
    SetFVFFn setFVF = nullptr;
    SetVertexShaderFn setVertexShader = nullptr;
    SetVertexShaderConstantFFn setVertexShaderConstantF = nullptr;
    SetStreamSourceFn setStreamSource = nullptr;
    SetStreamSourceFreqFn setStreamSourceFreq = nullptr;
    SetIndicesFn setIndices = nullptr;
    SetPixelShaderFn setPixelShader = nullptr;
    SetPixelShaderConstantFFn setPixelShaderConstantF = nullptr;

    std::array<D3DMATRIX, 512> transforms{};
    std::bitset<512> transformValid{};
    ViewportState viewport{};
    ScissorState scissor{};

    std::array<IDirect3DSurface9*, 4> renderTargets{};
    std::bitset<4> renderTargetValid{};
    IDirect3DSurface9* depthStencil = nullptr;
    bool depthStencilValid = false;

    MaterialState material{};
    std::array<LightState, 8> lights{};
    std::array<BOOL, 8> lightEnabled{};
    std::bitset<8> lightEnableValid{};
    std::array<ClipPlaneState, 6> clipPlanes{};

    float nPatchMode = 0.0f;
    bool nPatchModeValid = false;

    std::array<DWORD, 256> renderStates{};
    std::bitset<256> renderStateValid{};

    std::array<IDirect3DBaseTexture9*, 16> textures{};
    std::bitset<16> textureValid{};

    std::array<std::array<DWORD, 14>, 16> samplerStates{};
    std::array<std::bitset<14>, 16> samplerValid{};

    std::array<std::array<DWORD, 33>, 8> textureStageStates{};
    std::array<std::bitset<33>, 8> textureStageValid{};

    DWORD fvf = 0;
    bool fvfValid = false;

    IDirect3DVertexShader9* vertexShader = nullptr;
    bool vertexShaderValid = false;

    IDirect3DPixelShader9* pixelShader = nullptr;
    bool pixelShaderValid = false;

    IDirect3DVertexDeclaration9* declaration = nullptr;
    bool declarationValid = false;

    std::array<StreamState, 16> streams{};
    std::array<StreamFreqState, 16> streamFreqs{};

    IDirect3DIndexBuffer9* indices = nullptr;
    bool indicesValid = false;

    std::array<std::array<float, 4>, 256> vsConstants{};
    std::bitset<256> vsConstantValid{};

    std::array<std::array<float, 4>, 224> psConstants{};
    std::bitset<224> psConstantValid{};

    static constexpr std::size_t kMaxStateBlockVTables = 16;
    static constexpr std::size_t kMaxRecordedStateBlocks = 8;
    std::array<StateBlockPatch, kMaxStateBlockVTables> stateBlockPatches{};
    std::size_t stateBlockPatchCount = 0;
    std::unordered_map<IDirect3DStateBlock9*, RecordedStateBlock> stateBlocks{};
    std::size_t peakRecordedStateBlocks = 0;
    RecordedStateBlock recordingDelta{};

    // Proper Shaders compatibility profiler / conservative multipass guard.
    bool properShadersDetected = false;
    bool properShadersSafetyMode = false;
    std::uint64_t properProfileLastPresent = 0;
    D3D9StateCacheStats properProfileBaseline{};
    std::uint64_t properCriticalRSCalls = 0;
    std::uint64_t properCriticalRSSkips = 0;
    std::uint64_t properVSSwitches = 0;
    std::uint64_t properPSSwitches = 0;
    std::uint64_t properVSConstantVectors = 0;
    std::uint64_t properPSConstantVectors = 0;
    std::uint64_t properRTChanges = 0;
    std::uint64_t properDSChanges = 0;
    std::uint64_t properWorldChanges = 0;
    std::uint64_t properViewportChanges = 0;
    std::uint64_t properScissorChanges = 0;
    std::uint64_t properTexture0Changes = 0;
    IDirect3DVertexShader9* properLastVS = nullptr;
    IDirect3DPixelShader9* properLastPS = nullptr;
    std::array<IDirect3DSurface9*, 4> properLastRT{};
    IDirect3DSurface9* properLastDS = nullptr;
    IDirect3DBaseTexture9* properLastTexture0 = nullptr;
    D3DMATRIX properLastWorld{};
    bool properLastWorldValid = false;
    D3DVIEWPORT9 properLastViewport{};
    bool properLastViewportValid = false;
    RECT properLastScissor{};
    bool properLastScissorValid = false;

    D3D9StateCacheStats stats{};
};

Runtime g;

template <typename Fn>
bool PatchDeviceSlot(std::size_t slot, Fn hook, Fn& original) {
    if (!g.deviceVTable || slot >= kDeviceVTableSize)
        return false;

    void** address = &g.deviceVTable[slot];

    DWORD oldProtect = 0;
    if (!VirtualProtect(address, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    original = reinterpret_cast<Fn>(*address);
    *address = reinterpret_cast<void*>(hook);

    DWORD unused = 0;
    VirtualProtect(address, sizeof(void*), oldProtect, &unused);
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(void*));
    return original != nullptr;
}

template <typename Fn>
void RestoreDeviceSlot(std::size_t slot, Fn hook, Fn original) {
    if (!g.deviceVTable || !original || slot >= kDeviceVTableSize)
        return;

    void** address = &g.deviceVTable[slot];
    if (*address != reinterpret_cast<void*>(hook))
        return; // Another mod patched after us; do not break its chain.

    DWORD oldProtect = 0;
    if (!VirtualProtect(address, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return;

    *address = reinterpret_cast<void*>(original);

    DWORD unused = 0;
    VirtualProtect(address, sizeof(void*), oldProtect, &unused);
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(void*));
}

void InvalidateCache() {
    g.profileTargetKnown = false;
    // Keep validity together: invalidation touches 64 bytes rather than
    // striding through all 512 matrices on every Present/state-block Apply.
    g.transformValid.reset();
    g.viewport.valid = false;
    g.scissor.valid = false;
    g.renderTargetValid.reset();
    g.depthStencilValid = false;
    g.material.valid = false;
    for (auto& l : g.lights) l.valid = false;
    g.lightEnableValid.reset();
    for (auto& c : g.clipPlanes) c.valid = false;
    g.nPatchModeValid = false;

    g.renderStateValid.reset();
    g.textureValid.reset();

    for (auto& bits : g.samplerValid)
        bits.reset();
    for (auto& bits : g.textureStageValid)
        bits.reset();

    g.fvfValid = false;
    g.vertexShaderValid = false;
    g.pixelShaderValid = false;
    g.declarationValid = false;
    g.indicesValid = false;

    for (auto& s : g.streams)
        s.valid = false;
    for (auto& s : g.streamFreqs)
        s.valid = false;

    g.vsConstantValid.reset();
    g.psConstantValid.reset();

    ++g.stats.cacheInvalidations;
}

void ReleaseProfilePrimary();

void RefreshSkygfx(bool force = false) {
    if (!g.skygfxRequested) return;
    const auto now = GetTickCount64();
    if (!force && g.skygfxPollMs && now - g.skygfxPollMs < 1000) return;
    g.skygfxPollMs = now;
    const bool available = isSkygfxLoaded() && !ProperShadersLoaded();
    if (available == g.skygfxAvailable) return;
    g.skygfxAvailable = available;
    g.skygfxActive = available && g.skygfxConstantsRequested;
    g.skygfxBindingsActive = available && g.skygfxBindingsRequested;
    g.skygfxProfileActive = available && g.skygfxProfileRequested;
    g.profileBaseline = g.stats;
    if (!available) ReleaseProfilePrimary();
    g.stats.skygfxOptimizationActive = g.skygfxActive;
    g.stats.skygfxShaderBindingsActive = g.skygfxBindingsActive;
    g.stats.skygfxPassProfilingActive = g.skygfxProfileActive;
    g.config.optimizeShaderConstants = g.baseShaderConstants || g.skygfxActive;
    g.config.optimizeShaders = g.baseShaders || g.skygfxBindingsActive;
    g.effectiveVSConstants = g.effectivePSConstants = g.config.optimizeShaderConstants;
    g.effectiveVertexShader = g.effectivePixelShader = g.config.optimizeShaders;
    const auto mask = ADAPT_VSC | ADAPT_PSC | ADAPT_VS | ADAPT_PS;
    g.adaptiveConfiguredMask &= ~mask;
    if (g.config.optimizeShaderConstants) g.adaptiveConfiguredMask |= ADAPT_VSC | ADAPT_PSC;
    if (g.config.optimizeShaders) g.adaptiveConfiguredMask |= ADAPT_VS | ADAPT_PS;
    g.adaptiveEvaluatedMask &= ~mask;
    g.stats.adaptiveDisabledMask &= ~mask;
    g.adaptiveEvaluated = false;
    g.stats.adaptiveCompleted = false;
    g.sampleBaseline = g.stats;
    g.sampleStart = g.stats.presents;
    g.resampling = true;
    InvalidateCache();
    Log("SkyGFX device patches: constants=%d shader-bindings=%d profiling=%d; module %s.",
        g.skygfxActive, g.skygfxBindingsActive, g.skygfxProfileActive,
        available ? "available" : "unloaded or Proper Shaders present");
    Diagnostics::instance().registerFeature("Optimization.SkygfxShaderBindings", g.skygfxBindingsRequested);
    if (g.skygfxBindingsActive) Diagnostics::instance().active("Optimization.SkygfxShaderBindings", "loaded SkyGFX; shader-binding suppression active");
    else Diagnostics::instance().skipped("Optimization.SkygfxShaderBindings", "SkyGFX unavailable or automatic patch disabled");
    Diagnostics::instance().registerFeature("Optimization.SkygfxConstants", g.skygfxConstantsRequested);
    if (g.skygfxActive) Diagnostics::instance().active("Optimization.SkygfxConstants", "loaded SkyGFX; shader constant upload patch active");
    else Diagnostics::instance().skipped("Optimization.SkygfxConstants", "SkyGFX unavailable or Proper Shaders coexistence");
}

bool CanSuppress() {
    return g.installed && g.suppressionEnabled && !g.recordingStateBlock;
}

void ReleaseProfilePrimary() {
    if (g.profilePrimary) g.profilePrimary->Release();
    g.profilePrimary = nullptr;
    g.profileTarget = nullptr;
    g.profileTargetKnown = false;
}

struct DrawProfile {
    LARGE_INTEGER start{};
    int target = -1;
    bool active = false;
    explicit DrawProfile(IDirect3DDevice9* self, bool copy = false) {
        active = self == g.device && g.skygfxProfileActive &&
            g.profileFrequency.QuadPart > 0 && !DrawTelemetry::isOverlayDraw();
        if (!active) return;
        if (!copy) {
            if (!g.profilePrimary) {
                IDirect3DSurface9* primary = nullptr;
                const auto result = self->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &primary);
                if (SUCCEEDED(result)) g.profilePrimary = primary;
                else if (primary) primary->Release();
            }
            if (!g.profileTargetKnown) {
                IDirect3DSurface9* surface = nullptr;
                if (SUCCEEDED(self->GetRenderTarget(0, &surface)) && surface) {
                    g.profileTarget = surface;
                    g.profileTargetKnown = true;
                }
                if (surface) surface->Release();
            }
            if (g.profilePrimary && g.profileTargetKnown)
                target = g.profileTarget == g.profilePrimary ? 0 : 1;
        }
        QueryPerformanceCounter(&start);
    }
    void finish(HRESULT result, bool copy = false) {
        if (!active) return;
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        if (FAILED(result)) { ++g.stats.skygfxProfileFailures; return; }
        const double ms = double(end.QuadPart - start.QuadPart) * 1000.0 / double(g.profileFrequency.QuadPart);
        if (copy) { ++g.stats.skygfxCopies; g.stats.skygfxCopySubmitMs += ms; }
        else if (target == 0) { ++g.stats.skygfxMainDraws; g.stats.skygfxMainSubmitMs += ms; }
        else if (target == 1) { ++g.stats.skygfxOffscreenDraws; g.stats.skygfxOffscreenSubmitMs += ms; }
        else { ++g.stats.skygfxUnknownDraws; g.stats.skygfxUnknownSubmitMs += ms; }
    }
};

void LogSkygfxProfile() {
    if (!g.skygfxProfileActive || !g.profileLogInterval ||
        g.stats.presents - g.profileBaseline.presents < g.profileLogInterval) return;
    const auto& b = g.profileBaseline;
    std::ostringstream output;
    output << "[SkyGFX-PROFILE] CPU API-call submission time (not GPU time); off-screen includes reflection/postfx/other mods; frames="
        << g.stats.presents - b.presents
        << " mainDraws=" << g.stats.skygfxMainDraws - b.skygfxMainDraws
        << " mainSubmitMs=" << g.stats.skygfxMainSubmitMs - b.skygfxMainSubmitMs
        << " offscreenDraws=" << g.stats.skygfxOffscreenDraws - b.skygfxOffscreenDraws
        << " offscreenSubmitMs=" << g.stats.skygfxOffscreenSubmitMs - b.skygfxOffscreenSubmitMs
        << " unknownDraws=" << g.stats.skygfxUnknownDraws - b.skygfxUnknownDraws
        << " unknownSubmitMs=" << g.stats.skygfxUnknownSubmitMs - b.skygfxUnknownSubmitMs
        << " stretchRectCopies=" << g.stats.skygfxCopies - b.skygfxCopies
        << " copySubmitMs=" << g.stats.skygfxCopySubmitMs - b.skygfxCopySubmitMs
        << " failedCalls=" << g.stats.skygfxProfileFailures - b.skygfxProfileFailures
        << " shaderBindsAvoided=" << g.stats.skygfxShaderBindSkipped - b.skygfxShaderBindSkipped
        << " constantVectorsAvoided=" << g.stats.skygfxConstantVectorsSaved - b.skygfxConstantVectorsSaved;
    Log("%s", output.str().c_str());
    g.profileBaseline = g.stats;
}

bool SameFloat4(const std::array<float, 4>& a, const float* b) {
    return std::memcmp(a.data(), b, sizeof(float) * 4) == 0;
}

void StoreFloat4(std::array<float, 4>& a, const float* b) {
    std::memcpy(a.data(), b, sizeof(float) * 4);
}


template <typename T>
bool SamePOD(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

bool SameMatrix(const D3DMATRIX& a, const D3DMATRIX& b) {
    return std::memcmp(&a, &b, sizeof(D3DMATRIX)) == 0;
}


void ApplyRecordedStateBlock(const RecordedStateBlock& block) {
    for (std::size_t i = 0; i < block.renderStateValid.size(); ++i) {
        if (block.renderStateValid.test(i)) {
            g.renderStates[i] = block.renderStates[i];
            g.renderStateValid.set(i);
        }
    }

    for (std::size_t i = 0; i < block.textureValid.size(); ++i) {
        if (block.textureValid.test(i)) {
            g.textures[i] = block.textures[i];
            g.textureValid.set(i);
        }
    }

    for (std::size_t sampler = 0; sampler < block.samplerValid.size(); ++sampler) {
        for (std::size_t type = 0; type < block.samplerValid[sampler].size(); ++type) {
            if (block.samplerValid[sampler].test(type)) {
                g.samplerStates[sampler][type] = block.samplerStates[sampler][type];
                g.samplerValid[sampler].set(type);
            }
        }
    }

    for (std::size_t stage = 0; stage < block.textureStageValid.size(); ++stage) {
        for (std::size_t type = 0; type < block.textureStageValid[stage].size(); ++type) {
            if (block.textureStageValid[stage].test(type)) {
                g.textureStageStates[stage][type] = block.textureStageStates[stage][type];
                g.textureStageValid[stage].set(type);
            }
        }
    }

    if (block.fvfValid) {
        g.fvf = block.fvf;
        g.fvfValid = true;
        g.declarationValid = false;
    }

    if (block.vertexShaderValid) {
        g.vertexShader = block.vertexShader;
        g.vertexShaderValid = true;
    }

    if (block.pixelShaderValid) {
        g.pixelShader = block.pixelShader;
        g.pixelShaderValid = true;
    }

    if (block.declarationValid) {
        g.declaration = block.declaration;
        g.declarationValid = true;
        g.fvfValid = false;
    }

    for (std::size_t reg = 0; reg < block.vsConstantValid.size(); ++reg) {
        if (block.vsConstantValid.test(reg)) {
            g.vsConstants[reg] = block.vsConstants[reg];
            g.vsConstantValid.set(reg);
        }
    }

    for (std::size_t reg = 0; reg < block.psConstantValid.size(); ++reg) {
        if (block.psConstantValid.test(reg)) {
            g.psConstants[reg] = block.psConstants[reg];
            g.psConstantValid.set(reg);
        }
    }
}

const StateBlockPatch* FindStateBlockPatch(IDirect3DStateBlock9* block) {
    if (!block)
        return nullptr;

    void** vtable = *reinterpret_cast<void***>(block);
    for (std::size_t i = 0; i < g.stateBlockPatchCount; ++i) {
        const auto& patch = g.stateBlockPatches[i];
        if (patch.vtable == vtable)
            return &patch;
    }
    return nullptr;
}

HRESULT STDMETHODCALLTYPE StateBlockCaptureHook(IDirect3DStateBlock9* self) {
    const StateBlockPatch* patch = FindStateBlockPatch(self);
    if (!patch || !patch->originalCapture)
        return D3DERR_INVALIDCALL;

    const HRESULT hr = patch->originalCapture(self);
    if (SUCCEEDED(hr)) {
        ++g.stats.stateBlockCaptures;

        // Capture replaces the values stored in this state block with current device
        // values for its captured state set. SAVO cannot reconstruct that new value
        // set without querying every state, so the old recorded delta is now stale.
        //
        // Mark this block opaque permanently. Future Apply() calls will invalidate
        // SAVO's cache instead of replaying stale data.
        auto it = g.stateBlocks.find(self);
        if (it != g.stateBlocks.end()) {
            if (it->second.reconstructable)
                ++g.stats.stateBlockCaptureDowngrades;
            g.stateBlocks.erase(it);
        }

        // Capture itself does not change current device state, so the live cache
        // remains valid here.
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE StateBlockApplyHook(IDirect3DStateBlock9* self) {
    const StateBlockPatch* patch = FindStateBlockPatch(self);
    if (!patch || !patch->originalApply)
        return D3DERR_INVALIDCALL;

    const HRESULT hr = patch->originalApply(self);
    if (SUCCEEDED(hr)) {
        ++g.stats.stateBlockApplies;

        const auto it = g.stateBlocks.find(self);
        if (it != g.stateBlocks.end() && it->second.reconstructable) {
            // Exact tracked-state delta from BeginStateBlock/EndStateBlock.
            ++g.stats.stateBlockDeltaApplies;
            ApplyRecordedStateBlock(it->second);
        } else {
            ++g.stats.stateBlockOpaqueApplies;
            // CreateStateBlock captures implicit device state that SAVO cannot
            // reconstruct safely. Fall back to a full cache invalidation.
            InvalidateCache();
        }
    }
    return hr;
}

ULONG STDMETHODCALLTYPE StateBlockReleaseHook(IDirect3DStateBlock9* self) {
    const StateBlockPatch* patch = FindStateBlockPatch(self);
    if (!patch || !patch->originalRelease)
        return 0;

    const auto original = patch->originalRelease;
    const ULONG refs = original(self);

    if (refs == 0)
        g.stateBlocks.erase(self);

    return refs;
}

void HookStateBlock(IDirect3DStateBlock9* block, const RecordedStateBlock* recorded) {
    if (!block)
        return;

    void** vtable = *reinterpret_cast<void***>(block);
    if (!vtable)
        return;

    bool alreadyPatched = false;
    for (std::size_t i = 0; i < g.stateBlockPatchCount; ++i) {
        const auto& patch = g.stateBlockPatches[i];
        if (patch.vtable == vtable) {
            alreadyPatched = true;
            break;
        }
    }

    if (!alreadyPatched) {
        if (g.stateBlockPatchCount >= g.stateBlockPatches.size()) {
            // Refuse to patch an implementation we cannot remember and restore.
            g.suppressionEnabled = false;
            return;
        }
        constexpr std::size_t releaseSlot = 2;
        constexpr std::size_t captureSlot = 4;
        constexpr std::size_t applySlot = 5;

        auto originalRelease = reinterpret_cast<StateBlockReleaseFn>(vtable[releaseSlot]);
        auto originalCapture = reinterpret_cast<StateBlockCaptureFn>(vtable[captureSlot]);
        auto originalApply = reinterpret_cast<StateBlockApplyFn>(vtable[applySlot]);

        DWORD oldProtect = 0;
        if (!VirtualProtect(vtable, sizeof(void*) * 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            // Unobserved Apply calls could invalidate any cached state.
            g.suppressionEnabled = false;
            return;
        }

        vtable[releaseSlot] = reinterpret_cast<void*>(&StateBlockReleaseHook);
        vtable[captureSlot] = reinterpret_cast<void*>(&StateBlockCaptureHook);
        vtable[applySlot] = reinterpret_cast<void*>(&StateBlockApplyHook);

        DWORD unused = 0;
        VirtualProtect(vtable, sizeof(void*) * 6, oldProtect, &unused);
        FlushInstructionCache(GetCurrentProcess(), vtable, sizeof(void*) * 6);

        g.stateBlockPatches[g.stateBlockPatchCount++] =
            {vtable, originalCapture, originalApply, originalRelease};
    }

    if (recorded && recorded->reconstructable) {
        const auto existing = g.stateBlocks.find(block);
        if (existing != g.stateBlocks.end()) {
            existing->second = *recorded;
        } else if (g.stateBlocks.size() < Runtime::kMaxRecordedStateBlocks) {
            g.stateBlocks.emplace(block, *recorded);
            g.peakRecordedStateBlocks =
                std::max(g.peakRecordedStateBlocks, g.stateBlocks.size());
        } else {
            // Hard memory bound: an untracked block is still correct because
            // Apply() falls back to full cache invalidation.
            g.stateBlocks.erase(block);
        }
    } else {
        // Unknown blocks already take the conservative Apply path. In
        // particular, ImGui creates an opaque block every frame: no map
        // allocation or large zero-filled delta is needed for it.
        g.stateBlocks.erase(block);
    }
}




static double SkipRate(std::uint64_t skipped, std::uint64_t calls) {
    if (calls == 0)
        return 0.0;
    return (static_cast<double>(skipped) * 100.0) / static_cast<double>(calls);
}

static bool CategoryHasEnoughSamples(std::uint64_t calls) {
    return calls >= g.config.adaptiveMinimumCalls;
}

static void EvaluateOneCategory(
    bool configured,
    bool& effective,
    std::uint64_t skipped,
    std::uint64_t calls,
    std::uint32_t bit,
    const char* name)
{
    if (!configured) {
        effective = false;
        g.adaptiveEvaluatedMask |= bit;
        return;
    }

    if ((g.adaptiveEvaluatedMask & bit) != 0)
        return;

    const bool forceDecision = g.resampling ||
        g.stats.presents >= g.config.adaptiveMaxPresents;
    if (!CategoryHasEnoughSamples(calls) && !forceDecision)
        return; // Still pending; gameplay may not have reached this path yet.

    // Low-volume resamples retain the prior policy. Hysteresis prevents
    // categories close to the threshold from toggling every window.
    const bool previouslyKept = (g.previousDisabledMask & bit) == 0;
    const double threshold = std::min(100u, g.config.adaptiveMinSkipPercent +
        ((g.resampling && !previouslyKept) ? 2u : 0u));
    const bool keep = !CategoryHasEnoughSamples(calls)
        ? (!g.resampling || previouslyKept)
        : SkipRate(skipped, calls) >= threshold;

    effective = keep;
    g.adaptiveEvaluatedMask |= bit;

    if (!keep)
        g.stats.adaptiveDisabledMask |= bit;
    else
        g.stats.adaptiveDisabledMask &= ~bit;

    Log("Adaptive %s: calls=%llu skipped=%llu rate=%.2f%% samples=%s -> %s",
        name,
        static_cast<unsigned long long>(calls),
        static_cast<unsigned long long>(skipped),
        SkipRate(skipped, calls),
        CategoryHasEnoughSamples(calls) ? "qualified" : "insufficient/max-window",
        keep ? "KEEP" : "DISABLE");
    if (skipped > 0)
        mws::Diagnostics::instance().effective(
            "Optimization.D3DStateDedup",
            "adaptive counters observed redundant D3D9 state calls being skipped");
}

static void EvaluateAdaptivePolicy() {
    if (!g.config.productionMode || !g.config.adaptiveEnabled)
        return;

    if (g.adaptiveEvaluated) {
        if (!g.config.resampleInterval || g.stats.presents < g.nextSample)
            return;
        g.resampling = true;
        g.adaptiveEvaluated = false;
        g.stats.adaptiveCompleted = false;
        g.previousDisabledMask = g.stats.adaptiveDisabledMask;
        g.sampleBaseline = g.stats;
        g.sampleStart = g.stats.presents;
        g.adaptiveEvaluatedMask = ~g.adaptiveConfiguredMask;
        g.stats.adaptiveEvaluatedMask = g.adaptiveEvaluatedMask;
        // Re-enable configured categories to measure actual redundancy again.
        g.effectiveRenderState = g.config.optimizeRenderState;
        g.effectiveTexture = g.config.optimizeTexture;
        g.effectiveSamplerState = g.config.optimizeSamplerState;
        g.effectiveTextureStageState = g.config.optimizeTextureStageState;
        g.effectiveFVF = g.config.optimizeFVF;
        g.effectiveVertexShader = g.config.optimizeShaders;
        g.effectivePixelShader = g.config.optimizeShaders;
        g.effectiveVertexDeclaration = g.config.optimizeVertexDeclaration;
        g.effectiveVSConstants = g.config.optimizeShaderConstants;
        g.effectivePSConstants = g.config.optimizeShaderConstants;
        g.effectiveTransform = g.config.optimizeTransform;
        g.effectiveViewport = g.config.optimizeViewport;
        g.effectiveScissorRect = g.config.optimizeScissorRect;
        g.effectiveRenderTarget = g.config.optimizeRenderTarget;
        g.effectiveDepthStencil = g.config.optimizeDepthStencil;
        g.effectiveMaterial = g.config.optimizeMaterial;
        g.effectiveLight = g.config.optimizeLight;
        g.effectiveLightEnable = g.config.optimizeLightEnable;
        g.effectiveClipPlane = g.config.optimizeClipPlane;
        g.effectiveNPatchMode = g.config.optimizeNPatchMode;
        InvalidateCache();
    }

    if (g.resampling ? (g.stats.presents - g.sampleStart < g.config.resampleWindow)
                     : (g.stats.presents < g.config.adaptiveWarmupPresents))
        return;

    EvaluateOneCategory(
        g.config.optimizeRenderState, g.effectiveRenderState,
        (g.stats.renderStateSkipped - g.sampleBaseline.renderStateSkipped), (g.stats.renderStateCalls - g.sampleBaseline.renderStateCalls), ADAPT_RS, "RenderState");

    EvaluateOneCategory(
        g.config.optimizeTexture, g.effectiveTexture,
        (g.stats.textureSkipped - g.sampleBaseline.textureSkipped), (g.stats.textureCalls - g.sampleBaseline.textureCalls), ADAPT_TEX, "Texture");

    EvaluateOneCategory(
        g.config.optimizeSamplerState, g.effectiveSamplerState,
        (g.stats.samplerStateSkipped - g.sampleBaseline.samplerStateSkipped), (g.stats.samplerStateCalls - g.sampleBaseline.samplerStateCalls), ADAPT_SS, "SamplerState");

    EvaluateOneCategory(
        g.config.optimizeTextureStageState, g.effectiveTextureStageState,
        (g.stats.textureStageSkipped - g.sampleBaseline.textureStageSkipped), (g.stats.textureStageCalls - g.sampleBaseline.textureStageCalls), ADAPT_TSS, "TextureStageState");

    EvaluateOneCategory(
        g.config.optimizeFVF, g.effectiveFVF,
        (g.stats.fvfSkipped - g.sampleBaseline.fvfSkipped), (g.stats.fvfCalls - g.sampleBaseline.fvfCalls), ADAPT_FVF, "FVF");

    EvaluateOneCategory(
        g.config.optimizeShaders, g.effectiveVertexShader,
        (g.stats.vertexShaderSkipped - g.sampleBaseline.vertexShaderSkipped), (g.stats.vertexShaderCalls - g.sampleBaseline.vertexShaderCalls), ADAPT_VS, "VertexShader");

    EvaluateOneCategory(
        g.config.optimizeShaders, g.effectivePixelShader,
        (g.stats.pixelShaderSkipped - g.sampleBaseline.pixelShaderSkipped), (g.stats.pixelShaderCalls - g.sampleBaseline.pixelShaderCalls), ADAPT_PS, "PixelShader");

    EvaluateOneCategory(
        g.config.optimizeVertexDeclaration, g.effectiveVertexDeclaration,
        (g.stats.declarationSkipped - g.sampleBaseline.declarationSkipped), (g.stats.declarationCalls - g.sampleBaseline.declarationCalls), ADAPT_DECL, "VertexDeclaration");

    EvaluateOneCategory(
        g.config.optimizeShaderConstants, g.effectiveVSConstants,
        (g.stats.vsConstantSkipped - g.sampleBaseline.vsConstantSkipped) + (g.stats.vsConstantTrimmed - g.sampleBaseline.vsConstantTrimmed), (g.stats.vsConstantCalls - g.sampleBaseline.vsConstantCalls), ADAPT_VSC, "VSConstants");

    EvaluateOneCategory(
        g.config.optimizeShaderConstants, g.effectivePSConstants,
        (g.stats.psConstantSkipped - g.sampleBaseline.psConstantSkipped) + (g.stats.psConstantTrimmed - g.sampleBaseline.psConstantTrimmed), (g.stats.psConstantCalls - g.sampleBaseline.psConstantCalls), ADAPT_PSC, "PSConstants");

    EvaluateOneCategory(
        g.config.optimizeTransform, g.effectiveTransform,
        (g.stats.transformSkipped - g.sampleBaseline.transformSkipped), (g.stats.transformCalls - g.sampleBaseline.transformCalls), ADAPT_TRANSFORM, "Transform");

    EvaluateOneCategory(
        g.config.optimizeViewport, g.effectiveViewport,
        (g.stats.viewportSkipped - g.sampleBaseline.viewportSkipped), (g.stats.viewportCalls - g.sampleBaseline.viewportCalls), ADAPT_VIEWPORT, "Viewport");

    EvaluateOneCategory(
        g.config.optimizeScissorRect, g.effectiveScissorRect,
        (g.stats.scissorSkipped - g.sampleBaseline.scissorSkipped), (g.stats.scissorCalls - g.sampleBaseline.scissorCalls), ADAPT_SCISSOR, "ScissorRect");

    EvaluateOneCategory(
        g.config.optimizeRenderTarget, g.effectiveRenderTarget,
        (g.stats.renderTargetSkipped - g.sampleBaseline.renderTargetSkipped), (g.stats.renderTargetCalls - g.sampleBaseline.renderTargetCalls), ADAPT_RT, "RenderTarget");

    EvaluateOneCategory(
        g.config.optimizeDepthStencil, g.effectiveDepthStencil,
        (g.stats.depthStencilSkipped - g.sampleBaseline.depthStencilSkipped), (g.stats.depthStencilCalls - g.sampleBaseline.depthStencilCalls), ADAPT_DS, "DepthStencil");

    EvaluateOneCategory(
        g.config.optimizeMaterial, g.effectiveMaterial,
        (g.stats.materialSkipped - g.sampleBaseline.materialSkipped), (g.stats.materialCalls - g.sampleBaseline.materialCalls), ADAPT_MATERIAL, "Material");

    EvaluateOneCategory(
        g.config.optimizeLight, g.effectiveLight,
        (g.stats.lightSkipped - g.sampleBaseline.lightSkipped), (g.stats.lightCalls - g.sampleBaseline.lightCalls), ADAPT_LIGHT, "Light");

    EvaluateOneCategory(
        g.config.optimizeLightEnable, g.effectiveLightEnable,
        (g.stats.lightEnableSkipped - g.sampleBaseline.lightEnableSkipped), (g.stats.lightEnableCalls - g.sampleBaseline.lightEnableCalls), ADAPT_LIGHT_ENABLE, "LightEnable");

    EvaluateOneCategory(
        g.config.optimizeClipPlane, g.effectiveClipPlane,
        (g.stats.clipPlaneSkipped - g.sampleBaseline.clipPlaneSkipped), (g.stats.clipPlaneCalls - g.sampleBaseline.clipPlaneCalls), ADAPT_CLIP, "ClipPlane");

    EvaluateOneCategory(
        g.config.optimizeNPatchMode, g.effectiveNPatchMode,
        (g.stats.nPatchSkipped - g.sampleBaseline.nPatchSkipped), (g.stats.nPatchCalls - g.sampleBaseline.nPatchCalls), ADAPT_NPATCH, "NPatchMode");

    g.stats.adaptiveEvaluatedMask = g.adaptiveEvaluatedMask;

    if ((g.adaptiveEvaluatedMask & g.adaptiveConfiguredMask) == g.adaptiveConfiguredMask) {
        g.adaptiveEvaluated = true;
        g.nextSample = g.stats.presents + g.config.resampleInterval;
        g.stats.adaptiveCompleted = true;
        Log("Adaptive production tuning complete. evaluatedMask=0x%08X disabledMask=0x%08X",
            g.adaptiveEvaluatedMask,
            g.stats.adaptiveDisabledMask);
    }
}



HRESULT STDMETHODCALLTYPE DrawPrimitiveProfileHook(IDirect3DDevice9* self,
    D3DPRIMITIVETYPE type, UINT first, UINT count) {
    DrawProfile profile(self);
    const auto result = g.drawPrimitive(self, type, first, count);
    profile.finish(result);
    return result;
}
HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveProfileHook(IDirect3DDevice9* self,
    D3DPRIMITIVETYPE type, INT base, UINT first, UINT vertices, UINT index, UINT count) {
    DrawProfile profile(self);
    const auto result = g.drawIndexedPrimitive(self, type, base, first, vertices, index, count);
    profile.finish(result);
    return result;
}
HRESULT STDMETHODCALLTYPE StretchRectProfileHook(IDirect3DDevice9* self,
    IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* dest,
    const RECT* destRect, D3DTEXTUREFILTERTYPE filter) {
    DrawProfile profile(self, true);
    const auto result = g.stretchRect(self, source, sourceRect, dest, destRect, filter);
    profile.finish(result, true);
    return result;
}

HRESULT STDMETHODCALLTYPE SetRenderTargetHook(
    IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface)
{
    if (self != g.device)
        return g.setRenderTarget(self, index, surface);

    ++g.stats.renderTargetCalls;
    if (g.properShadersDetected && index < g.properLastRT.size() &&
        g.properLastRT[index] != surface) {
        ++g.properRTChanges;
        g.properLastRT[index] = surface;
    }

    // Target zero resets the viewport even when the surface is unchanged.
    if (!g.properShadersSafetyMode && index != 0 && g.effectiveRenderTarget && CanSuppress() &&
        index < g.renderTargets.size() &&
        g.renderTargetValid.test(index) &&
        g.renderTargets[index] == surface) {
        ++g.stats.renderTargetSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setRenderTarget(self, index, surface);
    if (SUCCEEDED(hr) && index == 0) {
        g.profileTarget = surface;
        g.profileTargetKnown = surface != nullptr;
        g.viewport.valid = false;
        g.scissor.valid = false;
    }
    if (SUCCEEDED(hr) && index < g.renderTargets.size()) {
        g.renderTargets[index] = surface;
        g.renderTargetValid.set(index);
    }

    // Render targets are not represented in SAVO's reconstructable state-block delta.
    // If called while recording, make the block opaque.
    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE SetDepthStencilSurfaceHook(
    IDirect3DDevice9* self, IDirect3DSurface9* surface)
{
    if (self != g.device)
        return g.setDepthStencilSurface(self, surface);

    ++g.stats.depthStencilCalls;
    if (g.properShadersDetected && g.properLastDS != surface) {
        ++g.properDSChanges;
        g.properLastDS = surface;
    }
    if (!g.effectiveDepthStencil && !g.recordingStateBlock)
        return g.setDepthStencilSurface(self, surface);

    if (!g.properShadersSafetyMode && g.effectiveDepthStencil && CanSuppress() &&
        g.depthStencilValid && g.depthStencil == surface) {
        ++g.stats.depthStencilSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setDepthStencilSurface(self, surface);
    if (SUCCEEDED(hr)) {
        g.depthStencil = surface;
        g.depthStencilValid = true;
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE SetTransformHook(
    IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
{
    if (self != g.device)
        return g.setTransform(self, state, matrix);

    ++g.stats.transformCalls;
    if (g.properShadersDetected && state == D3DTS_WORLD && matrix) {
        if (!g.properLastWorldValid ||
            std::memcmp(&g.properLastWorld, matrix, sizeof(D3DMATRIX)) != 0) {
            ++g.properWorldChanges;
            g.properLastWorld = *matrix;
            g.properLastWorldValid = true;
        }
    }
    if (!g.effectiveTransform && !g.recordingStateBlock)
        return g.setTransform(self, state, matrix);

    const std::size_t index = static_cast<std::size_t>(state);
    if (!g.properShadersSafetyMode && matrix && g.effectiveTransform && CanSuppress() &&
        index < g.transforms.size() &&
        g.transformValid.test(index) &&
        SameMatrix(g.transforms[index], *matrix)) {
        ++g.stats.transformSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setTransform(self, state, matrix);
    if (SUCCEEDED(hr) && matrix && index < g.transforms.size()) {
        g.transforms[index] = *matrix;
        g.transformValid.set(index);
    }

    // State blocks can contain transforms, but v17 conservatively treats any block
    // that records them as opaque rather than partially reconstructing them.
    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE MultiplyTransformHook(
    IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
{
    if (self != g.device)
        return g.multiplyTransform(self, state, matrix);

    const HRESULT hr = g.multiplyTransform(self, state, matrix);
    const auto index = static_cast<std::size_t>(state);
    if (SUCCEEDED(hr) && index < g.transforms.size())
        g.transformValid.reset(index);
    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE DrawPrimitiveUPHook(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count,
    const void* data, UINT stride)
{
    if (self != g.device)
        return g.drawPrimitiveUP(self, type, count, data, stride);

    DrawProfile profile(self);
    const HRESULT hr = g.drawPrimitiveUP(self, type, count, data, stride);
    profile.finish(hr);
    g.streams[0].valid = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUPHook(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minIndex,
    UINT vertices, UINT count, const void* indices, D3DFORMAT format,
    const void* data, UINT stride)
{
    if (self != g.device)
        return g.drawIndexedPrimitiveUP(self, type, minIndex, vertices, count, indices, format, data, stride);

    DrawProfile profile(self);
    const HRESULT hr = g.drawIndexedPrimitiveUP(
        self, type, minIndex, vertices, count, indices, format, data, stride);
    profile.finish(hr);
    g.streams[0].valid = false;
    g.indicesValid = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE SetViewportHook(
    IDirect3DDevice9* self, const D3DVIEWPORT9* viewport)
{
    if (self != g.device)
        return g.setViewport(self, viewport);

    ++g.stats.viewportCalls;
    if (g.properShadersDetected && viewport &&
        (!g.properLastViewportValid ||
         std::memcmp(&g.properLastViewport, viewport, sizeof(D3DVIEWPORT9)) != 0)) {
        ++g.properViewportChanges;
        g.properLastViewport = *viewport;
        g.properLastViewportValid = true;
    }
    if (!g.effectiveViewport && !g.recordingStateBlock)
        return g.setViewport(self, viewport);

    if (!g.properShadersSafetyMode && viewport && g.effectiveViewport && CanSuppress() &&
        g.viewport.valid &&
        SamePOD(g.viewport.viewport, *viewport)) {
        ++g.stats.viewportSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setViewport(self, viewport);
    if (SUCCEEDED(hr) && viewport) {
        g.viewport.viewport = *viewport;
        g.viewport.valid = true;
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE SetMaterialHook(
    IDirect3DDevice9* self, const D3DMATERIAL9* material)
{
    if (self != g.device)
        return g.setMaterial(self, material);

    ++g.stats.materialCalls;
    if (!g.effectiveMaterial && !g.recordingStateBlock)
        return g.setMaterial(self, material);

    if (material && g.effectiveMaterial && CanSuppress() &&
        g.material.valid && SamePOD(g.material.material, *material)) {
        ++g.stats.materialSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setMaterial(self, material);
    if (SUCCEEDED(hr) && material) {
        g.material.material = *material;
        g.material.valid = true;
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE SetLightHook(
    IDirect3DDevice9* self, DWORD index, const D3DLIGHT9* light)
{
    if (self != g.device)
        return g.setLight(self, index, light);

    ++g.stats.lightCalls;
    if (!g.effectiveLight && !g.recordingStateBlock)
        return g.setLight(self, index, light);

    if (light && g.effectiveLight && CanSuppress() &&
        index < g.lights.size() &&
        g.lights[index].valid &&
        SamePOD(g.lights[index].light, *light)) {
        ++g.stats.lightSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setLight(self, index, light);
    if (SUCCEEDED(hr) && light && index < g.lights.size()) {
        g.lights[index].light = *light;
        g.lights[index].valid = true;
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE LightEnableHook(
    IDirect3DDevice9* self, DWORD index, BOOL enable)
{
    if (self != g.device)
        return g.lightEnable(self, index, enable);

    ++g.stats.lightEnableCalls;
    if (!g.effectiveLightEnable && !g.recordingStateBlock)
        return g.lightEnable(self, index, enable);

    if (g.effectiveLightEnable && CanSuppress() &&
        index < g.lightEnabled.size() &&
        g.lightEnableValid.test(index) &&
        g.lightEnabled[index] == enable) {
        ++g.stats.lightEnableSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.lightEnable(self, index, enable);
    if (SUCCEEDED(hr) && index < g.lightEnabled.size()) {
        g.lightEnabled[index] = enable;
        g.lightEnableValid.set(index);
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE SetClipPlaneHook(
    IDirect3DDevice9* self, DWORD index, const float* plane)
{
    if (self != g.device)
        return g.setClipPlane(self, index, plane);

    ++g.stats.clipPlaneCalls;
    if (!g.effectiveClipPlane && !g.recordingStateBlock)
        return g.setClipPlane(self, index, plane);

    if (plane && g.effectiveClipPlane && CanSuppress() &&
        index < g.clipPlanes.size() &&
        g.clipPlanes[index].valid &&
        std::memcmp(g.clipPlanes[index].values, plane, sizeof(float) * 4) == 0) {
        ++g.stats.clipPlaneSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setClipPlane(self, index, plane);
    if (SUCCEEDED(hr) && plane && index < g.clipPlanes.size()) {
        std::memcpy(g.clipPlanes[index].values, plane, sizeof(float) * 4);
        g.clipPlanes[index].valid = true;
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE SetScissorRectHook(
    IDirect3DDevice9* self, const RECT* rect)
{
    if (self != g.device)
        return g.setScissorRect(self, rect);

    ++g.stats.scissorCalls;
    if (g.properShadersDetected && rect &&
        (!g.properLastScissorValid ||
         std::memcmp(&g.properLastScissor, rect, sizeof(RECT)) != 0)) {
        ++g.properScissorChanges;
        g.properLastScissor = *rect;
        g.properLastScissorValid = true;
    }
    if (!g.effectiveScissorRect && !g.recordingStateBlock)
        return g.setScissorRect(self, rect);

    if (!g.properShadersSafetyMode && rect && g.effectiveScissorRect && CanSuppress() &&
        g.scissor.valid && SamePOD(g.scissor.rect, *rect)) {
        ++g.stats.scissorSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setScissorRect(self, rect);
    if (SUCCEEDED(hr) && rect) {
        g.scissor.rect = *rect;
        g.scissor.valid = true;
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

HRESULT STDMETHODCALLTYPE SetNPatchModeHook(
    IDirect3DDevice9* self, float segments)
{
    if (self != g.device)
        return g.setNPatchMode(self, segments);

    ++g.stats.nPatchCalls;
    if (!g.effectiveNPatchMode && !g.recordingStateBlock)
        return g.setNPatchMode(self, segments);

    if (g.effectiveNPatchMode && CanSuppress() &&
        g.nPatchModeValid &&
        std::memcmp(&g.nPatchMode, &segments, sizeof(float)) == 0) {
        ++g.stats.nPatchSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setNPatchMode(self, segments);
    if (SUCCEEDED(hr)) {
        g.nPatchMode = segments;
        g.nPatchModeValid = true;
    }

    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;

    return hr;
}

// QPC is sampled identically on both sides; the non-timed calls pay only a
// small counter/branch. This compares CPU setter time, never GPU time or FPS.
// Each new D3D device gets fresh samples (so SkyGFX/ReFX aren't pooled).
struct SetterCostScope {
    unsigned category{};
    bool active{};
    unsigned side{};
    LARGE_INTEGER start{};
    explicit SetterCostScope(unsigned c) : category(c) {
        if (!g.costEnabled || !g.costRunning || g.recordingStateBlock ||
            !g.costFrequency.QuadPart || (++g.costCalls[c] & 127u)) return;
        side = g.costPassthrough ? 1u : 0u;
        QueryPerformanceCounter(&start);
        active = true;
    }
    ~SetterCostScope() {
        if (!active) return;
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        auto& stats = g.costSamples[category];
        ++stats.calls[side];
        stats.ticks[side] += static_cast<std::uint64_t>(end.QuadPart - start.QuadPart);
    }
};
void EvaluateSetterCost() {
    if (!g.costEnabled || !g.config.adaptiveEnabled ||
        !g.adaptiveEvaluated) return;
    if (!g.costRunning) {
        if (g.stats.presents < g.costNext) return;
        g.costRunning = true;
        g.costFrames = 0;
        g.costPassthrough = false;
        g.costSamples = {};
        g.costCalls[0] = g.costCalls[1] = g.costCalls[2] = g.costCalls[3] = 0;
        // Begin with the configured (optimized) path regardless of the
        // earlier count-only decision, to obtain a true control comparison.
        g.effectiveRenderState = g.config.optimizeRenderState;
        g.effectiveTexture = g.config.optimizeTexture;
        g.effectiveSamplerState = g.config.optimizeSamplerState;
        g.effectiveTextureStageState = g.config.optimizeTextureStageState;
        InvalidateCache();
    }
    if (++g.costFrames < g.costWindow) return;
    g.costFrames = 0;
    if (!g.costPassthrough) {
        g.costPassthrough = true;
        // Cached values cannot be trusted after a passthrough setter.
        InvalidateCache();
        return;
    }
    g.costPassthrough = false;
    g.costRunning = false;
    constexpr std::uint32_t masks[]{ADAPT_RS, ADAPT_TEX, ADAPT_SS, ADAPT_TSS};
    constexpr const char* names[]{"RenderState", "Texture", "Sampler", "TextureStage"};
    const bool configured[]{g.config.optimizeRenderState, g.config.optimizeTexture,
        g.config.optimizeSamplerState, g.config.optimizeTextureStageState};
    bool* effective[]{&g.effectiveRenderState, &g.effectiveTexture,
        &g.effectiveSamplerState, &g.effectiveTextureStageState};
    const char* renderer = g.properShadersDetected ? "ProperShaders" :
        g.skygfxAvailable ? "SkyGFX" : "stock/other";
    for (unsigned i = 0; i < 4; ++i) {
        if (!configured[i]) continue;
        const auto& s = g.costSamples[i];
        if (s.calls[0] < 32 || s.calls[1] < 32) continue;
        const double optimized = double(s.ticks[0]) / s.calls[0];
        const double passthrough = double(s.ticks[1]) / s.calls[1];
        // Require >5% measured CPU advantage; noisy/borderline paths
        // stay disabled until the next independent paired probe.
        const bool keep = optimized * 1.05 < passthrough;
        if (keep) {
            g.costDisabledMask &= ~masks[i];
            g.stats.adaptiveDisabledMask &= ~masks[i];
        } else {
            g.costDisabledMask |= masks[i];
            g.stats.adaptiveDisabledMask |= masks[i];
        }
        *effective[i] = keep;
        const double usFactor = g.costFrequency.QuadPart ?
            1e6 / double(g.costFrequency.QuadPart) : 0.0;
        Log("Setter-time A/B renderer=%s category=%s CPUus[optimized,passthrough]=%.3f,%.3f samples=%llu,%llu -> %s",
            renderer, names[i], optimized * usFactor, passthrough * usFactor,
            static_cast<unsigned long long>(s.calls[0]),
            static_cast<unsigned long long>(s.calls[1]),
            keep ? "KEEP" : "BYPASS");
    }
    // Respect previous adaptive decisions where A/B lacks sample volume.
    g.effectiveRenderState &= !(g.costDisabledMask & ADAPT_RS);
    g.effectiveTexture &= !(g.costDisabledMask & ADAPT_TEX);
    g.effectiveSamplerState &= !(g.costDisabledMask & ADAPT_SS);
    g.effectiveTextureStageState &= !(g.costDisabledMask & ADAPT_TSS);
    InvalidateCache();
    g.costNext = static_cast<unsigned>(g.stats.presents) +
        std::max(g.config.resampleInterval, g.costWindow * 2);
}
HRESULT STDMETHODCALLTYPE SetRenderStateHook(
    IDirect3DDevice9* self,
    D3DRENDERSTATETYPE state,
    DWORD value)
{
    if (self != g.device)
        return g.setRenderState(self, state, value);

    SetterCostScope timed(0);
    ++g.stats.renderStateCalls;
    if (g.costRunning && g.costPassthrough && !g.recordingStateBlock)
        return g.setRenderState(self, state, value);
    if (!g.effectiveRenderState && !g.recordingStateBlock)
        return g.setRenderState(self, state, value);
    const auto index = static_cast<std::size_t>(state);

    const bool properCritical =
        g.properShadersDetected && IsProperShadersCriticalRenderState(state);
    if (properCritical)
        ++g.properCriticalRSCalls;

    if (g.effectiveRenderState && CanSuppress() &&
        !(g.properShadersSafetyMode && properCritical) &&
        index < g.renderStates.size() &&
        g.renderStateValid.test(index) &&
        g.renderStates[index] == value) {
        ++g.stats.renderStateSkipped;
        if (properCritical)
            ++g.properCriticalRSSkips;
        return D3D_OK;
    }

    const HRESULT hr = g.setRenderState(self, state, value);
    if (SUCCEEDED(hr) && index < g.renderStates.size()) {
        g.renderStates[index] = value;
        g.renderStateValid.set(index);
        if (g.recordingStateBlock) {
            g.recordingDelta.renderStates[index] = value;
            g.recordingDelta.renderStateValid.set(index);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetTextureHook(
    IDirect3DDevice9* self,
    DWORD stage,
    IDirect3DBaseTexture9* texture)
{
    if (self != g.device)
        return g.setTexture(self, stage, texture);

    SetterCostScope timed(1);
    ++g.stats.textureCalls;
    if (g.costRunning && g.costPassthrough && !g.recordingStateBlock)
        return g.setTexture(self, stage, texture);
    if (g.properShadersDetected && stage == 0 && g.properLastTexture0 != texture) {
        ++g.properTexture0Changes;
        g.properLastTexture0 = texture;
    }
    if (!g.effectiveTexture && !g.recordingStateBlock)
        return g.setTexture(self, stage, texture);

    if (CanSuppress() && stage < g.textures.size() &&
        (g.effectiveTexture && g.textureValid.test(stage) &&
         g.textures[stage] == texture)) {
        ++g.stats.textureSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setTexture(self, stage, texture);
    if (SUCCEEDED(hr) && stage < g.textures.size()) {
        g.textures[stage] = texture;
        g.textureValid.set(stage);
        if (g.recordingStateBlock) {
            g.recordingDelta.textures[stage] = texture;
            g.recordingDelta.textureValid.set(stage);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetSamplerStateHook(
    IDirect3DDevice9* self,
    DWORD sampler,
    D3DSAMPLERSTATETYPE type,
    DWORD value)
{
    if (self != g.device)
        return g.setSamplerState(self, sampler, type, value);

    SetterCostScope timed(2);
    ++g.stats.samplerStateCalls;
    if (g.costRunning && g.costPassthrough && !g.recordingStateBlock)
        return g.setSamplerState(self, sampler, type, value);
    if (!g.effectiveSamplerState && !g.recordingStateBlock)
        return g.setSamplerState(self, sampler, type, value);
    const auto t = static_cast<std::size_t>(type);

    if (g.effectiveSamplerState && CanSuppress() &&
        sampler < g.samplerStates.size() &&
        t < g.samplerStates[sampler].size() &&
        g.samplerValid[sampler].test(t) &&
        g.samplerStates[sampler][t] == value) {
        ++g.stats.samplerStateSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setSamplerState(self, sampler, type, value);
    if (SUCCEEDED(hr) && sampler < g.samplerStates.size() &&
        t < g.samplerStates[sampler].size()) {
        g.samplerStates[sampler][t] = value;
        g.samplerValid[sampler].set(t);
        if (g.recordingStateBlock) {
            g.recordingDelta.samplerStates[sampler][t] = value;
            g.recordingDelta.samplerValid[sampler].set(t);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetTextureStageStateHook(
    IDirect3DDevice9* self,
    DWORD stage,
    D3DTEXTURESTAGESTATETYPE type,
    DWORD value)
{
    if (self != g.device)
        return g.setTextureStageState(self, stage, type, value);

    SetterCostScope timed(3);
    ++g.stats.textureStageCalls;
    if (g.costRunning && g.costPassthrough && !g.recordingStateBlock)
        return g.setTextureStageState(self, stage, type, value);
    if (!g.effectiveTextureStageState && !g.recordingStateBlock)
        return g.setTextureStageState(self, stage, type, value);
    const auto t = static_cast<std::size_t>(type);

    if (g.effectiveTextureStageState && CanSuppress() &&
        stage < g.textureStageStates.size() &&
        t < g.textureStageStates[stage].size() &&
        g.textureStageValid[stage].test(t) &&
        g.textureStageStates[stage][t] == value) {
        ++g.stats.textureStageSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setTextureStageState(self, stage, type, value);
    if (SUCCEEDED(hr) && stage < g.textureStageStates.size() &&
        t < g.textureStageStates[stage].size()) {
        g.textureStageStates[stage][t] = value;
        g.textureStageValid[stage].set(t);
        if (g.recordingStateBlock) {
            g.recordingDelta.textureStageStates[stage][t] = value;
            g.recordingDelta.textureStageValid[stage].set(t);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetFVFHook(IDirect3DDevice9* self, DWORD fvf) {
    if (self != g.device)
        return g.setFVF(self, fvf);

    ++g.stats.fvfCalls;

    if (g.effectiveFVF && CanSuppress() &&
        g.fvfValid && g.fvf == fvf) {
        ++g.stats.fvfSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setFVF(self, fvf);
    if (SUCCEEDED(hr)) {
        g.fvf = fvf;
        g.fvfValid = true;
        // D3D9 treats FVF and vertex declarations as mutually exclusive.
        g.declarationValid = false;
        if (g.recordingStateBlock) {
            g.recordingDelta.fvf = fvf;
            g.recordingDelta.fvfValid = true;
            g.recordingDelta.declarationValid = false;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetVertexShaderHook(
    IDirect3DDevice9* self,
    IDirect3DVertexShader9* shader)
{
    if (self != g.device)
        return g.setVertexShader(self, shader);

    ++g.stats.vertexShaderCalls;
    if (g.skygfxBindingsActive) ++g.stats.skygfxShaderBindCalls;
    if (g.properShadersDetected && g.properLastVS != shader) {
        ++g.properVSSwitches;
        g.properLastVS = shader;
    }
    if (!g.effectiveVertexShader && !g.recordingStateBlock)
        return g.setVertexShader(self, shader);

    if (g.effectiveVertexShader && CanSuppress() &&
        g.vertexShaderValid && g.vertexShader == shader) {
        ++g.stats.vertexShaderSkipped;
        if (g.skygfxBindingsActive) ++g.stats.skygfxShaderBindSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setVertexShader(self, shader);
    if (SUCCEEDED(hr)) {
        g.vertexShader = shader;
        g.vertexShaderValid = true;
        if (g.recordingStateBlock) {
            g.recordingDelta.vertexShader = shader;
            g.recordingDelta.vertexShaderValid = true;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetPixelShaderHook(
    IDirect3DDevice9* self,
    IDirect3DPixelShader9* shader)
{
    if (self != g.device)
        return g.setPixelShader(self, shader);

    ++g.stats.pixelShaderCalls;
    if (g.skygfxBindingsActive) ++g.stats.skygfxShaderBindCalls;
    if (g.properShadersDetected && g.properLastPS != shader) {
        ++g.properPSSwitches;
        g.properLastPS = shader;
    }
    if (!g.effectivePixelShader && !g.recordingStateBlock)
        return g.setPixelShader(self, shader);

    if (g.effectivePixelShader && CanSuppress() &&
        g.pixelShaderValid && g.pixelShader == shader) {
        ++g.stats.pixelShaderSkipped;
        if (g.skygfxBindingsActive) ++g.stats.skygfxShaderBindSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setPixelShader(self, shader);
    if (SUCCEEDED(hr)) {
        g.pixelShader = shader;
        g.pixelShaderValid = true;
        if (g.recordingStateBlock) {
            g.recordingDelta.pixelShader = shader;
            g.recordingDelta.pixelShaderValid = true;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetVertexDeclarationHook(
    IDirect3DDevice9* self,
    IDirect3DVertexDeclaration9* declaration)
{
    if (self != g.device)
        return g.setVertexDeclaration(self, declaration);

    ++g.stats.declarationCalls;

    if (g.effectiveVertexDeclaration && CanSuppress() &&
        g.declarationValid && g.declaration == declaration) {
        ++g.stats.declarationSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setVertexDeclaration(self, declaration);
    if (SUCCEEDED(hr)) {
        g.declaration = declaration;
        g.declarationValid = true;
        g.fvfValid = false;
        if (g.recordingStateBlock) {
            g.recordingDelta.declaration = declaration;
            g.recordingDelta.declarationValid = true;
            g.recordingDelta.fvfValid = false;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetStreamSourceHook(
    IDirect3DDevice9* self,
    UINT stream,
    IDirect3DVertexBuffer9* buffer,
    UINT offset,
    UINT stride)
{
    if (self != g.device)
        return g.setStreamSource(self, stream, buffer, offset, stride);

    ++g.stats.streamSourceCalls;

    if (g.config.optimizeStreamSource && CanSuppress() &&
        stream < g.streams.size()) {
        const auto& cached = g.streams[stream];
        if ((cached.valid && cached.buffer == buffer &&
                 cached.offset == offset && cached.stride == stride)) {
            ++g.stats.streamSourceSkipped;
            return D3D_OK;
        }
    }

    const HRESULT hr = g.setStreamSource(self, stream, buffer, offset, stride);
    if (SUCCEEDED(hr) && stream < g.streams.size()) {
        auto& cached = g.streams[stream];
        cached.buffer = buffer;
        cached.offset = offset;
        cached.stride = stride;
        cached.valid = true;
    }
    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE SetStreamSourceFreqHook(
    IDirect3DDevice9* self,
    UINT stream,
    UINT setting)
{
    if (self != g.device)
        return g.setStreamSourceFreq(self, stream, setting);

    if (g.config.optimizeStreamSource && CanSuppress() &&
        stream < g.streamFreqs.size()) {
        const auto& cached = g.streamFreqs[stream];
        if (cached.valid && cached.setting == setting)
            return D3D_OK;
    }

    const HRESULT hr = g.setStreamSourceFreq(self, stream, setting);
    if (SUCCEEDED(hr) && stream < g.streamFreqs.size()) {
        g.streamFreqs[stream].setting = setting;
        g.streamFreqs[stream].valid = true;
    }
    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE SetIndicesHook(
    IDirect3DDevice9* self,
    IDirect3DIndexBuffer9* indices)
{
    if (self != g.device)
        return g.setIndices(self, indices);

    ++g.stats.indicesCalls;

    if (g.config.optimizeIndices && CanSuppress() &&
        (g.indicesValid && g.indices == indices)) {
        ++g.stats.indicesSkipped;
        return D3D_OK;
    }

    const HRESULT hr = g.setIndices(self, indices);
    if (SUCCEEDED(hr)) {
        g.indices = indices;
        g.indicesValid = true;
    }
    if (g.recordingStateBlock)
        g.recordingDelta.reconstructable = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE SetVertexShaderConstantFHook(
    IDirect3DDevice9* self,
    UINT startRegister,
    const float* data,
    UINT vector4Count)
{
    if (self != g.device)
        return g.setVertexShaderConstantF(self, startRegister, data, vector4Count);

    ++g.stats.vsConstantCalls;
    if (g.properShadersDetected)
        g.properVSConstantVectors += vector4Count;
    if (g.skygfxActive) g.stats.skygfxConstantVectors += vector4Count;
    if (!g.effectiveVSConstants && !g.recordingStateBlock)
        return g.setVertexShaderConstantF(self, startRegister, data, vector4Count);

    UINT uploadOffset = 0;
    UINT uploadCount = vector4Count;
    if (data && g.effectiveVSConstants && CanSuppress() &&
        vector4Count > 0 && startRegister <= g.vsConstants.size() &&
        vector4Count <= g.vsConstants.size() - startRegister) {
        while (uploadOffset < vector4Count && g.vsConstantValid.test(startRegister + uploadOffset) &&
            SameFloat4(g.vsConstants[startRegister + uploadOffset], data + uploadOffset * 4))
            ++uploadOffset;
        if (uploadOffset == vector4Count) {
            ++g.stats.vsConstantSkipped;
            if (g.skygfxActive) g.stats.skygfxConstantVectorsSaved += vector4Count;
            return D3D_OK;
        }
        if (g.skygfxActive) {
            UINT end = vector4Count;
            while (end > uploadOffset && g.vsConstantValid.test(startRegister + end - 1) &&
                SameFloat4(g.vsConstants[startRegister + end - 1], data + (end - 1) * 4))
                --end;
            uploadCount = end - uploadOffset;
        } else {
            uploadOffset = 0;
        }
    }

    const HRESULT hr = g.setVertexShaderConstantF(self, startRegister + uploadOffset,
        data ? data + uploadOffset * 4 : nullptr, uploadCount);
    if (SUCCEEDED(hr) && g.skygfxActive && uploadCount < vector4Count) {
        ++g.stats.vsConstantTrimmed;
        g.stats.skygfxConstantVectorsSaved += vector4Count - uploadCount;
    }
    if (SUCCEEDED(hr) && data &&
        startRegister <= g.vsConstants.size() &&
        vector4Count <= g.vsConstants.size() - startRegister) {
        for (UINT i = 0; i < vector4Count; ++i) {
            const UINT reg = startRegister + i;
            StoreFloat4(g.vsConstants[reg], data + i * 4);
            g.vsConstantValid.set(reg);
            if (g.recordingStateBlock) {
                StoreFloat4(g.recordingDelta.vsConstants[reg], data + i * 4);
                g.recordingDelta.vsConstantValid.set(reg);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SetPixelShaderConstantFHook(
    IDirect3DDevice9* self,
    UINT startRegister,
    const float* data,
    UINT vector4Count)
{
    if (self != g.device)
        return g.setPixelShaderConstantF(self, startRegister, data, vector4Count);

    ++g.stats.psConstantCalls;
    if (g.properShadersDetected)
        g.properPSConstantVectors += vector4Count;
    if (g.skygfxActive) g.stats.skygfxConstantVectors += vector4Count;
    if (!g.effectivePSConstants && !g.recordingStateBlock)
        return g.setPixelShaderConstantF(self, startRegister, data, vector4Count);

    UINT uploadOffset = 0;
    UINT uploadCount = vector4Count;
    if (data && g.effectivePSConstants && CanSuppress() &&
        vector4Count > 0 && startRegister <= g.psConstants.size() &&
        vector4Count <= g.psConstants.size() - startRegister) {
        while (uploadOffset < vector4Count && g.psConstantValid.test(startRegister + uploadOffset) &&
            SameFloat4(g.psConstants[startRegister + uploadOffset], data + uploadOffset * 4))
            ++uploadOffset;
        if (uploadOffset == vector4Count) {
            ++g.stats.psConstantSkipped;
            if (g.skygfxActive) g.stats.skygfxConstantVectorsSaved += vector4Count;
            return D3D_OK;
        }
        if (g.skygfxActive) {
            UINT end = vector4Count;
            while (end > uploadOffset && g.psConstantValid.test(startRegister + end - 1) &&
                SameFloat4(g.psConstants[startRegister + end - 1], data + (end - 1) * 4))
                --end;
            uploadCount = end - uploadOffset;
        } else {
            uploadOffset = 0;
        }
    }

    const HRESULT hr = g.setPixelShaderConstantF(self, startRegister + uploadOffset,
        data ? data + uploadOffset * 4 : nullptr, uploadCount);
    if (SUCCEEDED(hr) && g.skygfxActive && uploadCount < vector4Count) {
        ++g.stats.psConstantTrimmed;
        g.stats.skygfxConstantVectorsSaved += vector4Count - uploadCount;
    }
    if (SUCCEEDED(hr) && data &&
        startRegister <= g.psConstants.size() &&
        vector4Count <= g.psConstants.size() - startRegister) {
        for (UINT i = 0; i < vector4Count; ++i) {
            const UINT reg = startRegister + i;
            StoreFloat4(g.psConstants[reg], data + i * 4);
            g.psConstantValid.set(reg);
            if (g.recordingStateBlock) {
                StoreFloat4(g.recordingDelta.psConstants[reg], data + i * 4);
                g.recordingDelta.psConstantValid.set(reg);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateStateBlockHook(
    IDirect3DDevice9* self,
    D3DSTATEBLOCKTYPE type,
    IDirect3DStateBlock9** block)
{
    if (self != g.device)
        return g.createStateBlock(self, type, block);

    const HRESULT hr = g.createStateBlock(self, type, block);
    if (SUCCEEDED(hr) && block && *block)
        HookStateBlock(*block, nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BeginStateBlockHook(IDirect3DDevice9* self) {
    if (self != g.device)
        return g.beginStateBlock(self);

    const HRESULT hr = g.beginStateBlock(self);
    if (SUCCEEDED(hr)) {
        g.recordingDelta.Clear();
        g.recordingStateBlock = true;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE EndStateBlockHook(
    IDirect3DDevice9* self,
    IDirect3DStateBlock9** block)
{
    if (self != g.device)
        return g.endStateBlock(self, block);

    const HRESULT hr = g.endStateBlock(self, block);
    if (SUCCEEDED(hr)) {
        g.recordingStateBlock = false;
        if (block && *block)
            HookStateBlock(*block, &g.recordingDelta);
    }
    // Recorded setters need not change live device state. Never retain
    // their temporary values as the post-recording live cache. A failed
    // End leaves recording active, so suppression must stay disabled.
    InvalidateCache();
    return hr;
}









bool PatchAll() {
    bool ok = true;

#define PATCH(slot, hook, original) \
    do { if (!PatchDeviceSlot(slot, hook, original)) ok = false; } while (0)

    if (g.config.optimizeRenderTarget || g.config.optimizeViewport || g.config.optimizeScissorRect || g.skygfxProfileRequested)
        PATCH(SLOT_SET_RENDER_TARGET, &SetRenderTargetHook, g.setRenderTarget);
    if (g.config.optimizeDepthStencil)
        PATCH(SLOT_SET_DEPTH_STENCIL_SURFACE, &SetDepthStencilSurfaceHook, g.setDepthStencilSurface);
    if (g.config.optimizeTransform) {
        PATCH(SLOT_SET_TRANSFORM, &SetTransformHook, g.setTransform);
        PATCH(SLOT_MULTIPLY_TRANSFORM, &MultiplyTransformHook, g.multiplyTransform);
    }
    if (g.config.optimizeViewport)
        PATCH(SLOT_SET_VIEWPORT, &SetViewportHook, g.setViewport);
    if (g.config.optimizeMaterial)
        PATCH(SLOT_SET_MATERIAL, &SetMaterialHook, g.setMaterial);
    if (g.config.optimizeLight)
        PATCH(SLOT_SET_LIGHT, &SetLightHook, g.setLight);
    if (g.config.optimizeLightEnable)
        PATCH(SLOT_LIGHT_ENABLE, &LightEnableHook, g.lightEnable);
    if (g.config.optimizeClipPlane)
        PATCH(SLOT_SET_CLIP_PLANE, &SetClipPlaneHook, g.setClipPlane);
    if (g.config.optimizeScissorRect)
        PATCH(SLOT_SET_SCISSOR_RECT, &SetScissorRectHook, g.setScissorRect);
    if (g.config.optimizeNPatchMode)
        PATCH(SLOT_SET_NPATCH_MODE, &SetNPatchModeHook, g.setNPatchMode);

    if (g.config.optimizeRenderState)
        PATCH(SLOT_SET_RENDER_STATE, &SetRenderStateHook, g.setRenderState);
    if (g.config.optimizeTexture)
        PATCH(SLOT_SET_TEXTURE, &SetTextureHook, g.setTexture);
    if (g.config.optimizeTextureStageState)
        PATCH(SLOT_SET_TEXTURE_STAGE_STATE, &SetTextureStageStateHook, g.setTextureStageState);
    if (g.config.optimizeSamplerState)
        PATCH(SLOT_SET_SAMPLER_STATE, &SetSamplerStateHook, g.setSamplerState);
    if (g.config.optimizeVertexDeclaration || g.config.optimizeFVF)
        PATCH(SLOT_SET_VERTEX_DECLARATION, &SetVertexDeclarationHook, g.setVertexDeclaration);
    if (g.config.optimizeFVF || g.config.optimizeVertexDeclaration)
        PATCH(SLOT_SET_FVF, &SetFVFHook, g.setFVF);
    if (g.config.optimizeShaders || g.properShadersDetected || g.skygfxBindingsRequested)
        PATCH(SLOT_SET_VERTEX_SHADER, &SetVertexShaderHook, g.setVertexShader);
    if (g.config.optimizeShaderConstants || g.properShadersDetected || g.skygfxConstantsRequested)
        PATCH(SLOT_SET_VERTEX_SHADER_CONSTANT_F, &SetVertexShaderConstantFHook, g.setVertexShaderConstantF);
    if (g.config.optimizeStreamSource) {
        PATCH(SLOT_SET_STREAM_SOURCE, &SetStreamSourceHook, g.setStreamSource);
        PATCH(SLOT_SET_STREAM_SOURCE_FREQ, &SetStreamSourceFreqHook, g.setStreamSourceFreq);
    }
    if (g.config.optimizeIndices) {
        PATCH(SLOT_SET_INDICES, &SetIndicesHook, g.setIndices);
    }
    if (g.skygfxProfileRequested) {
        PATCH(SLOT_DRAW_PRIMITIVE, &DrawPrimitiveProfileHook, g.drawPrimitive);
        PATCH(SLOT_DRAW_INDEXED_PRIMITIVE, &DrawIndexedPrimitiveProfileHook, g.drawIndexedPrimitive);
        PATCH(SLOT_STRETCH_RECT, &StretchRectProfileHook, g.stretchRect);
    }
    if (g.config.optimizeStreamSource || g.skygfxProfileRequested)
        PATCH(SLOT_DRAW_PRIMITIVE_UP, &DrawPrimitiveUPHook, g.drawPrimitiveUP);
    if (g.config.optimizeStreamSource || g.config.optimizeIndices || g.skygfxProfileRequested)
        PATCH(SLOT_DRAW_INDEXED_PRIMITIVE_UP, &DrawIndexedPrimitiveUPHook, g.drawIndexedPrimitiveUP);
    if (g.config.optimizeShaders || g.properShadersDetected || g.skygfxBindingsRequested)
        PATCH(SLOT_SET_PIXEL_SHADER, &SetPixelShaderHook, g.setPixelShader);
    if (g.config.optimizeShaderConstants || g.properShadersDetected || g.skygfxConstantsRequested)
        PATCH(SLOT_SET_PIXEL_SHADER_CONSTANT_F, &SetPixelShaderConstantFHook, g.setPixelShaderConstantF);

    PATCH(SLOT_CREATE_STATE_BLOCK, &CreateStateBlockHook, g.createStateBlock);
    PATCH(SLOT_BEGIN_STATE_BLOCK, &BeginStateBlockHook, g.beginStateBlock);
    PATCH(SLOT_END_STATE_BLOCK, &EndStateBlockHook, g.endStateBlock);


#undef PATCH
    return ok;
}

void RestoreAll() {
    RestoreDeviceSlot(SLOT_DRAW_PRIMITIVE, &DrawPrimitiveProfileHook, g.drawPrimitive);
    RestoreDeviceSlot(SLOT_DRAW_INDEXED_PRIMITIVE, &DrawIndexedPrimitiveProfileHook, g.drawIndexedPrimitive);
    RestoreDeviceSlot(SLOT_STRETCH_RECT, &StretchRectProfileHook, g.stretchRect);
    RestoreDeviceSlot(SLOT_MULTIPLY_TRANSFORM, &MultiplyTransformHook, g.multiplyTransform);
    RestoreDeviceSlot(SLOT_DRAW_PRIMITIVE_UP, &DrawPrimitiveUPHook, g.drawPrimitiveUP);
    RestoreDeviceSlot(SLOT_DRAW_INDEXED_PRIMITIVE_UP, &DrawIndexedPrimitiveUPHook, g.drawIndexedPrimitiveUP);
    RestoreDeviceSlot(SLOT_SET_RENDER_TARGET, &SetRenderTargetHook, g.setRenderTarget);
    RestoreDeviceSlot(SLOT_SET_DEPTH_STENCIL_SURFACE, &SetDepthStencilSurfaceHook, g.setDepthStencilSurface);
    RestoreDeviceSlot(SLOT_SET_TRANSFORM, &SetTransformHook, g.setTransform);
    RestoreDeviceSlot(SLOT_SET_VIEWPORT, &SetViewportHook, g.setViewport);
    RestoreDeviceSlot(SLOT_SET_MATERIAL, &SetMaterialHook, g.setMaterial);
    RestoreDeviceSlot(SLOT_SET_LIGHT, &SetLightHook, g.setLight);
    RestoreDeviceSlot(SLOT_LIGHT_ENABLE, &LightEnableHook, g.lightEnable);
    RestoreDeviceSlot(SLOT_SET_CLIP_PLANE, &SetClipPlaneHook, g.setClipPlane);
    RestoreDeviceSlot(SLOT_SET_SCISSOR_RECT, &SetScissorRectHook, g.setScissorRect);
    RestoreDeviceSlot(SLOT_SET_NPATCH_MODE, &SetNPatchModeHook, g.setNPatchMode);

    RestoreDeviceSlot(SLOT_SET_RENDER_STATE, &SetRenderStateHook, g.setRenderState);
    RestoreDeviceSlot(SLOT_SET_TEXTURE, &SetTextureHook, g.setTexture);
    RestoreDeviceSlot(SLOT_SET_TEXTURE_STAGE_STATE, &SetTextureStageStateHook, g.setTextureStageState);
    RestoreDeviceSlot(SLOT_SET_SAMPLER_STATE, &SetSamplerStateHook, g.setSamplerState);
    RestoreDeviceSlot(SLOT_SET_VERTEX_DECLARATION, &SetVertexDeclarationHook, g.setVertexDeclaration);
    RestoreDeviceSlot(SLOT_SET_FVF, &SetFVFHook, g.setFVF);
    RestoreDeviceSlot(SLOT_SET_VERTEX_SHADER, &SetVertexShaderHook, g.setVertexShader);
    RestoreDeviceSlot(SLOT_SET_VERTEX_SHADER_CONSTANT_F, &SetVertexShaderConstantFHook, g.setVertexShaderConstantF);
    RestoreDeviceSlot(SLOT_SET_STREAM_SOURCE, &SetStreamSourceHook, g.setStreamSource);
    RestoreDeviceSlot(SLOT_SET_STREAM_SOURCE_FREQ, &SetStreamSourceFreqHook, g.setStreamSourceFreq);
    RestoreDeviceSlot(SLOT_SET_INDICES, &SetIndicesHook, g.setIndices);
    RestoreDeviceSlot(SLOT_SET_PIXEL_SHADER, &SetPixelShaderHook, g.setPixelShader);
    RestoreDeviceSlot(SLOT_SET_PIXEL_SHADER_CONSTANT_F, &SetPixelShaderConstantFHook, g.setPixelShaderConstantF);

    RestoreDeviceSlot(SLOT_CREATE_STATE_BLOCK, &CreateStateBlockHook, g.createStateBlock);
    RestoreDeviceSlot(SLOT_BEGIN_STATE_BLOCK, &BeginStateBlockHook, g.beginStateBlock);
    RestoreDeviceSlot(SLOT_END_STATE_BLOCK, &EndStateBlockHook, g.endStateBlock);


    for (std::size_t i = 0; i < g.stateBlockPatchCount; ++i) {
        const auto& patch = g.stateBlockPatches[i];
        if (!patch.vtable)
            continue;

        constexpr std::size_t releaseSlot = 2;
        constexpr std::size_t captureSlot = 4;
        constexpr std::size_t applySlot = 5;

        DWORD oldProtect = 0;
        if (!VirtualProtect(patch.vtable, sizeof(void*) * 6, PAGE_EXECUTE_READWRITE, &oldProtect))
            continue;

        if (patch.originalRelease &&
            patch.vtable[releaseSlot] == reinterpret_cast<void*>(&StateBlockReleaseHook)) {
            patch.vtable[releaseSlot] = reinterpret_cast<void*>(patch.originalRelease);
        }

        if (patch.originalCapture &&
            patch.vtable[captureSlot] == reinterpret_cast<void*>(&StateBlockCaptureHook)) {
            patch.vtable[captureSlot] = reinterpret_cast<void*>(patch.originalCapture);
        }

        if (patch.originalApply &&
            patch.vtable[applySlot] == reinterpret_cast<void*>(&StateBlockApplyHook)) {
            patch.vtable[applySlot] = reinterpret_cast<void*>(patch.originalApply);
        }

        DWORD unused = 0;
        VirtualProtect(patch.vtable, sizeof(void*) * 6, oldProtect, &unused);
        FlushInstructionCache(GetCurrentProcess(), patch.vtable, sizeof(void*) * 6);
    }
}

} // namespace

D3D9StateCache& D3D9StateCache::instance() {
    static D3D9StateCache instance;
    return instance;
}

bool D3D9StateCache::install(
    IDirect3DDevice9* device,
    const Config& config) {
    if (!device || !config.optimizationsEnabled || !config.d3dStateDeduplication)
        return false;
    if (ProperShadersCompatibility::loaded()) {
        if (g.installed) uninstall();
        ProperShadersCompatibility::report(config);
        return false;
    }

    if (g.installed && g.device == device)
        return true;

    if (g.installed)
        uninstall();

    g = Runtime{};

    g.config.enabled = true;
    g.config.d3d9Enabled = config.d3dStateDeduplication;
    g.config.productionMode = true;
    g.config.adaptiveEnabled = config.d3dStateAdaptive;
    g.config.resampleInterval = config.d3dStateAdaptiveResampleInterval;
    g.config.resampleWindow = config.d3dStateAdaptiveResampleWindow;
    g.config.adaptiveWarmupPresents = config.d3dStateAdaptiveWarmupPresents;
    g.config.adaptiveMinimumCalls = config.d3dStateAdaptiveMinimumCalls;
    g.config.adaptiveMaxPresents = config.d3dStateAdaptiveMaxPresents;
    g.config.adaptiveMinSkipPercent = config.d3dStateAdaptiveMinSkipPercent;
    g.costEnabled = config.d3dStateTimeSampling && config.d3dStateAdaptive;
    g.costWindow = config.d3dStateTimeWindow;
    g.costNext = config.d3dStateAdaptiveWarmupPresents + 60;
    QueryPerformanceFrequency(&g.costFrequency);

    g.config.optimizeRenderState = config.d3dOptimizeRenderState;
    g.config.optimizeTexture = config.d3dOptimizeTexture;
    g.config.optimizeSamplerState = config.d3dOptimizeSamplerState;
    g.config.optimizeTextureStageState = config.d3dOptimizeTextureStageState;
    g.config.optimizeFVF = config.d3dOptimizeFVF;
    g.baseShaders = config.d3dOptimizeShaders;
    g.skygfxBindingsRequested = config.skygfxShaderBindingOptimization && config.optimizationsEnabled;
    g.skygfxConstantsRequested = config.skygfxShaderConstantOptimization && config.optimizationsEnabled;
    g.skygfxProfileRequested = config.skygfxPassProfiling && config.optimizationsEnabled;
    g.skygfxAvailable = isSkygfxLoaded() && !ProperShadersLoaded();
    g.skygfxBindingsActive = g.skygfxAvailable && g.skygfxBindingsRequested;
    g.skygfxProfileActive = g.skygfxAvailable && g.skygfxProfileRequested;
    g.stats.skygfxShaderBindingsActive = g.skygfxBindingsActive;
    g.stats.skygfxPassProfilingActive = g.skygfxProfileActive;
    g.profileLogInterval = config.optimizationLogInterval;
    QueryPerformanceFrequency(&g.profileFrequency);
    g.config.optimizeShaders = g.baseShaders || g.skygfxBindingsActive;
    g.config.optimizeVertexDeclaration = config.d3dOptimizeVertexDeclaration;
    g.config.optimizeStreamSource = config.d3dOptimizeStreamSource;
    g.config.optimizeIndices = config.d3dOptimizeIndices;
    g.baseShaderConstants = config.d3dOptimizeShaderConstants;
    g.skygfxRequested = g.skygfxConstantsRequested || g.skygfxBindingsRequested || g.skygfxProfileRequested;
    g.skygfxActive = g.skygfxConstantsRequested && g.skygfxAvailable;
    g.stats.skygfxOptimizationActive = g.skygfxActive;
    g.config.optimizeShaderConstants = g.baseShaderConstants || g.skygfxActive;
    g.config.optimizeTransform = config.d3dOptimizeTransform;
    g.config.optimizeViewport = config.d3dOptimizeViewport;
    g.config.optimizeScissorRect = config.d3dOptimizeScissorRect;
    g.config.optimizeRenderTarget = config.d3dOptimizeRenderTarget;
    g.config.optimizeDepthStencil = config.d3dOptimizeDepthStencil;
    g.config.optimizeMaterial = config.d3dOptimizeMaterial;
    g.config.optimizeLight = config.d3dOptimizeLight;
    g.config.optimizeLightEnable = config.d3dOptimizeLightEnable;
    g.config.optimizeClipPlane = config.d3dOptimizeClipPlane;
    g.config.optimizeNPatchMode = config.d3dOptimizeNPatchMode;
    g.config.invalidateOnPresent = config.d3dStateInvalidateOnPresent;
    g.config.disableSuppressionOnMultithreadedDevice =
        config.d3dStateDisableOnMultithreadedDevice;
    g.config.logEveryNFrames = config.d3dStateStatsLogEveryNFrames;

    g.device = device;
    g.deviceVTable = *reinterpret_cast<void***>(device);

    if (!g.deviceVTable)
        return false;
    g.stateBlocks.reserve(Runtime::kMaxRecordedStateBlocks);

    D3DDEVICE_CREATION_PARAMETERS creation{};
    bool multithreaded = false;
    if (SUCCEEDED(device->GetCreationParameters(&creation))) {
        multithreaded =
            (creation.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    }

    g.effectiveRenderState = g.config.optimizeRenderState;
    g.effectiveTexture = g.config.optimizeTexture;
    g.effectiveSamplerState = g.config.optimizeSamplerState;
    g.effectiveTextureStageState = g.config.optimizeTextureStageState;
    g.effectiveFVF = g.config.optimizeFVF;
    g.effectiveVertexShader = g.config.optimizeShaders;
    g.effectivePixelShader = g.config.optimizeShaders;
    g.effectiveVertexDeclaration = g.config.optimizeVertexDeclaration;
    g.effectiveVSConstants = g.config.optimizeShaderConstants;
    g.effectivePSConstants = g.config.optimizeShaderConstants;
    g.effectiveTransform = g.config.optimizeTransform;
    g.effectiveViewport = g.config.optimizeViewport;
    g.effectiveScissorRect = g.config.optimizeScissorRect;
    g.effectiveRenderTarget = g.config.optimizeRenderTarget;
    g.effectiveDepthStencil = g.config.optimizeDepthStencil;
    g.effectiveMaterial = g.config.optimizeMaterial;
    g.effectiveLight = g.config.optimizeLight;
    g.effectiveLightEnable = g.config.optimizeLightEnable;
    g.effectiveClipPlane = g.config.optimizeClipPlane;
    g.effectiveNPatchMode = g.config.optimizeNPatchMode;
    g.stats.drawProfilingActive = false;

    g.adaptiveConfiguredMask = 0;
    if (g.config.optimizeRenderState)       g.adaptiveConfiguredMask |= ADAPT_RS;
    if (g.config.optimizeTexture)           g.adaptiveConfiguredMask |= ADAPT_TEX;
    if (g.config.optimizeSamplerState)      g.adaptiveConfiguredMask |= ADAPT_SS;
    if (g.config.optimizeTextureStageState) g.adaptiveConfiguredMask |= ADAPT_TSS;
    if (g.config.optimizeFVF)               g.adaptiveConfiguredMask |= ADAPT_FVF;
    if (g.config.optimizeShaders)           g.adaptiveConfiguredMask |= ADAPT_VS | ADAPT_PS;
    if (g.config.optimizeVertexDeclaration) g.adaptiveConfiguredMask |= ADAPT_DECL;
    if (g.config.optimizeShaderConstants)   g.adaptiveConfiguredMask |= ADAPT_VSC | ADAPT_PSC;
    if (g.config.optimizeTransform)         g.adaptiveConfiguredMask |= ADAPT_TRANSFORM;
    if (g.config.optimizeViewport)          g.adaptiveConfiguredMask |= ADAPT_VIEWPORT;
    if (g.config.optimizeScissorRect)       g.adaptiveConfiguredMask |= ADAPT_SCISSOR;
    if (g.config.optimizeRenderTarget)      g.adaptiveConfiguredMask |= ADAPT_RT;
    if (g.config.optimizeDepthStencil)      g.adaptiveConfiguredMask |= ADAPT_DS;
    if (g.config.optimizeMaterial)          g.adaptiveConfiguredMask |= ADAPT_MATERIAL;
    if (g.config.optimizeLight)             g.adaptiveConfiguredMask |= ADAPT_LIGHT;
    if (g.config.optimizeLightEnable)       g.adaptiveConfiguredMask |= ADAPT_LIGHT_ENABLE;
    if (g.config.optimizeClipPlane)         g.adaptiveConfiguredMask |= ADAPT_CLIP;
    if (g.config.optimizeNPatchMode)        g.adaptiveConfiguredMask |= ADAPT_NPATCH;

    constexpr std::uint32_t allAdaptive =
        ADAPT_RS | ADAPT_TEX | ADAPT_SS | ADAPT_TSS | ADAPT_FVF |
        ADAPT_VS | ADAPT_PS | ADAPT_DECL | ADAPT_VSC | ADAPT_PSC |
        ADAPT_TRANSFORM | ADAPT_VIEWPORT | ADAPT_SCISSOR | ADAPT_RT |
        ADAPT_DS | ADAPT_MATERIAL | ADAPT_LIGHT | ADAPT_LIGHT_ENABLE |
        ADAPT_CLIP | ADAPT_NPATCH;

    g.adaptiveEvaluatedMask =
        (~g.adaptiveConfiguredMask) & allAdaptive;
    g.stats.adaptiveEvaluatedMask =
        g.adaptiveEvaluatedMask;

    g.properShadersDetected = ProperShadersLoaded();
    g.properShadersSafetyMode = g.properShadersDetected;
    if (g.properShadersDetected) {
        Log(
            "Proper Shaders compatibility profiler ACTIVE: module detected; "
            "high-risk multipass state suppression is bypassed while telemetry "
            "tracks shaders, constants, RT/depth, transforms, viewport/scissor, "
            "textures, critical render states, and GTA area.");
        Log(
            "Proper Shaders safety mode: RT/DS/WORLD/viewport/scissor and "
            "critical depth/blend/stencil/color-write render states pass through.");
    }

    g.suppressionEnabled = true;
    if (multithreaded &&
        g.config.disableSuppressionOnMultithreadedDevice) {
        g.suppressionEnabled = false;
        Log(
            "Integrated visibility optimizer: D3DCREATE_MULTITHREADED "
            "detected; cache hooks disabled for correctness.");
        // Disabling suppression alone still races on cache arrays/maps.
        // Avoid installing the unsynchronized cache on this device.
        g = Runtime{};
        return false;
    }

    if (!PatchAll()) {
        Log(
            "Integrated visibility optimizer: D3D9 hook installation "
            "failed; rolling back.");
        RestoreAll();
        g = Runtime{};
        return false;
    }

    g.installed = true;
    if (g.skygfxBindingsActive) {
        Log("SkyGFX shader-binding patch ACTIVE: identical known VS/PS bindings are suppressed; state blocks and resets remain authoritative.");
        Diagnostics::instance().active("Optimization.SkygfxShaderBindings", "loaded SkyGFX; shader-binding suppression active");
    }
    if (g.skygfxActive) {
        Log("SkyGFX shader-constant patch ACTIVE: installed SanFrame VS/PS constant hooks; redundant uploads and unchanged upload edges are removed.");
        Diagnostics::instance().active("Optimization.SkygfxConstants", "loaded SkyGFX; shader constant upload patch active");
    }
    InvalidateCache();

    Log(
        "Integrated SAVisibilityOptimizer v17 core ACTIVE: "
        "single-owner D3D9 state suppression, adaptive=%d, "
        "invalidate-on-present=%d.",
        g.config.adaptiveEnabled ? 1 : 0,
        g.config.invalidateOnPresent ? 1 : 0);

    return true;
}

void D3D9StateCache::uninstall() {
    if (!g.installed)
        return;

    ReleaseProfilePrimary();
    RestoreAll();
    g = Runtime{};
}

void D3D9StateCache::invalidate() noexcept {
    if (g.installed)
        InvalidateCache();
}

void D3D9StateCache::onPresent() noexcept {
    if (!g.installed)
        return;

    ++g.stats.presents;
    RefreshSkygfx();
    EvaluateAdaptivePolicy();
    EvaluateSetterCost();
    // Count-based resampling may re-enable a category, but an A/B loss is
    // authoritative until the next paired time experiment.
    if (!g.costRunning) {
        g.effectiveRenderState &= !(g.costDisabledMask & ADAPT_RS);
        g.effectiveTexture &= !(g.costDisabledMask & ADAPT_TEX);
        g.effectiveSamplerState &= !(g.costDisabledMask & ADAPT_SS);
        g.effectiveTextureStageState &= !(g.costDisabledMask & ADAPT_TSS);
    }
    LogSkygfxProfile();

    if (g.config.invalidateOnPresent)
        InvalidateCache();

    if (g.properShadersDetected &&
        (g.stats.presents - g.properProfileLastPresent) >= 120) {
        const auto& b = g.properProfileBaseline;
        const int area = ReadGtaCurrentArea();
        std::ostringstream ps;
        ps << "ProperShaders profile: frames="
           << (g.stats.presents - g.properProfileLastPresent)
           << " area=" << area << (area == 0 ? "(exterior)" : "(interior/other)")
           << " RS=" << (g.stats.renderStateCalls - b.renderStateCalls)
           << " skip=" << (g.stats.renderStateSkipped - b.renderStateSkipped)
           << " criticalRS=" << g.properCriticalRSCalls
           << " criticalSkip=" << g.properCriticalRSSkips
           << " TEX=" << (g.stats.textureCalls - b.textureCalls)
           << " tex0Changes=" << g.properTexture0Changes
           << " SS=" << (g.stats.samplerStateCalls - b.samplerStateCalls)
           << " TSS=" << (g.stats.textureStageCalls - b.textureStageCalls)
           << " VSbind=" << (g.stats.vertexShaderCalls - b.vertexShaderCalls)
           << " VSswitch=" << g.properVSSwitches
           << " PSbind=" << (g.stats.pixelShaderCalls - b.pixelShaderCalls)
           << " PSswitch=" << g.properPSSwitches
           << " VSC=" << (g.stats.vsConstantCalls - b.vsConstantCalls)
           << " VSCvec=" << g.properVSConstantVectors
           << " PSC=" << (g.stats.psConstantCalls - b.psConstantCalls)
           << " PSCvec=" << g.properPSConstantVectors
           << " RT=" << (g.stats.renderTargetCalls - b.renderTargetCalls)
           << " RTchanges=" << g.properRTChanges
           << " DS=" << (g.stats.depthStencilCalls - b.depthStencilCalls)
           << " DSchanges=" << g.properDSChanges
           << " WORLD=" << (g.stats.transformCalls - b.transformCalls)
           << " worldChanges=" << g.properWorldChanges
           << " VP=" << (g.stats.viewportCalls - b.viewportCalls)
           << " VPchanges=" << g.properViewportChanges
           << " SC=" << (g.stats.scissorCalls - b.scissorCalls)
           << " SCchanges=" << g.properScissorChanges
           << " stateBlockApply="
           << (g.stats.stateBlockApplies - b.stateBlockApplies)
           << " safety=high-risk-pass-through";
        mws::Logger::instance().info(ps.str());

        g.properProfileLastPresent = g.stats.presents;
        g.properProfileBaseline = g.stats;
        g.properCriticalRSCalls = 0;
        g.properCriticalRSSkips = 0;
        g.properVSSwitches = 0;
        g.properPSSwitches = 0;
        g.properVSConstantVectors = 0;
        g.properPSConstantVectors = 0;
        g.properRTChanges = 0;
        g.properDSChanges = 0;
        g.properWorldChanges = 0;
        g.properViewportChanges = 0;
        g.properScissorChanges = 0;
        g.properTexture0Changes = 0;
    }

    if (g.config.logEveryNFrames > 0 &&
        (g.stats.presents % g.config.logEveryNFrames) == 0) {
        std::ostringstream os;
        os << "D3D9 optimizer stats: presents=" << g.stats.presents
           << " RS=" << g.stats.renderStateSkipped << "/" << g.stats.renderStateCalls
           << " TEX=" << g.stats.textureSkipped << "/" << g.stats.textureCalls
           << " SS=" << g.stats.samplerStateSkipped << "/" << g.stats.samplerStateCalls
           << " TSS=" << g.stats.textureStageSkipped << "/" << g.stats.textureStageCalls
           << " VS=" << g.stats.vertexShaderSkipped << "/" << g.stats.vertexShaderCalls
           << " PS=" << g.stats.pixelShaderSkipped << "/" << g.stats.pixelShaderCalls
           << " VSC=" << g.stats.vsConstantSkipped << "/" << g.stats.vsConstantCalls
           << " PSC=" << g.stats.psConstantSkipped << "/" << g.stats.psConstantCalls
           << " SkyGFXvectorsSaved=" << g.stats.skygfxConstantVectorsSaved
           << "/" << g.stats.skygfxConstantVectors
           << " RT=" << g.stats.renderTargetSkipped << "/" << g.stats.renderTargetCalls
           << " VP=" << g.stats.viewportSkipped << "/" << g.stats.viewportCalls
           << " stateBlocks=" << std::dec << g.stateBlocks.size()
           << "/" << Runtime::kMaxRecordedStateBlocks
           << " peakStateBlocks=" << g.peakRecordedStateBlocks
           << " stateBlockVTables=" << g.stateBlockPatchCount
           << "/" << Runtime::kMaxStateBlockVTables
           << " adaptiveDisabledMask=0x" << std::hex << g.stats.adaptiveDisabledMask;
        mws::Logger::instance().info(os.str());
    }
}

void D3D9StateCache::onReset() noexcept {
    if (!g.installed)
        return;

    ++g.stats.resets;
    ReleaseProfilePrimary();
    // A reset can accompany a major workload change. Sample again on the
    // next Present instead of waiting for the normal cooldown.
    if (g.adaptiveEvaluated)
        g.nextSample = g.stats.presents;
    g.recordingStateBlock = false;
    g.recordingDelta.reconstructable = false;
    InvalidateCache();
}

bool D3D9StateCache::installed() const noexcept {
    return g.installed;
}

bool D3D9StateCache::suppressionEnabled() const noexcept {
    return g.installed && g.suppressionEnabled;
}

const D3D9StateCacheStats& D3D9StateCache::stats() const noexcept {
    return g.stats;
}

void appendStateOptimizerReport(OptimizationReport::Rows& rows, const Config& c) {
  rows.push_back({"state-calls", "SkygfxShaderBindingOptimization", "SkygfxShaderBindings", "calls", g.stats.skygfxShaderBindCalls, g.stats.skygfxShaderBindSkipped, c.d3dStateDeduplication && c.skygfxShaderBindingOptimization, g.installed && g.suppressionEnabled && g.skygfxBindingsActive, !(g.effectiveVertexShader || g.effectivePixelShader)});
  rows.push_back({"constant-vectors", "SkygfxShaderConstantOptimization", "SkygfxConstants", "float4-vectors", g.stats.skygfxConstantVectors, g.stats.skygfxConstantVectorsSaved, c.d3dStateDeduplication && c.skygfxShaderConstantOptimization, g.installed && g.suppressionEnabled && g.skygfxActive, !(g.effectiveVSConstants || g.effectivePSConstants)});
  rows.push_back({"state-calls", "D3DOptimizeRenderState", "RenderState", "calls", g.stats.renderStateCalls, g.stats.renderStateSkipped, c.d3dStateDeduplication && c.d3dOptimizeRenderState, g.installed && g.suppressionEnabled, !(g.effectiveRenderState)});
  rows.push_back({"state-calls", "D3DOptimizeTexture", "Texture", "calls", g.stats.textureCalls, g.stats.textureSkipped, c.d3dStateDeduplication && c.d3dOptimizeTexture, g.installed && g.suppressionEnabled, !(g.effectiveTexture)});
  rows.push_back({"state-calls", "D3DOptimizeSamplerState", "SamplerState", "calls", g.stats.samplerStateCalls, g.stats.samplerStateSkipped, c.d3dStateDeduplication && c.d3dOptimizeSamplerState, g.installed && g.suppressionEnabled, !(g.effectiveSamplerState)});
  rows.push_back({"state-calls", "D3DOptimizeTextureStageState", "TextureStageState", "calls", g.stats.textureStageCalls, g.stats.textureStageSkipped, c.d3dStateDeduplication && c.d3dOptimizeTextureStageState, g.installed && g.suppressionEnabled, !(g.effectiveTextureStageState)});
  rows.push_back({"state-calls", "D3DOptimizeFVF", "FVF", "calls", g.stats.fvfCalls, g.stats.fvfSkipped, c.d3dStateDeduplication && c.d3dOptimizeFVF, g.installed && g.suppressionEnabled, !(g.effectiveFVF)});
  rows.push_back({"state-calls", "D3DOptimizeShaders", "VertexShader", "calls", g.stats.vertexShaderCalls, g.stats.vertexShaderSkipped, c.d3dStateDeduplication && c.d3dOptimizeShaders, g.installed && g.suppressionEnabled, !(g.effectiveVertexShader)});
  rows.push_back({"state-calls", "D3DOptimizeShaders", "PixelShader", "calls", g.stats.pixelShaderCalls, g.stats.pixelShaderSkipped, c.d3dStateDeduplication && c.d3dOptimizeShaders, g.installed && g.suppressionEnabled, !(g.effectivePixelShader)});
  rows.push_back({"state-calls", "D3DOptimizeVertexDeclaration", "VertexDeclaration", "calls", g.stats.declarationCalls, g.stats.declarationSkipped, c.d3dStateDeduplication && c.d3dOptimizeVertexDeclaration, g.installed && g.suppressionEnabled, !(g.effectiveVertexDeclaration)});
  rows.push_back({"state-calls", "D3DOptimizeStreamSource", "StreamSource", "calls", g.stats.streamSourceCalls, g.stats.streamSourceSkipped, c.d3dStateDeduplication && c.d3dOptimizeStreamSource, g.installed && g.suppressionEnabled, !(g.config.optimizeStreamSource)});
  rows.push_back({"state-calls", "D3DOptimizeIndices", "Indices", "calls", g.stats.indicesCalls, g.stats.indicesSkipped, c.d3dStateDeduplication && c.d3dOptimizeIndices, g.installed && g.suppressionEnabled, !(g.config.optimizeIndices)});
  rows.push_back({"state-calls", "D3DOptimizeShaderConstants", "VSConstants", "calls", g.stats.vsConstantCalls, g.stats.vsConstantSkipped, c.d3dStateDeduplication && c.d3dOptimizeShaderConstants, g.installed && g.suppressionEnabled, !(g.effectiveVSConstants)});
  rows.push_back({"state-calls", "D3DOptimizeShaderConstants", "PSConstants", "calls", g.stats.psConstantCalls, g.stats.psConstantSkipped, c.d3dStateDeduplication && c.d3dOptimizeShaderConstants, g.installed && g.suppressionEnabled, !(g.effectivePSConstants)});
  rows.push_back({"state-calls", "D3DOptimizeTransform", "Transform", "calls", g.stats.transformCalls, g.stats.transformSkipped, c.d3dStateDeduplication && c.d3dOptimizeTransform, g.installed && g.suppressionEnabled, !(g.effectiveTransform)});
  rows.push_back({"state-calls", "D3DOptimizeViewport", "Viewport", "calls", g.stats.viewportCalls, g.stats.viewportSkipped, c.d3dStateDeduplication && c.d3dOptimizeViewport, g.installed && g.suppressionEnabled, !(g.effectiveViewport)});
  rows.push_back({"state-calls", "D3DOptimizeScissorRect", "ScissorRect", "calls", g.stats.scissorCalls, g.stats.scissorSkipped, c.d3dStateDeduplication && c.d3dOptimizeScissorRect, g.installed && g.suppressionEnabled, !(g.effectiveScissorRect)});
  rows.push_back({"state-calls", "D3DOptimizeRenderTarget", "RenderTarget", "calls", g.stats.renderTargetCalls, g.stats.renderTargetSkipped, c.d3dStateDeduplication && c.d3dOptimizeRenderTarget, g.installed && g.suppressionEnabled, !(g.effectiveRenderTarget)});
  rows.push_back({"state-calls", "D3DOptimizeDepthStencil", "DepthStencil", "calls", g.stats.depthStencilCalls, g.stats.depthStencilSkipped, c.d3dStateDeduplication && c.d3dOptimizeDepthStencil, g.installed && g.suppressionEnabled, !(g.effectiveDepthStencil)});
  rows.push_back({"state-calls", "D3DOptimizeMaterial", "Material", "calls", g.stats.materialCalls, g.stats.materialSkipped, c.d3dStateDeduplication && c.d3dOptimizeMaterial, g.installed && g.suppressionEnabled, !(g.effectiveMaterial)});
  rows.push_back({"state-calls", "D3DOptimizeLight", "Light", "calls", g.stats.lightCalls, g.stats.lightSkipped, c.d3dStateDeduplication && c.d3dOptimizeLight, g.installed && g.suppressionEnabled, !(g.effectiveLight)});
  rows.push_back({"state-calls", "D3DOptimizeLightEnable", "LightEnable", "calls", g.stats.lightEnableCalls, g.stats.lightEnableSkipped, c.d3dStateDeduplication && c.d3dOptimizeLightEnable, g.installed && g.suppressionEnabled, !(g.effectiveLightEnable)});
  rows.push_back({"state-calls", "D3DOptimizeClipPlane", "ClipPlane", "calls", g.stats.clipPlaneCalls, g.stats.clipPlaneSkipped, c.d3dStateDeduplication && c.d3dOptimizeClipPlane, g.installed && g.suppressionEnabled, !(g.effectiveClipPlane)});
  rows.push_back({"state-calls", "D3DOptimizeNPatchMode", "NPatchMode", "calls", g.stats.nPatchCalls, g.stats.nPatchSkipped, c.d3dStateDeduplication && c.d3dOptimizeNPatchMode, g.installed && g.suppressionEnabled, !(g.effectiveNPatchMode)});
}

} // namespace mws
