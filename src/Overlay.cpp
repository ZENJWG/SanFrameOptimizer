#include "mws/Overlay.hpp"
#include "mws/OverlayFont.hpp"
#include "mws/DxvkTuner.hpp"
#include "mws/DrawTelemetry.hpp"
#include "mws/ReFXNativeBackendInfo.hpp"
#include "mws/RendererBackendSelection.hpp"
#include <Psapi.h>
#include "mws/Diagnostics.hpp"
#include "mws/Logger.hpp"
#include "mws/StreamingMemoryTelemetry.hpp"
#include "mws/GtaSa10US.hpp"
#include "resource.h"

#include <Windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>

#pragma comment(lib, "version.lib")

#include <array>

#include <cstdio>

#include <cstring>

#include <string_view>

namespace {




std::wstring lowerCopy(std::wstring v) {
  std::transform(
      v.begin(), v.end(), v.begin(),
      [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
      });
  return v;
}

bool containsModuleName(std::wstring_view name, std::wstring_view part) {
  return std::search(name.begin(), name.end(), part.begin(), part.end(),
      [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }) != name.end();
}


bool looksLikeDxvkVersion(const std::wstring& value) {
  if (value.empty())
    return false;

  // Reject Windows-style resource versions such as:
  // "10.0.17763.1 (WinBuild.160101.0800)"
  if (lowerCopy(value).find(L"winbuild") != std::wstring::npos)
    return false;

  std::wstring v = value;
  if (!v.empty() && (v[0] == L'v' || v[0] == L'V'))
    v.erase(v.begin());

  int dots = 0;
  bool sawDigit = false;

  for (wchar_t c : v) {
    if (c >= L'0' && c <= L'9') {
      sawDigit = true;
      continue;
    }

    if (c == L'.') {
      ++dots;
      continue;
    }

    if (c == L'-' || c == L'+' || c == L' ' ||
        (c >= L'a' && c <= L'z') ||
        (c >= L'A' && c <= L'Z')) {
      continue;
    }

    return false;
  }

  return sawDigit && dots >= 1 && dots <= 3;
}

} // namespace

namespace mws {

Overlay& Overlay::instance() {
  static Overlay overlay;
  return overlay;
}

void Overlay::setReFxDxvkDevice(IDirect3DDevice9* device) noexcept {
  refxDxvkDevice_ = device;
  lastRendererRefreshMs_ = 0;
  rendererDetected_ = nativeRendererDetected_ = false;
}

void Overlay::configure(const Config& config) {
  config_ = config;
  QueryPerformanceFrequency(&qpcFreq_);
  configured_ = true;
  gpuMemory_.start(config_.overlayEnabled &&
      (config_.overlayShowGpuMemory || config_.overlayShowSharedGpuMemory));
  cache64Memory_.start(config_.overlayEnabled && config_.overlayShowCache64Memory);
  usage_.start(config_.overlayEnabled && config_.overlayShowCpuUsage,
      config_.overlayEnabled && config_.overlayShowGpuUsage,
      config_.overlayEnabled && config_.overlayShowProcessCpuUsage,
      config_.overlayEnabled && config_.overlayShowThreadUsage);

  std::ostringstream os;
  os << "Overlay configured:"
     << " enabled=" << (config_.overlayEnabled ? 1 : 0)
     << ", api=" << (config_.overlayShowApi ? 1 : 0)
     << ", gpu=" << (config_.overlayShowGpu ? 1 : 0)
     << ", cpuUsage=" << (config_.overlayShowCpuUsage ? 1 : 0)
     << ", gpuUsage=" << (config_.overlayShowGpuUsage ? 1 : 0)
     << ", gpuMemory=" << (config_.overlayShowGpuMemory ? 1 : 0)
     << ", sharedGpuMemory=" << (config_.overlayShowSharedGpuMemory ? 1 : 0)
     << ", drawCalls=" << config_.overlayShowDrawCalls
     << ", primitives=" << config_.overlayShowPrimitives
     << ", processCpu=" << config_.overlayShowProcessCpuUsage
     << ", threadUsage=" << config_.overlayShowThreadUsage
     << ", threadMode=" << config_.overlayShowThreadMode
     << ", fps=" << (config_.overlayShowFps ? 1 : 0)
     << ", frametime=" << (config_.overlayShowFrameTime ? 1 : 0)
     << ", streammem=" << (config_.overlayShowStreamingMemory ? 1 : 0)
     << ", cache64Memory=" << (config_.overlayShowCache64Memory ? 1 : 0)
     << ", clock=" << (config_.overlayShowClock ? 1 : 0)
     << ", date=" << (config_.overlayShowDate ? 1 : 0);
  Logger::instance().info(os.str());
}

std::string Overlay::narrow(const std::wstring& value) {
  if (value.empty())
    return {};

  const int bytes =
      WideCharToMultiByte(
          CP_UTF8, 0,
          value.c_str(),
          static_cast<int>(value.size()),
          nullptr, 0, nullptr, nullptr);

  if (bytes <= 0)
    return {};

  std::string result(static_cast<std::size_t>(bytes), '\0');

  WideCharToMultiByte(
      CP_UTF8, 0,
      value.c_str(),
      static_cast<int>(value.size()),
      result.data(), bytes, nullptr, nullptr);

  return result;
}

bool Overlay::containsInsensitive(
    const std::wstring& haystack,
    const std::wstring& needle) {
  return lowerCopy(haystack).find(lowerCopy(needle)) != std::wstring::npos;
}

std::wstring Overlay::loadedD3D9Path() const {
  HMODULE d3d9 = nullptr;
  // GetDirect3D is not detoured by SanFrame. Resolve the implementation owning
  // this device instead of the ambiguous first module named d3d9.dll.
  if (device_) {
    auto** vtable = *reinterpret_cast<void***>(device_);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(vtable[6]), &d3d9);
  } else {
    d3d9 = GetModuleHandleW(L"d3d9.dll");
  }
  if (!d3d9)
    return {};

