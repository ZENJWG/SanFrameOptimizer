#include "mws/DxvkTuner.hpp"
#include "mws/DxvkBinaryIdentity.hpp"
#include "mws/RendererBackendSelection.hpp"
#include <Windows.h>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<unsigned char> fixture(bool annotations, bool refx = false) {
  std::vector<unsigned char> bytes(0x400);
  auto word = [&](size_t p, uint16_t v) { std::memcpy(bytes.data() + p, &v, 2); };
  auto dword = [&](size_t p, uint32_t v) { std::memcpy(bytes.data() + p, &v, 4); };
  word(0, 0x5a4d); dword(0x3c, 0x80); dword(0x80, 0x4550);
  word(0x84, 0x14c); word(0x86, 1); word(0x94, 224); word(0x96, 0x2000);
  word(0x98, 0x10b); dword(0x98 + 92, 16); dword(0x98 + 96, 0x1000);
  dword(0x178 + 12, 0x1000); dword(0x178 + 16, 0x200); dword(0x178 + 20, 0x200);
  dword(0x210, 28256); dword(0x214, 3); dword(0x218, refx ? 2 : 1);
  dword(0x21c, 0x1040); dword(0x220, 0x1050);
  dword(0x240, 0x1100); dword(0x244, annotations ? 0x1110 : 0);
  dword(0x248, annotations ? 0x1120 : 0);
  dword(0x250, 0x1060); dword(0x254, 0x1080);
  std::memcpy(bytes.data() + 0x260, "Direct3DCreate9", sizeof("Direct3DCreate9"));
  std::memcpy(bytes.data() + 0x280, "ReFX_DXVK_SubmitSunlightFrame", sizeof("ReFX_DXVK_SubmitSunlightFrame"));
  return bytes;
}

