#pragma once
#include "mws/Config.hpp"
#include <cstdint>
#include <d3d9.h>

namespace mws {

struct D3D9StateCacheStats {
    std::uint64_t presents = 0;
    std::uint64_t resets = 0;
    std::uint64_t cacheInvalidations = 0;
    std::uint64_t stateBlockApplies = 0;
    std::uint64_t stateBlockDeltaApplies = 0;
    std::uint64_t stateBlockOpaqueApplies = 0;
    std::uint64_t stateBlockCaptures = 0;
    std::uint64_t stateBlockCaptureDowngrades = 0;

    std::uint64_t renderStateCalls = 0;
    std::uint64_t renderStateSkipped = 0;

    std::uint64_t textureCalls = 0;
    std::uint64_t textureSkipped = 0;

    std::uint64_t samplerStateCalls = 0;
    std::uint64_t samplerStateSkipped = 0;

    std::uint64_t textureStageCalls = 0;
    std::uint64_t textureStageSkipped = 0;

    std::uint64_t fvfCalls = 0;
    std::uint64_t fvfSkipped = 0;

    std::uint64_t vertexShaderCalls = 0;
    std::uint64_t vertexShaderSkipped = 0;

    std::uint64_t pixelShaderCalls = 0;
    std::uint64_t pixelShaderSkipped = 0;

    std::uint64_t declarationCalls = 0;
    std::uint64_t declarationSkipped = 0;

    std::uint64_t streamSourceCalls = 0;
    std::uint64_t streamSourceSkipped = 0;

    std::uint64_t indicesCalls = 0;
    std::uint64_t indicesSkipped = 0;

    std::uint64_t vsConstantCalls = 0;
    std::uint64_t vsConstantSkipped = 0;

    std::uint64_t psConstantCalls = 0;
    std::uint64_t psConstantSkipped = 0;

    std::uint64_t vsConstantTrimmed = 0;
    std::uint64_t psConstantTrimmed = 0;
    std::uint64_t skygfxConstantVectors = 0;
    std::uint64_t skygfxConstantVectorsSaved = 0;
    bool skygfxOptimizationActive = false;
    bool skygfxShaderBindingsActive = false;
    bool skygfxPassProfilingActive = false;
    std::uint64_t skygfxShaderBindCalls = 0;
    std::uint64_t skygfxShaderBindSkipped = 0;
    std::uint64_t skygfxMainDraws = 0, skygfxOffscreenDraws = 0, skygfxUnknownDraws = 0;
    std::uint64_t skygfxCopies = 0, skygfxProfileFailures = 0;
    double skygfxMainSubmitMs = 0.0, skygfxOffscreenSubmitMs = 0.0;
    double skygfxUnknownSubmitMs = 0.0, skygfxCopySubmitMs = 0.0;

    std::uint64_t transformCalls = 0;
    std::uint64_t transformSkipped = 0;

    std::uint64_t viewportCalls = 0;
    std::uint64_t viewportSkipped = 0;

    std::uint64_t scissorCalls = 0;
    std::uint64_t scissorSkipped = 0;

    std::uint64_t renderTargetCalls = 0;
    std::uint64_t renderTargetSkipped = 0;

    std::uint64_t depthStencilCalls = 0;
    std::uint64_t depthStencilSkipped = 0;

    std::uint64_t materialCalls = 0;
    std::uint64_t materialSkipped = 0;

    std::uint64_t lightCalls = 0;
    std::uint64_t lightSkipped = 0;

    std::uint64_t lightEnableCalls = 0;
    std::uint64_t lightEnableSkipped = 0;

    std::uint64_t clipPlaneCalls = 0;
    std::uint64_t clipPlaneSkipped = 0;

    std::uint64_t nPatchCalls = 0;
    std::uint64_t nPatchSkipped = 0;

    std::uint64_t drawCalls = 0;
    std::uint64_t primitives = 0;

    bool adaptiveCompleted = false;
    bool drawProfilingActive = true;
    std::uint32_t adaptiveDisabledMask = 0;
    std::uint32_t adaptiveEvaluatedMask = 0;
};

class D3D9StateCache {
public:
  static D3D9StateCache& instance();

  bool install(IDirect3DDevice9* device, const Config& config);
  void uninstall();
  void invalidate() noexcept;
  void onPresent() noexcept;
  void onReset() noexcept;

  bool installed() const noexcept;
  bool suppressionEnabled() const noexcept;
  const D3D9StateCacheStats& stats() const noexcept;

private:
  D3D9StateCache() = default;
};

} // namespace mws
