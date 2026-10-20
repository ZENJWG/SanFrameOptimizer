#include "mws/FxpOptimizer.hpp"
#include "mws/NativeVulkan.hpp"
#include <Windows.h>
#include <iostream>
#include <stdexcept>

// This host has no native renderer. All other linked code is production code.
namespace mws::NativeVulkan { bool isNativeBackend() { return false; } }
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    wchar_t temp[MAX_PATH]{}, ini[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp); GetTempFileNameW(temp, L"fxp", 0, ini);
    try {
        mws::Config defaults;
        check(!mws::FxpOptimizer::install(defaults), "non-GTA host accepted");
        defaults.optimizationsEnabled = false;
        check(!mws::FxpOptimizer::install(defaults), "master-off installed FX hooks");
        WritePrivateProfileStringW(L"Optimization", L"Enabled", L"0", ini);
        for(const wchar_t* key : {L"AlphaListIndex",L"ShadowGroupIndex"})
            WritePrivateProfileStringW(L"Optimization",key,L"1",ini);
        for(const wchar_t* key : {L"OperatorCache",L"OperatorPlans"})
            WritePrivateProfileStringW(L"FXP",key,L"1",ini);
        auto config = mws::Config::load(ini);
        check(!config.fxpEnabled && !config.fxpReplaceSimulation && !config.fxpCompiledCurves &&
              !config.fxpInstancedSubmission && !config.fxpMatrixCache && !config.fxpProfile,
              "configuration master switch did not gate FX settings");
        check(!config.fxpOperatorCache && !config.fxpOperatorPlans && !config.alphaListIndex &&
              !config.shadowGroupIndex,"master switch did not gate experimental passes");
        WritePrivateProfileStringW(L"Optimization", L"Enabled", L"1", ini);
        WritePrivateProfileStringW(L"FXP", L"StatsIntervalSeconds", L"-1", ini);
        config = mws::Config::load(ini);
        check(config.fxpEnabled && !config.fxpProfile && config.fxpStatsIntervalSeconds == 0,
              "FX defaults or interval clamp incorrect");
        WritePrivateProfileStringW(L"FXP", L"StatsIntervalSeconds", L"9999", ini);
        check(mws::Config::load(ini).fxpStatsIntervalSeconds == 300, "FX interval not bounded");
        WritePrivateProfileStringW(L"FXP", L"Enabled", L"0", ini);
        check(!mws::FxpOptimizer::install(mws::Config::load(ini)), "FX-off policy not respected");
        DeleteFileW(ini);
        std::cout << "Unsupported host, master switch and FX configuration policy passed\n";
        return 0;
    } catch (const std::exception& error) {
        DeleteFileW(ini); mws::FxpOptimizer::uninstall();
        std::cerr << error.what() << '\n'; return 1;
    }
}
