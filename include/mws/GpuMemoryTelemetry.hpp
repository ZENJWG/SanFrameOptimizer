#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <d3d9.h>

namespace mws {

// Windows/WDDM per-process video memory for the device adapter.
// Sampled on a worker; never queries DXGI from the render thread.
// All byte counters use uint64_t, even in the Win32 ASI.
class GpuMemoryTelemetry {
public:
  struct Snapshot {
    bool valid{};
    std::uint64_t localUsageBytes{};
    std::uint64_t localBudgetBytes{};
    std::uint64_t nonLocalUsageBytes{};
    std::uint64_t nonLocalBudgetBytes{};
  };
  void start(bool enabled);
  void stop();
  void setDeviceAdapter(IDirect3DDevice9* device) noexcept;
  Snapshot sample() const noexcept;

private:
  static DWORD WINAPI workerEntry(void* self);
  void run() noexcept;

  HANDLE stopEvent_{};
  HANDLE worker_{};
  std::atomic<std::uintptr_t> monitor_{0};
  std::atomic<std::uint64_t> pciId_{0};
  std::atomic<bool> active_{false};
  std::atomic<bool> valid_{false};
  std::atomic<std::uint64_t> localUsage_{0};
  std::atomic<std::uint64_t> localBudget_{0};
  std::atomic<std::uint64_t> nonLocalUsage_{0};
  std::atomic<std::uint64_t> nonLocalBudget_{0};
};

} // namespace mws
