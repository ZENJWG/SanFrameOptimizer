#include "mws/ProperShadersCompatibility.hpp"
#include "mws/OptimizationReport.hpp"
#include "mws/FxpOptimizer.hpp"
#include "mws/D3D9Hook.hpp"
#include "mws/DxvkTuner.hpp"
#include "mws/NativeVulkan.hpp"
#include "mws/Logger.hpp"
#include "mws/GtaSa10US.hpp"
#include "mws/OverlayHookPolicy.hpp"
#include "mws/FramePacer.hpp"
#include "mws/Overlay.hpp"
#include "mws/DrawTelemetry.hpp"
#include <Windows.h>
#include <TlHelp32.h>
#include "mws/D3D9StateCache.hpp"
#include "mws/RenderWareBatcher.hpp"
#include "mws/PassProfiler.hpp"
#include "mws/RenderSubmission.hpp"
#include "mws/EngineOptimizations.hpp"
#include "mws/EnginePass.hpp"
#include "mws/OptimizationRuntime.hpp"
#include "mws/Diagnostics.hpp"
#include <d3d9.h>
#include <MinHook.h>
#include <cstdint>
#include <cctype>
#include <cwctype>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <intrin.h>
#include <sstream>
#include <vector>

namespace {

using Direct3DCreate9Fn =
    IDirect3D9* (WINAPI*)(UINT);

using Direct3DCreate9ExFn =
    HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);

using CreateDeviceFn =
    HRESULT(STDMETHODCALLTYPE*)(
        IDirect3D9*,
        UINT,
        D3DDEVTYPE,
        HWND,
        DWORD,
        D3DPRESENT_PARAMETERS*,
        IDirect3DDevice9**);

using ResetFn =
    HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*,
        D3DPRESENT_PARAMETERS*);

using PresentFn =
    HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*,
        const RECT*,
        const RECT*,
        HWND,
        const RGNDATA*);

using EndSceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
EndSceneFn g_originalEndScene = nullptr;
void* g_endSceneHookAddress = nullptr;
bool g_overlayDrawn = false;
bool g_overlayRendering = false;
bool g_refxOverlayOnly = false;
bool g_distantStreamingPresentOnly = false;

bool distantStreamingLoaded() noexcept {
  // Do not load the ASI or call its private APIs. A loaded module is enough
  // to avoid installing a second inline EndScene detour in DistantStreaming's
  // cloned/observed D3D9 device-vtable chain.
  return GetModuleHandleW(L"DistantStreaming.asi") != nullptr;
}
// The ReFX ASI factory is the game's entry point when its IAT bridge is active.
bool g_factoryIsReFx = false;
void* g_d3d9ExportHookAddress = nullptr;

Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;
Direct3DCreate9ExFn g_direct3DCreate9Ex = nullptr;
CreateDeviceFn g_originalCreateDevice = nullptr;
ResetFn g_originalReset = nullptr;
PresentFn g_originalPresent = nullptr;
std::uint64_t g_presentCalls{},g_presentSuccesses{};
void* g_createDeviceHookAddress = nullptr;
void* g_resetHookAddress = nullptr;
void* g_presentHookAddress = nullptr;
bool g_createDeviceHookInstalled = false;
bool g_resetHookInstalled = false;
bool g_presentHookInstalled = false;
D3DPRESENT_PARAMETERS g_activePP{};
mws::Config g_installConfig{};
IDirect3DDevice9* g_activeDevice = nullptr;
IDirect3DDevice9Ex* g_activeDeviceEx = nullptr;
bool g_useExRuntime = false;
bool g_properShadersCompatibility = false;

void applyPacingPolicy(D3DPRESENT_PARAMETERS& pp) {
  if (g_installConfig.vsync == mws::VSyncMode::On)
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  else if (g_installConfig.vsync == mws::VSyncMode::Off)
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}
bool properShadersModuleLoaded() {
  // GetModuleHandle checks loaded modules, not files on disk. Log the exact
  // module responsible for entering strict compatibility mode.
  constexpr const wchar_t* candidates[] = {
      L"ProperShaders.asi", L"ProperShaders.dll",
      L"proper_shaders.asi", L"proper_shaders.dll"};
  bool detected = false;
  for (const wchar_t* candidate : candidates) {
    HMODULE module = GetModuleHandleW(candidate);
    if (!module)
      continue;
    detected = true;
    wchar_t path[32768] = {};
    DWORD length = GetModuleFileNameW(module, path, 32768);
    std::ostringstream os;
    os << "Proper Shaders detection: matched loaded module ";
    // Module names are ASCII; path is emitted via the existing UTF-8 helper
    // below after its declaration, so report the module name and handle here.
    for (const wchar_t* p = candidate; *p; ++p)
      os << static_cast<char>(*p);
    os << " at handle=0x" << std::hex
       << reinterpret_cast<std::uintptr_t>(module);
    if (length && length < 32768) {
      os << " path=";
      // UTF-8 conversion without reliance on the helper declared below.
      int bytes = WideCharToMultiByte(CP_UTF8, 0, path,
          static_cast<int>(length), nullptr, 0, nullptr, nullptr);
      if (bytes > 0) {
        std::string utf8(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(length),
            &utf8[0], bytes, nullptr, nullptr);
        os << utf8;
      }
    }
    mws::Logger::instance().info(os.str());
  }
  if (!detected)
    mws::Logger::instance().info(
        "Proper Shaders detection: no matching loaded module; "
        "strict compatibility disabled.");
  return detected;
}

struct ExCompatibilityFinding {
  std::string module;
  std::string reason;
  bool blocksAuto{false};
};

std::wstring lowerWide(std::wstring value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
}

std::string wideToUtf8(const std::wstring& value) {
  if (value.empty())
    return {};
  const int needed = WideCharToMultiByte(
      CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1)
    return {};
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, value.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
  if (!utf8.empty() && utf8.back() == '\0')
    utf8.pop_back();
  return utf8;
}

bool moduleImportsD3D9(HMODULE module) {
  if (!module)
    return false;

#if defined(_MSC_VER)
  __try {
#endif
    const std::uint8_t* base =
        reinterpret_cast<const std::uint8_t*>(module);
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
      return false;

    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + static_cast<std::size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE)
      return false;

    const IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size)
      return false;

    const IMAGE_IMPORT_DESCRIPTOR* desc =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            base + dir->VirtualAddress);
    const std::size_t maxDescriptors =
        dir->Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);

    for (std::size_t i = 0; i < maxDescriptors && desc[i].Name; ++i) {
      const char* imported =
          reinterpret_cast<const char*>(base + desc[i].Name);
      if (imported && lstrcmpiA(imported, "d3d9.dll") == 0)
        return true;
    }
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif

  return false;
}

