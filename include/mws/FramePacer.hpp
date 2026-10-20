#pragma once
#include "mws/Config.hpp"
#include <Windows.h>
#include <cstdint>
#include <vector>

struct IDirect3DDevice9;

namespace mws {

class FramePacer {
public:
  static FramePacer& instance();

  void configure(const Config& config);
  void onDeviceCreated(IDirect3DDevice9* device);
  void resetTiming();
  void beforePresent(HWND hwnd);
  void afterPresent(HWND hwnd);
  void shutdown();
  void captureDiagnostics();

private:
  FramePacer() = default;

  void pace(HWND hwnd);
  void initializeThreadPolicy();
  void initializeTimer();
  void waitUntil(std::int64_t deadlineQpc);
  double activeTargetFps(HWND hwnd);
  double refreshRateForWindow(HWND hwnd);
  void recordFrame(std::int64_t nowQpc);
  void maybeLogStats(std::int64_t nowQpc);
  void applyD3D9ExLatency(IDirect3DDevice9* device);

  Config config_{};

  LARGE_INTEGER qpcFrequency_{};
  std::int64_t nextDeadline_{0};
  double lastTargetFps_{0.0};
  double fractionalTicks_{0.0};
  std::int64_t lastFrameQpc_{0};
  std::int64_t statsStartQpc_{0};

  HANDLE timer_{};
  HANDLE mmcssHandle_{};
  DWORD mmcssTaskIndex_{};

  bool configured_{false};
  bool threadPolicyInitialized_{false};
  bool timerPeriodSet_{false};

  HMONITOR refreshMonitor_{};
  double refreshRateHz_{0.0};
  ULONGLONG refreshRateQueryTick_{0};
  bool vrrAutoNoticeLogged_{false};
  double lastVrrCapHz_{0.0};

  std::vector<double> frameTimesMs_;
  std::uint64_t paceCalls_{},pacedFrames_{},uncappedFrames_{};
};

} // namespace mws
