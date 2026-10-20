#pragma once
#include "mws/Config.hpp"
#include <cstddef>
#include <functional>

namespace mws {

class OptimizationRuntime {
public:
  static OptimizationRuntime& instance();

  bool initialize(const Config& config);
  void shutdown();
  void captureDiagnostics();

  // Pure/background work only. Never call GTA/RenderWare/D3D from these jobs.
  void enqueue(std::function<void()> job);

  // Small thread-local scratch allocator for SanFrame's own per-frame
  // preparation data. This deliberately does NOT replace GTA's allocator.
  void beginFrame() noexcept;
  void* scratchAlloc(std::size_t bytes, std::size_t alignment = 16) noexcept;

  // Memory directly tracked by the worker/transient runtime. This excludes
  // allocator metadata and arbitrary heap storage captured inside queued jobs.
  std::size_t trackedMemoryBytes() const noexcept;

private:
  OptimizationRuntime() = default;
};

} // namespace mws