std::vector<ExCompatibilityFinding> scanD3D9ExCompatibility() {
  std::vector<ExCompatibilityFinding> findings;

  HANDLE snapshot = CreateToolhelp32Snapshot(
      TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE)
    return findings;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);

  if (!Module32FirstW(snapshot, &entry)) {
    CloseHandle(snapshot);
    return findings;
  }

  do {
    const std::wstring moduleName = entry.szModule;
    const std::wstring lowerName = lowerWide(moduleName);
    const std::string utf8Name = wideToUtf8(moduleName);

    // SanFrame itself and the game/runtime are not third-party compatibility
    // findings. DXVK's d3d9.dll is handled by the normal Ex capability probe.
    if (lowerName == L"gta_sa.exe" ||
        lowerName == L"sanframeoptimizer.asi" ||
        lowerName == L"d3d9.dll")
      continue;

    bool explicitBlock = false;
    std::string reason;

    // Confirmed/known GTA SA compatibility-sensitive modules.
    if (lowerName == L"ginputsa.asi" ||
        lowerName == L"ginput.asi") {
      explicitBlock = true;
      reason =
          "GInput has been observed to crash before first Present when GTA owns a real D3D9Ex device";
    } else if (lowerName == L"skygfx.asi" ||
               lowerName.find(L"skygfx") != std::wstring::npos) {
      explicitBlock = true;
      reason =
          "SkyGFX directly integrates with GTA/RenderWare's classic IDirect3DDevice9 pipeline";
    }

    if (explicitBlock) {
      findings.push_back({utf8Name, reason, true});
      continue;
    }

    const bool isAsi =
        lowerName.size() >= 4 &&
        lowerName.compare(lowerName.size() - 4, 4, L".asi") == 0;

    // Generic future-proofing: an ASI that directly imports d3d9.dll is
    // participating in the renderer API surface. Auto treats this as unsafe
    // unless explicitly known compatible; Force remains the opt-in override.
    if (isAsi && moduleImportsD3D9(entry.hModule)) {
      findings.push_back({
          utf8Name,
          "third-party ASI directly imports d3d9.dll; classic-D3D9 assumptions cannot be ruled out",
          true});
      continue;
    }

    // Advisory-only integration modules. ReShade supports D3D9Ex itself, so
    // its presence is logged but does not force Auto back to classic D3D9.
    if (lowerName.find(L"reshade") != std::wstring::npos ||
        lowerName.find(L"enbseries") != std::wstring::npos ||
        lowerName.find(L"specialk") != std::wstring::npos) {
      findings.push_back({
          utf8Name,
          "graphics injection layer detected; Ex compatibility depends on that layer's configuration",
          false});
    }
  } while (Module32NextW(snapshot, &entry));

  CloseHandle(snapshot);
  return findings;
}

std::string joinCompatibilityModules(
    const std::vector<ExCompatibilityFinding>& findings,
    bool onlyBlocking) {
  std::ostringstream os;
  bool first = true;
  for (const auto& finding : findings) {
    if (onlyBlocking && !finding.blocksAuto)
      continue;
    if (!first)
      os << ", ";
    first = false;
    os << finding.module;
  }
  return os.str();
}


// D3D9Ex compatibility is a process-start policy decision. GTA and many ASI
// loaders can call Direct3DCreate9 more than once during startup; rescanning
// on every call duplicates warnings and can make diagnostics look like the
// policy changed when it did not. Cache the first complete result and reuse it
// for all subsequent enumerator/device creation attempts.
bool g_exCompatibilityEvaluated = false;
std::vector<ExCompatibilityFinding> g_exCompatibilityFindings;

const std::vector<ExCompatibilityFinding>&
getD3D9ExCompatibilityFindings() {
  if (!g_exCompatibilityEvaluated) {
    g_exCompatibilityFindings = scanD3D9ExCompatibility();
    g_exCompatibilityEvaluated = true;

    for (const auto& finding : g_exCompatibilityFindings) {
      std::ostringstream os;
      os << "D3D9Ex compatibility scan: " << finding.module
         << " - " << finding.reason
         << (finding.blocksAuto ? " [AUTO-BLOCK]" : " [ADVISORY]");
      if (finding.blocksAuto)
        mws::Logger::instance().warn(os.str());
      else
        mws::Logger::instance().info(os.str());
    }
  }

  return g_exCompatibilityFindings;
}

// Device-loss telemetry state. We only emit on transitions/recovery or on a
// failed Present/Reset after the configured poll interval.
HRESULT g_lastDeviceProbe = D3D_OK;
bool g_deviceIssueActive = false;
ULONGLONG g_deviceIssueStartMs = 0;
ULONGLONG g_lastDeviceTelemetryMs = 0;

void markExUnavailable(const std::string& detail, bool failed) {
  auto& diagnostics = mws::Diagnostics::instance();
  if (failed) {
    diagnostics.failed("D3D9Ex.Backend", detail);
    diagnostics.failed("D3D9Ex.PresentEx", detail);
    diagnostics.failed("D3D9Ex.ResetEx", detail);
    diagnostics.failed("D3D9Ex.FrameLatency", detail);
  } else {
    diagnostics.skipped("D3D9Ex.Backend", detail);
    diagnostics.skipped("D3D9Ex.PresentEx", detail);
    diagnostics.skipped("D3D9Ex.ResetEx", detail);
    diagnostics.skipped("D3D9Ex.FrameLatency", detail);
  }
}

bool isGamePresentationWindow(HWND window) {
  if (!window || !IsWindow(window) ||
      !mws::gta::addressIsReadable(mws::gta::kRsGlobal, sizeof(mws::gta::RsGlobalTypeLite)))
    return false;

  // GTA assigns RsGlobal.ps->window before RenderWare initializes D3D9.
  // Wrappers/other ASIs may create temporary devices on their own hidden
  // windows. They must never receive GTA's window styles or presentation.
  const auto* platform = static_cast<const mws::gta::RsPlatformSpecificLite*>(
      mws::gta::rsGlobal().ps);
  if (!platform || !mws::gta::addressIsReadable(
          reinterpret_cast<std::uintptr_t>(platform), sizeof(*platform)) ||
      platform->window != window)
    return false;

  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  return process == GetCurrentProcessId() &&
         (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) == 0;
}

thread_local bool g_creatingGameDevice = false;

struct GameDeviceCreationScope {
  GameDeviceCreationScope() { g_creatingGameDevice = true; }
  ~GameDeviceCreationScope() { g_creatingGameDevice = false; }
};

void refreshPresentationParameters(IDirect3DDevice9* device, HWND fallbackWindow) {
  IDirect3DSwapChain9* chain = nullptr;
  if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain) {
    D3DPRESENT_PARAMETERS actual{};
    if (SUCCEEDED(chain->GetPresentParameters(&actual)))
      g_activePP = actual;
    chain->Release();
  }
  if (!g_activePP.hDeviceWindow)
    g_activePP.hDeviceWindow = fallbackWindow;
}

void logHr(const char* label, HRESULT hr) {
  std::ostringstream os;
  os << label << ": HRESULT=0x"
     << std::hex << std::uppercase
     << static_cast<unsigned long>(hr);

  if (SUCCEEDED(hr))
    mws::Logger::instance().info(os.str());
  else
    mws::Logger::instance().warn(os.str());
}

const char* deviceStateName(HRESULT hr) {
  if (hr == D3D_OK) return "OK";
  if (hr == D3DERR_DEVICELOST) return "DEVICELOST";
  if (hr == D3DERR_DEVICENOTRESET) return "DEVICENOTRESET";
#ifdef D3DERR_DEVICEHUNG
  if (hr == D3DERR_DEVICEHUNG) return "DEVICEHUNG";
#endif
#ifdef D3DERR_DEVICEREMOVED
  if (hr == D3DERR_DEVICEREMOVED) return "DEVICEREMOVED";
#endif
#ifdef S_PRESENT_OCCLUDED
  if (hr == S_PRESENT_OCCLUDED) return "PRESENT_OCCLUDED";
#endif
  return SUCCEEDED(hr) ? "READY" : "ERROR";
}

