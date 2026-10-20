#include "mws/FramePacer.hpp"
#include "mws/Logger.hpp"
#include "mws/Diagnostics.hpp"
#include <Windows.h>
#include <d3d9.h>
#include <avrt.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace {
constexpr DWORD kHighResolutionTimerFlag = 0x00000002;

double percentile(std::vector<double>& values, double p) {
  if (values.empty())
    return 0.0;

  const double index = p * static_cast<double>(values.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(index));
  const auto hi = static_cast<std::size_t>(std::ceil(index));

  // Select only the two ranks we need; a full sort adds avoidable work to
  // the Present thread whenever a statistics interval expires.
  auto lower = values.begin() + lo;
  std::nth_element(values.begin(), lower, values.end());
  const double lowValue = *lower;
  if (lo == hi)
    return lowValue;

  const double t = index - static_cast<double>(lo);
  const double highValue = *std::min_element(lower + 1, values.end());
  return lowValue * (1.0 - t) + highValue * t;
}
}

namespace mws {

FramePacer& FramePacer::instance() {
  static FramePacer pacer;
  return pacer;
}

void FramePacer::configure(const Config& config) {
  config_ = config;
  QueryPerformanceFrequency(&qpcFrequency_);
  configured_ = true;
  resetTiming();
  refreshMonitor_ = nullptr;
  refreshRateHz_ = 0.0;
  refreshRateQueryTick_ = 0;
  vrrAutoNoticeLogged_ = false;
  lastVrrCapHz_ = 0.0;
  // Reserve once, rather than growing storage on the Present thread.
  if (config_.enableFrameStats)
    frameTimesMs_.reserve(16384);

  std::ostringstream os;
  os << "FramePacer configured:"
     << " enabled=" << (config_.frameLimiterEnabled ? 1 : 0)
     << ", target=" << config_.targetFps
     << ", background=" << config_.backgroundFps
     << ", point=" << frameLimitPointName(config_.frameLimitPoint)
     << ", profile=" << latencyProfileName(config_.latencyProfile)
     << ", VRR=" << vrrModeName(config_.vrrMode)
     << ", renderAhead=" << config_.maxRenderAheadFrames
     << ", busyWaitUs=" << config_.busyWaitMicroseconds
     << ", highResTimer=" << (config_.useHighResolutionTimer ? 1 : 0)
     << ", MMCSS=" << (config_.useMmcss ? 1 : 0);
  Logger::instance().info(os.str());
}

void FramePacer::initializeTimer() {
  if (timer_)
    return;

  if (config_.useHighResolutionTimer) {
    timer_ = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        kHighResolutionTimerFlag,
        TIMER_MODIFY_STATE | SYNCHRONIZE);

    if (timer_) {
      Logger::instance().info(
          "FramePacer: high-resolution waitable timer active.");
    }
  }

  if (!timer_) {
    timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (timer_) {
      Logger::instance().info(
          "FramePacer: standard waitable timer active.");
    }
  }

  if (config_.useTimerResolution1ms && !timerPeriodSet_) {
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
      timerPeriodSet_ = true;
      Logger::instance().info(
          "FramePacer: process timer resolution requested at 1 ms.");
    }
  }
}

void FramePacer::initializeThreadPolicy() {
  if (threadPolicyInitialized_)
    return;

  threadPolicyInitialized_ = true;

  if (config_.useMmcss) {
    mmcssHandle_ =
        AvSetMmThreadCharacteristicsW(L"Games", &mmcssTaskIndex_);

    if (mmcssHandle_) {
      AvSetMmThreadPriority(
          mmcssHandle_,
          config_.latencyProfile == LatencyProfile::LowLatency
              ? AVRT_PRIORITY_HIGH
              : AVRT_PRIORITY_NORMAL);

      Logger::instance().info(
          "FramePacer: render/present thread registered with MMCSS Games.");
    } else {
      std::ostringstream os;
      os << "FramePacer: MMCSS registration failed, error="
         << GetLastError();
      Logger::instance().warn(os.str());
    }
  }

  initializeTimer();
}

