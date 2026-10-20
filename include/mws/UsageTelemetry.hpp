#pragma once
#include <Windows.h>
#include <atomic>
#include <mutex>

namespace mws {

// Optional system telemetry. PDH work never runs on the rendering thread.
class UsageTelemetry {
public:
  struct ThreadSample {
    double processCores{-1.0};
    double renderPercent{-1.0};
    double otherCores{-1.0};
  };
  void start(bool cpu, bool gpu, bool process = false, bool threads = false);
  void stop();
  void setRenderThread(DWORD id) noexcept { renderThread_.store(id); }
  ThreadSample threadSample() const { std::lock_guard lock(sampleMutex_); return threadSample_; }
  double cpuPercent() const noexcept { return cpuPercent_.load(); }
  double gpuPercent() const noexcept { return gpuPercent_.load(); }

private:
  static DWORD WINAPI workerEntry(void* context);
  void run();
  HANDLE stopEvent_{};
  HANDLE worker_{};
  bool cpuEnabled_{};
  bool gpuEnabled_{};
  bool processEnabled_{};
  bool threadsEnabled_{};
  std::atomic<DWORD> renderThread_{};
  mutable std::mutex sampleMutex_;
  ThreadSample threadSample_;
  // Negative means unavailable or still collecting the initial sample.
  std::atomic<double> cpuPercent_{-1.0};
  std::atomic<double> gpuPercent_{-1.0};
};

} // namespace mws