void releaseActiveDeviceEx() {
  if (g_activeDeviceEx) {
    g_activeDeviceEx->Release();
    g_activeDeviceEx = nullptr;
  }
}

HRESULT probeDeviceState(HWND hwnd) {
  if (!g_activeDevice)
    return D3DERR_INVALIDCALL;

  if (g_activeDeviceEx) {
    HWND target = hwnd ? hwnd : g_activePP.hDeviceWindow;
    return g_activeDeviceEx->CheckDeviceState(target);
  }

  return g_activeDevice->TestCooperativeLevel();
}

std::string surfaceDesc(IDirect3DSurface9* surface) {
  if (!surface)
    return "none";

  D3DSURFACE_DESC d{};
  if (FAILED(surface->GetDesc(&d)))
    return "unavailable";

  std::ostringstream os;
  os << d.Width << "x" << d.Height
     << "/fmt=0x" << std::hex << std::uppercase
     << static_cast<unsigned>(d.Format);
  return os.str();
}

void logDeviceSnapshot(
    const char* reason,
    HRESULT operationHr,
    HRESULT probeHr,
    HWND hwnd) {
  if (!g_activeDevice)
    return;

  IDirect3DSurface9* rt = nullptr;
  IDirect3DSurface9* ds = nullptr;
  HRESULT rtHr = D3DERR_DEVICELOST;
  HRESULT dsHr = D3DERR_DEVICELOST;

  // Never make additional resource calls while the device is known-lost.
  // D3D9Ex in particular restricts legal calls after certain reset failures.
  const bool safeToInspectSurfaces =
      SUCCEEDED(operationHr) && SUCCEEDED(probeHr);
  if (safeToInspectSurfaces) {
    rtHr = g_activeDevice->GetRenderTarget(0, &rt);
    dsHr = g_activeDevice->GetDepthStencilSurface(&ds);
  }

  HWND target = hwnd ? hwnd : g_activePP.hDeviceWindow;

  std::ostringstream os;
  os << "D3D9 device state:"
     << " reason=" << (reason ? reason : "unknown")
     << ", operation=0x" << std::hex << std::uppercase
     << static_cast<unsigned long>(operationHr)
     << ", probe=" << deviceStateName(probeHr)
     << "(0x" << static_cast<unsigned long>(probeHr) << ")"
     << std::dec
     << ", backend=" << (g_activeDeviceEx ? "D3D9Ex" : "D3D9")
     << ", window=" << (target ? "valid" : "none")
     << ", foreground="
     << ((target && GetForegroundWindow() == target) ? 1 : 0)
     << ", minimized="
     << ((target && IsIconic(target)) ? 1 : 0)
     << ", backbuffer=" << g_activePP.BackBufferWidth
     << "x" << g_activePP.BackBufferHeight
     << ", windowed=" << (g_activePP.Windowed ? 1 : 0)
     << ", rt0="
     << (safeToInspectSurfaces
             ? (SUCCEEDED(rtHr) ? surfaceDesc(rt) : "query-failed")
             : "skipped-lost")
     << ", depth="
     << (safeToInspectSurfaces
             ? (SUCCEEDED(dsHr) ? surfaceDesc(ds) : "query-failed")
             : "skipped-lost");

  mws::Logger::instance().warn(os.str());

  if (rt) rt->Release();
  if (ds) ds->Release();
}

void updateDeviceTelemetry(
    const char* reason,
    HRESULT operationHr,
    HWND hwnd) {
  if (!g_installConfig.deviceLossTelemetry || !g_activeDevice)
    return;

  // Microsoft recommends CheckDeviceState only after a failed Ex Present.
  // Once an issue is active, one successful operation is enough to probe and
  // log the recovery transition.
  if (SUCCEEDED(operationHr) && !g_deviceIssueActive)
    return;

  const ULONGLONG now = GetTickCount64();
  const HRESULT probeHr = probeDeviceState(hwnd);
  const bool unhealthy =
      FAILED(operationHr) || FAILED(probeHr);
  const bool stateChanged = probeHr != g_lastDeviceProbe;

  if (unhealthy) {
    if (!g_deviceIssueActive) {
      g_deviceIssueActive = true;
      g_deviceIssueStartMs = now;
    }

    if (stateChanged ||
        now - g_lastDeviceTelemetryMs >= g_installConfig.deviceStatePollMs) {
      logDeviceSnapshot(reason, operationHr, probeHr, hwnd);
      g_lastDeviceTelemetryMs = now;
    }
  } else if (g_deviceIssueActive) {
    std::ostringstream os;
    os << "D3D9 device recovered:"
       << " reason=" << (reason ? reason : "unknown")
       << ", state=" << deviceStateName(probeHr)
       << ", recoveryMs="
       << (now - g_deviceIssueStartMs)
       << ", backend=" << (g_activeDeviceEx ? "D3D9Ex" : "D3D9");
    mws::Logger::instance().info(os.str());

    g_deviceIssueActive = false;
    g_deviceIssueStartMs = 0;
    g_lastDeviceTelemetryMs = now;
  }

  g_lastDeviceProbe = probeHr;
}


bool buildFullscreenModeEx(
    IDirect3DDevice9Ex* deviceEx,
    const D3DPRESENT_PARAMETERS& pp,
    D3DDISPLAYMODEEX& mode) {
  mode = {};
  mode.Size = sizeof(mode);

  D3DDISPLAYROTATION rotation = D3DDISPLAYROTATION_IDENTITY;
  if (deviceEx) {
    D3DDISPLAYMODEEX current{};
    current.Size = sizeof(current);
    if (SUCCEEDED(deviceEx->GetDisplayModeEx(0, &current, &rotation))) {
      mode = current;
      mode.Size = sizeof(mode);
    }
  }

  if (pp.BackBufferWidth)
    mode.Width = pp.BackBufferWidth;
  if (pp.BackBufferHeight)
    mode.Height = pp.BackBufferHeight;
  if (pp.FullScreen_RefreshRateInHz)
    mode.RefreshRate = pp.FullScreen_RefreshRateInHz;
  if (pp.BackBufferFormat != D3DFMT_UNKNOWN)
    mode.Format = pp.BackBufferFormat;
  if (mode.ScanLineOrdering == D3DSCANLINEORDERING_UNKNOWN)
    mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

  return mode.Width != 0 &&
         mode.Height != 0 &&
         mode.Format != D3DFMT_UNKNOWN;
}

HRESULT presentPrimary(
    IDirect3DDevice9* device,
    const RECT* source,
    const RECT* dest,
    HWND overrideWindow,
    const RGNDATA* dirtyRegion) {
  if (g_useExRuntime && g_activeDeviceEx && device == g_activeDevice) {
    const HRESULT hr = g_activeDeviceEx->PresentEx(
        source, dest, overrideWindow, dirtyRegion, 0);
    if (SUCCEEDED(hr)) {
      mws::Diagnostics::instance().effective(
          "D3D9Ex.PresentEx",
          "primary presentation executed through IDirect3DDevice9Ex::PresentEx");
    } else {
      std::ostringstream os;
      os << "PresentEx failed with HRESULT=0x"
         << std::hex << std::uppercase
         << static_cast<unsigned long>(hr);
      mws::Diagnostics::instance().failed(
          "D3D9Ex.PresentEx", os.str());
    }
    return hr;
  }

  return g_originalPresent
      ? g_originalPresent(device, source, dest, overrideWindow, dirtyRegion)
      : D3DERR_INVALIDCALL;
}

