#include "mws/DxvkConfigFile.hpp"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void save(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}
std::string read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
int main() {
  using namespace mws;
  const auto directory = std::filesystem::current_path() /
      ("dxvk-ownership-fixture-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directory(directory);
  const auto path = directory / "dxvk.conf";
  const auto journal = directory / "dxvk.conf.SanFrameOptimizer.ownership";
  const auto backup = directory / "dxvk.conf.SanFrameOptimizer.bak";
  std::string error;
  DxvkRestoreResult restored;
  int result = 0;
  try {
    const std::string original = "# user settings\r\nd3d9.maxFrameLatency = 4 # keep comment\r\nother.key = yes\r\n[gta_sa.exe]\r\nd3d9.maxFrameLatency = 3\r\n[other.exe]\r\nd3d9.maxFrameLatency = 7\r\n";
    save(path, original);
    check(writeDxvkConfig(path, {{"d3d9.maxFrameLatency", "1"}, {"d3d9.maxFrameRate", "-1"}}, "gta_sa.exe", error), "tracked tuning failed");
    auto edited = read(path);
    edited += "# unrelated new comment\r\n";
    save(path, edited);
    check(restoreDxvkConfig(path, "gta_sa.exe", restored, error), "restore failed");
    check(read(path) == original + "# unrelated new comment\r\n", "restore changed unrelated or scoped assignments");
    check(restored.restored == 3 && restored.preserved == 0, "restore counts wrong");
    check(!std::filesystem::exists(journal), "completed ownership record not retired");
    check(writeDxvkConfig(path, {{"d3d9.maxFrameLatency", "1"}}, "gta_sa.exe", error), "second tune failed");
    edited = read(path);
    const auto value = edited.find("= 1 # keep");
    check(value != std::string::npos, "test assignment missing");
    edited.replace(value, 3, "= 9");
    save(path, edited);
    check(restoreDxvkConfig(path, "gta_sa.exe", restored, error), "edited restore failed");
    check(read(path).find("= 9 # keep") != std::string::npos && restored.preserved == 1,
        "cleanup overwrote a later user edit");
    // Repeated tuning retains the user's baseline instead of the previous tuned value.
    save(path, "d3d9.maxFrameLatency = 4\n");
    check(writeDxvkConfig(path, {{"d3d9.maxFrameLatency", "1"}}, "gta_sa.exe", error), "first retune failed");
    check(writeDxvkConfig(path, {{"d3d9.maxFrameLatency", "2"}}, "gta_sa.exe", error), "second retune failed");
    check(restoreDxvkConfig(path, "gta_sa.exe", restored, error), "retune restore failed");
    check(read(path) == "d3d9.maxFrameLatency = 4\n", "retuning lost original baseline");
    save(path, "d3d9.maxFrameLatency = 4\n");
    check(writeDxvkConfig(path, {{"d3d9.maxFrameLatency", "1"}}, "gta_sa.exe", error), "duplicate baseline tune failed");
    save(path, read(path) + "d3d9.maxFrameLatency = 9\n");
    edited = read(path);
    check(restoreDxvkConfig(path, "gta_sa.exe", restored, error), "duplicate restore failed");
    check(read(path) == edited, "ambiguous user-added duplicate was modified");
    save(path, "\xef\xbb\xbf" "d3d9.maxFrameLatency = 4\n");
    check(writeDxvkConfig(path, {{"d3d9.maxFrameLatency", "1"}}, "gta_sa.exe", error), "BOM tune failed");
    check(restoreDxvkConfig(path, "gta_sa.exe", restored, error), "BOM restore failed");
    check(read(path).find("= 4") != std::string::npos, "BOM baseline assignment was deleted");
    save(journal, "corrupt ownership record");
    edited = read(path);
    check(!restoreDxvkConfig(path, "gta_sa.exe", restored, error), "corrupt ownership record accepted");
    check(read(path) == edited, "corrupt ownership record changed config");
    std::filesystem::remove(journal);
    check(restoreDxvkConfig(path, "gta_sa.exe", restored, error) && restored.legacyBackup,
        "untracked legacy backup not reported");
    check(read(path) == edited, "legacy backup blindly replaced current config");
    const auto overrides = restoreDxvkOverrides(
        "user.key = yes; d3d9.maxFrameLatency = 9; d3d9.maxFrameRate = -1; newer.key = yes",
        "user.key = yes; d3d9.maxFrameLatency = 4",
        "user.key = yes; d3d9.maxFrameLatency = 1; d3d9.maxFrameRate = -1",
        {{"d3d9.maxFrameLatency", "1"}, {"d3d9.maxFrameRate", "-1"}});
    check(overrides.find("maxFrameLatency = 9") != std::string::npos &&
        overrides.find("maxFrameRate") == std::string::npos && overrides.find("newer.key") != std::string::npos,
        "environment cleanup failed to preserve later edits");
    std::cout << "DXVK ownership regression checks passed\n";
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n'; result = 1;
  }
  for (const auto& file : {path, journal, backup}) std::filesystem::remove(file);
  std::filesystem::remove(directory);
  return result;
}