  std::vector<wchar_t> path(32768);
  const DWORD len =
      GetModuleFileNameW(
          d3d9,
          path.data(),
          static_cast<DWORD>(path.size()));

  if (len == 0 || len >= path.size())
    return {};

  return std::wstring(path.data(), len);
}

bool Overlay::isSystemD3D9(const std::wstring& path) const {
  if (path.empty())
    return false;

  wchar_t systemDir[32768]{};
  const UINT len =
      GetSystemDirectoryW(
          systemDir,
          static_cast<UINT>(std::size(systemDir)));

  if (len == 0 || len >= std::size(systemDir))
    return false;

  std::error_code ec;
  const auto modulePath =
      std::filesystem::weakly_canonical(path, ec);

  ec.clear();
  const auto sysPath =
      std::filesystem::weakly_canonical(systemDir, ec);

  if (ec)
    return containsInsensitive(path, L"\\windows\\system32\\") ||
           containsInsensitive(path, L"\\windows\\syswow64\\");

  const auto parent = lowerCopy(modulePath.parent_path().wstring());
  const auto system = lowerCopy(sysPath.wstring());

  // 32-bit process on 64-bit Windows may transparently see SysWOW64 through
  // GetSystemDirectory depending on WOW64 redirection state.
  return parent == system ||
         containsInsensitive(path, L"\\windows\\system32\\d3d9.dll") ||
         containsInsensitive(path, L"\\windows\\syswow64\\d3d9.dll");
}

std::wstring Overlay::fileVersionField(
    const std::wstring& filePath,
    const wchar_t* key) const {
  if (filePath.empty() || !key)
    return {};

  DWORD handle = 0;
  const DWORD size =
      GetFileVersionInfoSizeW(filePath.c_str(), &handle);

  if (!size)
    return {};

  std::vector<std::uint8_t> data(size);

  if (!GetFileVersionInfoW(
          filePath.c_str(), 0, size, data.data())) {
    return {};
  }

  struct LangAndCodePage {
    WORD language;
    WORD codePage;
  };

  LangAndCodePage* translation = nullptr;
  UINT translationBytes = 0;

  if (!VerQueryValueW(
          data.data(),
          L"\\VarFileInfo\\Translation",
          reinterpret_cast<LPVOID*>(&translation),
          &translationBytes) ||
      !translation ||
      translationBytes < sizeof(LangAndCodePage)) {
    return {};
  }

  wchar_t query[256]{};
  swprintf_s(
      query,
      L"\\StringFileInfo\\%04x%04x\\%s",
      translation[0].language,
      translation[0].codePage,
      key);

  wchar_t* text = nullptr;
  UINT chars = 0;

  if (!VerQueryValueW(
          data.data(),
          query,
          reinterpret_cast<LPVOID*>(&text),
          &chars) ||
      !text ||
      chars == 0) {
    return {};
  }

  return std::wstring(text);
}

