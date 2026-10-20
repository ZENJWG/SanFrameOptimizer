#include "mws/Config.hpp"
#include "mws/Logger.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <cmath>
#include <sstream>
#include <string>

namespace {

int readInt(
    const std::filesystem::path& p,
    const wchar_t* sec,
    const wchar_t* key,
    int def) {
  return GetPrivateProfileIntW(sec, key, def, p.c_str());
}

float readFloat(
    const std::filesystem::path& p,
    const wchar_t* sec,
    const wchar_t* key,
    float def) {
  wchar_t buf[64]{};
  wchar_t defBuf[64]{};
  swprintf_s(defBuf, L"%.6f", def);
  GetPrivateProfileStringW(
      sec, key, defBuf, buf, static_cast<DWORD>(std::size(buf)), p.c_str());

  wchar_t* end = nullptr;
  const float v = std::wcstof(buf, &end);
  return (end != buf && std::isfinite(v)) ? v : def;
}

std::wstring readString(
    const std::filesystem::path& p,
    const wchar_t* sec,
    const wchar_t* key,
    const wchar_t* def) {
  wchar_t buf[128]{};
  GetPrivateProfileStringW(
      sec, key, def, buf, static_cast<DWORD>(std::size(buf)), p.c_str());
  return buf;
}

mws::VSyncMode parseVSync(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"On") == 0)
    return mws::VSyncMode::On;
  if (_wcsicmp(s.c_str(), L"Off") == 0)
    return mws::VSyncMode::Off;
  return mws::VSyncMode::Game;
}


mws::FrameLimitPoint parseFrameLimitPoint(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"AfterPresent") == 0)
    return mws::FrameLimitPoint::AfterPresent;
  return mws::FrameLimitPoint::BeforePresent;
}

mws::LatencyProfile parseLatencyProfile(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"Balanced") == 0)
    return mws::LatencyProfile::Balanced;
  if (_wcsicmp(s.c_str(), L"Smooth") == 0)
    return mws::LatencyProfile::Smooth;
  return mws::LatencyProfile::LowLatency;
}

mws::VrrMode parseVrrMode(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"Off") == 0 ||
      _wcsicmp(s.c_str(), L"Disabled") == 0)
    return mws::VrrMode::Off;
  if (_wcsicmp(s.c_str(), L"On") == 0 ||
      _wcsicmp(s.c_str(), L"Enabled") == 0 ||
      _wcsicmp(s.c_str(), L"Force") == 0)
    return mws::VrrMode::On;
  return mws::VrrMode::Auto;
}

mws::D3D9ExMode parseD3D9ExMode(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"Off") == 0 ||
      _wcsicmp(s.c_str(), L"Disabled") == 0)
    return mws::D3D9ExMode::Off;
  if (_wcsicmp(s.c_str(), L"Force") == 0 ||
      _wcsicmp(s.c_str(), L"On") == 0 ||
      _wcsicmp(s.c_str(), L"Enabled") == 0)
    return mws::D3D9ExMode::Force;
  return mws::D3D9ExMode::Auto;
}


mws::OverlayScaleMode parseOverlayScaleMode(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"Manual") == 0)
    return mws::OverlayScaleMode::Manual;
  if (_wcsicmp(s.c_str(), L"DPIOnly") == 0)
    return mws::OverlayScaleMode::DpiOnly;
  if (_wcsicmp(s.c_str(), L"ResolutionOnly") == 0)
    return mws::OverlayScaleMode::ResolutionOnly;
  return mws::OverlayScaleMode::Auto;
}


mws::OverlayTextEffect parseOverlayTextEffect(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"None") == 0)
    return mws::OverlayTextEffect::None;
  if (_wcsicmp(s.c_str(), L"Outline") == 0)
    return mws::OverlayTextEffect::Outline;
  return mws::OverlayTextEffect::Shadow;
}