// An additional swap chain can be Lite's visible MatchWindow target. Draw
// before EndScene so that restoring the primary target cannot hide the OSD.
bool overlayTargetParameters(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS& pp) {
  IDirect3DSurface9* target = nullptr;
  if (FAILED(device->GetRenderTarget(0, &target)) || !target) return false;
  IDirect3DSwapChain9* chain = nullptr;
  D3DSURFACE_DESC desc{};
  D3DVIEWPORT9 viewport{};
  bool visible = SUCCEEDED(target->GetContainer(__uuidof(IDirect3DSwapChain9),
      reinterpret_cast<void**>(&chain))) && chain;
  if (visible) {
    visible = SUCCEEDED(chain->GetPresentParameters(&pp)) &&
        SUCCEEDED(target->GetDesc(&desc)) && SUCCEEDED(device->GetViewport(&viewport)) &&
        viewport.X == 0 && viewport.Y == 0 &&
        viewport.Width == desc.Width && viewport.Height == desc.Height &&
        (!pp.hDeviceWindow || pp.hDeviceWindow == g_activePP.hDeviceWindow);
    pp.BackBufferWidth = desc.Width;
    pp.BackBufferHeight = desc.Height;
    if (!pp.hDeviceWindow) pp.hDeviceWindow = g_activePP.hDeviceWindow;
    chain->Release();
  }
  target->Release();
  return visible;
}
void drawOverlay(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& pp) {
  // Flush game submissions and invalidate state around ImGui's state-block
  // capture/restore, even when InvalidateOnPresent was disabled in the INI.
  mws::RenderSubmission::flush();
  mws::D3D9StateCache::instance().invalidate();
  g_overlayRendering = true;
  mws::Overlay::instance().render(device, pp);
  mws::RenderSubmission::flush();
  g_overlayRendering = false;
  mws::D3D9StateCache::instance().invalidate();
  g_overlayDrawn = true;
}
HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* device) {
  // If an ASI loads late, stop OSD drawing in this hook. The Present path
  // remains available. Existing MinHook detours cannot be safely uninstalled
  // while executing on the detoured method's stack.
  if (distantStreamingLoaded()) {
    g_distantStreamingPresentOnly = true;
    return g_originalEndScene ? g_originalEndScene(device) : D3DERR_INVALIDCALL;
  }
  if (device == g_activeDevice && !g_overlayRendering && !g_overlayDrawn &&
      mws::Overlay::instance().enabled() && device->TestCooperativeLevel() == D3D_OK) {
    D3DPRESENT_PARAMETERS pp{};
    if (overlayTargetParameters(device, pp)) drawOverlay(device, pp);
  }
  return g_originalEndScene ? g_originalEndScene(device) : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device,
    D3DPRESENT_PARAMETERS* pp) {
  if (device != g_activeDevice || !pp)
    return g_originalReset ? g_originalReset(device, pp) : D3DERR_INVALIDCALL;
  g_overlayDrawn = false;
  mws::Overlay::instance().beforeReset();
  mws::FramePacer::instance().resetTiming();
  mws::FxpOptimizer::beforeDeviceReset();
  mws::PassProfiler::reset();
  mws::RenderWareBatcher::onReset();
  mws::RenderSubmission::onReset();
  mws::D3D9StateCache::instance().onReset();
  D3DPRESENT_PARAMETERS modified = *pp;
  const HWND deviceWindow = pp->hDeviceWindow ? pp->hDeviceWindow : g_activePP.hDeviceWindow;
  applyPacingPolicy(modified);
  // Pass the game's mode, dimensions and reset request through unchanged.
  HRESULT hr = D3DERR_INVALIDCALL;
  bool usedResetEx = false;

  if (g_useExRuntime && g_activeDeviceEx) {
    D3DDISPLAYMODEEX fullscreenMode{};
    D3DDISPLAYMODEEX* fullscreenPtr = nullptr;

    if (!modified.Windowed) {
      if (!buildFullscreenModeEx(
              g_activeDeviceEx,
              modified,
              fullscreenMode)) {
        mws::Logger::instance().error(
            "D3D9Ex ResetEx: could not construct a valid fullscreen display mode.");
        mws::Diagnostics::instance().failed(
            "D3D9Ex.ResetEx",
            "fullscreen D3DDISPLAYMODEEX construction failed");
        return D3DERR_INVALIDCALL;
      }
      fullscreenPtr = &fullscreenMode;
    }

    hr = g_activeDeviceEx->ResetEx(&modified, fullscreenPtr);
    usedResetEx = true;

    if (SUCCEEDED(hr)) {
      mws::Diagnostics::instance().effective(
          "D3D9Ex.ResetEx",
          modified.Windowed
              ? "windowed reset executed through IDirect3DDevice9Ex::ResetEx"
              : "fullscreen reset executed through IDirect3DDevice9Ex::ResetEx");
    } else {
      std::ostringstream exError;
      exError << "ResetEx failed with HRESULT=0x"
              << std::hex << std::uppercase
              << static_cast<unsigned long>(hr);
      mws::Diagnostics::instance().failed(
          "D3D9Ex.ResetEx",
          exError.str());
    }
  } else {
    hr = g_originalReset(device, &modified);
  }

  if (SUCCEEDED(hr)) {
    *pp = modified;
    g_activePP = modified;
    refreshPresentationParameters(device, deviceWindow);
    mws::FramePacer::instance().onDeviceCreated(device);
  }
  mws::Overlay::instance().afterReset(device, g_activePP, hr);
  mws::D3D9StateCache::instance().invalidate();
  logHr(usedResetEx ? "IDirect3DDevice9Ex::ResetEx" : "IDirect3DDevice9::Reset", hr);
  updateDeviceTelemetry("Reset", hr, deviceWindow);
  return hr;
}