void FramePacer::applyD3D9ExLatency(IDirect3DDevice9* device) {
  if (!device)
    return;

  if (config_.d3d9ExMode == D3D9ExMode::Off)
    return;

  IDirect3DDevice9Ex* ex = nullptr;
  const HRESULT qi =
      device->QueryInterface(
          __uuidof(IDirect3DDevice9Ex),
          reinterpret_cast<void**>(&ex));

  if (FAILED(qi) || !ex) {
    Logger::instance().warn(
        "FramePacer: D3D9Ex requested but active device does not expose IDirect3DDevice9Ex.");
    if (config_.d3d9ExMode == D3D9ExMode::Force) {
      Diagnostics::instance().failed(
          "D3D9Ex.FrameLatency",
          "Force mode active device does not expose IDirect3DDevice9Ex");
    } else {
      Diagnostics::instance().skipped(
          "D3D9Ex.FrameLatency",
          "Auto mode fell back to a classic D3D9 device");
    }
    return;
  }

  const UINT latency =
      std::clamp<UINT>(config_.maxRenderAheadFrames, 1u, 20u);
  const HRESULT hr =
      ex->SetMaximumFrameLatency(latency);

  std::ostringstream os;
  os << "FramePacer: D3D9Ex SetMaximumFrameLatency("
     << latency
     << ") HRESULT=0x" << std::hex << std::uppercase
     << static_cast<unsigned long>(hr);

  if (SUCCEEDED(hr)) {
    Logger::instance().info(os.str());

    UINT verified = 0;
    const HRESULT verifyHr = ex->GetMaximumFrameLatency(&verified);
    std::ostringstream verify;
    verify << "FramePacer: D3D9Ex GetMaximumFrameLatency => "
           << verified
           << " HRESULT=0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(verifyHr);

    if (SUCCEEDED(verifyHr) && verified == latency) {
      Logger::instance().info(verify.str());
      Diagnostics::instance().effective(
          "D3D9Ex.FrameLatency",
          "SetMaximumFrameLatency verified by GetMaximumFrameLatency");
    } else {
      Logger::instance().warn(verify.str());
      Diagnostics::instance().failed(
          "D3D9Ex.FrameLatency",
          SUCCEEDED(verifyHr)
              ? "GetMaximumFrameLatency did not match the configured value"
              : "GetMaximumFrameLatency verification failed");
    }
  } else {
    Logger::instance().warn(os.str());
    Diagnostics::instance().failed(
        "D3D9Ex.FrameLatency",
        "SetMaximumFrameLatency failed");
  }

  ex->Release();
}

void FramePacer::onDeviceCreated(IDirect3DDevice9* device) {
  if (!configured_)
    return;

  resetTiming();
  applyD3D9ExLatency(device);
}

void FramePacer::resetTiming() {
  nextDeadline_ = lastFrameQpc_ = statsStartQpc_ = 0;
  lastTargetFps_ = fractionalTicks_ = 0.0;
  frameTimesMs_.clear();
  refreshRateQueryTick_ = 0;
}

