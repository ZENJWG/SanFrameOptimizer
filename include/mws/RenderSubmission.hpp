#pragma once
#include "mws/Config.hpp"
#include <d3d9.h>
#include <cstddef>

namespace mws::RenderSubmission {
bool install(IDirect3DDevice9* device, const Config& config);
void uninstall();
void flush();
void onReset();
void onPresent();
// Only the fingerprinted, immutable stock PC building pipeline may instance.
void enterStaticBuilding(bool eligible);
void leaveStaticBuilding();
// Tracked CPU/GPU memory owned by the submission optimizer.
std::size_t trackedMemoryBytes() noexcept;

}