void installReFxSkygfxConstants(IDirect3DDevice9* device) {
  if (!(g_installConfig.skygfxShaderConstantOptimization || g_installConfig.skygfxShaderBindingOptimization ||
      g_installConfig.skygfxPassProfiling) || !g_installConfig.optimizationsEnabled)
    return;
  // Only a real DXVK D3D9 implementation may use these shader patches.
  // ReFX's native Vulkan capture facade keeps its own optimizer ownership.
  HMODULE implementation = nullptr;
  auto** vtable = *reinterpret_cast<void***>(device);
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(vtable[6]), &implementation)) return;
  wchar_t path[32768]{};
  if (!GetModuleFileNameW(implementation, path, static_cast<DWORD>(std::size(path)))) return;
  std::string version;
  if (!mws::DxvkTuner::instance().identifyDxvk(path, version)) return;
  mws::Config constants = g_installConfig;
  constants.d3dOptimizeRenderState = false;
  constants.d3dOptimizeTexture = false;
  constants.d3dOptimizeSamplerState = false;
  constants.d3dOptimizeTextureStageState = false;
  constants.d3dOptimizeFVF = false;
  constants.d3dOptimizeShaders = false;
  constants.d3dOptimizeVertexDeclaration = false;
  constants.d3dOptimizeStreamSource = false;
  constants.d3dOptimizeIndices = false;
  constants.d3dOptimizeShaderConstants = false;
  constants.d3dOptimizeTransform = false;
  constants.d3dOptimizeViewport = false;
  constants.d3dOptimizeScissorRect = false;
  constants.d3dOptimizeRenderTarget = false;
  constants.d3dOptimizeDepthStencil = false;
  constants.d3dOptimizeMaterial = false;
  constants.d3dOptimizeLight = false;
  constants.d3dOptimizeLightEnable = false;
  constants.d3dOptimizeClipPlane = false;
  constants.d3dOptimizeNPatchMode = false;
  if (mws::D3D9StateCache::instance().install(device, constants)) {
    mws::OptimizationReport::configure(constants, false);
    mws::Logger::instance().info("ReFX DXVK SkyGFX compatibility: shader hooks prepared; activation requires loaded SkyGFX. DXVK auto-tuning remains excluded.");
  }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device,
    const RECT* source, const RECT* dest, HWND overrideWindow,
    const RGNDATA* dirtyRegion) {
  if (device != g_activeDevice)
    return g_originalPresent ? g_originalPresent(device, source, dest, overrideWindow, dirtyRegion) : D3DERR_INVALIDCALL;
  ++g_presentCalls;
  if (g_refxOverlayOnly) {
    if (!g_overlayDrawn && mws::Overlay::instance().enabled() &&
        device->TestCooperativeLevel() == D3D_OK && SUCCEEDED(device->BeginScene())) {
      drawOverlay(device, g_activePP);
      device->EndScene();
    }
    const HRESULT result = g_originalPresent(device, source, dest, overrideWindow, dirtyRegion);
    if(SUCCEEDED(result)) ++g_presentSuccesses;
    mws::D3D9StateCache::instance().onPresent();
    mws::OptimizationReport::onPresent();
    mws::DrawTelemetry::onFrame();
    mws::Logger::instance().flushIfDue();
    g_overlayDrawn = false;
    return result;
  }
  mws::OptimizationRuntime::instance().beginFrame();
  mws::RenderSubmission::onPresent();
  mws::RenderWareBatcher::onPresent();
  mws::EngineOptimizations::onPresent();
  mws::EnginePass::onPresent();
  const HWND pacingWindow = overrideWindow ? overrideWindow : g_activePP.hDeviceWindow;
  mws::FramePacer::instance().beforePresent(pacingWindow);
  if (!g_overlayDrawn && mws::Overlay::instance().enabled() &&
      device->TestCooperativeLevel() == D3D_OK && SUCCEEDED(device->BeginScene())) {
    auto pp = g_activePP;
    pp.hDeviceWindow = pacingWindow;
    drawOverlay(device, pp);
    device->EndScene();
  }
  // Sample only after deferred batches and overlay have submitted their work.
  mws::PassProfiler::onPresent();
  const HRESULT hr = presentPrimary(device, source, dest, overrideWindow, dirtyRegion);
  if(SUCCEEDED(hr)) ++g_presentSuccesses;
  if (SUCCEEDED(hr))
    mws::Diagnostics::instance().effective("D3D9.DevicePresent", "game device presented");
  updateDeviceTelemetry("Present", hr, pacingWindow);
  mws::D3D9StateCache::instance().onPresent();
  mws::OptimizationReport::onPresent();
  mws::DrawTelemetry::onFrame();
  mws::FramePacer::instance().afterPresent(pacingWindow);
  g_overlayDrawn = false;
  return hr;
}

bool installDeviceHooks(IDirect3DDevice9* device) {
  if (!device) return false;

  const bool distantDetected = distantStreamingLoaded();
  g_distantStreamingPresentOnly =
      !mws::allowEndSceneOverlayHook(distantDetected, g_installConfig.overlayPresentOnly);
  if (g_distantStreamingPresentOnly && g_installConfig.overlayEnabled) {
    mws::Logger::instance().info(
        distantDetected
            ? "DistantStreaming compatibility: detected loaded DistantStreaming.asi; "
              "leaving EndScene unhooked and rendering the OSD through Present only."
            : "D3D9 compatibility: OverlayPresentOnly=1; leaving EndScene "
              "unhooked and rendering the OSD through Present only.");
  }
  void** vtable = *reinterpret_cast<void***>(device);
  if (!g_resetHookInstalled) {
    void* resetAddress = vtable[16];

    auto st = MH_CreateHook(
        resetAddress,
        reinterpret_cast<void*>(&HookReset),
        reinterpret_cast<void**>(&g_originalReset));

    // An already-created MinHook belongs to another owner. MinHook does not
    // return that owner's trampoline; pretending it is ours leaves
    // g_originalReset null and can crash during Reset. Never adopt it.
    if (st != MH_OK) {
      mws::Logger::instance().error(
          st == MH_ERROR_ALREADY_CREATED
              ? "D3D9 Reset hook conflict: target is already MinHook-owned; "
                "refusing to adopt an unknown trampoline."
              : "Failed to create IDirect3DDevice9::Reset hook.");
      return false;
    }

    st = MH_EnableHook(resetAddress);

    if (st != MH_OK && st != MH_ERROR_ENABLED) {
      MH_RemoveHook(resetAddress);
      g_originalReset = nullptr;
      mws::Logger::instance().error(
          "Failed to enable IDirect3DDevice9::Reset hook.");
      return false;
    }

    g_resetHookInstalled = true;
    g_resetHookAddress = resetAddress;

    std::ostringstream os;
    os << "D3D9 Reset hook installed at 0x"
       << std::hex << std::uppercase
       << reinterpret_cast<std::uintptr_t>(resetAddress);
    mws::Logger::instance().info(os.str());
  }


  if (!g_presentHookInstalled) {
    void* presentAddress = vtable[17];

    auto st = MH_CreateHook(
        presentAddress,
        reinterpret_cast<void*>(&HookPresent),
        reinterpret_cast<void**>(&g_originalPresent));

    if (st != MH_OK) {
      mws::Logger::instance().warn(
          st == MH_ERROR_ALREADY_CREATED
              ? "D3D9 Present hook conflict: another MinHook owns target; "
                "refusing to adopt its trampoline."
              : "Failed to create IDirect3DDevice9::Present hook; "
                "optimizer frame maintenance will be unavailable.");
    } else {
      st = MH_EnableHook(presentAddress);

      if (st == MH_OK || st == MH_ERROR_ENABLED) {
        g_presentHookInstalled = true;
        g_presentHookAddress = presentAddress;

        std::ostringstream os;
        os << "D3D9 Present hook installed at 0x"
           << std::hex << std::uppercase
           << reinterpret_cast<std::uintptr_t>(presentAddress);
        mws::Logger::instance().info(os.str());
      } else {
        MH_RemoveHook(presentAddress);
        g_originalPresent = nullptr;
      }
    }
  }

  if (g_installConfig.overlayEnabled && !g_properShadersCompatibility &&
      !g_distantStreamingPresentOnly && !g_endSceneHookAddress) {
    void* address = vtable[42];
    if (MH_CreateHook(address, reinterpret_cast<void*>(&HookEndScene),
            reinterpret_cast<void**>(&g_originalEndScene)) == MH_OK) {
      if (MH_EnableHook(address) == MH_OK) g_endSceneHookAddress = address;
      else { MH_RemoveHook(address); g_originalEndScene = nullptr; }
    }
    if (!g_endSceneHookAddress)
      mws::Logger::instance().warn("OSD EndScene hook unavailable; using Present fallback.");
  }
  return g_resetHookInstalled && g_presentHookInstalled;
}

