#include "mws/DxvkTuner.hpp"
#include "mws/DxvkBinaryIdentity.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/Logger.hpp"
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring lowerCopy(std::wstring value) {
  std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
      });
  return value;
}

std::string narrowUtf8(const std::wstring& value) {
  if (value.empty())
    return {};

  const int n =
      WideCharToMultiByte(
          CP_UTF8,
          0,
          value.c_str(),
          static_cast<int>(value.size()),
          nullptr,
          0,
          nullptr,
          nullptr);

  if (n <= 0)
    return {};

  std::string result(static_cast<std::size_t>(n), '\0');

  WideCharToMultiByte(
      CP_UTF8,
      0,
      value.c_str(),
      static_cast<int>(value.size()),
      result.data(),
      n,
      nullptr,
      nullptr);

  return result;
}

std::wstring getEnvironment(const wchar_t* name) {
  const DWORD needed =
      GetEnvironmentVariableW(name, nullptr, 0);

  if (!needed)
    return {};

  std::vector<wchar_t> buffer(needed + 1);

  const DWORD written =
      GetEnvironmentVariableW(
          name,
          buffer.data(),
          static_cast<DWORD>(buffer.size()));

  if (!written || written >= buffer.size())
    return {};

  return std::wstring(buffer.data(), written);
}


bool looksLikeSemanticVersion(const std::wstring& value) {
  if (value.empty())
    return false;

  const auto lower = lowerCopy(value);

  if (lower.find(L"winbuild") != std::wstring::npos)
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

DxvkTuner& DxvkTuner::instance() {
  static DxvkTuner tuner;
  return tuner;
}

bool DxvkTuner::containsInsensitive(
    const std::wstring& haystack,
    const std::wstring& needle) {
  return lowerCopy(haystack).find(lowerCopy(needle)) != std::wstring::npos;
}

std::wstring DxvkTuner::versionField(
    const std::filesystem::path& path,
    const wchar_t* key) const {
  DWORD handle = 0;
  const DWORD size =
      GetFileVersionInfoSizeW(path.c_str(), &handle);

  if (!size)
    return {};

  std::vector<std::uint8_t> data(size);

  if (!GetFileVersionInfoW(
          path.c_str(),
          0,
          size,
          data.data())) {
    return {};
  }

  struct Translation {
    WORD language;
    WORD codePage;
  };

  Translation* translations = nullptr;
  UINT bytes = 0;

  if (!VerQueryValueW(
          data.data(),
          L"\\VarFileInfo\\Translation",
          reinterpret_cast<void**>(&translations),
          &bytes) ||
      !translations ||
      bytes < sizeof(Translation)) {
    return {};
  }

  wchar_t query[256]{};
  swprintf_s(
      query,
      L"\\StringFileInfo\\%04x%04x\\%s",
      translations[0].language,
      translations[0].codePage,
      key);

  wchar_t* text = nullptr;
  UINT chars = 0;

  if (!VerQueryValueW(
          data.data(),
          query,
          reinterpret_cast<void**>(&text),
          &chars) ||
      !text ||
      chars == 0) {
    return {};
  }

  return std::wstring(text);
}

bool DxvkTuner::identifyDxvk(
    const std::filesystem::path& d3d9Path,
    std::string& versionOut) const {
  versionOut.clear();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(d3d9Path, ec))
    return false;

  const auto modified = std::filesystem::last_write_time(d3d9Path, ec);
  if (ec) return false;
  const auto size = std::filesystem::file_size(d3d9Path, ec);
  if (ec) return false;
  const auto cached = identityCache_.find(d3d9Path);
  if (cached != identityCache_.end() && cached->second.modified == modified &&
      cached->second.size == size) {
    versionOut = cached->second.version;
    return cached->second.dxvk;
  }

  const auto product =
      versionField(d3d9Path, L"ProductName");
  const auto description =
      versionField(d3d9Path, L"FileDescription");
  const auto company =
      versionField(d3d9Path, L"CompanyName");
  const auto version =
      versionField(d3d9Path, L"FileVersion");

  const bool positive =
      containsInsensitive(product, L"dxvk") ||
      containsInsensitive(description, L"dxvk") ||
      containsInsensitive(company, L"dxvk");

  bool refxExport = false;
  const bool binaryDxvk = hasDxvkD3D9Exports(d3d9Path, &refxExport);
  if (!positive && !binaryDxvk) {
    identityCache_[d3d9Path] = {modified, size, false, false, {}};
    return false;
  }

  if (looksLikeSemanticVersion(version))
    versionOut = narrowUtf8(version);
  else
    versionOut.clear();

  bool refx = refxExport || containsInsensitive(product, L"refx") ||
      containsInsensitive(description, L"refx") || containsInsensitive(company, L"refx");
  for (const auto& component : d3d9Path)
    refx |= lowerCopy(component.wstring()) == L"refxdxvk";
  identityCache_[d3d9Path] = {modified, size, true, refx, versionOut};
  return true;
}