double FramePacer::refreshRateForWindow(HWND hwnd) {
  if (!hwnd)
    return 0.0;

  const HMONITOR monitor =
      MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  const ULONGLONG now = GetTickCount64();

  if (monitor == refreshMonitor_ &&
      refreshRateQueryTick_ != 0 &&
      now - refreshRateQueryTick_ < 2000) {
    return refreshRateHz_;
  }

  // Cache failed/unknown queries too. Otherwise an unavailable display can
  // make EnumDisplaySettingsEx run on every frame until it recovers.
  const HMONITOR previousMonitor = refreshMonitor_;
  const double previousRate = refreshRateHz_;
  refreshMonitor_ = monitor;
  refreshRateHz_ = 0.0;
  refreshRateQueryTick_ = now;

  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi))
    return 0.0;

  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsExW(
          mi.szDevice,
          ENUM_CURRENT_SETTINGS,
          &dm,
          EDS_RAWMODE)) {
    return 0.0;
  }

  double hz = 0.0;
  if ((dm.dmFields & DM_DISPLAYFREQUENCY) &&
      dm.dmDisplayFrequency > 1) {
    hz = static_cast<double>(dm.dmDisplayFrequency);
  }

  const bool changed =
      monitor != previousMonitor ||
      (hz > 0.0 && std::abs(hz - previousRate) > 0.5);

  refreshMonitor_ = monitor;
  refreshRateHz_ = hz;
  refreshRateQueryTick_ = now;

  if (changed && hz > 0.0) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2)
       << "FramePacer: monitor refresh detected at "
       << hz << " Hz.";
    Logger::instance().info(os.str());
  }

  return hz;
}

double FramePacer::activeTargetFps(HWND hwnd) {
  if (!config_.frameLimiterEnabled)
    return 0.0;

  const bool foreground =
      hwnd && GetForegroundWindow() == hwnd;

  if (!foreground &&
      config_.backgroundFps > 0.0) {
    return config_.backgroundFps;
  }

  double target = config_.targetFps;

  if (!foreground ||
      config_.vrrMode == VrrMode::Off ||
      target <= 0.0) {
    return target;
  }

  // There is no portable D3D9/Win32 capability query for "this output is
  // currently VRR-capable". Auto therefore avoids changing synchronized
  // presentation and only applies the safe below-refresh policy when the user
  // explicitly runs VSync Off. On is the opt-in override for known VRR setups.
  bool applyVrrCap = config_.vrrMode == VrrMode::On;
  if (config_.vrrMode == VrrMode::Auto) {
    applyVrrCap = config_.vsync == VSyncMode::Off;
    if (!vrrAutoNoticeLogged_) {
      vrrAutoNoticeLogged_ = true;
      Logger::instance().info(
          applyVrrCap
              ? "FramePacer: VRR Auto enabled below-refresh cap because VSync=Off."
              : "FramePacer: VRR Auto is conservative with VSync!=Off; set VRRMode=On for a known VRR display.");
    }
  }

  if (!applyVrrCap)
    return target;

  const double refresh = refreshRateForWindow(hwnd);
  if (refresh < config_.vrrMinimumRefreshHz)
    return target;

  const double cap =
      std::max(1.0, refresh - config_.vrrRefreshMarginHz);

  if (target > cap) {
    if (std::abs(lastVrrCapHz_ - cap) > 0.1) {
      lastVrrCapHz_ = cap;
      std::ostringstream os;
      os << std::fixed << std::setprecision(2)
         << "FramePacer: refresh-aware cap active at "
         << cap << " FPS (refresh=" << refresh
         << " Hz, margin=" << config_.vrrRefreshMarginHz << " Hz).";
      Logger::instance().info(os.str());
    }
    return cap;
  }

  lastVrrCapHz_ = 0.0;
  return target;
}