bool Overlay::refreshNativeRenderer() {
  const ULONGLONG now = GetTickCount64();
  if (lastRendererRefreshMs_ && now - lastRendererRefreshMs_ < 1000)
    return nativeRendererDetected_;
  lastRendererRefreshMs_ = now;

  HMODULE modules[1024]{};
  DWORD bytes = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
    return nativeRendererDetected_;
  const size_t count = std::min(size_t(bytes / sizeof(HMODULE)), std::size(modules));
  const auto& dxvk = DxvkTuner::instance();
  const auto devicePath = loadedD3D9Path();
  std::string deviceVersion;
  const bool dxvkDevice = device_ && dxvk.identifyDxvk(devicePath, deviceVersion);
  // A ReFX wrapper's vtable may point into the ASI, not DXVK itself.
  const bool refxWrappedDevice = device_ && device_ == refxDxvkDevice_;
  bool refxLoaded = false;
  bool telemetryAvailable = false;
  ReFXNativeBackendInfoV1 nativeInfo{};
  std::filesystem::path loadedDxvk;
  std::string loadedVersion;
  bool privateDxvk = false;
  // GetModuleFileNameW writes the returned path itself. Clearing 64 KiB for
  // every loaded module and allocating lowercased paths is unnecessary here.
  wchar_t path[32768];
  for (size_t i = 0; i < count; ++i) {
    const DWORD length = GetModuleFileNameW(modules[i], path, static_cast<DWORD>(std::size(path)));
    if (length && length < std::size(path)) {
      const std::wstring_view fullPath(path, length);
      const auto slash = fullPath.find_last_of(L"\\/");
      const auto filename = slash == std::wstring_view::npos ? fullPath : fullPath.substr(slash + 1);
      const auto dot = filename.find_last_of(L'.');
      const auto extension = dot == std::wstring_view::npos ? std::wstring_view{} : filename.substr(dot);
      refxLoaded |= containsModuleName(filename, L"refx") &&
          ((extension.size() == 4 && _wcsnicmp(extension.data(), L".asi", 4) == 0) ||
           GetProcAddress(modules[i], "Direct3DCreate9") != nullptr);
      if (_wcsicmp(filename.data(), L"d3d9.dll") == 0) {
        const std::filesystem::path modulePath(path);
        std::string version;
        if (dxvk.identifyDxvk(modulePath, version)) {
          const bool isPrivate = dxvk.isReFxDxvk(modulePath);
          if (loadedDxvk.empty() || isPrivate) {
            loadedDxvk = modulePath;
            loadedVersion = version;
            privateDxvk = isPrivate;
          }
        }
      }
    }
    auto query = reinterpret_cast<ReFXQueryNativeBackendInfoFn>(
        GetProcAddress(modules[i], "ReFX_QueryNativeBackendInfo"));
    if (!query) continue;
    refxLoaded = true;
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(query), &retained)) continue;
    ReFXNativeBackendInfoV1 info{};
    const bool valid = query(sizeof(info), &info) != 0 &&
        info.structSize == sizeof(info) && info.abiVersion == 1;
    FreeLibrary(retained);
    if (!valid) continue;
    // Prefer a provider with visible native presentation over an idle provider.
    if (!telemetryAvailable || ((info.flags & 3u) == 3u && info.presentedFrames))
      nativeInfo = info;
    telemetryAvailable = true;
  }

  const bool nativeLive = telemetryAvailable && (nativeInfo.flags & 1u) &&
      nativeInfo.vulkanDevice && nativeInfo.apiVersion;
  const bool nativePresenting = nativeLive && (nativeInfo.flags & 2u) &&
      nativeInfo.presentedFrames;
  // A captured ReFX factory alone does not establish the renderer mode:
  // require the matching private DXVK DLL to be loaded into this process.
  const bool verifiedRefxDxvk = refxWrappedDevice && privateDxvk;
  const auto backend = selectRendererBackend(nativeLive, nativePresenting,
      dxvkDevice || verifiedRefxDxvk, !loadedDxvk.empty(), refxLoaded);
  std::string text;
  if (backend == RendererBackend::NativeVulkan) {
    std::ostringstream api;
    api << "Vulkan " << ((nativeInfo.apiVersion >> 22) & 0x7Fu) << '.'
        << ((nativeInfo.apiVersion >> 12) & 0x3FFu) << '.' << (nativeInfo.apiVersion & 0xFFFu)
        << (nativePresenting ? " (ReFX native)" : " (ReFX native initialized; presentation unverified)");
    text = api.str();
    nativeInfo.gpuName[255] = '\0';
    gpuText_ = nativeInfo.gpuName;
    std::ostringstream driver;
    driver << "Vulkan driver 0x" << std::hex << nativeInfo.driverVersion;
    driverText_ = driver.str();
    d3d9PathText_.clear();
  } else if (backend == RendererBackend::Dxvk) {
    const bool refxDxvk = verifiedRefxDxvk ||
        (dxvkDevice ? dxvk.isReFxDxvk(devicePath) : privateDxvk);
    text = refxDxvk ? "ReFX DXVK / Vulkan" : "D3D9 via DXVK / Vulkan";
    const auto& version = dxvkDevice ? deviceVersion : loadedVersion;
    if (!version.empty()) text += " (" + version + ")";
    if (!dxvkDevice && !verifiedRefxDxvk)
      text += " (DLL loaded; device unverified)";
    text += " / ";
    text += dxvk.tuningLabel(refxDxvk);
    d3d9PathText_ = dxvkDevice ? narrow(devicePath) : narrow(loadedDxvk.wstring());
    updateD3D9AdapterInfo(device_);
  } else if (backend == RendererBackend::RefxUnverified) {
    text = "ReFX renderer (active backend unverified)";
    updateD3D9AdapterInfo(device_);
    d3d9PathText_ = narrow(devicePath);
  } else {
    if (nativeRendererDetected_) {
      rendererDetected_ = false;
      gpuText_.clear(); driverText_.clear(); d3d9PathText_.clear();
    }
    nativeRendererDetected_ = false;
    return false;
  }
  if (!nativeRendererDetected_ || apiText_ != text) {
    Logger::instance().info("Renderer detected: " + text + ", d3d9=" + d3d9PathText_);
    Diagnostics::instance().active("Overlay.RenderAPI", text);
  }
  apiText_ = text;
  nativeRendererDetected_ = rendererDetected_ = true;
  return true;
}

void Overlay::updateD3D9AdapterInfo(IDirect3DDevice9* device) {
  gpuText_.clear(); driverText_.clear();
  if (!device) return;
  IDirect3D9* d3d = nullptr;
  if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(device->GetCreationParameters(&cp))) {
      D3DADAPTER_IDENTIFIER9 id{};
      if (SUCCEEDED(
              d3d->GetAdapterIdentifier(
                  cp.AdapterOrdinal,
                  0,
                  &id))) {
        gpuText_ = id.Description;

        std::ostringstream driver;
        driver << id.Driver;

        const std::uint32_t hi =
            static_cast<std::uint32_t>(
                (id.DriverVersion.QuadPart >> 32) & 0xffffffffULL);
        const std::uint32_t lo =
            static_cast<std::uint32_t>(
                id.DriverVersion.QuadPart & 0xffffffffULL);

        driver
            << " "
            << HIWORD(hi) << "."
            << LOWORD(hi) << "."
            << HIWORD(lo) << "."
            << LOWORD(lo);

        driverText_ = driver.str();
      }
    }

    d3d->Release();
  }

}

