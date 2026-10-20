#pragma once
#include <d3d9.h>
#include <cstdint>

namespace mws::DrawTelemetry {
struct Sample {
  bool available{};
  bool ready{};
  double callsPerFrame{};
  double callsPerSecond{};
  double primitivesPerFrame{};
};
bool bind(IDirect3DDevice9* device);
void shutdown();
void onFrame();
Sample sample();
bool isOverlayDraw() noexcept;
// Suppress OSD draws on the current thread; nestable for backend callbacks.
struct OverlayScope {
  OverlayScope();
  ~OverlayScope();
};
}
