#include "mws/PassProfiler.hpp"
#include "mws/Logger.hpp"
#include "mws/Diagnostics.hpp"
#include "src/fxp/renderer/GpuTimer.h"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace mws::PassProfiler {
namespace {
struct Record {
    fxp::gpu::Timer gpu;
    std::uint64_t calls{}, samples{};
    double cpuMs{}, maximumCpuMs{};
    unsigned divisor{1};
    bool gpuSupported{}, gpuChecked{};
};
std::array<Record, static_cast<unsigned>(Pass::Count)> records;
IDirect3DDevice9* device{};
DWORD thread{};
LARGE_INTEGER frequency{};
unsigned interval{}, presents{};
bool enabled{}, frameRunning{};
std::int64_t priorPresent{};
constexpr const char* names[] = {"frame-interval", "buildings-pc", "buildings-day-night", "shadows"};
Record& record(Pass p) { return records[static_cast<unsigned>(p)]; }
std::int64_t ticks() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
bool beginGpu(Record& r) {
    if (!r.gpuChecked) {
        r.gpuChecked = true;
        r.gpuSupported = r.gpu.initialize(device);
    }
    return r.gpuSupported && r.gpu.begin(false);
}
void logStats() {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3)
        << "[PASS-TIMING] independent CPU and delayed GPU timing (ms);"
        << " CPU frame interval includes game, waiting and presenting, NOT game-logic attribution";
    for (unsigned i = 0; i < records.size(); ++i) {
        auto& r = records[i];
        const auto gpu = r.gpu.take(); // no D3DGETDATA_FLUSH, do not stall the render thread
        if (!r.samples && !gpu.samples && !gpu.pending) continue;
        output << " | " << names[i] << ":cpuAvg="
               << (r.samples ? r.cpuMs / r.samples : 0)
               << " cpuMax=" << r.maximumCpuMs << " cpuSamples=" << r.samples
               << " gpuAvg=" << (gpu.samples ? gpu.milliseconds / gpu.samples : 0)
               << " gpuMax=" << gpu.maximum << " gpuSamples=" << gpu.samples
               << " pending=" << gpu.pending << " dropped=" << gpu.dropped;
        r.cpuMs = r.maximumCpuMs = 0;
        r.samples = 0;
    }
    Logger::instance().info(output.str());
}
} // namespace

void install(IDirect3DDevice9* current, const Config& config) {
    shutdown();
    if (!current || !config.passTiming || !config.optimizationsEnabled) return;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(current->GetCreationParameters(&creation)) ||
        (creation.BehaviorFlags & (D3DCREATE_MULTITHREADED | D3DCREATE_PUREDEVICE))) {
        Diagnostics::instance().skipped("Optimization.PassTiming",
            "multithreaded or pure D3D9 device; pass timing unavailable");
        return;
    }
    QueryPerformanceFrequency(&frequency);
    if (!frequency.QuadPart) return;
    device = current;
    thread = GetCurrentThreadId();
    interval = config.optimizationLogInterval ? config.optimizationLogInterval : 1800;
    records[0].divisor = 16; // one GPU frame interval in every 16
    records[1].divisor = 128; // high-frequency building callbacks, sampled
    records[2].divisor = 128;
    records[3].divisor = 16;
    enabled = true;
    Diagnostics::instance().active("Optimization.PassTiming",
        "sampled QPC CPU scopes and nonblocking D3D9 timestamp/disjoint queries");
}
void reset() {
    // Do not query or wait for pending GPU results during device loss.
    for (auto& r : records) {
        r.gpu.reset();
        r.gpuChecked = r.gpuSupported = false;
    }
    frameRunning = false;
    priorPresent = 0;
}
void shutdown() {
    reset();
    enabled = false;
    device = nullptr;
    thread = 0;
    presents = 0;
    for (auto& r : records) { r.calls = r.samples = 0; r.cpuMs = r.maximumCpuMs = 0; }
}
void onPresent() {
    if (!enabled || GetCurrentThreadId() != thread) return;
    if (frameRunning) { record(Pass::Frame).gpu.end(); frameRunning = false; }
    const auto now = ticks();
    auto& frame = record(Pass::Frame);
    if (priorPresent && (frame.calls % frame.divisor == 0)) {
        const double ms = 1000.0 * double(now - priorPresent) / double(frequency.QuadPart);
        frame.cpuMs += ms;
        frame.maximumCpuMs = std::max(frame.maximumCpuMs, ms);
        ++frame.samples;
    }
    priorPresent = now;
    if (++frame.calls % frame.divisor == 0) frameRunning = beginGpu(frame);
    if (++presents >= interval) { presents = 0; logStats(); }
}
Scope::Scope(Pass pass) noexcept : pass_(pass) {
    if (!enabled || GetCurrentThreadId() != thread || !device) return;
    auto& r = record(pass);
    if (++r.calls % r.divisor) return;
    start_ = ticks();
    sampled_ = true;
    gpu_ = beginGpu(r);
}
Scope::~Scope() {
    if (!sampled_) return;
    if (gpu_) record(pass_).gpu.end();
    const auto stop = ticks();
    auto& r = record(pass_);
    const double ms = double(stop - start_) * 1000.0 / double(frequency.QuadPart);
    r.cpuMs += ms;
    r.maximumCpuMs = std::max(r.maximumCpuMs, ms);
    ++r.samples;
}
}