bool DxvkTuner::isReFxDxvk(const std::filesystem::path& path) const {
  std::string version;
  if (!identifyDxvk(path, version)) return false;
  return identityCache_.at(path).refx;
}

void DxvkTuner::appendConfig(
    std::wstring& config,
    const std::wstring& key,
    const std::wstring& value) {
  fileValues_[narrowUtf8(key)] = narrowUtf8(value);
  if (!config.empty() && config.back() != L';')
    config += L"; ";

  config += key;
  config += L" = ";
  config += value;
}


// Locate D3D9 runtimes already mapped into this process and private
// ReFX runtimes owned by *loaded* ASIs. Files merely present in ModLoader
// directories must not activate ReFX detection or suppress DXVK tuning.
std::vector<std::filesystem::path> refxDxvkCandidates() {
  std::vector<std::filesystem::path> found;
  const auto add = [&](const std::filesystem::path& p) {
    if (std::find(found.begin(), found.end(), p) == found.end())
      found.push_back(p);
  };
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot != INVALID_HANDLE_VALUE) {
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
      do {
        const std::filesystem::path module(entry.szExePath);
        const auto parent = module.parent_path();
        const auto name = lowerCopy(module.filename().wstring());
        if (name == L"d3d9.dll")
          add(module);
        if (name.size() >= 4 && name.substr(name.size() - 4) == L".asi" &&
            name.find(L"refx") != std::wstring::npos)
          add(parent / L"ReFXDXVK" / L"d3d9.dll");
      } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
  }
  return found;
}

const char* DxvkTuner::tuningLabel(bool refx) const noexcept {
  if (refx) return "tuning managed by ReFX";
  if (refxExcluded_) return "SanFrame tuning skipped: ReFX present";
  return tuningActive_ ? "SanFrame tuning enabled" : "SanFrame tuning disabled";
}

bool DxvkTuner::setOwnedEnvironment(const wchar_t* name, const std::wstring& value) {
  const auto current = getEnvironment(name);
  auto found = ownedEnvironment_.find(name);
  auto before = found != ownedEnvironment_.end() && current == found->second.second
      ? found->second.first : current;
  if (std::wstring(name) == L"DXVK_CONFIG" && found != ownedEnvironment_.end()) {
    const auto restored = restoreDxvkOverrides(narrowUtf8(current), narrowUtf8(found->second.first),
        narrowUtf8(found->second.second), ownedConfigValues_);
    const int count = MultiByteToWideChar(CP_UTF8, 0, restored.data(), static_cast<int>(restored.size()), nullptr, 0);
    before.resize(count);
    if (count) MultiByteToWideChar(CP_UTF8, 0, restored.data(), static_cast<int>(restored.size()), before.data(), count);
  }
  if (!SetEnvironmentVariableW(name, value.c_str())) return false;
  ownedEnvironment_[name] = {before, value};
  if (std::wstring(name) == L"DXVK_CONFIG") ownedConfigValues_.insert(fileValues_.begin(), fileValues_.end());
  return true;
}

