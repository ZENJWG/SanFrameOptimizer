#pragma once
#include <d3d9.h>
#include "mws/Config.hpp"

namespace mws::RenderWareBatcher {
// Fingerprinted stock PC and day/night callbacks; other pipelines pass through.
bool install(IDirect3DDevice9* device, const Config& config);
void onPresent();
void onReset();
void uninstall();
}