void FramePacer::waitUntil(std::int64_t deadlineQpc) {
  if (qpcFrequency_.QuadPart <= 0)
    return;

  initializeThreadPolicy();

  const std::int64_t busyTicks =
      static_cast<std::int64_t>(
          (static_cast<long double>(config_.busyWaitMicroseconds) *
           static_cast<long double>(qpcFrequency_.QuadPart)) /
          1000000.0L);

  for (;;) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    const std::int64_t remaining =
        deadlineQpc - now.QuadPart;

    if (remaining <= 0)
      break;

    if (remaining > busyTicks && timer_) {
      const std::int64_t sleepTicks =
          remaining - busyTicks;

      const long double seconds =
          static_cast<long double>(sleepTicks) /
          static_cast<long double>(qpcFrequency_.QuadPart);

      LARGE_INTEGER due{};
      // Relative waitable timer units are 100 ns and must be negative.
      due.QuadPart =
          -static_cast<LONGLONG>(
              std::max<long double>(
                  1.0L,
                  seconds * 10000000.0L));

      if (SetWaitableTimer(
              timer_,
              &due,
              0,
              nullptr,
              nullptr,
              FALSE)) {
        // A timer can be cancelled during shutdown. Never keep retrying a
        // failed wait in a hot loop without yielding the render thread.
        if (WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0)
          Sleep(1);
      } else {
        Sleep(1);
      }
    } else if (remaining > busyTicks) {
      // Timer creation can fail. Avoid spinning for the entire frame.
      const auto sleepMs = static_cast<DWORD>(
          (remaining - busyTicks) * 1000 / qpcFrequency_.QuadPart);
      if (sleepMs > 1)
        Sleep(sleepMs - 1);
      else
        SwitchToThread();
    } else {
      // Final few hundred microseconds: avoid a scheduler wakeup overshoot.
      YieldProcessor();
    }
  }
}

void FramePacer::recordFrame(std::int64_t nowQpc) {
  if (!config_.enableFrameStats ||
      qpcFrequency_.QuadPart <= 0) {
    lastFrameQpc_ = nowQpc;
    return;
  }

  if (lastFrameQpc_ != 0) {
    const double ms =
        (static_cast<double>(nowQpc - lastFrameQpc_) * 1000.0) /
        static_cast<double>(qpcFrequency_.QuadPart);

    // Long stalls are part of frame-time stability too; dropping them made
    // the reported lows look better precisely when the game was hitching.
    if (std::isfinite(ms) && ms > 0.0 &&
        frameTimesMs_.size() < 16384)
      frameTimesMs_.push_back(ms);
  }

  lastFrameQpc_ = nowQpc;

  if (statsStartQpc_ == 0)
    statsStartQpc_ = nowQpc;

  maybeLogStats(nowQpc);
}

void FramePacer::maybeLogStats(std::int64_t nowQpc) {
  if (frameTimesMs_.empty() ||
      statsStartQpc_ == 0 ||
      qpcFrequency_.QuadPart <= 0) {
    return;
  }

  const double elapsedMs =
      (static_cast<double>(nowQpc - statsStartQpc_) * 1000.0) /
      static_cast<double>(qpcFrequency_.QuadPart);

  if (elapsedMs < config_.frameStatsIntervalMs && frameTimesMs_.size() < 16384)
    return;

  const double sum =
      std::accumulate(
          frameTimesMs_.begin(),
          frameTimesMs_.end(),
          0.0);

  const double average =
      sum / static_cast<double>(frameTimesMs_.size());

  double variance = 0.0;
  for (const double x : frameTimesMs_) {
    const double d = x - average;
    variance += d * d;
  }

  variance /= static_cast<double>(frameTimesMs_.size());
  const double stddev = std::sqrt(variance);

  // These are frame-time percentiles; the inverse of P99 approximates
  // 1% low FPS. Compute from the same bounded sample window.
  const double p95 = percentile(frameTimesMs_, 0.95);
  const double p99 = percentile(frameTimesMs_, 0.99);
  const double p999 = percentile(frameTimesMs_, 0.999);
  const double fpsOnePercentLow = p99 > 0.0 ? 1000.0 / p99 : 0.0;
  const double fpsPointOnePercentLow = p999 > 0.0 ? 1000.0 / p999 : 0.0;

  std::ostringstream os;
  os << std::fixed << std::setprecision(3)
     << "FrameStats:"
     << " samples=" << frameTimesMs_.size()
     << ", avg=" << average << " ms"
     << ", fps=" << (average > 0.0 ? 1000.0 / average : 0.0)
     << ", p95=" << p95 << " ms"
     << ", p99=" << p99 << " ms"
     << ", p99.9=" << p999 << " ms"
     << ", approx1pctLow=" << fpsOnePercentLow << " fps"
     << ", approx0.1pctLow=" << fpsPointOnePercentLow << " fps"
     << ", stddev=" << stddev << " ms";

  Logger::instance().info(os.str());

  frameTimesMs_.clear();
  statsStartQpc_ = nowQpc;
}