mws::OverlayAlignment parseOverlayAlignment(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"TopCenter") == 0)
    return mws::OverlayAlignment::TopCenter;
  if (_wcsicmp(s.c_str(), L"TopRight") == 0)
    return mws::OverlayAlignment::TopRight;
  if (_wcsicmp(s.c_str(), L"BottomLeft") == 0)
    return mws::OverlayAlignment::BottomLeft;
  if (_wcsicmp(s.c_str(), L"BottomCenter") == 0)
    return mws::OverlayAlignment::BottomCenter;
  if (_wcsicmp(s.c_str(), L"BottomRight") == 0)
    return mws::OverlayAlignment::BottomRight;
  return mws::OverlayAlignment::TopLeft;
}


} // namespace

namespace mws {

const char* vsyncModeName(VSyncMode mode) noexcept {
  switch (mode) {
    case VSyncMode::On: return "On";
    case VSyncMode::Off: return "Off";
    default: return "Game";
  }
}

const char* frameLimitPointName(FrameLimitPoint point) noexcept {
  return point == FrameLimitPoint::AfterPresent
      ? "AfterPresent"
      : "BeforePresent";
}

const char* latencyProfileName(LatencyProfile profile) noexcept {
  switch (profile) {
    case LatencyProfile::Balanced: return "Balanced";
    case LatencyProfile::Smooth: return "Smooth";
    default: return "LowLatency";
  }
}

const char* vrrModeName(VrrMode mode) noexcept {
  switch (mode) {
    case VrrMode::Off: return "Off";
    case VrrMode::On: return "On";
    default: return "Auto";
  }
}

const char* d3d9ExModeName(D3D9ExMode mode) noexcept {
  switch (mode) {
    case D3D9ExMode::Off: return "Off";
    case D3D9ExMode::Force: return "Force";
    default: return "Auto";
  }
}

const char* overlayScaleModeName(OverlayScaleMode mode) noexcept {
  switch (mode) {
    case OverlayScaleMode::Manual: return "Manual";
    case OverlayScaleMode::DpiOnly: return "DPIOnly";
    case OverlayScaleMode::ResolutionOnly: return "ResolutionOnly";
    default: return "Auto";
  }
}

const char* overlayTextEffectName(OverlayTextEffect effect) noexcept {
  switch (effect) {
    case OverlayTextEffect::None: return "None";
    case OverlayTextEffect::Outline: return "Outline";
    default: return "Shadow";
  }
}

const char* overlayAlignmentName(OverlayAlignment alignment) noexcept {
  switch (alignment) {
    case OverlayAlignment::TopCenter: return "TopCenter";
    case OverlayAlignment::TopRight: return "TopRight";
    case OverlayAlignment::BottomLeft: return "BottomLeft";
    case OverlayAlignment::BottomCenter: return "BottomCenter";
    case OverlayAlignment::BottomRight: return "BottomRight";
    default: return "TopLeft";
  }
}

Config Config::load(const std::filesystem::path& iniPath) {
  Config c{};
  c.iniPath = iniPath;

  c.enabled = readInt(iniPath, L"Main", L"Enabled", 1) != 0;

  c.vsync = parseVSync(
      readString(iniPath, L"Rendering", L"VSync", L"Game"));

  c.optimizationsEnabled =
      readInt(iniPath, L"Optimization", L"Enabled", 1) != 0;
  c.d3dStateDeduplication =
      readInt(iniPath, L"Optimization", L"D3DStateDeduplication", 1) != 0;
  c.d3dStateAdaptive =
      readInt(iniPath, L"Optimization", L"D3DStateAdaptive", 1) != 0;
  c.d3dStateAdaptiveWarmupPresents =
      static_cast<unsigned>(std::max(
          60,
          readInt(iniPath, L"Optimization", L"D3DStateAdaptiveWarmupPresents", 1800)));
  c.d3dStateAdaptiveMinimumCalls =
      static_cast<unsigned>(std::max(
          1000,
          readInt(iniPath, L"Optimization", L"D3DStateAdaptiveMinimumCalls", 50000)));
  c.d3dStateAdaptiveMaxPresents =
      static_cast<unsigned>(std::max(
          static_cast<int>(c.d3dStateAdaptiveWarmupPresents),
          readInt(iniPath, L"Optimization", L"D3DStateAdaptiveMaxPresents", 12000)));
  c.d3dStateAdaptiveMinSkipPercent =
      static_cast<unsigned>(std::clamp(
          readInt(iniPath, L"Optimization", L"D3DStateAdaptiveMinSkipPercent", 5),
          0,
          100));
  c.d3dStateInvalidateOnPresent =
      readInt(iniPath, L"Optimization", L"D3DStateInvalidateOnPresent", 1) != 0;
  c.d3dStateDisableOnMultithreadedDevice =
      readInt(iniPath, L"Optimization", L"D3DStateDisableOnMultithreadedDevice", 1) != 0;
  c.d3dStateStatsLogEveryNFrames =
      static_cast<unsigned>(std::max(
          0,
          readInt(iniPath, L"Optimization", L"D3DStateStatsLogEveryNFrames", 0)));

  c.d3dStateAdaptiveResampleInterval = static_cast<unsigned>(std::clamp(
      readInt(iniPath, L"Optimization", L"D3DStateAdaptiveResampleInterval", 3600), 0, 216000));
  c.d3dStateAdaptiveResampleWindow = static_cast<unsigned>(std::clamp(
      readInt(iniPath, L"Optimization", L"D3DStateAdaptiveResampleWindow", 300), 60, 3600));
  c.d3dStateTimeSampling = readInt(iniPath, L"Optimization", L"D3DStateTimeSampling", 0) != 0;
  c.d3dStateTimeWindow = static_cast<unsigned>(std::clamp(
      readInt(iniPath, L"Optimization", L"D3DStateTimeWindow", 300), 120, 3600));
  c.renderWareBatching =
      readInt(iniPath, L"Optimization", L"RenderWareBatching", 1) != 0;
  c.renderWareDayNightBatching = readInt(iniPath, L"Optimization", L"RenderWareDayNightBatching", 1) != 0;
  c.renderWarePlanCache = readInt(iniPath, L"Optimization", L"RenderWarePlanCache", 1) != 0;
  c.immediateBatching = readInt(iniPath, L"Optimization", L"ImmediateBatching", 1) != 0;
  c.staticPropInstancing = readInt(iniPath, L"Optimization", L"StaticPropInstancing", 1) != 0;
  c.adaptiveBatching = readInt(iniPath, L"Optimization", L"AdaptiveBatching", 1) != 0;
  c.adaptiveBatchWindow = static_cast<unsigned>(std::clamp(
      readInt(iniPath, L"Optimization", L"AdaptiveBatchWindow", 600), 120, 36000));
  c.adaptiveBatchProbe = static_cast<unsigned>(std::clamp(
      readInt(iniPath, L"Optimization", L"AdaptiveBatchProbe", 240), 60, 36000));
  c.visibilityBoundsCache = readInt(iniPath, L"Optimization", L"VisibilityBoundsCache", 1) != 0;
  c.optimizationLogInterval = static_cast<unsigned>(std::clamp(
      readInt(iniPath, L"Optimization", L"OptimizationLogInterval", 0), 0, 216000));
  c.passTiming = readInt(iniPath, L"Optimization", L"PassTiming", 0) != 0;

  c.d3dOptimizeRenderState =
      readInt(iniPath, L"Optimization", L"D3DOptimizeRenderState", 1) != 0;
  c.d3dOptimizeTexture =
      readInt(iniPath, L"Optimization", L"D3DOptimizeTexture", 1) != 0;
  c.d3dOptimizeSamplerState =
      readInt(iniPath, L"Optimization", L"D3DOptimizeSamplerState", 1) != 0;
  c.d3dOptimizeTextureStageState =
      readInt(iniPath, L"Optimization", L"D3DOptimizeTextureStageState", 1) != 0;
  c.d3dOptimizeFVF =
      readInt(iniPath, L"Optimization", L"D3DOptimizeFVF", 1) != 0;
  c.d3dOptimizeShaders =
      readInt(iniPath, L"Optimization", L"D3DOptimizeShaders", 0) != 0;
  c.d3dOptimizeVertexDeclaration =
      readInt(iniPath, L"Optimization", L"D3DOptimizeVertexDeclaration", 1) != 0;
  c.d3dOptimizeStreamSource =
      readInt(iniPath, L"Optimization", L"D3DOptimizeStreamSource", 0) != 0;
  c.d3dOptimizeIndices =
      readInt(iniPath, L"Optimization", L"D3DOptimizeIndices", 0) != 0;
  c.d3dOptimizeShaderConstants =
      readInt(iniPath, L"Optimization", L"D3DOptimizeShaderConstants", 0) != 0;
  c.skygfxShaderConstantOptimization =
      readInt(iniPath, L"Optimization", L"SkygfxShaderConstantOptimization", 1) != 0;
  c.skygfxShaderBindingOptimization =
      readInt(iniPath, L"Optimization", L"SkygfxShaderBindingOptimization", 1) != 0;
  c.skygfxPassProfiling =
      readInt(iniPath, L"Optimization", L"SkygfxPassProfiling", 0) != 0;
  c.d3dOptimizeTransform =
      readInt(iniPath, L"Optimization", L"D3DOptimizeTransform", 1) != 0;
  c.d3dOptimizeViewport =
      readInt(iniPath, L"Optimization", L"D3DOptimizeViewport", 1) != 0;
  c.d3dOptimizeScissorRect =
      readInt(iniPath, L"Optimization", L"D3DOptimizeScissorRect", 1) != 0;
  c.d3dOptimizeRenderTarget =
      readInt(iniPath, L"Optimization", L"D3DOptimizeRenderTarget", 1) != 0;
  c.d3dOptimizeDepthStencil =
      readInt(iniPath, L"Optimization", L"D3DOptimizeDepthStencil", 1) != 0;
  c.d3dOptimizeMaterial =
      readInt(iniPath, L"Optimization", L"D3DOptimizeMaterial", 1) != 0;
  c.d3dOptimizeLight =
      readInt(iniPath, L"Optimization", L"D3DOptimizeLight", 1) != 0;
  c.d3dOptimizeLightEnable =
      readInt(iniPath, L"Optimization", L"D3DOptimizeLightEnable", 1) != 0;
  c.d3dOptimizeClipPlane =
      readInt(iniPath, L"Optimization", L"D3DOptimizeClipPlane", 1) != 0;
  c.d3dOptimizeNPatchMode =
      readInt(iniPath, L"Optimization", L"D3DOptimizeNPatchMode", 1) != 0;
  c.workerPreparation =
      readInt(iniPath, L"Optimization", L"WorkerPreparation", 0) != 0;
  c.workerThreads =
      std::clamp(
          readInt(iniPath, L"Optimization", L"WorkerThreads", 1),
          1,
          4);
  c.sanFrameTransientArena =
      readInt(iniPath, L"Optimization", L"TransientArena", 0) != 0;

  c.fxpEnabled = readInt(iniPath, L"FXP", L"Enabled", 1) != 0;
  c.fxpReplaceSimulation = readInt(iniPath, L"FXP", L"ReplaceSimulation", 1) != 0;
  c.fxpCompiledCurves = readInt(iniPath, L"FXP", L"CompiledCurves", 1) != 0;
  c.fxpMatrixCache = readInt(iniPath, L"FXP", L"MatrixCache", 1) != 0;
  c.fxpInstancedSubmission = readInt(iniPath, L"FXP", L"InstancedSubmission", 0) != 0;
  c.fxpOperatorCache = readInt(iniPath, L"FXP", L"OperatorCache", 0) != 0;
  c.fxpOperatorPlans = readInt(iniPath, L"FXP", L"OperatorPlans", 0) != 0;
  c.alphaListIndex = readInt(iniPath, L"Optimization", L"AlphaListIndex", 0) != 0;
  c.shadowGroupIndex = readInt(iniPath, L"Optimization", L"ShadowGroupIndex", 0) != 0;
  c.fxpProfile = readInt(iniPath, L"FXP", L"Profile", 0) != 0;
  c.fxpStatsIntervalSeconds = static_cast<unsigned>(std::clamp(
      readInt(iniPath, L"FXP", L"StatsIntervalSeconds", 0), 0, 300));


  c.frameLimiterEnabled =
      readInt(iniPath, L"Performance", L"FrameLimiter", 0) != 0;
  c.targetFps =
      std::max(0.0f, readFloat(iniPath, L"Performance", L"TargetFPS", 60.0f));
  c.backgroundFps =
      std::max(0.0f, readFloat(iniPath, L"Performance", L"BackgroundFPS", 30.0f));
  if (c.targetFps > 0.0)
    c.targetFps = std::clamp(c.targetFps, 1.0, 10000.0);
  if (c.backgroundFps > 0.0)
    c.backgroundFps = std::clamp(c.backgroundFps, 1.0, 10000.0);
  c.frameLimitPoint = parseFrameLimitPoint(
      readString(iniPath, L"Performance", L"LimitPoint", L"BeforePresent"));
  c.latencyProfile = parseLatencyProfile(
      readString(iniPath, L"Performance", L"LatencyProfile", L"LowLatency"));
  c.useHighResolutionTimer =
      readInt(iniPath, L"Performance", L"HighResolutionTimer", 1) != 0;
  c.useMmcss =
      readInt(iniPath, L"Performance", L"MMCSS", 1) != 0;
  c.useTimerResolution1ms =
      readInt(iniPath, L"Performance", L"TimerResolution1ms", 0) != 0;
  c.busyWaitMicroseconds = static_cast<unsigned>(
      std::clamp(readInt(iniPath, L"Performance", L"BusyWaitMicroseconds", 350), 0, 10000));
  c.maxCatchUpFrames = static_cast<unsigned>(
      std::clamp(readInt(iniPath, L"Performance", L"MaxCatchUpFrames", 2), 0, 120));
  c.enableFrameStats =
      readInt(iniPath, L"Performance", L"FrameStats", 0) != 0;
  c.frameStatsIntervalMs = static_cast<unsigned>(
      std::max(1000, readInt(iniPath, L"Performance", L"FrameStatsIntervalMs", 5000)));
  // MaxRenderAheadFrames replaces the older D3D9Ex-only latency knob while
  // retaining backwards compatibility with the upstream latency setting.
  const int legacyExLatency =
      std::clamp(
          readInt(iniPath, L"Performance", L"D3D9ExMaxFrameLatency", 1),
          1,
          20);
  c.maxRenderAheadFrames = static_cast<unsigned>(
      std::clamp(
          readInt(
              iniPath,
              L"Performance",
              L"MaxRenderAheadFrames",
              legacyExLatency),
          1,
          16));
  c.vrrMode = parseVrrMode(
      readString(iniPath, L"Performance", L"VRRMode", L"Auto"));
  c.vrrRefreshMarginHz =
      std::clamp<double>(
          readFloat(iniPath, L"Performance", L"VRRRefreshMarginHz", 3.0f),
          0.5,
          20.0);
  c.vrrMinimumRefreshHz =
      std::clamp<double>(
          readFloat(iniPath, L"Performance", L"VRRMinimumRefreshHz", 75.0f),
          50.0,
          500.0);
  c.d3d9ExMode = parseD3D9ExMode(
      readString(iniPath, L"Performance", L"D3D9Ex", L"Off"));
  c.deviceLossTelemetry =
      readInt(iniPath, L"Performance", L"DeviceLossTelemetry", 1) != 0;
  c.deviceStatePollMs = static_cast<unsigned>(
      std::clamp(
          readInt(iniPath, L"Performance", L"DeviceStatePollMs", 250),
          50,
          5000));

  c.dxvkAutoTune =
      readInt(iniPath, L"DXVK", L"AutoTune", 1) != 0;
  c.dxvkProperShadersResetWorkaround =
      readInt(iniPath, L"DXVK", L"ProperShadersResetWorkaround", 1) != 0;
  c.dxvkMaxFrameLatency =
      readInt(iniPath, L"DXVK", L"MaxFrameLatency", -1);
  c.dxvkDisableInternalLimiterWhenSanFrameLimiter =
      readInt(
          iniPath,
          L"DXVK",
          L"DisableInternalLimiterWhenSanFrameLimiter",
          1) != 0;
  c.dxvkDisableLatencySleepWhenSanFrameLimiter =
      readInt(
          iniPath,
          L"DXVK",
          L"DisableLatencySleepWhenSanFrameLimiter",
          1) != 0;
  c.dxvkMatchVSyncPolicy =
      readInt(iniPath, L"DXVK", L"MatchVSyncPolicy", 1) != 0;
  c.dxvkUseAllCompilerThreads =
      readInt(iniPath, L"DXVK", L"UseAllCompilerThreads", 0) != 0;
  c.dxvkForceDeviceLocalConstantBuffers =
      readInt(iniPath, L"DXVK", L"ForceDeviceLocalConstantBuffers", 0) != 0;
  c.dxvkDisableFileLogging =
      readInt(iniPath, L"DXVK", L"DisableFileLogging", 0) != 0;
  c.dxvkUseLocalShaderCache =
      readInt(iniPath, L"DXVK", L"UseLocalShaderCache", 0) != 0;

  // Native Vulkan integration uses internal defaults; the optimization master gate still applies.

  c.overlayPresentOnly =
      readInt(iniPath, L"Compatibility", L"OverlayPresentOnly", 0) != 0;

  c.overlayEnabled =
      readInt(iniPath, L"Overlay", L"Enabled", 1) != 0;
  c.overlayShowTitle =
      readInt(iniPath, L"Overlay", L"ShowTitle", 1) != 0;
  c.overlayShowApi =
      readInt(iniPath, L"Overlay", L"ShowAPI", 1) != 0;
  c.overlayShowGpu =
      readInt(iniPath, L"Overlay", L"ShowGPU", 1) != 0;
  c.overlayShowCpuUsage =
      readInt(iniPath, L"Overlay", L"ShowCPUUsage", 0) != 0;
  c.overlayShowGpuUsage =
      readInt(iniPath, L"Overlay", L"ShowGPUUsage", 0) != 0;
  c.overlayShowGpuMemory =
      readInt(iniPath, L"Overlay", L"ShowGPUMemory", 1) != 0;
  c.overlayShowSharedGpuMemory =
      readInt(iniPath, L"Overlay", L"ShowSharedGPUMemory", 1) != 0;
  c.overlayShowDrawCalls =
      readInt(iniPath, L"Overlay", L"ShowDrawCalls", 0) != 0;
  c.overlayShowPrimitives =
      readInt(iniPath, L"Overlay", L"ShowPrimitives", 0) != 0;
  c.overlayShowProcessCpuUsage =
      readInt(iniPath, L"Overlay", L"ShowProcessCPUUsage", 0) != 0;
  c.overlayShowThreadUsage =
      readInt(iniPath, L"Overlay", L"ShowThreadUsage", 0) != 0;
  c.overlayShowThreadMode =
      readInt(iniPath, L"Overlay", L"ShowThreadMode", 0) != 0;
  c.overlayShowOptimizationStatus =
      readInt(iniPath, L"Overlay", L"ShowOptimizationStatus", 0) != 0;
  c.overlayShowFps =
      readInt(iniPath, L"Overlay", L"ShowFPS", 1) != 0;
  c.overlayShowFrameTime =
      readInt(iniPath, L"Overlay", L"ShowFrameTime", 1) != 0;
  c.overlayShowResolution =
      readInt(iniPath, L"Overlay", L"ShowResolution", 1) != 0;
  c.overlayShowStreamingMemory =
      readInt(iniPath, L"Overlay", L"ShowStreamingMemory", 1) != 0;
  c.overlayShowCache64Memory =
      readInt(iniPath, L"Overlay", L"ShowCache64Memory", 1) != 0;
  c.overlayShowClock =
      readInt(iniPath, L"Overlay", L"ShowClock", 1) != 0;
  c.overlayShowDate =
      readInt(iniPath, L"Overlay", L"ShowDate", 1) != 0;
  c.overlayShowSeconds =
      readInt(iniPath, L"Overlay", L"ShowSeconds", 1) != 0;
  c.overlayPosX =
      readInt(iniPath, L"Overlay", L"PositionX", 12);
  c.overlayPosY =
      readInt(iniPath, L"Overlay", L"PositionY", 12);
  c.overlayScaleMode = parseOverlayScaleMode(
      readString(iniPath, L"Overlay", L"ScaleMode", L"Auto"));
  c.overlayScale =
      std::clamp(
          readFloat(iniPath, L"Overlay", L"Scale", 0.6f),
          0.25f,
          4.0f);
  c.overlayManualScale =
      std::clamp(
          readFloat(iniPath, L"Overlay", L"ManualScale", 2.0f),
          0.5f,
          8.0f);
  c.overlayMinScale =
      std::clamp(
          readFloat(iniPath, L"Overlay", L"MinScale", 1.0f),
          0.5f,
          8.0f);
  c.overlayMaxScale =
      std::clamp(
          readFloat(iniPath, L"Overlay", L"MaxScale", 4.0f),
          c.overlayMinScale,
          8.0f);
  c.overlayReferenceHeight =
      std::clamp(
          readInt(iniPath, L"Overlay", L"ReferenceHeight", 1080),
          480,
          4320);
  c.overlayUseDpiScale =
      readInt(iniPath, L"Overlay", L"UseDPIScale", 1) != 0;
  c.overlayUseResolutionScale =
      readInt(iniPath, L"Overlay", L"UseResolutionScale", 1) != 0;
  c.overlayUsePresentationScale =
      readInt(iniPath, L"Overlay", L"UsePresentationScale", 1) != 0;
  c.overlayDynamicDpi =
      readInt(iniPath, L"Overlay", L"DynamicDPI", 1) != 0;
  c.overlayTextEffect = parseOverlayTextEffect(
      readString(iniPath, L"Overlay", L"TextEffect", L"Shadow"));
  c.overlayAlignment = parseOverlayAlignment(
      readString(iniPath, L"Overlay", L"Alignment", L"TopLeft"));

  c.overlayShadowOffsetX =
      std::clamp(
          readInt(iniPath, L"Overlay", L"ShadowOffsetX", 2),
          0,
          32);
  c.overlayShadowOffsetY =
      std::clamp(
          readInt(iniPath, L"Overlay", L"ShadowOffsetY", 2),
          0,
          32);
  c.overlayOutlineThickness =
      std::clamp(
          readInt(iniPath, L"Overlay", L"OutlineThickness", 2),
          1,
          8);

  c.overlayTextR =
      std::clamp(readInt(iniPath, L"Overlay", L"TextR", 250), 0, 255);
  c.overlayTextG =
      std::clamp(readInt(iniPath, L"Overlay", L"TextG", 250), 0, 255);
  c.overlayTextB =
      std::clamp(readInt(iniPath, L"Overlay", L"TextB", 250), 0, 255);
  c.overlayTextAlpha =
      std::clamp(
          readFloat(iniPath, L"Overlay", L"TextAlpha", 1.0f),
          0.0f,
          1.0f);

  c.overlayEffectR =
      std::clamp(readInt(iniPath, L"Overlay", L"EffectR", 0), 0, 255);
  c.overlayEffectG =
      std::clamp(readInt(iniPath, L"Overlay", L"EffectG", 0), 0, 255);
  c.overlayEffectB =
      std::clamp(readInt(iniPath, L"Overlay", L"EffectB", 0), 0, 255);
  c.overlayShadowAlpha =
      std::clamp(
          readFloat(iniPath, L"Overlay", L"EffectAlpha", 0.90f),
          0.0f,
          1.0f);
  c.overlayFontSize =
      std::clamp(
          readFloat(iniPath, L"Overlay", L"FontSize", 18.0f),
          10.0f,
          64.0f);

  // Disable all performance changes when the optimizer master switch is off.
  if (!c.optimizationsEnabled) {
    c.d3dStateDeduplication = false;
    c.d3dStateAdaptive = false;
    c.d3dStateInvalidateOnPresent = false;
    c.renderWareBatching = false;
    c.renderWareDayNightBatching = false;
    c.immediateBatching = false;
    c.staticPropInstancing = false;
    c.visibilityBoundsCache = false;
    c.optimizationLogInterval = 0;

    c.d3dOptimizeRenderState = false;
    c.d3dOptimizeTexture = false;
    c.d3dOptimizeSamplerState = false;
    c.d3dOptimizeTextureStageState = false;
    c.d3dOptimizeFVF = false;
    c.d3dOptimizeShaders = false;
    c.d3dOptimizeVertexDeclaration = false;
    c.d3dOptimizeStreamSource = false;
    c.d3dOptimizeIndices = false;
    c.d3dOptimizeShaderConstants = false;
    c.skygfxShaderConstantOptimization = false;
    c.skygfxShaderBindingOptimization = false;
    c.skygfxPassProfiling = false;
    c.d3dOptimizeTransform = false;
    c.d3dOptimizeViewport = false;
    c.d3dOptimizeScissorRect = false;
    c.d3dOptimizeRenderTarget = false;
    c.d3dOptimizeDepthStencil = false;
    c.d3dOptimizeMaterial = false;
    c.d3dOptimizeLight = false;
    c.d3dOptimizeLightEnable = false;
    c.d3dOptimizeClipPlane = false;
    c.d3dOptimizeNPatchMode = false;

    c.workerPreparation = false;
    c.sanFrameTransientArena = false;
    c.fxpOperatorCache = c.fxpOperatorPlans = false;
    c.alphaListIndex = c.shadowGroupIndex = false;
    c.fxpEnabled = false;
    c.fxpReplaceSimulation = c.fxpCompiledCurves = c.fxpMatrixCache = false;
    c.fxpInstancedSubmission = c.fxpProfile = false;

    // DXVK AutoTune changes queue/presentation policy for performance, so it is
    // part of the optimization master gate. DXVK detection/rendering itself is
    // unaffected.
    c.dxvkAutoTune = false;
    c.frameLimiterEnabled = false;
    c.maxRenderAheadFrames = 0;
    c.d3d9ExMode = D3D9ExMode::Off;
    c.vsync = VSyncMode::Game;
  }

  c.debugLogging =
      readInt(iniPath, L"Debug", L"VerboseLogging", 0) != 0;
  c.iniDiagnostics =
      readInt(iniPath, L"Debug", L"IniDiagnostics", 0) != 0;
  c.runtimeReportIntervalSeconds=static_cast<unsigned>(std::clamp(
      readInt(iniPath,L"Debug",L"RuntimeReportIntervalSeconds",0),0,3600));

  std::ostringstream os;
  os << "Config loaded: Optimizers=" << (c.optimizationsEnabled ? "on" : "off")
     << ", D3DStateDedup=" << c.d3dStateDeduplication
     << ", RWBatching=" << c.renderWareBatching
     << ", ImmediateBatching=" << c.immediateBatching
     << ", StaticInstancing=" << c.staticPropInstancing
     << ", VisibilityCache=" << c.visibilityBoundsCache
     << ", Overlay=" << c.overlayEnabled
     << ", FrameLimiter=" << c.frameLimiterEnabled
     << ", VSync=" << vsyncModeName(c.vsync)
     << ", D3D9Ex=" << d3d9ExModeName(c.d3d9ExMode)
     << ", DXVKAutoTune=" << c.dxvkAutoTune;
  Logger::instance().info(os.str());
  return c;
}
} // namespace mws
