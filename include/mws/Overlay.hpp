#pragma once
#include "mws/Config.hpp"
#include "mws/UsageTelemetry.hpp"
#include "mws/GpuMemoryTelemetry.hpp"
#include "mws/Cache64MemoryTelemetry.hpp"
#include <Windows.h>
#include <d3d9.h>
#include <imgui.h>
#include <cstdint>
#include <string>

namespace mws {

class Overlay {
public:
  static Overlay& instance();

  void configure(const Config& config);
  // Track devices created via ReFX's exported factory. Renderer detection
  // separately verifies that ReFX's private DXVK module is actually loaded.
  void setReFxDxvkDevice(IDirect3DDevice9* device) noexcept;
  void onDeviceCreated(
      IDirect3DDevice9* device,
      const D3DPRESENT_PARAMETERS& pp);
  void beforeReset();
  void afterReset(
      IDirect3DDevice9* device,
      const D3DPRESENT_PARAMETERS& pp,
      HRESULT resetResult);
  void render(
      IDirect3DDevice9* device,
      const D3DPRESENT_PARAMETERS& pp);
  void shutdown();

  bool enabled() const noexcept { return config_.overlayEnabled; }

private:
  Overlay() = default;

  void initializeImGui(IDirect3DDevice9* device);
  void detectRenderer(IDirect3DDevice9* device);
  void updateD3D9AdapterInfo(IDirect3DDevice9* device);
  bool refreshNativeRenderer();
  void updateTiming();
  void buildClockStrings();
  float queryWindowDpiScale(HWND hwnd) const;
  float computeEffectiveScale(
      HWND hwnd,
      const D3DPRESENT_PARAMETERS& pp,
      UINT viewportWidth,
      UINT viewportHeight) const;
  void applyRuntimeScale(float effectiveScale);

  std::wstring loadedD3D9Path() const;
  std::wstring fileVersionField(
      const std::wstring& filePath,
      const wchar_t* key) const;
  bool isSystemD3D9(const std::wstring& path) const;
  static std::string narrow(const std::wstring& value);
  static bool containsInsensitive(
      const std::wstring& haystack,
      const std::wstring& needle);

  Config config_{};
  UsageTelemetry usage_;
  GpuMemoryTelemetry gpuMemory_;
  Cache64MemoryTelemetry cache64Memory_;
  bool configured_{false};
  bool imguiInitialized_{false};
  bool rendererDetected_{false};
  bool nativeRendererDetected_{false};
  ULONGLONG lastRendererRefreshMs_{0};

  IDirect3DDevice9* device_{};
  IDirect3DDevice9* refxDxvkDevice_{}; // non-owning, compared only by identity

  LARGE_INTEGER qpcFreq_{};
  std::int64_t lastPresentQpc_{0};
  std::int64_t fpsWindowStartQpc_{0};
  unsigned fpsWindowFrames_{0};
  double fps_{0.0};
  double frameTimeMs_{0.0};
  double smoothedFrameTimeMs_{0.0};

  std::string apiText_;
  std::string gpuText_;
  std::string driverText_;
  std::string d3d9PathText_;
  std::string clockText_;
  std::string dateText_;
  ULONGLONG lastClockUpdateMs_{0};
  bool runtimeScaleInitialized_{false};

  ImGuiStyle baseStyle_{};
  bool baseStyleCaptured_{false};
  float currentEffectiveScale_{1.0f};
  float currentDpiScale_{1.0f};
  float currentResolutionScale_{1.0f};
  float currentPresentationScale_{1.0f};

  ImFont* font_{nullptr};
  bool embeddedFont_{false};
  bool fontDrawReported_{false};
};

} // namespace mws
