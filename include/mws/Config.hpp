#pragma once
#include <filesystem>
#include <string>

namespace mws {

enum class VSyncMode {
  Game,
  On,
  Off
};

enum class FrameLimitPoint {
  BeforePresent,
  AfterPresent
};

enum class LatencyProfile {
  Balanced,
  LowLatency,
  Smooth
};

enum class VrrMode {
  Off,
  Auto,
  On
};

enum class D3D9ExMode {
  Off,
  Auto,
  Force
};

enum class OverlayScaleMode {
  Auto,
  Manual,
  DpiOnly,
  ResolutionOnly
};

enum class OverlayTextEffect {
  None,
  Shadow,
  Outline
};

enum class OverlayAlignment {
  TopLeft,
  TopCenter,
  TopRight,
  BottomLeft,
  BottomCenter,
  BottomRight
};

struct Config {
  bool enabled{true};

  VSyncMode vsync{VSyncMode::Game};

  // Master switch for renderer, pacing and backend tuning.
  bool optimizationsEnabled{true};
  bool d3dStateDeduplication{true};

  // Integrated SAVisibilityOptimizer v17 production policy.
  // SanFrame is the sole D3D9 hook owner; no second optimizer ASI is required.
  bool d3dStateAdaptive{true};
  unsigned d3dStateAdaptiveWarmupPresents{1800};
  unsigned d3dStateAdaptiveMinimumCalls{50000};
  unsigned d3dStateAdaptiveMaxPresents{12000};
  unsigned d3dStateAdaptiveMinSkipPercent{5};
  bool d3dStateInvalidateOnPresent{true};
  bool d3dStateDisableOnMultithreadedDevice{true};
  unsigned d3dStateStatsLogEveryNFrames{0};
  unsigned d3dStateAdaptiveResampleInterval{3600};
  unsigned d3dStateAdaptiveResampleWindow{300};
  // Explicit opt-in A/B sampled CPU setter-time measurements. Compared
  // separately for each new D3D9 device/backend; never GPU-time estimates.
  bool d3dStateTimeSampling{false};
  unsigned d3dStateTimeWindow{300};
  bool renderWareBatching{true};
  bool renderWareDayNightBatching{true};
  bool renderWarePlanCache{true};
  bool immediateBatching{true};
  bool staticPropInstancing{true};
  // Structural and CPU-cost gated adaptive bypass; periodically probes again.
  bool adaptiveBatching{true};
  unsigned adaptiveBatchWindow{600};
  unsigned adaptiveBatchProbe{240};
  bool visibilityBoundsCache{true};
  unsigned optimizationLogInterval{0};
  bool passTiming{false};

  bool d3dOptimizeRenderState{true};
  bool d3dOptimizeTexture{true};
  bool d3dOptimizeSamplerState{true};
  bool d3dOptimizeTextureStageState{true};
  bool d3dOptimizeFVF{true};
  bool d3dOptimizeShaders{false};
  bool d3dOptimizeVertexDeclaration{true};
  bool d3dOptimizeStreamSource{false};
  bool d3dOptimizeIndices{false};
  bool d3dOptimizeShaderConstants{false};
  // Auto-activate constant suppression/partial upload trimming for loaded SkyGFX.
  bool skygfxShaderConstantOptimization{true};
  bool skygfxShaderBindingOptimization{true};
  bool skygfxPassProfiling{false};
  bool d3dOptimizeTransform{true};
  bool d3dOptimizeViewport{true};
  bool d3dOptimizeScissorRect{true};
  bool d3dOptimizeRenderTarget{true};
  bool d3dOptimizeDepthStencil{true};
  bool d3dOptimizeMaterial{true};
  bool d3dOptimizeLight{true};
  bool d3dOptimizeLightEnable{true};
  bool d3dOptimizeClipPlane{true};
  bool d3dOptimizeNPatchMode{true};

  bool workerPreparation{false};
  int workerThreads{1};
  bool sanFrameTransientArena{false};

  // Integrated FXP runtime; retains authored particle counts and update rate.
  bool fxpEnabled{true};
  bool fxpReplaceSimulation{true};
  bool fxpCompiledCurves{true};
  bool fxpMatrixCache{true};
  bool fxpInstancedSubmission{false};
  bool fxpOperatorCache{false};
  bool fxpOperatorPlans{false};
  bool alphaListIndex{false};
  bool shadowGroupIndex{false};
  bool fxpProfile{false};
  unsigned fxpStatsIntervalSeconds{0};

  // Frame pacing / latency.
  bool frameLimiterEnabled{false};
  double targetFps{60.0};
  double backgroundFps{30.0};
  FrameLimitPoint frameLimitPoint{FrameLimitPoint::BeforePresent};
  LatencyProfile latencyProfile{LatencyProfile::LowLatency};
  bool useHighResolutionTimer{true};
  bool useMmcss{true};
  bool useTimerResolution1ms{false};
  unsigned busyWaitMicroseconds{350};
  unsigned maxCatchUpFrames{2};
  bool enableFrameStats{false};
  unsigned frameStatsIntervalMs{5000};