void DxvkTuner::restoreOwnedTuning() {
  for (const auto& [name, change] : ownedEnvironment_) {
    const auto current = getEnvironment(name.c_str());
    std::wstring restored = change.first;
    const bool configOverrides = name == L"DXVK_CONFIG";
    if (configOverrides) {
      const auto text = restoreDxvkOverrides(narrowUtf8(current), narrowUtf8(change.first),
          narrowUtf8(change.second), ownedConfigValues_);
      const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      restored.resize(count);
      if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), restored.data(), count);
    }
    if (configOverrides || current == change.second) {
      if (!SetEnvironmentVariableW(name.c_str(), restored.empty() ? nullptr : restored.c_str()))
        Logger::instance().warn("DXVK cleanup: cannot restore process environment variable " + narrowUtf8(name));
    }
  }
  ownedEnvironment_.clear();
  ownedConfigValues_.clear();
  wchar_t executable[MAX_PATH]{};
  GetModuleFileNameW(nullptr, executable, MAX_PATH);
  DxvkRestoreResult result;
  std::string error;
  if (!restoreDxvkConfig(configPath_, narrowUtf8(std::filesystem::path(executable).filename().wstring()), result, error)) {
    Logger::instance().warn("DXVK cleanup: " + error);
  } else if (result.restored || result.preserved) {
    Logger::instance().info("DXVK cleanup: restored " + std::to_string(result.restored) +
        " SanFrame assignments; preserved " + std::to_string(result.preserved) + " subsequently edited assignments.");
  } else if (result.legacyBackup) {
    Logger::instance().warn("DXVK cleanup: legacy backup has no ownership record; existing settings preserved because their authorship cannot be verified.");
  }
  tuningActive_ = false;
  properShadersResetWorkaround_ = false;
  properShadersOverrideApplied_ = false;
}