HRESULT createPreferredGameDevice(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** outDevice,
    bool& usedEx) {
  usedEx = false;

  if (!self || !pp || !outDevice || !g_originalCreateDevice)
    return D3DERR_INVALIDCALL;

  *outDevice = nullptr;

  // Compatibility is critical in GTA SA's ASI ecosystem.  The application's
  // call must continue through IDirect3D9::CreateDevice so other mods that
  // observe/hook the normal D3D9 creation path still receive their expected
  // initialization event.  When `self` is the IDirect3D9Ex object returned by
  // Direct3DCreate9Ex, the inherited CreateDevice path can still produce a
  // device exposing IDirect3DDevice9Ex.  We prove that with QueryInterface
  // rather than bypassing the chain with a direct CreateDeviceEx call.
  const HRESULT hr =
      g_originalCreateDevice(
          self,
          adapter,
          deviceType,
          focusWindow,
          behaviorFlags,
          pp,
          outDevice);

  if (FAILED(hr) || !*outDevice)
    return hr;

  if (g_installConfig.d3d9ExMode == mws::D3D9ExMode::Off)
    return hr;

  IDirect3DDevice9Ex* probeEx = nullptr;
  const HRESULT qi =
      (*outDevice)->QueryInterface(
          __uuidof(IDirect3DDevice9Ex),
          reinterpret_cast<void**>(&probeEx));

  if (SUCCEEDED(qi) && probeEx) {
    probeEx->Release();
    usedEx = true;
    return hr;
  }

  if (g_installConfig.d3d9ExMode == mws::D3D9ExMode::Force) {
    mws::Logger::instance().error(
        "D3D9Ex Force mode: inherited IDirect3D9::CreateDevice completed, "
        "but the resulting device does not expose IDirect3DDevice9Ex.");
    markExUnavailable(
        "CreateDevice result did not expose IDirect3DDevice9Ex in Force mode",
        true);
    (*outDevice)->Release();
    *outDevice = nullptr;
    return FAILED(qi) ? qi : D3DERR_NOTAVAILABLE;
  }

  markExUnavailable(
      "CreateDevice result did not expose IDirect3DDevice9Ex; Auto mode using classic D3D9",
      false);
  return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** outDevice) {
  if (!g_originalCreateDevice || !pp)
    return g_originalCreateDevice
        ? g_originalCreateDevice(
              self,
              adapter,
              deviceType,
              focusWindow,
              behaviorFlags,
              pp,
              outDevice)
        : D3DERR_INVALIDCALL;

  // hDeviceWindow owns windowed presentation; hFocusWindow can legitimately
  // name a different window. Do not promote a helper target to the game HWND.
  const HWND presentationWindow = pp->hDeviceWindow ? pp->hDeviceWindow : focusWindow;
  if (g_creatingGameDevice || !isGamePresentationWindow(presentationWindow)) {
    if (!g_creatingGameDevice) {
      std::ostringstream os;
      os << "D3D9 helper device left untouched: hwnd=0x"
         << std::hex << reinterpret_cast<std::uintptr_t>(presentationWindow);
      mws::Logger::instance().info(os.str());
    }
    return g_originalCreateDevice(self, adapter, deviceType, focusWindow,
        behaviorFlags, pp, outDevice);
  }
  GameDeviceCreationScope creationScope;
  mws::DxvkTuner::instance().prepareForDirect3DCreate();

  // ReFX may load after our startup scan. Its device is a CPU facade; native
  // ReFX owns native GPU presentation; SanFrame retains detection/OSD only.
  if (mws::NativeVulkan::attach()) {
    const HRESULT hr = g_originalCreateDevice(self, adapter, deviceType,
        focusWindow, behaviorFlags, pp, outDevice);
    if (SUCCEEDED(hr) && outDevice && *outDevice) {
      g_refxOverlayOnly = true;
      g_activeDevice = *outDevice;
      g_activePP = *pp;
      mws::Logger::instance().info(
          "ReFX DXVK overlay-only: captured compatibility device; SanFrame native Vulkan optimization is removed.");
      if (!installDeviceHooks(*outDevice))
        mws::Logger::instance().warn("ReFX DXVK overlay-only: device hook installation failed.");
      else
        installReFxSkygfxConstants(*outDevice);
      // This is not the normal D3D9 renderer: native Vulkan submits the
      // scene. Never present mere INI enablement as an active optimization.
      auto& diagnostics = mws::Diagnostics::instance();
      for (const char* feature : {
          "Optimization.RenderWareBatching",
          "Optimization.RenderWareDayNightBatching",
          "Optimization.ImmediateBatching",
          "Optimization.StaticPropInstancing",
          "Optimization.VisibilityBoundsCache",
          "Optimization.PassTiming"}) {
        diagnostics.autoDisable(feature,
            "ReFX owns native Vulkan submission; SanFrame is in overlay-only mode");
      }
      mws::Logger::instance().info(diagnostics.optimizationAvailability(true));
    }
    return hr;
  }

  D3DPRESENT_PARAMETERS modified = *pp;
  g_properShadersCompatibility = properShadersModuleLoaded();
  // VSync is a pacing option; window mode and render size remain game-owned.
  applyPacingPolicy(modified);
  // Immediate batching/static instancing need normal D3D9 Get* state queries.
  // D3DCREATE_PUREDEVICE deliberately disables those queries.  PUREDEVICE is
  // only an optional CPU-overhead hint; clearing it keeps hardware vertex
  // processing while restoring the state-query contract required by SanFrame.
  DWORD effectiveBehaviorFlags = behaviorFlags;
  if (!g_properShadersCompatibility &&
      (g_installConfig.immediateBatching || g_installConfig.staticPropInstancing) &&
      (effectiveBehaviorFlags & D3DCREATE_PUREDEVICE)) {
    effectiveBehaviorFlags &= ~D3DCREATE_PUREDEVICE;
    std::ostringstream os;
    os << "D3D9 submission compatibility: clearing D3DCREATE_PUREDEVICE "
          "(requested flags=0x"
       << std::hex << std::uppercase << behaviorFlags
       << ", effective flags=0x" << effectiveBehaviorFlags
       << ") so enabled batching/instancing can query device state.";
    mws::Logger::instance().info(os.str());
  }

  if (g_properShadersCompatibility) {
    mws::Logger::instance().info(
        "Proper Shaders strict compatibility: preserving GTA's original D3D9 "
        "device behavior flags, passing real Reset calls through, disabling "
        "renderer optimizers.");
  }

  bool usedExBackend = false;
  HRESULT hr =
      createPreferredGameDevice(
          self,
          adapter,
          deviceType,
          focusWindow,
          effectiveBehaviorFlags,
          &modified,
          outDevice,
          usedExBackend);

  logHr(
      usedExBackend
          ? "IDirect3D9Ex inherited CreateDevice (Ex-capable result)"
          : "IDirect3D9::CreateDevice",
      hr);

  if (SUCCEEDED(hr) && outDevice && *outDevice) {
    releaseActiveDeviceEx();
    g_activeDevice = *outDevice;
    g_useExRuntime = false;

    // Query once and retain the Ex interface for CheckDeviceState and unified
    // queue-depth control. This is also useful if a wrapper returned an Ex
    // device through the classic creation path.
    (*outDevice)->QueryInterface(
        __uuidof(IDirect3DDevice9Ex),
        reinterpret_cast<void**>(&g_activeDeviceEx));

    g_useExRuntime = usedExBackend && g_activeDeviceEx != nullptr;
    if (usedExBackend && !g_activeDeviceEx) {
      mws::Logger::instance().error(
          "D3D9Ex invariant failure: Ex-capable CreateDevice was detected but IDirect3DDevice9Ex QueryInterface failed.");
      markExUnavailable(
          "Ex-capable CreateDevice result lost IDirect3DDevice9Ex during activation",
          true);
    }
    if (g_useExRuntime) {
      mws::Diagnostics::instance().effective(
          "D3D9Ex.Backend",
          "Direct3DCreate9Ex enumerator + inherited CreateDevice preserved the D3D9 hook chain and produced an IDirect3DDevice9Ex game device");
      mws::Diagnostics::instance().active(
          "D3D9Ex.PresentEx",
          "Ex backend active; awaiting first primary PresentEx");
      mws::Diagnostics::instance().active(
          "D3D9Ex.ResetEx",
          "Ex backend active; awaiting a genuine device reset");
    }

    g_lastDeviceProbe = D3D_OK;
    g_deviceIssueActive = false;
    g_deviceIssueStartMs = 0;
    g_lastDeviceTelemetryMs = 0;

    if (g_activeDeviceEx) {
      mws::Logger::instance().info(
          usedExBackend
              ? "D3D9Ex backend ACTIVE: inherited CreateDevice preserved compatibility and returned an Ex-capable device."
              : "D3D9Ex-capable device detected through classic creation path.");
    }

    *pp = modified;
    g_activePP = modified;
    refreshPresentationParameters(*outDevice, presentationWindow);
    g_overlayDrawn = false;
    if (g_factoryIsReFx) {
      // ReFX's wrapped device can expose ASI-owned vtable methods instead of
      // methods in the private DXVK DLL. Track its factory provenance; the
      // OSD will verify the live DXVK module before labeling the renderer.
      mws::Overlay::instance().setReFxDxvkDevice(*outDevice);
      mws::Logger::instance().info(
          "ReFX D3D9 factory: GTA device captured; installing Present/EndScene hooks for OSD.");
    }
    mws::Overlay::instance().onDeviceCreated(*outDevice, g_activePP);
    mws::FramePacer::instance().onDeviceCreated(*outDevice);
    if (!installDeviceHooks(*outDevice)) {
      mws::Logger::instance().error(
          "Renderer optimizers skipped: required Reset/Present hooks are unavailable.");
      return hr;
    }
    if (g_properShadersCompatibility) {
      // The proxy's setters have side effects beyond changing D3D9 state.
      // Do not install suppression hooks, even with legacy visibility opt-in.
      mws::ProperShadersCompatibility::report(g_installConfig);
      mws::Logger::instance().info(
          "Proper Shaders compatibility ACTIVE: state suppression, draw batching, "
          "instancing, visibility caching and alpha/shadow indexing auto-disabled; "
          "particle simulation, curve and matrix optimizers remain eligible.");
    } else {
      mws::D3D9StateCache::instance().install(*outDevice, g_installConfig);
      mws::RenderSubmission::install(*outDevice, g_installConfig);
      mws::RenderWareBatcher::install(*outDevice, g_installConfig);
      mws::EngineOptimizations::install(g_installConfig);
    }
    mws::PassProfiler::install(*outDevice, g_installConfig);
    mws::OptimizationReport::configure(g_installConfig, g_properShadersCompatibility);
    mws::Logger::instance().info(
        mws::Diagnostics::instance().optimizationAvailability(true));
  }

  return hr;
}