void Overlay::detectRenderer(IDirect3DDevice9* device) {
  if (refreshNativeRenderer()) return;
  if (rendererDetected_ || !device)
    return;

  rendererDetected_ = true;

  const std::wstring d3dPath = loadedD3D9Path();
  const std::wstring product =
      fileVersionField(d3dPath, L"ProductName");
  const std::wstring description =
      fileVersionField(d3dPath, L"FileDescription");
  const std::wstring company =
      fileVersionField(d3dPath, L"CompanyName");
  const std::wstring version =
      fileVersionField(d3dPath, L"FileVersion");

  const bool metadataSaysDxvk =
      containsInsensitive(product, L"dxvk") ||
      containsInsensitive(description, L"dxvk") ||
      containsInsensitive(company, L"dxvk");

  const bool systemD3D9 = isSystemD3D9(d3dPath);
  const bool vulkanLoaded =
      GetModuleHandleW(L"vulkan-1.dll") != nullptr;

  if (metadataSaysDxvk) {
    apiText_ = "D3D9 via DXVK / Vulkan";

    if (looksLikeDxvkVersion(version))
      apiText_ += " (" + narrow(version) + ")";
  } else if (!systemD3D9 && vulkanLoaded) {
    apiText_ =
        "D3D9 wrapper (Vulkan loader present; backend unverified)";
  } else if (!systemD3D9) {
    apiText_ = "D3D9 via non-system d3d9.dll wrapper";
  } else {
    apiText_ = "Direct3D 9 (native Windows runtime)";
  }

  d3d9PathText_ = narrow(d3dPath);

  updateD3D9AdapterInfo(device);

  std::ostringstream os;
  os << "Renderer detected: " << apiText_;

  if (!gpuText_.empty())
    os << ", GPU=" << gpuText_;

  if (!d3d9PathText_.empty())
    os << ", d3d9=" << d3d9PathText_;

  Logger::instance().info(os.str());
}

float Overlay::queryWindowDpiScale(HWND hwnd) const {
  if (!hwnd || !config_.overlayUseDpiScale)
    return 1.0f;

  // Query DPI without changing GTA's process/thread DPI-awareness state.
  // GetDpiForWindow is safe to dynamically resolve on Windows 10+.
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

  static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  static const auto getDpiForWindow =
      reinterpret_cast<GetDpiForWindowFn>(
          user32
              ? GetProcAddress(user32, "GetDpiForWindow")
              : nullptr);

  if (getDpiForWindow) {
    const UINT dpi = getDpiForWindow(hwnd);
    if (dpi >= 48 && dpi <= 768)
      return static_cast<float>(dpi) / 96.0f;
  }

  // Fallback: monitor DC logical DPI. This does not mutate process DPI mode.
  HMONITOR monitor =
      MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);

  if (monitor && GetMonitorInfoW(monitor, &mi)) {
    HDC dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (dc) {
      const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
      DeleteDC(dc);

      if (dpi >= 48 && dpi <= 768)
        return static_cast<float>(dpi) / 96.0f;
    }
  }

  return 1.0f;
}

float Overlay::computeEffectiveScale(
    HWND hwnd,
    const D3DPRESENT_PARAMETERS& pp,
    UINT viewportWidth,
    UINT viewportHeight) const {
  (void)viewportWidth;
  const bool useDpi = config_.overlayScaleMode == OverlayScaleMode::Auto ||
                      config_.overlayScaleMode == OverlayScaleMode::DpiOnly;
  const bool usePresentation = config_.overlayScaleMode == OverlayScaleMode::Auto ||
                               config_.overlayScaleMode == OverlayScaleMode::ResolutionOnly;
  const float dpiScale =
      useDpi && config_.overlayUseDpiScale
          ? queryWindowDpiScale(hwnd)
          : 1.0f;

  const float resolutionScale =
      config_.overlayUseResolutionScale &&
              config_.overlayReferenceHeight > 0 &&
              viewportHeight > 0
          ? static_cast<float>(viewportHeight) /
                static_cast<float>(config_.overlayReferenceHeight)
          : 1.0f;

  float presentationScale = 1.0f;

  if (usePresentation && config_.overlayUsePresentationScale &&
      hwnd &&
      pp.BackBufferWidth > 0 &&
      pp.BackBufferHeight > 0) {
    RECT client{};
    if (GetClientRect(hwnd, &client)) {
      const int cw = client.right - client.left;
      const int ch = client.bottom - client.top;

      if (cw > 0 && ch > 0) {
        const float sx =
            static_cast<float>(pp.BackBufferWidth) /
            static_cast<float>(cw);
        const float sy =
            static_cast<float>(pp.BackBufferHeight) /
            static_cast<float>(ch);

        // Use the smaller axis so aspect-ratio differences do not over-scale.
        presentationScale = std::max(1.0f, std::min(sx, sy));
      }
    }
  }

  float automatic = 1.0f;

  switch (config_.overlayScaleMode) {
    case OverlayScaleMode::Manual:
      automatic = config_.overlayManualScale;
      break;

    case OverlayScaleMode::DpiOnly:
      automatic = dpiScale;
      break;

    case OverlayScaleMode::ResolutionOnly:
      automatic = std::max(resolutionScale, presentationScale);
      break;

    case OverlayScaleMode::Auto:
    default:
      // Pick the strongest applicable scale instead of multiplying them.
      // Multiplication would over-scale a 4K screen at 200% DPI (4x).
      automatic =
          std::max(
              dpiScale,
              std::max(resolutionScale, presentationScale));
      break;
  }

  automatic *= config_.overlayScale;

  return std::clamp(
      automatic,
      config_.overlayMinScale,
      config_.overlayMaxScale);
}

