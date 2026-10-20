#include "mws/OptimizationRuntime.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/Logger.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <new>
#include <thread>
#include <string>
#include <vector>

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_jobs;
constexpr std::size_t kMaxQueuedJobs = 64;
std::size_t g_peakQueuedJobs = 0;
std::vector<std::thread> g_workers;
bool g_stopping = false;
bool g_enabled = false;
std::atomic<bool> g_scratchEnabled{true};
std::atomic<std::uint64_t> g_frameEpoch{1};
std::atomic<std::size_t> g_scratchArenaCount{0};
std::atomic<std::uint64_t> g_queued{},g_completed{},g_sync{},g_rejected{},g_failed{};
std::atomic<std::uint64_t> g_scratchCalls{},g_scratchSuccess{};

constexpr std::size_t kScratchBytes = 256 * 1024;

struct ThreadScratch {
  ThreadScratch() {
    g_scratchArenaCount.fetch_add(1, std::memory_order_relaxed);
  }
  ~ThreadScratch() {
    g_scratchArenaCount.fetch_sub(1, std::memory_order_relaxed);
  }
  alignas(64) std::array<std::byte, kScratchBytes> bytes{};
  std::size_t offset{0};
  std::uint64_t epoch{0};
};

// Most game/driver threads never use this API. Do not reserve and zero 256 KiB
// of static TLS for every thread in the process just by loading SanFrame.
thread_local std::unique_ptr<ThreadScratch> g_scratch;

void workerMain() {
  for (;;) {
    std::function<void()> job;

    {
      std::unique_lock lock(g_mutex);
      g_cv.wait(lock, [] { return g_stopping || !g_jobs.empty(); });

      if (g_stopping && g_jobs.empty())
        return;

      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }

    if (job) {
      try {
        job();
        g_completed.fetch_add(1,std::memory_order_relaxed);
        mws::Diagnostics::instance().effective(
            "Optimization.WorkerPreparation",
            "background worker executed a queued preparation job");
      } catch (...) {
        g_failed.fetch_add(1,std::memory_order_relaxed);
        mws::Logger::instance().warn("Optimization background job failed.");
        mws::Diagnostics::instance().failed(
            "Optimization.WorkerPreparation",
            "background preparation job threw an exception");
      }
    }
  }
}

} // namespace