void DxvkTuner::configureBeforeD3D9Load(
    const Config& config,
    const std::filesystem::path& gameDirectory) {
  config_ = config;
  gameDirectory_ = gameDirectory;
  configured_ = true;
  tuningActive_ = false;
  refxExcluded_ = false;
  detected_ = false;
  fileValues_.clear();
  configPath_ = gameDirectory / L"dxvk.conf";
  properShadersResetWorkaround_ = false;
  properShadersOverrideApplied_ = false;
  version_.clear();
  detectedPath_.clear();

  // A private ReFX DXVK DLL can remain installed after its ASI is disabled.
  // Only probe it when the ASI is loaded or the module scan finds a live owner.
  const auto privateReFx = gameDirectory / L"ReFXDXVK" / L"d3d9.dll";
  const auto discovered = refxDxvkCandidates();
  const bool refxAsiLoaded = GetModuleHandleW(L"ReFXDXVK.asi") != nullptr;
  const bool privateReFxOwned = refxAsiLoaded ||
      std::find(discovered.begin(), discovered.end(), privateReFx) != discovered.end();
  std::vector<std::filesystem::path> candidates = {gameDirectory / L"d3d9.dll"};
  if (privateReFxOwned) {
    candidates.push_back(privateReFx);
  } else {
    std::error_code ec;
    if (std::filesystem::is_regular_file(privateReFx, ec))
      Logger::instance().info(
          "DXVK tuner: installed ReFX private DLL ignored (no loaded ReFX ASI).");
  }
  candidates.insert(candidates.end(), discovered.begin(), discovered.end());
  std::string detectedVersion;
  for (const auto& candidate : candidates) {
    std::string candidateVersion;
    if (!identifyDxvk(candidate, candidateVersion)) continue;
    const bool refx = isReFxDxvk(candidate);
    if (detectedPath_.empty() || refx) {
      detectedPath_ = candidate;
      detectedVersion = candidateVersion;
    }
    // Overrides are process-wide: a stock DLL alongside ReFX must not cause
    // us to change the private ReFX renderer's configuration.
    if (refx) break;
  }
  if (detectedPath_.empty()) {
    Logger::instance().info(
        "DXVK tuner: no positively identified DXVK DLL in game directory, loaded modules, or ReFX ModLoader plugin folders; no overrides applied.");
    Diagnostics::instance().skipped("DXVK.AutoTune", "DXVK DLL not identified in game or ReFX ModLoader locations");
    return;
  }
  Logger::instance().info("DXVK tuner: identified DXVK DLL at " + narrowUtf8(detectedPath_.wstring()));
  detected_ = true;
  version_ = detectedVersion;
  if (isReFxDxvk(detectedPath_)) {
    refxExcluded_ = true;
    restoreOwnedTuning();
    Logger::instance().info("ReFX DXVK identified; SanFrame auto-tuning skipped (ReFX owns its configuration).");
    Diagnostics::instance().skipped("DXVK.AutoTune", "ReFX DXVK owns its configuration; no overrides applied");
    return;
  }
  if (!config.dxvkAutoTune) {
    restoreOwnedTuning();
    Logger::instance().info("DXVK identified; auto-tuning disabled by configuration.");
    return;
  }
  properShadersResetWorkaround_ = config.dxvkProperShadersResetWorkaround;

  std::wstring dxvkConfig =
      getEnvironment(L"DXVK_CONFIG");

  if (!dxvkConfig.empty()) {
    Logger::instance().info(
        "DXVK tuner: existing DXVK_CONFIG detected; SanFrame will append only its coordinated overrides.");
  }

  int latency = config.dxvkMaxFrameLatency;

  // New unified queue-depth policy: unless the DXVK-specific override is
  // explicitly set, mirror MaxRenderAheadFrames to d3d9.maxFrameLatency.
  // This keeps native D3D9Ex and DXVK using the same queue target.
  if (latency < 0)
    latency = static_cast<int>(config.maxRenderAheadFrames);

  latency = std::clamp(latency, 0, 16);

  if (latency > 0) {
    appendConfig(
        dxvkConfig,
        L"d3d9.maxFrameLatency",
        std::to_wstring(latency));
  }

  // DXVK's own documentation recommends using an external limiter when one is
  // available. Avoid running two independent pacing algorithms at once.
  if (config.frameLimiterEnabled &&
      config.dxvkDisableInternalLimiterWhenSanFrameLimiter) {
    appendConfig(
        dxvkConfig,
        L"d3d9.maxFrameRate",
        L"-1");
  }

  if (config.frameLimiterEnabled &&
      config.dxvkDisableLatencySleepWhenSanFrameLimiter) {
    appendConfig(
        dxvkConfig,
        L"dxvk.latencySleep",
        L"False");
  }

  // Match explicit SanFrame VSync policy. "Game" intentionally leaves DXVK
  // untouched so application/DXVK defaults continue to apply.
  if (config.dxvkMatchVSyncPolicy) {
    if (config.vsync == VSyncMode::On) {
      appendConfig(
          dxvkConfig,
          L"d3d9.presentInterval",
          L"1");
    } else if (config.vsync == VSyncMode::Off) {
      appendConfig(
          dxvkConfig,
          L"d3d9.presentInterval",
          L"0");
    }
  }

  if (config.dxvkUseAllCompilerThreads) {
    appendConfig(
        dxvkConfig,
        L"dxvk.numCompilerThreads",
        L"0");
  }

  if (config.dxvkForceDeviceLocalConstantBuffers) {
    appendConfig(
        dxvkConfig,
        L"d3d9.deviceLocalConstantBuffers",
        L"True");
  }

  const bool environmentApplied = setOwnedEnvironment(L"DXVK_CONFIG", dxvkConfig);
  if (!environmentApplied) {
    std::ostringstream os;
    os << "DXVK tuner: failed to set DXVK_CONFIG, error="
       << GetLastError();
    Logger::instance().warn(os.str());
  }

  const bool fileApplied = persistConfig();
  tuningActive_ = environmentApplied || fileApplied;

  if (config.dxvkDisableFileLogging) {
    setOwnedEnvironment(L"DXVK_LOG_PATH", L"none");
  }

  if (config.dxvkUseLocalShaderCache) {
    const auto cacheDir =
        gameDirectory / L"SanFrameOptimizer-DXVK-Cache";

    std::error_code ec;
    std::filesystem::create_directories(
        cacheDir,
        ec);

    if (!ec) {
      setOwnedEnvironment(L"DXVK_SHADER_CACHE_PATH", cacheDir.wstring());
    } else {
      Logger::instance().warn(
          "DXVK tuner: could not create local shader cache directory.");
    }
  }

  std::ostringstream os;
  os << "DXVK tuner ACTIVE:"
     << " version="
     << (version_.empty() ? "unverified/omitted" : version_)
     << ", maxFrameLatency=" << latency
     << ", SanFrameLimiter="
     << (config.frameLimiterEnabled ? "on" : "off");

  Logger::instance().info(os.str());
  Diagnostics::instance().active("DXVK.AutoTune", "DXVK overrides requested; check DXVK effective configuration for acceptance");

  prepareForDirect3DCreate();
}