void FramePacer::pace(HWND hwnd) {
  if (!configured_)
    return;

  if (!config_.frameLimiterEnabled && !config_.enableFrameStats)
    return;
  ++paceCalls_;

  const double targetFps = activeTargetFps(hwnd);
  if (!std::isfinite(targetFps) || targetFps <= 0.0 ||
      qpcFrequency_.QuadPart <= 0) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    recordFrame(now.QuadPart);
    ++uncappedFrames_;
    nextDeadline_ = 0;
    lastTargetFps_ = fractionalTicks_ = 0.0;
    return;
  }

  const double exactFrameTicks =
      static_cast<double>(qpcFrequency_.QuadPart) / targetFps;
  const auto frameTicks = static_cast<std::int64_t>(exactFrameTicks);

  if (frameTicks <= 0)
    return;

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);

  if (nextDeadline_ == 0 || lastTargetFps_ != targetFps) {
    nextDeadline_ = now.QuadPart + frameTicks;
    lastTargetFps_ = targetFps;
    fractionalTicks_ = exactFrameTicks - static_cast<double>(frameTicks);
  } else {
    const std::int64_t maxBehind =
        frameTicks *
        static_cast<std::int64_t>(
            config_.maxCatchUpFrames);

    if (now.QuadPart - nextDeadline_ > maxBehind) {
      // This frame is already late: do not penalize it with another full
      // interval. Advance below from now, so the NEXT frame has a fresh
      // deadline and cannot race through an obsolete catch-up schedule.
      nextDeadline_ = now.QuadPart;
      fractionalTicks_ = 0.0;
    }
  }

  waitUntil(nextDeadline_);
  ++pacedFrames_;

  QueryPerformanceCounter(&now);
  recordFrame(now.QuadPart);

  nextDeadline_ += frameTicks;
  // Carry the remainder instead of truncating every interval. This keeps
  // fractional FPS targets from accumulating clock drift over long sessions.
  fractionalTicks_ += exactFrameTicks - static_cast<double>(frameTicks);
  const auto wholeTicks = static_cast<std::int64_t>(fractionalTicks_);
  nextDeadline_ += wholeTicks;
  fractionalTicks_ -= static_cast<double>(wholeTicks);
}

void FramePacer::beforePresent(HWND hwnd) {
  if (config_.frameLimitPoint == FrameLimitPoint::BeforePresent)
    pace(hwnd);
}

void FramePacer::afterPresent(HWND hwnd) {
  if (config_.frameLimitPoint == FrameLimitPoint::AfterPresent)
    pace(hwnd);
}

void FramePacer::shutdown() {
  captureDiagnostics();
  if (mmcssHandle_) {
    AvRevertMmThreadCharacteristics(mmcssHandle_);
    mmcssHandle_ = nullptr;
  }

  if (timer_) {
    CancelWaitableTimer(timer_);
    CloseHandle(timer_);
    timer_ = nullptr;
  }

  if (timerPeriodSet_) {
    timeEndPeriod(1);
    timerPeriodSet_ = false;
  }

  threadPolicyInitialized_ = false;
  configured_ = false;
  refreshMonitor_ = nullptr;
  refreshRateHz_ = 0.0;
  refreshRateQueryTick_ = 0;
  lastVrrCapHz_ = 0.0;
  Logger::instance().info("FramePacer shutdown complete.");
}
void FramePacer::captureDiagnostics() {
  Diagnostics::instance().activity("Performance.FrameLimiter",paceCalls_,pacedFrames_,0,uncappedFrames_);
}

} // namespace mws
