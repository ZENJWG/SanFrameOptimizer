#pragma once
#include "mws/Config.hpp"

namespace mws::FxpOptimizer {
bool install(const Config& config);
void uninstall();
void beforeDeviceReset();
void captureDiagnostics();
}