bool installCreateDeviceHook(IDirect3D9* d3d) {
  if (g_createDeviceHookInstalled || !d3d)
    return true;

  void** vtable = *reinterpret_cast<void***>(d3d);
  void* createDeviceAddress = vtable[16];

  auto st = MH_CreateHook(
      createDeviceAddress,
      reinterpret_cast<void*>(&HookCreateDevice),
      reinterpret_cast<void**>(&g_originalCreateDevice));

  if (st != MH_OK) {
    mws::Logger::instance().error(
        "Failed to create IDirect3D9::CreateDevice hook.");
    return false;
  }

  st = MH_EnableHook(createDeviceAddress);

  if (st != MH_OK && st != MH_ERROR_ENABLED) {
    MH_RemoveHook(createDeviceAddress);
    g_originalCreateDevice = nullptr;
    mws::Logger::instance().error(
        "Failed to enable IDirect3D9::CreateDevice hook.");
    return false;
  }

  g_createDeviceHookInstalled = true;
  g_createDeviceHookAddress = createDeviceAddress;

  std::ostringstream os;
  os << "D3D9 CreateDevice hook installed at 0x"
     << std::hex << std::uppercase
     << reinterpret_cast<std::uintptr_t>(createDeviceAddress);
  mws::Logger::instance().info(os.str());
  return true;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion) {
  mws::DxvkTuner::instance().prepareForDirect3DCreate();
  if (mws::NativeVulkan::attach()) {
    auto* native = g_originalDirect3DCreate9 ? g_originalDirect3DCreate9(sdkVersion) : nullptr;
    if (native && mws::Overlay::instance().enabled())
      installCreateDeviceHook(native);
    return native;
  }
  IDirect3D9* d3d = nullptr;

  // Compatibility policy: Auto is conservative when a loaded ASI is known
  // to depend on the classic GTA/RenderWare D3D9 device or directly imports
  // d3d9.dll. Force is an explicit diagnostic override and never silently
  // downgrades because of a policy finding.
  if (g_installConfig.d3d9ExMode != mws::D3D9ExMode::Off) {
    const auto& findings = getD3D9ExCompatibilityFindings();

    const bool hasBlockingFinding =
        std::any_of(
            findings.begin(), findings.end(),
            [](const ExCompatibilityFinding& finding) {
              return finding.blocksAuto;
            });

    if (hasBlockingFinding &&
        g_installConfig.d3d9ExMode == mws::D3D9ExMode::Auto) {
      const std::string blockers =
          joinCompatibilityModules(findings, true);

      mws::Logger::instance().warn(
          "D3D9Ex Auto policy selected classic D3D9 because compatibility-sensitive modules are loaded: " +
          blockers);
      markExUnavailable(
          "Auto compatibility policy selected classic D3D9; blockers: " +
              blockers,
          false);

      d3d =
          g_originalDirect3DCreate9
              ? g_originalDirect3DCreate9(sdkVersion)
              : nullptr;

      if (d3d)
        installCreateDeviceHook(d3d);

      return d3d;
    }

    if (hasBlockingFinding &&
        g_installConfig.d3d9ExMode == mws::D3D9ExMode::Force) {
      mws::Logger::instance().warn(
          "D3D9Ex Force override: compatibility blockers detected (" +
          joinCompatibilityModules(findings, true) +
          "). Continuing with a real Ex device because Force was explicitly requested; process instability is possible.");
    }
  }

  if (g_installConfig.d3d9ExMode != mws::D3D9ExMode::Off &&
      g_direct3DCreate9Ex) {
    IDirect3D9Ex* ex = nullptr;
    const HRESULT exHr =
        g_direct3DCreate9Ex(sdkVersion, &ex);

    if (SUCCEEDED(exHr) && ex) {
      d3d = static_cast<IDirect3D9*>(ex);
      mws::Logger::instance().info(
          "D3D9Ex enumerator created through Direct3DCreate9Ex.");
    } else {
      std::ostringstream os;
      os << "Direct3DCreate9Ex failed: HRESULT=0x"
         << std::hex << std::uppercase
         << static_cast<unsigned long>(exHr);

      if (g_installConfig.d3d9ExMode == mws::D3D9ExMode::Force) {
        mws::Logger::instance().error(
            os.str() + " (Force mode; Direct3DCreate9 returns null).");
        markExUnavailable(
            "Direct3DCreate9Ex failed in Force mode",
            true);
        return nullptr;
      }

      mws::Logger::instance().warn(
          os.str() + " (Auto mode; using classic Direct3DCreate9).");
      markExUnavailable(
          "Direct3DCreate9Ex failed; Auto mode using classic D3D9",
          false);
    }
  } else if (g_installConfig.d3d9ExMode == mws::D3D9ExMode::Force) {
    mws::Logger::instance().error(
        "D3D9Ex Force mode requested but Direct3DCreate9Ex export is unavailable.");
    markExUnavailable(
        "Direct3DCreate9Ex export unavailable in Force mode",
        true);
    return nullptr;
  } else if (g_installConfig.d3d9ExMode == mws::D3D9ExMode::Auto &&
             !g_direct3DCreate9Ex) {
    markExUnavailable(
        "Direct3DCreate9Ex export unavailable; Auto mode using classic D3D9",
        false);
  }

  if (!d3d) {
    d3d =
        g_originalDirect3DCreate9
            ? g_originalDirect3DCreate9(sdkVersion)
            : nullptr;
  }

  if (d3d)
    installCreateDeviceHook(d3d);

  return d3d;
}

} // namespace

