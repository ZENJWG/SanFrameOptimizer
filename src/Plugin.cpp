#include "mws/Plugin.hpp"
#include "mws/OptimizationRuntime.hpp"
#include "mws/Config.hpp"
#include "mws/D3D9Hook.hpp"
#include "mws/NativeVulkan.hpp"
#include "mws/Overlay.hpp"
#include "mws/DxvkTuner.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/ExecutableInfo.hpp"
#include "mws/HookManager.hpp"
#include "mws/FxpOptimizer.hpp"
#include "mws/EnginePass.hpp"
#include "mws/OptimizationReport.hpp"
#include "mws/Logger.hpp"
#include <filesystem>
#include <cwchar>
#include <sstream>
#include <string>
#include <vector>

namespace {
mws::HookManager g_hookRuntime;
mws::D3D9Hook g_d3d9;
HMODULE g_selfModule = nullptr;
std::filesystem::path selfDir() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(g_selfModule, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

std::filesystem::path gameDir() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}


std::string narrowUtf8(const std::wstring& value) {
  if (value.empty())
    return {};

  const int bytes = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
      nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
    return "<utf8-conversion-failed>";

  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
      out.data(), bytes, nullptr, nullptr);
  return out;
}

void logDiagnosticCheckpoint(
    const char* area,
    const char* state,
    const std::string& detail) {
  std::ostringstream os;
  os << "[DIAG][" << area << "][" << state << "] " << detail;
  if (state[0] == 'F')
    mws::Logger::instance().error(os.str());
  else if (state[0] == 'S')
    mws::Logger::instance().warn(os.str());
  else
    mws::Logger::instance().info(os.str());
}

void logIniInventory(const std::filesystem::path& iniPath) {
  constexpr DWORD kBufferChars = 32768;
  std::vector<wchar_t> sections(kBufferChars, L'\0');
  const DWORD sectionChars = GetPrivateProfileSectionNamesW(
      sections.data(), kBufferChars, iniPath.c_str());

  if (sectionChars == 0) {
    logDiagnosticCheckpoint(
        "INI", "FAIL",
        "No INI sections could be enumerated from " + iniPath.string());
    return;
  }

  std::size_t sectionCount = 0;
  std::size_t keyCount = 0;

  for (const wchar_t* section = sections.data(); *section;) {
    ++sectionCount;
    std::vector<wchar_t> entries(kBufferChars, L'\0');
    const DWORD entryChars = GetPrivateProfileSectionW(
        section, entries.data(), kBufferChars, iniPath.c_str());

    if (entryChars == 0) {
      logDiagnosticCheckpoint(
          "INI", "SKIP",
          "[" + narrowUtf8(section) + "] contains no readable key/value pairs.");
      section += std::wcslen(section) + 1;
      continue;
    }

    for (const wchar_t* entry = entries.data(); *entry;) {
      const std::wstring line(entry);
      const auto equals = line.find(L'=');
      if (equals != std::wstring::npos) {
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        std::ostringstream os;
        os << "[DIAG][INI][READ] ["
           << narrowUtf8(section) << "] "
           << narrowUtf8(key) << "=" << narrowUtf8(value);
        mws::Logger::instance().info(os.str());
        ++keyCount;
      }
      entry += std::wcslen(entry) + 1;
    }

    section += std::wcslen(section) + 1;
  }

  std::ostringstream summary;
  summary << "Enumerated " << keyCount << " key(s) across "
          << sectionCount << " section(s); every listed value was readable.";
  logDiagnosticCheckpoint("INI", "PASS", summary.str());
}

} // namespace

