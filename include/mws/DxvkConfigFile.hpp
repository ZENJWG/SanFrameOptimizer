#pragma once
#include <filesystem>
#include <map>
#include <string>

namespace mws {
using DxvkValues = std::map<std::string, std::string>;
// Replaces applicable assignments and inserts missing keys in the global scope.
std::string mergeDxvkConfig(const std::string& input, const DxvkValues& values,
    const std::string& executable);
std::string restoreDxvkOverrides(const std::string& current, const std::string& before,
    const std::string& after, const DxvkValues& keys);
struct DxvkRestoreResult {
  unsigned restored = 0;
  unsigned preserved = 0;
  bool legacyBackup = false;
};
// Restores only assignments still identical to the recorded SanFrame writes.
bool restoreDxvkConfig(const std::filesystem::path& path,
    const std::string& executable, DxvkRestoreResult& result, std::string& error);
bool writeDxvkConfig(const std::filesystem::path& path, const DxvkValues& values,
    const std::string& executable, std::string& error);
}