void Overlay::applyRuntimeScale(float effectiveScale) {
  if (!imguiInitialized_)
    return;

  effectiveScale =
      std::clamp(
          effectiveScale,
          config_.overlayMinScale,
          config_.overlayMaxScale);

  if (std::abs(effectiveScale - currentEffectiveScale_) < 0.01f)
    return;

  currentEffectiveScale_ = effectiveScale;

  ImGuiStyle& style = ImGui::GetStyle();

  if (baseStyleCaptured_) {
    style = baseStyle_;
    style.ScaleAllSizes(effectiveScale);
  }

  // Dear ImGui 1.92 separates the application's main font scale from DPI.
  // We use FontScaleMain so the scalable TTF remains sharp without rebuilding
  // the font atlas every time the window moves between monitors.
  style.FontScaleMain = effectiveScale;

  std::ostringstream os;
  os << std::fixed << std::setprecision(2)
     << "Overlay scale updated: effective=" << effectiveScale
     << "x";
  Logger::instance().info(os.str());
}

void Overlay::initializeImGui(IDirect3DDevice9* device) {
  if (imguiInitialized_ || !device || !config_.overlayEnabled)
    return;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  const float fontPixels = config_.overlayFontSize;

  // Resolve the module by code address: ASI renaming must not lose resources.
  const auto self = OverlayFont::containingModule(reinterpret_cast<const void*>(&Overlay::instance));
  wchar_t modulePath[MAX_PATH]{};
  if(self) GetModuleFileNameW(self,modulePath,MAX_PATH);
  const auto resource=self?FindResourceW(self,MAKEINTRESOURCEW(IDR_BANKGOTHIC_BOLD_TTF),MAKEINTRESOURCEW(10)):nullptr;
  const auto resourceBytes=resource?SizeofResource(self,resource):0;
  std::ostringstream fontSource;
  fontSource<<"Overlay font source: module="<<std::filesystem::path(modulePath).string()
      <<" resource="<<IDR_BANKGOTHIC_BOLD_TTF<<" bytes="<<resourceBytes;
  Logger::instance().info(fontSource.str());
  font_ = OverlayFont::addEmbedded(*io.Fonts, self, IDR_BANKGOTHIC_BOLD_TTF, fontPixels);
  embeddedFont_=font_!=nullptr;
  fontDrawReported_=false;
  if (font_) {
    Logger::instance().info("Overlay: embedded BankGothic Bold loaded as default HUD font.");
    Diagnostics::instance().active("Overlay.EmbeddedFont","BankGothic resource loaded; awaiting an OSD draw");
  } else {
    font_ = OverlayFont::addFallback(*io.Fonts, fontPixels);
    Logger::instance().warn("Overlay: embedded font unavailable; using scalable ImGui fallback at configured size.");
    Diagnostics::instance().skipped("Overlay.EmbeddedFont",resourceBytes?
        "embedded font could not be added; using fallback":"loaded ASI has no BankGothic resource; use the font-embedded release ASI");
  }
  io.FontDefault = font_;

  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(10.0f, 8.0f);
  style.WindowRounding = 6.0f;
  style.WindowBorderSize = 1.0f;
  style.ItemSpacing = ImVec2(6.0f, 3.0f);

  baseStyle_ = style;
  baseStyleCaptured_ = true;
  currentEffectiveScale_ = 1.0f;

  if (!ImGui_ImplDX9_Init(device)) {
    Logger::instance().error(
        "Overlay: ImGui D3D9 backend initialization failed.");
    ImGui::DestroyContext();
    font_ = nullptr;
    return;
  }

  imguiInitialized_ = true;
  Logger::instance().info(
      "Overlay: Dear ImGui D3D9 renderer initialized.");
  Diagnostics::instance().active("Overlay.Renderer", "Dear ImGui D3D9 renderer initialized");
}

void Overlay::onDeviceCreated(
    IDirect3DDevice9* device,
    const D3DPRESENT_PARAMETERS&) {
  if (!configured_ || !config_.overlayEnabled || !device)
    return;

  if (imguiInitialized_ && device_ != device) {
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized_ = false;
    font_ = nullptr;
    runtimeScaleInitialized_ = false;
    Logger::instance().info("Overlay: presentation device changed; rebuilding OSD resources.");
  }
  if (device_ != device) {
    rendererDetected_ = nativeRendererDetected_ = false;
    lastRendererRefreshMs_ = 0;
  }
  device_ = device;
  gpuMemory_.setDeviceAdapter(device);
  if (config_.overlayShowDrawCalls || config_.overlayShowPrimitives) {
    if (!DrawTelemetry::bind(device))
      Logger::instance().warn("OSD draw telemetry: device hooks unavailable.");
  }
  detectRenderer(device);
  initializeImGui(device);
}

void Overlay::beforeReset() {
  if (imguiInitialized_)
    ImGui_ImplDX9_InvalidateDeviceObjects();
}

void Overlay::afterReset(
    IDirect3DDevice9* device,
    const D3DPRESENT_PARAMETERS&,
    HRESULT resetResult) {
  lastRendererRefreshMs_ = 0;
  if (!imguiInitialized_ || FAILED(resetResult))
    return;

  device_ = device;
  ImGui_ImplDX9_CreateDeviceObjects();
}

void Overlay::updateTiming() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);

  if (qpcFreq_.QuadPart <= 0)
    return;

  if (lastPresentQpc_ != 0) {
    frameTimeMs_ =
        (static_cast<double>(now.QuadPart - lastPresentQpc_) * 1000.0) /
        static_cast<double>(qpcFreq_.QuadPart);

    if (smoothedFrameTimeMs_ <= 0.0)
      smoothedFrameTimeMs_ = frameTimeMs_;
    else
      smoothedFrameTimeMs_ =
          smoothedFrameTimeMs_ * 0.90 +
          frameTimeMs_ * 0.10;
  }

  lastPresentQpc_ = now.QuadPart;

  if (fpsWindowStartQpc_ == 0)
    fpsWindowStartQpc_ = now.QuadPart;

  ++fpsWindowFrames_;

  const double windowSeconds =
      static_cast<double>(now.QuadPart - fpsWindowStartQpc_) /
      static_cast<double>(qpcFreq_.QuadPart);

  if (windowSeconds >= 0.50) {
    fps_ =
        static_cast<double>(fpsWindowFrames_ - 1) /
        windowSeconds;

    fpsWindowStartQpc_ = now.QuadPart;
    fpsWindowFrames_ = 1;
  }
}

