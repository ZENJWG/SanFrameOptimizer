#include "mws/UsageTelemetry.hpp"
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {
std::uint64_t ticks(const FILETIME& time) {
  return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
      time.dwLowDateTime;
}

struct GpuQuery {
  PDH_HQUERY query{};
  PDH_HCOUNTER counter{};
  ~GpuQuery() { close(); }
  void close() {
    if (query) PdhCloseQuery(query);
    query = nullptr;
    counter = nullptr;
  }
  bool open() {
    close();
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query,
            L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
      close();
      return false;
    }
    PdhCollectQueryData(query); // Prime rate counters; wait for the next sample.
    return true;
  }
  double sample() {
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) return -1.0;
    DWORD bytes = 0, count = 0;
    constexpr DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr) != PDH_MORE_DATA)
      return -1.0;
    // Allocate suitably aligned storage for the counter array and its names.
    std::vector<std::uint64_t> storage((bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
    if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, items) != ERROR_SUCCESS)
      return -1.0;
    std::map<std::wstring, double> engines;
    for (DWORD i = 0; i < count; ++i) {
      const auto& value = items[i].FmtValue;
      if (!items[i].szName ||
          (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
          !std::isfinite(value.doubleValue) || value.doubleValue < 0.0)
        continue;
      const std::wstring name(items[i].szName);
      const auto adapter = name.find(L"luid_");
      if (adapter == std::wstring::npos) continue;
      // Combine all processes using the same adapter/physical GPU/engine.
      // Summing independent engines would incorrectly exceed total utilization.
      engines[name.substr(adapter)] += value.doubleValue;
    }
    double busiest = -1.0;
    for (const auto& engine : engines) busiest = std::max(busiest, engine.second);
    return busiest < 0.0 ? -1.0 : std::clamp(busiest, 0.0, 100.0);
  }
};

bool executionTicks(HANDLE handle, bool thread, std::uint64_t& result) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  const BOOL ok = thread ? GetThreadTimes(handle, &creation, &exit, &kernel, &user)
      : GetProcessTimes(handle, &creation, &exit, &kernel, &user);
  if (!ok) return false;
  result = ticks(kernel) + ticks(user);
  return true;
}
bool renderTicks(DWORD id, std::uint64_t& result) {
  if (!id) return false;
  const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
  if (!thread) return false;
  const bool ok = executionTicks(thread, true, result);
  CloseHandle(thread);
  return ok;
}
} // namespace

namespace mws {
void UsageTelemetry::start(bool cpu, bool gpu, bool process, bool threads) {
  stop();
  if (!cpu && !gpu && !process && !threads) return;
  cpuEnabled_ = cpu;
  gpuEnabled_ = gpu;
  processEnabled_ = process;
  threadsEnabled_ = threads;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) return;
  worker_ = CreateThread(nullptr, 0, workerEntry, this, 0, nullptr);
  if (!worker_) {
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
}

void UsageTelemetry::stop() {
  if (worker_) {
    SetEvent(stopEvent_);
    WaitForSingleObject(worker_, INFINITE);
    CloseHandle(worker_);
    worker_ = nullptr;
  }
  if (stopEvent_) CloseHandle(stopEvent_);
  stopEvent_ = nullptr;
  cpuPercent_.store(-1.0);
  gpuPercent_.store(-1.0);
  renderThread_.store(0);
  { std::lock_guard lock(sampleMutex_); threadSample_ = {}; }
}

DWORD WINAPI UsageTelemetry::workerEntry(void* context) {
  auto& self = *static_cast<UsageTelemetry*>(context);
  try {
    self.run();
  } catch (...) {
    // Allocation/provider failures must not terminate the game.
    self.cpuPercent_.store(-1.0);
    self.gpuPercent_.store(-1.0);
    { std::lock_guard lock(self.sampleMutex_); self.threadSample_ = {}; }
  }
  return 0;
}

void UsageTelemetry::run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  FILETIME idle{}, kernel{}, user{};
  bool haveCpu = cpuEnabled_ && GetSystemTimes(&idle, &kernel, &user);
  GpuQuery gpu;
  if (gpuEnabled_) gpu.open();
  unsigned retry = 0;
  LARGE_INTEGER frequency{}, previousWall{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&previousWall);
  std::uint64_t previousProcess{}, previousRender{};
  bool haveProcess = (processEnabled_ || threadsEnabled_) &&
      executionTicks(GetCurrentProcess(), false, previousProcess);
  DWORD previousThread = renderThread_.load();
  bool haveRender = threadsEnabled_ && renderTicks(previousThread, previousRender);
  while (WaitForSingleObject(stopEvent_, 1000) == WAIT_TIMEOUT) {
    if (processEnabled_ || threadsEnabled_) {
      LARGE_INTEGER wall{};
      QueryPerformanceCounter(&wall);
      const double elapsedTicks = frequency.QuadPart > 0
          ? static_cast<double>(wall.QuadPart - previousWall.QuadPart) * 10000000.0 /
              static_cast<double>(frequency.QuadPart) : 0.0;
      std::uint64_t process{}, render{};
      const bool validProcess = executionTicks(GetCurrentProcess(), false, process);
      const DWORD thread = renderThread_.load();
      const bool validRender = threadsEnabled_ && renderTicks(thread, render);
      ThreadSample sample;
      if (elapsedTicks > 0.0 && validProcess && haveProcess && process >= previousProcess) {
        sample.processCores = static_cast<double>(process - previousProcess) / elapsedTicks;
        if (validRender && haveRender && thread == previousThread && render >= previousRender) {
          const double renderCores = static_cast<double>(render - previousRender) / elapsedTicks;
          sample.renderPercent = std::clamp(renderCores * 100.0, 0.0, 100.0);
          sample.otherCores = std::max(0.0, sample.processCores - renderCores);
        }
      }
      { std::lock_guard lock(sampleMutex_); threadSample_ = sample; }
      previousProcess = process; previousRender = render; previousWall = wall;
      previousThread = thread; haveProcess = validProcess; haveRender = validRender;
    }
    if (cpuEnabled_) {
      FILETIME nextIdle{}, nextKernel{}, nextUser{};
      const bool valid = GetSystemTimes(&nextIdle, &nextKernel, &nextUser) != FALSE;
      double percent = -1.0;
      if (valid && haveCpu && ticks(nextKernel) >= ticks(kernel) &&
          ticks(nextUser) >= ticks(user) && ticks(nextIdle) >= ticks(idle)) {
        const auto total = ticks(nextKernel) - ticks(kernel) + ticks(nextUser) - ticks(user);
        const auto idleDelta = ticks(nextIdle) - ticks(idle);
        if (total > 0 && idleDelta <= total)
          percent = 100.0 * static_cast<double>(total - idleDelta) / static_cast<double>(total);
      }
      cpuPercent_.store(percent);
      idle = nextIdle; kernel = nextKernel; user = nextUser;
      haveCpu = valid;
    }
    if (gpuEnabled_) {
      const double percent = gpu.query ? gpu.sample() : -1.0;
      gpuPercent_.store(percent);
      // Retry missing/failed providers, without continuously rebuilding PDH queries.
      if (percent < 0.0) {
        if (++retry >= 10) { gpu.open(); retry = 0; }
      } else {
        retry = 0;
      }
    }
  }
}
} // namespace mws