bool DxvkTuner::persistConfig() {
  wchar_t executable[MAX_PATH]{};
  GetModuleFileNameW(nullptr, executable, MAX_PATH);
  std::string error;
  if (!writeDxvkConfig(configPath_, fileValues_,
          narrowUtf8(std::filesystem::path(executable).filename().wstring()), error)) {
    Logger::instance().warn("DXVK tuner: cannot update " + configPath_.string() + ": " + error);
    return false;
  }
  // Use the executable-directory file even when a launcher changes the CWD.
  if (!setOwnedEnvironment(L"DXVK_CONFIG_FILE", configPath_.wstring()))
    Logger::instance().warn("DXVK tuner: failed to set DXVK_CONFIG_FILE.");
  Logger::instance().info("DXVK tuner: merged SanFrame values into " + configPath_.string() +
      "; unrelated entries preserved. If DXVK initialized earlier, restart to apply the file.");
  return true;
}

void DxvkTuner::prepareForDirect3DCreate() {
  if (!configured_ || preparing_) return;
  struct Guard { bool& busy; ~Guard() { busy = false; } } guard{preparing_};
  preparing_ = true;
  const auto discovered = refxDxvkCandidates();
  const auto privateReFx = gameDirectory_ / L"ReFXDXVK" / L"d3d9.dll";
  const bool privateReFxOwned = GetModuleHandleW(L"ReFXDXVK.asi") != nullptr ||
      std::find(discovered.begin(), discovered.end(), privateReFx) != discovered.end();
  std::vector<std::filesystem::path> candidates = {gameDirectory_ / L"d3d9.dll"};
  if (privateReFxOwned)
    candidates.push_back(privateReFx);
  candidates.insert(candidates.end(), discovered.begin(), discovered.end());
  bool refxNow = false;
  bool stockNow = false;
  for (const auto& path : candidates) {
    std::string version;
    if (!identifyDxvk(path, version)) continue;
    if (isReFxDxvk(path)) refxNow = true; else stockNow = true;
  }
  if (refxNow && !refxExcluded_) {
    Logger::instance().info("Late ReFX DXVK discovery: withdrawing SanFrame-owned tuning before device creation.");
    configureBeforeD3D9Load(config_, gameDirectory_);
  } else if (!detected_ && stockNow && !refxNow) {
    configureBeforeD3D9Load(config_, gameDirectory_);
  }
  if (refxExcluded_) return;
  if (!detected_ || !properShadersResetWorkaround_ || properShadersOverrideApplied_)
    return;
  if (!GetModuleHandleW(L"ProperShaders.asi") &&
      !GetModuleHandleW(L"ProperShaders.dll") &&
      !GetModuleHandleW(L"proper_shaders.asi") &&
      !GetModuleHandleW(L"proper_shaders.dll"))
    return;

  // Preserve unrelated overrides. DXVK resolves repeated keys in order, with
  // the last value winning. Append only once per configuration pass.
  auto overrides = getEnvironment(L"DXVK_CONFIG");
  appendConfig(overrides, L"d3d9.countLosableResources", L"False");
  const bool saved = persistConfig();
  if (!setOwnedEnvironment(L"DXVK_CONFIG", overrides)) {
    Logger::instance().warn(
        "DXVK Proper Shaders reset workaround: failed to set DXVK_CONFIG; will retry before Direct3DCreate9.");
    return;
  }
  properShadersOverrideApplied_ = saved;
  Logger::instance().info(
      "DXVK Proper Shaders reset workaround requested: d3d9.countLosableResources=False. "
      "Allows real Reset with outstanding resources; does not repair their ownership. "
      "Check DXVK's effective configuration to confirm acceptance. If another mod "
      "initialized DXVK earlier, use the same setting in dxvk.conf and restart.");
  Diagnostics::instance().active("DXVK.ProperShadersResetWorkaround",
      "process-local override requested; requires DXVK configuration initialization after this point");
}

} // namespace mws