void Overlay::buildClockStrings() {
  if (!config_.overlayShowClock && !config_.overlayShowDate)
    return;
  const ULONGLONG nowMs = GetTickCount64();
  if (lastClockUpdateMs_ != 0 && nowMs - lastClockUpdateMs_ < 100)
    return;
  lastClockUpdateMs_ = nowMs;
  SYSTEMTIME local{};
  GetLocalTime(&local);

  unsigned hour12 = local.wHour % 12;
  if (hour12 == 0)
    hour12 = 12;

  const char* ampm =
      local.wHour < 12 ? "AM" : "PM";

  char clock[64]{};
  if (config_.overlayShowSeconds) {
    sprintf_s(
        clock,
        "%u:%02u:%02u %s",
        hour12,
        local.wMinute,
        local.wSecond,
        ampm);
  } else {
    sprintf_s(
        clock,
        "%u:%02u %s",
        hour12,
        local.wMinute,
        ampm);
  }

  char date[64]{};
  sprintf_s(
      date,
      "%04u-%02u-%02u",
      local.wYear,
      local.wMonth,
      local.wDay);

  clockText_ = clock;
  dateText_ = date;
}

void Overlay::render(
    IDirect3DDevice9* device,
    const D3DPRESENT_PARAMETERS& pp) {
  if (!configured_ || !config_.overlayEnabled || !device)
    return;

  // ReFX can replace the canonical public facade after a helper-device
  // creation. Rebind resources to the actual device used for this frame.
  if (!imguiInitialized_ || device_ != device) {
    onDeviceCreated(device, pp);
    if (!imguiInitialized_)
      return;
  }

  if (device->TestCooperativeLevel() != D3D_OK)
    return;

  detectRenderer(device);
  if (config_.overlayShowThreadUsage)
    usage_.setRenderThread(GetCurrentThreadId());
  updateTiming();
  buildClockStrings();

  D3DVIEWPORT9 viewport{};
  if (FAILED(device->GetViewport(&viewport)))
    return;

  HWND hwnd = pp.hDeviceWindow;
  if (!hwnd) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(device->GetCreationParameters(&cp)))
      hwnd = cp.hFocusWindow;
  }

  if (config_.overlayDynamicDpi || !runtimeScaleInitialized_) {
    applyRuntimeScale(computeEffectiveScale(hwnd, pp, viewport.Width, viewport.Height));
    runtimeScaleInitialized_ = true;
  }

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize =
      ImVec2(
          static_cast<float>(viewport.Width),
          static_cast<float>(viewport.Height));

  if (qpcFreq_.QuadPart > 0 && frameTimeMs_ > 0.0) {
    io.DeltaTime =
        static_cast<float>(
            std::clamp(frameTimeMs_ / 1000.0, 0.0001, 0.25));
  } else {
    io.DeltaTime = 1.0f / 60.0f;
  }

  ImGui_ImplDX9_NewFrame();
  ImGui::NewFrame();

  if (font_)
    ImGui::PushFont(font_, config_.overlayFontSize);

  // Fixed local storage avoids heap allocation and remains available when
  // the optional transient arena is disabled or exhausted.
  std::array<std::array<char, 256>, 20> lineStorage;
  std::array<const char*, 20> lines{};
  std::size_t lineCount = 0;

  auto pushText =
      [&](std::string_view text) {
        if (lineCount >= lines.size())
          return;

        auto* dst = lineStorage[lineCount].data();
        const auto count = std::min(text.size(), lineStorage[lineCount].size() - 1);
        std::memcpy(dst, text.data(), count);
        dst[count] = '\0';
        lines[lineCount++] = dst;
      };

  auto pushFormatted =
      [&](const char* format, auto... args) {
        char temp[256]{};
        const int n =
            std::snprintf(
                temp,
                sizeof(temp),
                format,
                args...);

        if (n > 0)
          pushText(
              std::string_view(
                  temp,
                  static_cast<std::size_t>(
                      std::min<int>(
                          n,
                          static_cast<int>(sizeof(temp) - 1)))));
      };

  if (config_.overlayShowTitle)
    pushText("SanFrame Optimizer");

  if (config_.overlayShowApi && !apiText_.empty())
    pushText(apiText_);

  if (config_.overlayShowGpu && !gpuText_.empty())
    pushFormatted("GPU: %s", gpuText_.c_str());

  if (config_.overlayShowCpuUsage) {
    const double percent = usage_.cpuPercent();
    if (percent >= 0.0)
      pushFormatted("CPU usage (system): %.1f%%", percent);
    else
      pushText("CPU usage (system): unavailable");
  }

  if (config_.overlayShowGpuUsage) {
    const double percent = usage_.gpuPercent();
    if (percent >= 0.0)
      pushFormatted("GPU usage (busiest): %.1f%%", percent);
    else
      pushText("GPU usage (busiest): unavailable");
  }

  // Process memory on GTA's selected physical GPU, not GPU utilization,
  // GTA's CPU streaming pool, or the helper's RAM. DXGI is sampled off-thread.
  if (config_.overlayShowGpuMemory || config_.overlayShowSharedGpuMemory) {
    const auto mem = gpuMemory_.sample();
    constexpr double MiB = 1024.0 * 1024.0;
    if (!mem.valid) {
      pushText("GPU memory (GTA): unavailable (WDDM/DXGI adapter)");
    } else {
      if (config_.overlayShowGpuMemory)
        pushFormatted("VRAM (GTA): %.0f / %.0f MiB (Windows budget)",
            static_cast<double>(mem.localUsageBytes) / MiB,
            static_cast<double>(mem.localBudgetBytes) / MiB);
      if (config_.overlayShowSharedGpuMemory)
        pushFormatted("Shared GPU (GTA): %.0f / %.0f MiB (Windows budget)",
            static_cast<double>(mem.nonLocalUsageBytes) / MiB,
            static_cast<double>(mem.nonLocalBudgetBytes) / MiB);
    }
  }

  if (config_.overlayShowProcessCpuUsage || config_.overlayShowThreadUsage) {
    const auto cpu = usage_.threadSample();
    if (config_.overlayShowProcessCpuUsage) {
      if (cpu.processCores >= 0.0)
        pushFormatted("CPU (game, all threads): %.2f cores", cpu.processCores);
      else
        pushText("CPU (game, all threads): unavailable");
    }
    if (config_.overlayShowThreadUsage) {
      if (cpu.renderPercent >= 0.0)
        pushFormatted("CPU threads: render %.1f%% | others %.2f cores", cpu.renderPercent, cpu.otherCores);
      else
        pushText("CPU threads: unavailable (collecting render thread)");
    }
  }
  if (config_.overlayShowOptimizationStatus) {
    pushText(Diagnostics::instance().optimizationAvailability(false));
  }
  if (config_.overlayShowThreadMode) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (SUCCEEDED(device->GetCreationParameters(&creation)))
      pushFormatted("D3D9 threading: %s", (creation.BehaviorFlags & D3DCREATE_MULTITHREADED)
          ? "multithread safe" : "single-thread device");
    else
      pushText("D3D9 threading: unavailable");
  }
  if (config_.overlayShowDrawCalls || config_.overlayShowPrimitives) {
    const auto draw = DrawTelemetry::sample();
    if (config_.overlayShowDrawCalls) {
      if (draw.available && draw.ready)
        pushFormatted("Draw calls: %.0f/frame | %.0f/sec", draw.callsPerFrame, draw.callsPerSecond);
      else
        pushText(draw.available ? "Draw calls: collecting" : "Draw calls: unavailable");
    }
    if (config_.overlayShowPrimitives) {
      if (draw.available && draw.ready)
        pushFormatted("D3D primitives: %.0f/frame", draw.primitivesPerFrame);
      else
        pushText(draw.available ? "D3D primitives: collecting" : "D3D primitives: unavailable");
    }
  }

  if (config_.overlayShowResolution) {
    pushFormatted(
        "Render: %ux%u",
        pp.BackBufferWidth,
        pp.BackBufferHeight);
  }

  if (config_.overlayShowFps || config_.overlayShowFrameTime) {
    if (config_.overlayShowFps && config_.overlayShowFrameTime) {
      pushFormatted(
          "FPS: %.2f  |  %.2f ms",
          fps_,
          smoothedFrameTimeMs_);
    } else if (config_.overlayShowFps) {
      pushFormatted("FPS: %.2f", fps_);
    } else {
      pushFormatted("Frame: %.2f ms", smoothedFrameTimeMs_);
    }
  }

  if (config_.overlayShowStreamingMemory) {
    const auto& stream = StreamingMemoryTelemetry::instance().sample();
    constexpr double kMiB = 1024.0 * 1024.0;
    if (stream.valid) {
      const double usedMiB = static_cast<double>(stream.usedBytes) / kMiB;
      const double limitMiB = static_cast<double>(stream.limitBytes) / kMiB;
      const double peakMiB = static_cast<double>(stream.peakUsedBytes) / kMiB;
      const double percent =
          stream.limitBytes != 0
              ? (100.0 * static_cast<double>(stream.usedBytes) /
                 static_cast<double>(stream.limitBytes))
              : 0.0;

      pushFormatted(
          "GTA Stream [engine]: %.1f / %.1f MiB (%.1f%%)  Peak %.1f MiB",
          usedMiB,
          limitMiB,
          percent,
          peakMiB);

    } else {
      pushText("GTA Stream: unavailable (waiting for CStreaming)");
    }
  }
  // External x64 helper memory, not GTA's 32-bit streaming pool or VRAM.
  // WMI samples only from the background worker; these loads are atomics.
  if (config_.overlayShowCache64Memory) {
    const auto helper = cache64Memory_.sample();
    constexpr double kMiB = 1024.0 * 1024.0;
    if (helper.state == Cache64MemoryTelemetry::State::Available) {
      pushFormatted(
          "Cache64 RAM: %.0f MiB private | %.0f MiB working set (PID %lu)",
          static_cast<double>(helper.privateBytes) / kMiB,
          static_cast<double>(helper.workingSetBytes) / kMiB,
          static_cast<unsigned long>(helper.pid));
    } else if (helper.state == Cache64MemoryTelemetry::State::Waiting) {
      pushText("Cache64 RAM: not running / waiting for helper");
    } else {
      pushText("Cache64 RAM: unavailable (Windows WMI)");
    }
  }


  if (config_.overlayShowClock || config_.overlayShowDate) {
    if (config_.overlayShowClock && config_.overlayShowDate) {
      pushFormatted(
          "%s  %s",
          clockText_.c_str(),
          dateText_.c_str());
    } else if (config_.overlayShowClock) {
      pushText(clockText_);
    } else {
      pushText(dateText_);
    }
  }

  ImDrawList* drawList = ImGui::GetForegroundDrawList();
  // Bind the selected font and its current scaled size to every OSD layer.
  const auto drawFont=font_?font_:ImGui::GetFont();
  const float drawFontPixels=ImGui::GetFontSize();

  const ImU32 textColor =
      IM_COL32(
          config_.overlayTextR,
          config_.overlayTextG,
          config_.overlayTextB,
          static_cast<int>(255.0f * config_.overlayTextAlpha));

  const ImU32 effectColor =
      IM_COL32(
          config_.overlayEffectR,
          config_.overlayEffectG,
          config_.overlayEffectB,
          static_cast<int>(255.0f * config_.overlayShadowAlpha));

  const float lineAdvance =
      ImGui::GetTextLineHeightWithSpacing();

  float blockWidth = 0.0f;
  for (std::size_t i = 0; i < lineCount; ++i) {
    const ImVec2 size =
        ImGui::CalcTextSize(lines[i]);
    blockWidth = std::max(blockWidth, size.x);
  }

  const float blockHeight =
      lineCount == 0
          ? 0.0f
          : lineAdvance * static_cast<float>(lineCount);

  const float offsetX =
      static_cast<float>(config_.overlayPosX) * currentEffectiveScale_;
  const float offsetY =
      static_cast<float>(config_.overlayPosY) * currentEffectiveScale_;

  ImVec2 origin(offsetX, offsetY);

  switch (config_.overlayAlignment) {
    case OverlayAlignment::TopCenter:
      origin.x = (io.DisplaySize.x - blockWidth) * 0.5f + offsetX;
      origin.y = offsetY;
      break;

    case OverlayAlignment::TopRight:
      origin.x = io.DisplaySize.x - blockWidth - offsetX;
      origin.y = offsetY;
      break;

    case OverlayAlignment::BottomLeft:
      origin.x = offsetX;
      origin.y = io.DisplaySize.y - blockHeight - offsetY;
      break;

    case OverlayAlignment::BottomCenter:
      origin.x = (io.DisplaySize.x - blockWidth) * 0.5f + offsetX;
      origin.y = io.DisplaySize.y - blockHeight - offsetY;
      break;

    case OverlayAlignment::BottomRight:
      origin.x = io.DisplaySize.x - blockWidth - offsetX;
      origin.y = io.DisplaySize.y - blockHeight - offsetY;
      break;

    case OverlayAlignment::TopLeft:
    default:
      origin.x = offsetX;
      origin.y = offsetY;
      break;
  }

  for (std::size_t i = 0; i < lineCount; ++i) {
    ImVec2 p = origin;
    p.y += lineAdvance * static_cast<float>(i);

    if (config_.overlayTextEffect == OverlayTextEffect::Shadow) {
      const ImVec2 shadowP(
          p.x + static_cast<float>(config_.overlayShadowOffsetX) * currentEffectiveScale_,
          p.y + static_cast<float>(config_.overlayShadowOffsetY) * currentEffectiveScale_);

      drawList->AddText(
          drawFont,
          drawFontPixels,
          shadowP,
          effectColor,
          lines[i]);
    } else if (config_.overlayTextEffect == OverlayTextEffect::Outline) {
      const float t =
          static_cast<float>(config_.overlayOutlineThickness) *
          currentEffectiveScale_;

      const ImVec2 offsets[] = {
          {-t, -t}, {0.0f, -t}, {t, -t},
          {-t, 0.0f},            {t, 0.0f},
          {-t, t},  {0.0f, t},   {t, t}
      };

      for (const ImVec2& delta : offsets) {
        drawList->AddText(
            drawFont,
            drawFontPixels,
            ImVec2(p.x + delta.x, p.y + delta.y),
            effectColor,
            lines[i]);
      }
    }

    drawList->AddText(
        drawFont,
        drawFontPixels,
        p,
        textColor,
        lines[i]);
  }

  if (font_)
    ImGui::PopFont();

  ImGui::Render();
  DrawTelemetry::OverlayScope overlayDraws;
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
  if(lineCount && !fontDrawReported_) {
    fontDrawReported_=true;
    std::ostringstream selected;
    selected<<"Overlay OSD font submitted: "<<(embeddedFont_?"BankGothic Bold (embedded)":"ImGui fallback")
        <<" pixels="<<drawFontPixels<<" lines="<<lineCount;
    Logger::instance().info(selected.str());
    if(embeddedFont_) Diagnostics::instance().effective("Overlay.EmbeddedFont",selected.str());
  }
  Diagnostics::instance().effective("Overlay.Renderer", "overlay draw data submitted to D3D9");
}

void Overlay::shutdown() {
  cache64Memory_.stop();
  gpuMemory_.stop();
  usage_.stop();
  DrawTelemetry::shutdown();
  if (imguiInitialized_) {
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext();
  }

  imguiInitialized_ = false;
  rendererDetected_ = false;
  nativeRendererDetected_ = false;
  lastRendererRefreshMs_ = 0;
  device_ = nullptr;
  refxDxvkDevice_ = nullptr;
  baseStyleCaptured_ = false;
  currentEffectiveScale_ = 1.0f;
  runtimeScaleInitialized_ = false;
  lastClockUpdateMs_ = 0;
  currentDpiScale_ = 1.0f;
  currentResolutionScale_ = 1.0f;
  currentPresentationScale_ = 1.0f;
  font_ = nullptr;
  embeddedFont_=fontDrawReported_=false;
  StreamingMemoryTelemetry::instance().reset();

  Logger::instance().info("Overlay shutdown complete.");
}

} // namespace mws
