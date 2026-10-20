#pragma once
#include "mws/Config.hpp"
#include "mws/DxvkConfigFile.hpp"
#include <filesystem>
#include <string>
#include <map>

namespace mws {

class DxvkTuner {
public:
  static DxvkTuner& instance();

  // Must be called before d3d9.dll is loaded.
  void configureBeforeD3D9Load(
      const Config& config,
      const std::filesystem::path& gameDirectory);

  // Recheck late-loaded ASIs before DXVK constructs its configuration.
  void prepareForDirect3DCreate();

  // Read-only identification, independent of tuning and DLL load order.
  bool identifyDxvk(
      const std::filesystem::path& d3d9Path,
      std::string& versionOut) const;

  bool isReFxDxvk(const std::filesystem::path& path) const;

  bool tuningActive() const noexcept { return tuningActive_; }
  bool refxExcluded() const noexcept { return refxExcluded_; }
  const char* tuningLabel(bool refx) const noexcept;

  bool detected() const noexcept { return detected_; }
  const std::string& version() const noexcept { return version_; }
  const std::filesystem::path& detectedPath() const noexcept { return detectedPath_; }

private:
  DxvkTuner() = default;


  std::wstring versionField(
      const std::filesystem::path& path,
      const wchar_t* key) const;

  void appendConfig(
      std::wstring& config,
      const std::wstring& key,
      const std::wstring& value);
  bool persistConfig();
  bool setOwnedEnvironment(const wchar_t* name, const std::wstring& value);
  void restoreOwnedTuning();

  static bool containsInsensitive(
      const std::wstring& haystack,
      const std::wstring& needle);

  struct Identity {
    std::filesystem::file_time_type modified;
    std::uintmax_t size;
    bool dxvk;
    bool refx;
    std::string version;
  };
  mutable std::map<std::filesystem::path, Identity> identityCache_;
  Config config_{};
  std::filesystem::path gameDirectory_;
  bool configured_{false};
  bool preparing_{false};
  bool tuningActive_{false};
  bool refxExcluded_{false};
  std::map<std::wstring, std::pair<std::wstring, std::wstring>> ownedEnvironment_;
  DxvkValues ownedConfigValues_;
  bool detected_{false};
  bool properShadersResetWorkaround_{false};
  bool properShadersOverrideApplied_{false};
  std::string version_;
  std::filesystem::path detectedPath_;
  std::filesystem::path configPath_;
  DxvkValues fileValues_;
};

} // namespace mws
