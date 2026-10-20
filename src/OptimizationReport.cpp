#include "mws/OptimizationReport.hpp"
#include "mws/Logger.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/FxpOptimizer.hpp"
#include "mws/EnginePass.hpp"
#include "mws/FramePacer.hpp"
#include "mws/OptimizationRuntime.hpp"
#include "mws/D3D9Hook.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace mws::OptimizationReport {
namespace {
Config config;
bool configured{}, proper{};
std::uint64_t frames{}, totalFrames{};
ULONGLONG startTick{};
Rows previous;
Rows collect() {
  Rows rows;
  rows.reserve(32);
  appendStateOptimizerReport(rows, config);
  RenderWareBatcher::appendOptimizerReport(rows, config);
  RenderSubmission::appendOptimizerReport(rows, config);
  EngineOptimizations::appendOptimizerReport(rows, config);
  return rows;
}
void captureDiagnostics() {
  auto& diagnostics=Diagnostics::instance();
  for(const auto& row:collect()) {
    const std::string name=std::string("Optimization.")+row.name;
    diagnostics.ensureFeature(name,row.requested);
    diagnostics.activity(name,row.input,0,row.saved,0,0,std::string("inputUnit=")+row.inputUnit+
        " routeActive="+std::to_string(row.active)+" adaptiveOff="+std::to_string(row.adaptiveOff)+
        "; completion/failure/fallback outcomes not measured by ranking counters");
    if(row.active) {
      diagnostics.active(name,row.inputUnit);
      if(row.saved) diagnostics.effective(name,std::string("observed counter activity; unit=")+row.inputUnit);
    }
  }
  FxpOptimizer::captureDiagnostics();EnginePass::captureDiagnostics();
  D3D9Hook::captureDiagnostics();
  FramePacer::instance().captureDiagnostics();OptimizationRuntime::instance().captureDiagnostics();
}
const char* status(const Row& r) {
  if (!config.optimizationsEnabled) return "MASTER-OFF";
  if (!r.requested) return "CONFIG-OFF";
  if (proper) {
    const bool visibility = std::strcmp(r.name, "VisibilityBoundsCache") == 0;
    const bool draw = std::strcmp(r.name, "RenderWareBatching") == 0 ||
                      std::strcmp(r.name, "RenderWareDayNightBatching") == 0 ||
                      std::strcmp(r.name, "ImmediateBatching") == 0 ||
                      std::strcmp(r.name, "StaticPropInstancing") == 0;
    const bool state = std::strcmp(r.group, "state-calls") == 0;
    if (draw || visibility || state)
      return "COMPAT-OFF(ProperShaders)";
  }
  if (!r.active) return "UNAVAILABLE(see-startup-diagnostics)";
  if (r.adaptiveOff) return "ADAPTIVE-OFF";
  return "ACTIVE";
}
bool comparable(const Row& r) {
  if (!config.optimizationsEnabled || !r.requested || !r.active) return false;
  if (!proper) return true;
  return false; // Renderer/visibility rows are compatibility-disabled.
}
std::uint64_t delta(std::uint64_t now, std::uint64_t before) {
  // Resource/hook reinstallation can reset counters. Never unsigned-wrap.
  return now >= before ? now - before : now;
}
void report(const char* reason) {
  auto rows = collect();
  const auto now = GetTickCount64();
  const double seconds = static_cast<double>(now - startTick) / 1000.0;
  std::ostringstream output;
  output << std::fixed << std::setprecision(2)
         << "[OPT-RANK] " << reason << " frames=" << frames
         << " seconds=" << seconds << " totalFrames=" << totalFrames
         << "; window deltas; ranked by work avoided, NOT CPU/GPU time or FPS gain."
         << " Logging and hook overhead are not subtracted.";
  Logger::instance().info(output.str());

  Rows window = rows;
  for (std::size_t i = 0; i < window.size(); ++i) {
    if (i < previous.size() && std::strcmp(rows[i].name, previous[i].name) == 0) {
      window[i].input = delta(rows[i].input, previous[i].input);
      window[i].saved = delta(rows[i].saved, previous[i].saved);
    }
    const auto& r = window[i];
    const bool scheduling = std::strcmp(r.group, "scheduling-events") == 0;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2)
         << "[OPT-RANK] " << r.name << " status=" << status(r)
         << " group=" << r.group << " " << r.inputUnit << "=" << r.input
         << (scheduling ? " events=" : " avoided=") << r.saved;
    if (!scheduling) {
      line << " avoided/frame=" << (frames ? double(r.saved) / double(frames) : 0.0);
      // RW inputs count callbacks; a callback can remove many draws.
      if (std::strcmp(r.inputUnit, "callbacks") != 0) {
        if (r.input) line << " hit/reduction%=" << 100.0 * double(r.saved) / double(r.input);
        else line << " hit/reduction%=n/a";
      }
    }
    line << (scheduling ? " lifetimeEvents=" : " lifetimeAvoided=") << rows[i].saved
         << " ini=[Optimization]." << r.key;
    if (comparable(r) && !r.input && !r.saved) line << " assessment=NO-SAMPLE";
    else if (comparable(r) && !r.saved && !scheduling)
      line << " assessment=ZERO-SAVINGS-IN-WINDOW";
    if (scheduling) line << " assessment=ACTIVITY-ONLY(not-time-saved)";
    Logger::instance().info(line.str());
  }
  for (const char* group : {"state-calls", "draw-submissions", "bounds-recomputations"}) {
    std::vector<const Row*> ranked;
    for (const auto& row : window)
      if (std::strcmp(row.group, group) == 0 && comparable(row) && (row.input || row.saved))
        ranked.push_back(&row);
    std::stable_sort(ranked.begin(), ranked.end(), [](const Row* a, const Row* b) {
      return a->saved > b->saved;
    });
    std::ostringstream line;
    line << "[OPT-RANK] group=" << group;
    if (ranked.empty()) {
      line << " most=n/a least=n/a (no active sampled optimizer)";
    } else {
      line << " most=";
      bool first = true;
      for (const auto* r : ranked) if (r->saved == ranked.front()->saved) {
        if (!first) line << ',';
        first = false;
        line << r->name;
      }
      line << " least=";
      first = true;
      for (const auto* r : ranked) if (r->saved == ranked.back()->saved) {
        if (!first) line << ',';
        first = false;
        line << r->name;
      }
      line << " order(high-to-low)=";
      for (std::size_t i = 0; i < ranked.size(); ++i) {
        if (i) line << ',';
        line << ranked[i]->name << ':' << ranked[i]->saved;
      }
      line << "; ties share rank; compare representative gameplay before disabling";
    }
    Logger::instance().info(line.str());
  }
  previous = std::move(rows);
  frames = 0;
  startTick = now;
}
}
void configure(const Config& c, bool properShaders) {
  config = c;
  proper = properShaders;
  configured = config.optimizationLogInterval != 0;
  frames = totalFrames = 0;
  startTick = GetTickCount64();
  previous.clear();
  if (!configured) return;
  previous = collect();
  Logger::instance().info(
      std::string("[OPT-RANK] enabled: first report after ") +
      std::to_string(config.optimizationLogInterval) +
      " presented frames; OptimizationLogInterval controls window reports; "
      "state categories, batching and visibility ranked separately. "
      "WorkerPreparation, TransientArena, frame pacing "
      "and DXVK tuning: benefit UNMEASURED, excluded from rankings. "
      "D3D state rows belong to D3DStateDeduplication; shared INI keys control both shader stages.");
}
void configureDiagnostics(const Config& c) {config=c;}
void onPresent() {
  if(Diagnostics::instance().runtimeReportDue()) {captureDiagnostics();Diagnostics::instance().printRuntimeSummary();}
  if (!configured) return;
  ++frames;
  ++totalFrames;
  if (frames >= config.optimizationLogInterval) report("interval");
}
void finish() {
  captureDiagnostics();
  if (configured && frames) report("final-partial-window");
  configured = false;
}
}