namespace mws {

bool D3D9Hook::install(const Config& config) {
  if (installed_)
    return true;

  g_installConfig = config;

  FramePacer::instance().configure(config);

  // ReFX's ASI mode can redirect GTA's GetProcAddress IAT entry straight to
  // ReFXDXVK.asi!Direct3DCreate9. Hooking system d3d9.dll, or the private
  // ReFXDXVK/d3d9.dll, never observes that factory and therefore never
  // discovers the game device or initializes the OSD.
  //
  // Hook the loaded ASI's public factory first. Do NOT load the private DXVK
  // DLL ourselves: ReFX owns its lifetime and configuration.
  HMODULE d3d9 = GetModuleHandleW(L"ReFXDXVK.asi");
  g_factoryIsReFx = d3d9 && GetProcAddress(d3d9, "Direct3DCreate9");
  if (g_factoryIsReFx) {
    Logger::instance().info(
        "ReFX ASI factory detected: hooking ReFXDXVK.asi!Direct3DCreate9 (GTA IAT bridge compatible).");
  } else {
    d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9)
      d3d9 = LoadLibraryW(L"d3d9.dll");
    if (GetModuleHandleW(L"ReFXDXVK.asi"))
      Logger::instance().warn(
          "ReFX ASI loaded without a Direct3DCreate9 export; using standard D3D9 factory.");
  }

  if (!d3d9) {
    Logger::instance().error(
        "Could not load/find d3d9.dll.");
    return false;
  }

  auto* exportAddress =
      GetProcAddress(d3d9, "Direct3DCreate9");
  g_direct3DCreate9Ex =
      reinterpret_cast<Direct3DCreate9ExFn>(
          GetProcAddress(d3d9, "Direct3DCreate9Ex"));

  if (!exportAddress) {
    Logger::instance().error(
        "Direct3DCreate9 export not found.");
    return false;
  }

  auto st = MH_CreateHook(
      reinterpret_cast<void*>(exportAddress),
      reinterpret_cast<void*>(&HookDirect3DCreate9),
      reinterpret_cast<void**>(&g_originalDirect3DCreate9));

  if (st != MH_OK) {
    Logger::instance().error(
        st == MH_ERROR_ALREADY_CREATED
            ? "D3D9 factory conflict: another MinHook already owns Direct3DCreate9; "
              "cannot safely adopt its trampoline."
            : "Failed to create Direct3DCreate9 hook.");
    return false;
  }

  st = MH_EnableHook(
      reinterpret_cast<void*>(exportAddress));

  if (st != MH_OK && st != MH_ERROR_ENABLED) {
    Logger::instance().error(
        "Failed to enable Direct3DCreate9 hook.");
    return false;
  }

  g_d3d9ExportHookAddress = reinterpret_cast<void*>(exportAddress);
  installed_ = true;

  if (config.d3d9ExMode != D3D9ExMode::Off) {
    Logger::instance().info(
        g_direct3DCreate9Ex
            ? "D3D9Ex capability: Direct3DCreate9Ex export available."
            : "D3D9Ex capability: Direct3DCreate9Ex export unavailable; Auto will use classic D3D9.");
  }

  std::ostringstream os;
  os << "Direct3DCreate9 hook installed at 0x"
     << std::hex << std::uppercase
     << reinterpret_cast<std::uintptr_t>(exportAddress);
  Logger::instance().info(os.str());

  Logger::instance().info(
      "D3D9 factory hook armed; awaiting the game device.");
  return true;
}

void D3D9Hook::uninstall() {
  captureDiagnostics();
  g_distantStreamingPresentOnly = false;
  RenderSubmission::flush();
  OptimizationReport::finish();
  EngineOptimizations::uninstall();
  RenderWareBatcher::uninstall();
  PassProfiler::shutdown();
  RenderSubmission::uninstall();
  D3D9StateCache::instance().uninstall();
  for (void* address : {g_createDeviceHookAddress, g_presentHookAddress, g_resetHookAddress, g_endSceneHookAddress}) {
    if (address) { MH_DisableHook(address); MH_RemoveHook(address); }
  }
  if (g_d3d9ExportHookAddress) {
    MH_DisableHook(g_d3d9ExportHookAddress);
    MH_RemoveHook(g_d3d9ExportHookAddress);
    g_d3d9ExportHookAddress = nullptr;
  }
  releaseActiveDeviceEx();
  g_activeDevice = nullptr;
  g_useExRuntime = false;
  g_createDeviceHookAddress = g_presentHookAddress = g_resetHookAddress = nullptr;
  g_originalCreateDevice = nullptr;
  g_endSceneHookAddress = nullptr;
  g_originalEndScene = nullptr;
  g_overlayDrawn = false;
  g_overlayRendering = false;
  g_refxOverlayOnly = false;
  g_originalPresent = nullptr;
  g_originalReset = nullptr;
  g_originalDirect3DCreate9 = nullptr;
  g_direct3DCreate9Ex = nullptr;
  g_createDeviceHookInstalled = g_presentHookInstalled = g_resetHookInstalled = false;
  FramePacer::instance().shutdown();
  installed_ = false;
  g_factoryIsReFx = false;
  Logger::instance().info("D3D9 optimizer hooks shut down.");
}
void D3D9Hook::captureDiagnostics() {
  Diagnostics::instance().activity("D3D9.DevicePresent",g_presentCalls,g_presentSuccesses,0,0,g_presentCalls-g_presentSuccesses);
}
} // namespace mws