int main(int argc, char** argv) {
  using namespace mws;
  const auto directory = std::filesystem::current_path() /
      ("renderer-detection-fixture-" + std::to_string(GetCurrentProcessId()));
  const auto privateDirectory = directory / "ReFXDXVK";
  const auto dll = privateDirectory / "d3d9.dll";
  std::filesystem::create_directories(privateDirectory);
  const auto save = [&](const std::vector<unsigned char>& bytes) {
    std::ofstream file(dll, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(bool(file), "fixture write failed");
  };
  int result = 0;
  HMODULE refxFixture = nullptr;
  try {
    check(argc >= 2, "missing ReFX ASI fixture argument");
    save(fixture(true));
    check(hasDxvkD3D9Exports(dll), "resource-less DXVK not recognized");
    Config config;
    config.dxvkAutoTune = false;
    SetEnvironmentVariableW(L"DXVK_CONFIG", L"test.sentinel = True");
    auto& tuner = DxvkTuner::instance();
    tuner.configureBeforeD3D9Load(config, directory);
    check(!tuner.detected(), "inactive ReFX DLL was detected with AutoTune=0");
    check(tuner.version().empty(), "invented a DXVK version without metadata");
    wchar_t environment[128]{};
    GetEnvironmentVariableW(L"DXVK_CONFIG", environment, 128);
    check(std::wstring(environment) == L"test.sentinel = True", "disabled tuning changed overrides");
    check(!std::filesystem::exists(directory / "dxvk.conf"), "disabled tuning wrote config");
    check(!GetModuleHandleW(dll.c_str()), "identification loaded the DLL");
    config.dxvkAutoTune = true;
    config.dxvkDisableFileLogging = true;
    config.dxvkUseLocalShaderCache = true;
    SetEnvironmentVariableW(L"DXVK_LOG_PATH", L"test-log-sentinel");
    SetEnvironmentVariableW(L"DXVK_SHADER_CACHE_PATH", L"test-cache-sentinel");
    SetEnvironmentVariableW(L"DXVK_CONFIG_FILE", L"test-file-sentinel");
    tuner.configureBeforeD3D9Load(config, directory);
    check(!tuner.detected() && !tuner.refxExcluded(),
        "installed-but-disabled ReFX incorrectly suppressed DXVK detection");
    check(tuner.isReFxDxvk(dll), "private ReFX DLL was not recognized as a binary");
    // A disabled ModLoader ASI on disk must not be mistaken for a mapped ASI.
    const auto disabledPlugin = directory / "modloader" / "DisabledReFX";
    std::filesystem::create_directories(disabledPlugin / "ReFXDXVK");
    std::filesystem::copy_file(argv[1], disabledPlugin / "ReFXDXVK.asi");
    std::filesystem::copy_file(dll, disabledPlugin / "ReFXDXVK" / "d3d9.dll");
    tuner.configureBeforeD3D9Load(config, directory);
    check(!tuner.detected() && !tuner.refxExcluded(),
        "disabled ModLoader ReFX ASI incorrectly activated private DXVK");
    // A loaded ASI explicitly activates probing of its private DLL.
    refxFixture = LoadLibraryW(std::filesystem::path(argv[1]).wstring().c_str());
    check(refxFixture && GetModuleHandleW(L"ReFXDXVK.asi"), "ReFX fixture ASI could not load");
    tuner.configureBeforeD3D9Load(config, directory);
    check(tuner.detected() && tuner.detectedPath() == dll && tuner.refxExcluded(),
        "loaded ReFX ASI did not activate private DXVK ownership");
    tuner.prepareForDirect3DCreate();
    GetEnvironmentVariableW(L"DXVK_CONFIG", environment, 128);
    check(std::wstring(environment) == L"test.sentinel = True", "ReFX tuning changed overrides");
    GetEnvironmentVariableW(L"DXVK_LOG_PATH", environment, 128);
    check(std::wstring(environment) == L"test-log-sentinel", "ReFX tuning changed logging");
    GetEnvironmentVariableW(L"DXVK_SHADER_CACHE_PATH", environment, 128);
    check(std::wstring(environment) == L"test-cache-sentinel", "ReFX tuning changed cache");
    GetEnvironmentVariableW(L"DXVK_CONFIG_FILE", environment, 128);
    check(std::wstring(environment) == L"test-file-sentinel", "ReFX tuning changed config file");
    check(!std::filesystem::exists(directory / "dxvk.conf"), "ReFX tuning wrote config");
    check(!std::filesystem::exists(directory / "SanFrameOptimizer-DXVK-Cache"), "ReFX tuning created cache");
    const auto stockDll = directory / "d3d9.dll";
    std::filesystem::copy_file(dll, stockDll);
    tuner.configureBeforeD3D9Load(config, directory);
    check(tuner.detectedPath() == dll, "stock DLL alongside ReFX bypassed tuning exclusion");
    GetEnvironmentVariableW(L"DXVK_CONFIG", environment, 128);
    check(std::wstring(environment) == L"test.sentinel = True", "mixed install tuned ReFX");
    // A ReFX fork copied outside its private folder is recognized by exports.
    {
      const auto bytes = fixture(false, true);
      std::ofstream file(stockDll, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    check(tuner.isReFxDxvk(stockDll), "relocated ReFX fork not classified");
    tuner.configureBeforeD3D9Load(config, directory);
    GetEnvironmentVariableW(L"DXVK_CONFIG", environment, 128);
    check(std::wstring(environment) == L"test.sentinel = True", "relocated ReFX fork was tuned");
    std::filesystem::remove(stockDll);
    FreeLibrary(refxFixture);
    refxFixture = nullptr;
    check(!GetModuleHandleW(L"ReFXDXVK.asi"), "ReFX fixture failed to unload");
    tuner.configureBeforeD3D9Load(config, directory);
    check(!tuner.detected(), "disabled ReFX remained active after ASI unload");
    // Stock DXVK remains selectable when the dormant ReFX private DLL exists.
    config.dxvkAutoTune = false;
    std::filesystem::copy_file(dll, stockDll);
    tuner.configureBeforeD3D9Load(config, directory);
    check(tuner.detected() && tuner.detectedPath() == stockDll &&
        !tuner.refxExcluded(), "dormant ReFX DLL masked the stock DXVK proxy");
    std::filesystem::remove(stockDll);
    config.dxvkAutoTune = true;
    save(fixture(false));
    check(!hasDxvkD3D9Exports(dll), "ordinary D3D9 wrapper mislabeled DXVK");
    save(fixture(false, true));
    check(hasDxvkD3D9Exports(dll), "ReFX DXVK export not recognized");
    save(std::vector<unsigned char>(32));
    check(!hasDxvkD3D9Exports(dll), "truncated PE accepted");
    auto malformed = fixture(true);
    std::fill(malformed.begin() + 0x3c, malformed.begin() + 0x40, static_cast<unsigned char>(0xff));
    save(malformed);
    check(!hasDxvkD3D9Exports(dll), "out-of-range PE offset accepted");
    check(selectRendererBackend(true, true, false, true, true) == RendererBackend::NativeVulkan,
        "loaded DXVK overrode active native presentation");
    check(selectRendererBackend(false, false, true, true, true) == RendererBackend::Dxvk,
        "empty native telemetry masked active DXVK");
    check(selectRendererBackend(true, false, true, true, true) == RendererBackend::Dxvk,
        "idle native device masked active DXVK");
    check(selectRendererBackend(false, false, false, true, true) == RendererBackend::Dxvk,
        "private loaded DXVK missed behind ReFX facade");
    check(selectRendererBackend(false, false, false, false, true) == RendererBackend::RefxUnverified,
        "ReFX without backend evidence mislabeled native Vulkan");
    check(selectRendererBackend(false, false, false, true, false) == RendererBackend::D3D9,
        "unrelated loaded DXVK overrode D3D9 device");
    // A stock installation without ReFX keeps the existing auto-tune behavior.
    save(fixture(true));
    std::filesystem::rename(dll, directory / "d3d9.dll");
    config.dxvkDisableFileLogging = false;
    config.dxvkUseLocalShaderCache = false;
    tuner.configureBeforeD3D9Load(config, directory);
    check(tuner.detected() && !tuner.isReFxDxvk(tuner.detectedPath()), "stock DXVK classified as ReFX");
    wchar_t stockOverrides[4096]{};
    GetEnvironmentVariableW(L"DXVK_CONFIG", stockOverrides, 4096);
    check(std::wstring(stockOverrides).find(L"d3d9.maxFrameLatency") != std::wstring::npos,
        "stock DXVK was not auto-tuned");
    check(std::filesystem::exists(directory / "dxvk.conf"), "stock DXVK config not written");
    check(std::string(tuner.tuningLabel(false)) == "SanFrame tuning enabled", "stock OSD tuning status wrong");
    // ReFX arrives after startup. Withdraw only our changes before another device is created.
    SetEnvironmentVariableW(L"DXVK_CONFIG", (std::wstring(stockOverrides) + L"; test.late = True").c_str());
    SetEnvironmentVariableW(L"DXVK_LOG_PATH", L"later-user-log");
    save(fixture(true));
    // Simulate a late ASI load after stock-DXVK tuning was applied.
    refxFixture = LoadLibraryW(std::filesystem::path(argv[1]).wstring().c_str());
    check(refxFixture != nullptr, "late ReFX fixture ASI could not load");
    tuner.prepareForDirect3DCreate();
    check(tuner.refxExcluded() && !tuner.tuningActive(), "late ReFX did not withdraw tuning");
    GetEnvironmentVariableW(L"DXVK_CONFIG", stockOverrides, 4096);
    check(std::wstring(stockOverrides).find(L"d3d9.maxFrameLatency") == std::wstring::npos,
        "late ReFX retained SanFrame environment override");
    check(std::wstring(stockOverrides).find(L"test.late = True") != std::wstring::npos,
        "late ReFX cleanup removed another mod's override");
    GetEnvironmentVariableW(L"DXVK_LOG_PATH", environment, 128);
    check(std::wstring(environment) == L"later-user-log", "cleanup removed later logging preference");
    check(std::string(tuner.tuningLabel(true)) == "tuning managed by ReFX", "ReFX OSD ownership wrong");
    {
      std::ifstream file(directory / "dxvk.conf", std::ios::binary);
      const std::string text(std::istreambuf_iterator<char>(file), {});
      check(text.find("d3d9.maxFrameLatency") == std::string::npos, "late ReFX retained owned config assignment");
    }
    const auto ini = directory / "config-test.ini";
    // Native Vulkan optimization policy was removed in 1.4.1. Legacy INI
    // settings must be ignored, not reintroduced as dead Config members.
    {
      std::ofstream file(ini);
      file << "[Main]\nEnabled=1\n";
    }
    const auto defaults = Config::load(ini);
    {
      std::ofstream file(ini, std::ios::trunc);
      file << "[NativeVulkan]\nEnabled=0\nDynamicStateDeduplication=0\n"
              "DescriptorBindingDeduplication=0\nBufferBindingDeduplication=0\n"
              "PipelineBindingDeduplication=0\n";
    }
    const auto legacy = Config::load(ini);
    check(legacy.optimizationsEnabled == defaults.optimizationsEnabled &&
        legacy.d3dStateDeduplication == defaults.d3dStateDeduplication &&
        legacy.renderWareBatching == defaults.renderWareBatching &&
        legacy.dxvkAutoTune == defaults.dxvkAutoTune &&
        legacy.overlayEnabled == defaults.overlayEnabled,
        "obsolete NativeVulkan section changed current configuration");
    {
      std::ofstream file(ini, std::ios::trunc);
      file << "[Optimization]\nEnabled=0\n";
    }
    const auto masterOff = Config::load(ini);
    check(!masterOff.optimizationsEnabled && !masterOff.d3dStateDeduplication &&
        !masterOff.renderWareBatching && !masterOff.fxpEnabled &&
        !masterOff.skygfxShaderConstantOptimization && !masterOff.skygfxShaderBindingOptimization &&
        !masterOff.skygfxPassProfiling && masterOff.overlayEnabled,
        "master gate did not disable optimizations or unexpectedly disabled the OSD");
    std::filesystem::remove(ini);
    std::cout << "Renderer detection, ownership and DXVK tuning regression checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  if (refxFixture)
    FreeLibrary(refxFixture);
  std::filesystem::remove(directory / "config-test.ini");
  std::filesystem::remove(dll);
  std::filesystem::remove(directory / "d3d9.dll");
  std::filesystem::remove(directory / "dxvk.conf");
  std::filesystem::remove(directory / "dxvk.conf.SanFrameOptimizer.ownership");
  std::filesystem::remove(directory / "dxvk.conf.SanFrameOptimizer.bak");
  std::filesystem::remove(privateDirectory);
  std::filesystem::remove_all(directory / "modloader");
  std::filesystem::remove(directory);
  return result;
}