namespace mws {

OptimizationRuntime& OptimizationRuntime::instance() {
  static OptimizationRuntime rt;
  return rt;
}

bool OptimizationRuntime::initialize(const Config& config) {
  {
    std::lock_guard lock(g_mutex);
    if (g_enabled)
      return true;
    g_stopping = false;
  }

  g_scratchEnabled = config.sanFrameTransientArena;
  if(config.sanFrameTransientArena) Diagnostics::instance().active("Optimization.TransientArena","scratch allocation enabled; awaiting real allocation requests");

  if (!config.workerPreparation)
    return false;

  unsigned count = static_cast<unsigned>(std::max(1, config.workerThreads));
  count = std::min(count, 4u);

  try {
    g_workers.reserve(count);
    for (unsigned i = 0; i < count; ++i)
      g_workers.emplace_back(&workerMain);
  } catch (...) {
    shutdown();
    {
      std::lock_guard lock(g_mutex);
      g_stopping = false; // Preserve enqueue's synchronous fallback.
    }
    Logger::instance().warn(
        "Optimization worker pool could not be created.");
    Diagnostics::instance().failed(
        "Optimization.WorkerPreparation",
        "worker pool creation failed");
    return false;
  }

  {
    std::lock_guard lock(g_mutex);
    g_enabled = true;
  }
  Logger::instance().info(
      "Optimization worker pool ACTIVE: pure/background preparation moved off the GTA thread.");
  Diagnostics::instance().active("Optimization.WorkerPreparation", "worker pool created");

  // Real queued preparation work, not a diagnostic-only no-op: allocate the
  // per-worker transient scratch arena ahead of first use. workerMain promotes
  // the feature to EFFECTIVE after this queued task completes successfully.
  if(config.sanFrameTransientArena) enqueue([] {
    if (!g_scratch) {
      g_scratch = std::make_unique<ThreadScratch>();
    }
  });
  return true;
}

void OptimizationRuntime::shutdown() {
  {
    std::lock_guard lock(g_mutex);
    g_stopping = true;
    g_enabled = false;
  }
  g_cv.notify_all();

  for (auto& t : g_workers) {
    if (t.joinable())
      t.join();
  }

  g_workers.clear();
  captureDiagnostics();

  {
    std::lock_guard lock(g_mutex);
    if (g_peakQueuedJobs != 0) {
      Logger::instance().info(
          "Optimization worker queue peak=" + std::to_string(g_peakQueuedJobs) +
          "/" + std::to_string(kMaxQueuedJobs));
    }
    g_jobs.clear();
    g_peakQueuedJobs = 0;
  }

}

void OptimizationRuntime::enqueue(std::function<void()> job) {
  if (!job)
    return;

  bool runSynchronously = false;
  {
    std::lock_guard lock(g_mutex);
    if (g_stopping) {
      g_rejected.fetch_add(1,std::memory_order_relaxed);
      return;
    }
    if (g_enabled && g_jobs.size() < kMaxQueuedJobs) {
      g_jobs.emplace_back(std::move(job));
      g_queued.fetch_add(1,std::memory_order_relaxed);
      g_peakQueuedJobs = std::max(g_peakQueuedJobs, g_jobs.size());
      g_cv.notify_one();
      return;
    }
    // Never allow an unbounded producer burst to retain arbitrary std::function
    // captures. Overflow work remains correct by running on the caller.
    runSynchronously = true;
  }
  if (runSynchronously) {
    g_sync.fetch_add(1,std::memory_order_relaxed);
    try {job();} catch(...) {g_failed.fetch_add(1,std::memory_order_relaxed);throw;}
  }
}

void OptimizationRuntime::beginFrame() noexcept {
  Logger::instance().flushIfDue();
  if (g_scratchEnabled.load(std::memory_order_relaxed))
    g_frameEpoch.fetch_add(1, std::memory_order_relaxed);
}

void* OptimizationRuntime::scratchAlloc(
    std::size_t bytes,
    std::size_t alignment) noexcept {
  if (!g_scratchEnabled.load(std::memory_order_relaxed) || bytes == 0)
    return nullptr;
  g_scratchCalls.fetch_add(1,std::memory_order_relaxed);

  alignment = std::max<std::size_t>(alignment, 1);
  if ((alignment & (alignment - 1)) != 0 || alignment > kScratchBytes ||
      bytes > kScratchBytes)
    return nullptr;
  if (!g_scratch) {
    g_scratch.reset(new (std::nothrow) ThreadScratch);
    if (!g_scratch)
      return nullptr;
  }
  auto& scratch = *g_scratch;

  const auto epoch = g_frameEpoch.load(std::memory_order_relaxed);
  if (scratch.epoch != epoch) {
    scratch.epoch = epoch;
    scratch.offset = 0;
  }

  const std::size_t mask = alignment - 1;
  const auto base = reinterpret_cast<std::uintptr_t>(scratch.bytes.data());
  const auto address = base + scratch.offset;
  const std::size_t padding = (alignment - (address & mask)) & mask;
  const std::size_t aligned = scratch.offset + padding;

  if (aligned > scratch.bytes.size() ||
      bytes > scratch.bytes.size() - aligned) {
    return nullptr;
  }

  void* result = scratch.bytes.data() + aligned;
  scratch.offset = aligned + bytes;
  g_scratchSuccess.fetch_add(1,std::memory_order_relaxed);
  return result;
}
void OptimizationRuntime::captureDiagnostics() {
  auto& diagnostics=Diagnostics::instance();if(!diagnostics.enabled()) return;
  const auto calls=g_scratchCalls.load(std::memory_order_relaxed);
  const auto success=g_scratchSuccess.load(std::memory_order_relaxed);
  diagnostics.activity("Optimization.TransientArena",calls,success,0,calls>=success?calls-success:0);
  if(success) diagnostics.effective("Optimization.TransientArena","real scratch allocations served; benefit unmeasured");
  diagnostics.activity("Optimization.WorkerPreparation",g_queued.load()+g_sync.load()+g_rejected.load(),
      g_completed.load(),0,g_sync.load()+g_rejected.load(),g_failed.load(),
      "inputs=queued/synchronous/rejected jobs; completed=background jobs; fallbacks=synchronous/rejected jobs");
  Logger::instance().info("[DIAG][RUNTIME] retainedScratchBytes="+std::to_string(trackedMemoryBytes()));
}

std::size_t OptimizationRuntime::trackedMemoryBytes() const noexcept {
  // Scratch arenas dominate this runtime's predictable retained footprint.
  // std::deque/std::function allocator metadata and capture allocations are not
  // portable to measure exactly, so they are deliberately not guessed here.
  return g_scratchArenaCount.load(std::memory_order_relaxed) * sizeof(ThreadScratch);
}

} // namespace mws