  // Unified CPU/GPU queue depth policy. Applied through D3D9Ex when available
  // and mirrored to DXVK's d3d9.maxFrameLatency before the wrapper loads.
  unsigned maxRenderAheadFrames{1};

  // Refresh-aware cap policy. D3D9 has no portable API that reports whether a
  // monitor is actually VRR-capable, so Auto is deliberately conservative:
  // it only applies the below-refresh cap when VSync is explicitly Off.
  VrrMode vrrMode{VrrMode::Auto};
  double vrrRefreshMarginHz{3.0};
  double vrrMinimumRefreshHz{75.0};

  // D3D9Ex backend. Auto attempts Direct3DCreate9Ex/CreateDeviceEx and falls
  // back to classic D3D9 on any failure. Force does not silently fall back at
  // device creation. Off preserves the original Direct3D9 path.
  D3D9ExMode d3d9ExMode{D3D9ExMode::Off};

  // Transition-only device-loss diagnostics (no per-frame log spam).
  bool deviceLossTelemetry{true};
  unsigned deviceStatePollMs{250};

  // DXVK integration. Applied only when the game's local d3d9.dll is
  // positively identified as stock DXVK before it is loaded. ReFX DXVK is excluded.
  bool dxvkAutoTune{true};
  bool dxvkProperShadersResetWorkaround{true};
  int dxvkMaxFrameLatency{-1}; // -1 = use MaxRenderAheadFrames
  bool dxvkDisableInternalLimiterWhenSanFrameLimiter{true};
  bool dxvkDisableLatencySleepWhenSanFrameLimiter{true};
  bool dxvkMatchVSyncPolicy{true};
  bool dxvkUseAllCompilerThreads{false}; // false = leave DXVK default
  bool dxvkForceDeviceLocalConstantBuffers{false};
  bool dxvkDisableFileLogging{false};
  bool dxvkUseLocalShaderCache{false};

  // Force Present-only OSD when another ASI owns a device EndScene detour.
  // DistantStreaming.asi automatically selects this path when loaded.
  bool overlayPresentOnly{false};

  // In-game diagnostics HUD / OSD.
  bool overlayEnabled{true};
  bool overlayShowApi{true};
  bool overlayShowTitle{true};
  bool overlayShowGpu{true};
  bool overlayShowCpuUsage{false};
  bool overlayShowGpuUsage{false};
  bool overlayShowGpuMemory{true};
  bool overlayShowSharedGpuMemory{true};
  bool overlayShowDrawCalls{false};
  bool overlayShowPrimitives{false};
  bool overlayShowProcessCpuUsage{false};
  bool overlayShowThreadUsage{false};
  bool overlayShowThreadMode{false};
  bool overlayShowOptimizationStatus{false};
  bool overlayShowFps{true};
  bool overlayShowFrameTime{true};
  bool overlayShowResolution{true};
  bool overlayShowStreamingMemory{true};
  bool overlayShowCache64Memory{true};
  bool overlayShowClock{true};
  bool overlayShowDate{true};
  bool overlayShowSeconds{true};
  int overlayPosX{12};
  int overlayPosY{12};

  // OSD sizing.
  OverlayScaleMode overlayScaleMode{OverlayScaleMode::Auto};
  float overlayScale{0.6f};          // user multiplier after automatic scale
  float overlayManualScale{2.0f};    // used only for ScaleMode=Manual
  float overlayMinScale{1.0f};
  float overlayMaxScale{4.0f};
  int overlayReferenceHeight{1080};  // 2160p -> 2.0x resolution scale
  bool overlayUseDpiScale{true};
  bool overlayUseResolutionScale{true};
  bool overlayUsePresentationScale{true};
  bool overlayDynamicDpi{true};

  // Direct-drawn text styling.
  OverlayTextEffect overlayTextEffect{OverlayTextEffect::Shadow};
  OverlayAlignment overlayAlignment{OverlayAlignment::TopLeft};

  int overlayShadowOffsetX{2};
  int overlayShadowOffsetY{2};
  int overlayOutlineThickness{2};

  int overlayTextR{250};
  int overlayTextG{250};
  int overlayTextB{250};
  float overlayTextAlpha{1.0f};

  int overlayEffectR{0};
  int overlayEffectG{0};
  int overlayEffectB{0};
  float overlayShadowAlpha{0.90f};

  float overlayFontSize{18.0f};

  // Optimizer diagnostics.
  bool debugLogging{false};
  bool iniDiagnostics{false};
  unsigned runtimeReportIntervalSeconds{0};

  std::filesystem::path iniPath;
  static Config load(const std::filesystem::path& iniPath);
};

const char* vsyncModeName(VSyncMode mode) noexcept;
const char* frameLimitPointName(FrameLimitPoint point) noexcept;
const char* latencyProfileName(LatencyProfile profile) noexcept;
const char* vrrModeName(VrrMode mode) noexcept;
const char* d3d9ExModeName(D3D9ExMode mode) noexcept;

const char* overlayScaleModeName(OverlayScaleMode mode) noexcept;
const char* overlayTextEffectName(OverlayTextEffect effect) noexcept;
const char* overlayAlignmentName(OverlayAlignment alignment) noexcept;

} // namespace mws
