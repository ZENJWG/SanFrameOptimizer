#pragma once
#include <cstdint>
#include "../renderer/GpuTimer.h"
namespace fxp::engine {
using Logger = void(*)(const char*);
struct Counters {
    uint64_t particles{}, packets{}, curves{}, curveFallbacks{}, triangles{}, draws{}, renderFallbacks{};
    gpu::Stats gpu;
};
bool Install(bool simulation, bool curves, bool renderer, Logger logger);
void SetGameThread(uint32_t thread);
void Uninstall();
void ClearProject();
void BeginRender(bool profile = false, bool explosion = false);
void EndRender();
Counters TakeCounters();
}