namespace mws {
void setPluginModule(HMODULE module) noexcept { g_selfModule = module; }
Plugin& Plugin::instance() { static Plugin plugin; return plugin; }

bool Plugin::initialize() {
  if (initialized_) return true;
  const auto dir = selfDir();
  Logger::instance().open(dir / "SanFrameOptimizer.log");
  Logger::instance().info("SanFrame Optimizer starting. Created by Zeneric.");
  Logger::instance().info("Target: GTA San Andreas Classic PC US 1.0 / Win32.");
  const auto iniPath = dir / "SanFrameOptimizer.ini";
  const auto cfg = Config::load(iniPath);
  Logger::instance().setDebugEnabled(cfg.debugLogging);
  auto& diagnostics = Diagnostics::instance();
  diagnostics.configure(cfg.iniDiagnostics,cfg.runtimeReportIntervalSeconds);
  diagnostics.reset();
  OptimizationReport::configureDiagnostics(cfg);
  diagnostics.registerFeature("D3D9.DevicePresent", true);
  diagnostics.registerFeature("Overlay.Renderer", cfg.overlayEnabled);
  diagnostics.registerFeature("Overlay.EmbeddedFont", cfg.overlayEnabled);
  const bool ex = cfg.d3d9ExMode != D3D9ExMode::Off;
  diagnostics.registerFeature("D3D9Ex.Backend", ex);
  diagnostics.registerFeature("D3D9Ex.PresentEx", ex);
  diagnostics.registerFeature("D3D9Ex.ResetEx", ex);
  diagnostics.registerFeature("D3D9Ex.FrameLatency", ex);
  diagnostics.registerFeature("DXVK.AutoTune", cfg.dxvkAutoTune);
  diagnostics.registerFeature("Optimization.D3DStateDedup", cfg.d3dStateDeduplication);
  diagnostics.registerFeature("Optimization.RenderWareBatching", cfg.renderWareBatching);
  diagnostics.registerFeature("Optimization.RenderWareDayNightBatching", cfg.renderWareDayNightBatching);
  diagnostics.registerFeature("Optimization.ImmediateBatching", cfg.immediateBatching);
  diagnostics.registerFeature("Optimization.PassTiming", cfg.passTiming);
  diagnostics.registerFeature("Optimization.StaticPropInstancing", cfg.staticPropInstancing);
  diagnostics.registerFeature("Optimization.SkygfxShaderBindings", cfg.skygfxShaderBindingOptimization && cfg.d3dStateDeduplication);
  diagnostics.registerFeature("Optimization.SkygfxConstants", cfg.skygfxShaderConstantOptimization && cfg.d3dStateDeduplication);
  diagnostics.registerFeature("Optimization.VisibilityBoundsCache", cfg.visibilityBoundsCache);
  diagnostics.registerFeature("Optimization.WorkerPreparation", cfg.workerPreparation);
  diagnostics.registerFeature("Optimization.TransientArena", cfg.sanFrameTransientArena);
  diagnostics.registerFeature("FXP.OperatorCache", cfg.fxpEnabled && cfg.fxpOperatorCache);
  diagnostics.registerFeature("FXP.OperatorPlans", cfg.fxpEnabled && cfg.fxpOperatorPlans);
  diagnostics.registerFeature("Optimization.AlphaListIndex", cfg.alphaListIndex);
  diagnostics.registerFeature("Optimization.ShadowGroupIndex", cfg.shadowGroupIndex);
  diagnostics.registerFeature("FXP.ReplaceSimulation", cfg.fxpEnabled && cfg.fxpReplaceSimulation);
  diagnostics.registerFeature("FXP.CompiledCurves", cfg.fxpEnabled && cfg.fxpCompiledCurves);
  diagnostics.registerFeature("FXP.MatrixCache", cfg.fxpEnabled && cfg.fxpMatrixCache);
  diagnostics.registerFeature("FXP.InstancedSubmission", cfg.fxpEnabled && cfg.fxpInstancedSubmission);
  diagnostics.registerFeature("Performance.FrameLimiter", cfg.frameLimiterEnabled);
  if (cfg.iniDiagnostics) logIniInventory(iniPath);
  if (!cfg.enabled) {
    Logger::instance().info("Plugin disabled by configuration.");
    Logger::instance().flush();
    return false;
  }
  ExecutableInfo::current().log();
  if (!g_hookRuntime.initialize()) return false;
  Overlay::instance().configure(cfg);
  // Initialize worker/scratch policy before any hooked device can use it.
  OptimizationRuntime::instance().initialize(cfg);
  NativeVulkan::configure();
  NativeVulkan::attach();
  // ReFX may attach its Vulkan integration while using a private DXVK D3D9 DLL.
  // Configure DXVK before ReFX creates its D3D9 device, regardless of native attachment.
  DxvkTuner::instance().configureBeforeD3D9Load(cfg, gameDir());
  if (!g_d3d9.install(cfg)) {
    g_d3d9.uninstall();
    Overlay::instance().shutdown();
    OptimizationRuntime::instance().shutdown();
    NativeVulkan::shutdown();
    g_hookRuntime.shutdown();
    Logger::instance().error("D3D9 optimizer hooks failed to initialize.");
    Logger::instance().flush();
    return false;
  }
  if (!FxpOptimizer::install(cfg)) {
    for (const char* feature : {"FXP.ReplaceSimulation", "FXP.CompiledCurves", "FXP.MatrixCache", "FXP.InstancedSubmission", "FXP.OperatorCache", "FXP.OperatorPlans"})
      diagnostics.skipped(feature, "FX runtime disabled/unavailable; see [FXP] startup details");
  }
  EnginePass::install(cfg);
  initialized_ = true;
  Logger::instance().info("SanFrame Optimizer initialization complete.");
  Logger::instance().flush();
  return true;
}

void Plugin::shutdown() {
  if (!initialized_) return;
  EnginePass::uninstall();
  FxpOptimizer::uninstall();
  g_d3d9.uninstall();
  NativeVulkan::shutdown();
  Overlay::instance().shutdown();
  OptimizationRuntime::instance().shutdown();
  g_hookRuntime.shutdown();
  Diagnostics::instance().printSummary();
  initialized_ = false;
  Logger::instance().flush();
}
} // namespace mws
