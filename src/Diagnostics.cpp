#include "mws/Diagnostics.hpp"
#include "mws/Logger.hpp"
#include <map>
#include <mutex>
#include <sstream>
#include <vector>
#include <algorithm>
#include <Windows.h>

namespace {
struct FeatureRecord {
  bool enabled{false};
  mws::DiagnosticFeatureState state{mws::DiagnosticFeatureState::Unexercised};
  std::string detail;
  bool sampled{};
  std::uint64_t inputs{}, completed{}, avoided{}, fallbacks{}, failures{}, executionReports{};
  std::string activityContext;
};

std::mutex g_mutex;
std::map<std::string, FeatureRecord, std::less<>> g_features;
bool g_summaryPrinted = false;
std::atomic<unsigned> g_runtimeIntervalMs{};
std::atomic<ULONGLONG> g_nextRuntimeReport{};

const char* stateName(mws::DiagnosticFeatureState s) {
  switch (s) {
    case mws::DiagnosticFeatureState::Active: return "ACTIVE";
    case mws::DiagnosticFeatureState::Effective: return "EFFECTIVE";
    case mws::DiagnosticFeatureState::Skipped: return "SKIPPED";
    case mws::DiagnosticFeatureState::Failed: return "FAILED";
    default: return "UNEXERCISED";
  }
}
}

namespace mws {

Diagnostics& Diagnostics::instance() {
  static Diagnostics d;
  return d;
}

void Diagnostics::configure(bool enabled, unsigned runtimeIntervalSeconds) {
  enabled_ = enabled;
  const auto milliseconds=std::min(runtimeIntervalSeconds,3600u)*1000u;
  g_runtimeIntervalMs=milliseconds;g_nextRuntimeReport=GetTickCount64()+milliseconds;
}

void Diagnostics::reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_features.clear();
  g_summaryPrinted = false;
}

void Diagnostics::registerFeature(std::string_view name, bool enabled) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto [it,inserted]=g_features.try_emplace(std::string(name),FeatureRecord{enabled,DiagnosticFeatureState::Unexercised,{}});
  if(!inserted) {
    if(enabled && !it->second.enabled) {it->second.state=DiagnosticFeatureState::Unexercised;it->second.detail.clear();}
    it->second.enabled=enabled;
  }
}
void Diagnostics::ensureFeature(std::string_view name,bool enabled) {
  std::lock_guard lock(g_mutex);
  g_features.try_emplace(std::string(name),FeatureRecord{enabled,DiagnosticFeatureState::Unexercised,{}});
}

void Diagnostics::setState(std::string_view name, DiagnosticFeatureState state,
                           std::string_view detail) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_features.find(name);
    if (it == g_features.end() || !it->second.enabled)
      return;
    auto& record = it->second;
    if(state==DiagnosticFeatureState::Effective) ++record.executionReports;
    // Avoid per-frame diagnostic spam once a feature has already reached
    // the same proof state.
    if (record.state == state)
      return;
    // FAILED/SKIPPED are terminal evidence for this run. EFFECTIVE outranks
    // ACTIVE and should not be downgraded by later bookkeeping.
    if (record.state == DiagnosticFeatureState::Failed ||
        record.state == DiagnosticFeatureState::Skipped)
      return;
    if (record.state == DiagnosticFeatureState::Effective &&
        state == DiagnosticFeatureState::Active)
      return;
    record.state = state;
    if (!detail.empty()) record.detail = detail;
  }

  if (!enabled_) return; // Keep availability status without verbose diagnostic logging.
  std::ostringstream os;
  os << "[DIAG][FEATURE][" << stateName(state) << "] " << name;
  if (!detail.empty()) os << " - " << detail;
  if (state == DiagnosticFeatureState::Failed)
    Logger::instance().error(os.str());
  else if (state == DiagnosticFeatureState::Skipped)
    Logger::instance().warn(os.str());
  else
    Logger::instance().info(os.str());
}

void Diagnostics::active(std::string_view n, std::string_view d) {
  setState(n, DiagnosticFeatureState::Active, d);
}
void Diagnostics::effective(std::string_view n, std::string_view d) {
  setState(n, DiagnosticFeatureState::Effective, d);
}
void Diagnostics::skipped(std::string_view n, std::string_view d) {
  setState(n, DiagnosticFeatureState::Skipped, d);
}
void Diagnostics::failed(std::string_view n, std::string_view d) {
  setState(n, DiagnosticFeatureState::Failed, d);
}

void Diagnostics::autoDisable(std::string_view name, std::string_view detail) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_features.find(name);
    if (it == g_features.end() || !it->second.enabled)
      return;
    it->second.enabled = false;
    it->second.detail = detail;
    changed = true;
  }
  if (changed && enabled_) {
    std::ostringstream os;
    os << "[DIAG][FEATURE][AUTO-DISABLED] " << name;
    if (!detail.empty()) os << " - " << detail;
    Logger::instance().info(os.str());
  }
}

std::string Diagnostics::optimizationAvailability(bool detailed) const {
    struct Key { const char* name; const char* label; };
    static constexpr Key keys[] = {
        {"Optimization.D3DStateDedup","State"},
        {"Optimization.RenderWareBatching","RW-PC"},
        {"Optimization.RenderWareDayNightBatching","RW-DN"},
        {"Optimization.ImmediateBatching","UP"},
        {"Optimization.StaticPropInstancing","Instances"},
        {"Optimization.VisibilityBoundsCache","Visibility"},
        {"Optimization.PassTiming","PassTimers"}};
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream text;
    text << "Optimizations available:";
    for (const auto& key : keys) {
        const auto it = g_features.find(key.name);
        text << " " << key.label << "=";
        if (it == g_features.end()) { text << "NOT-REGISTERED"; continue; }
        const auto& f = it->second;
        if (!f.enabled) text << (f.detail.empty() ? "OFF" : "AUTO-OFF");
        else text << stateName(f.state);
        if (detailed && !f.detail.empty())
            text << "(" << f.detail << ")";
        text << ";";
    }
    return text.str();
}
void Diagnostics::printSummary() {
  if (!enabled_) return;

  unsigned enabledCount = 0, active = 0, effective = 0;
  unsigned skipped = 0, failed = 0, unexercised = 0;
  std::vector<std::string> activeNames, pending, skippedNames, failedNames;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The GTA window can reach WM_DESTROY before Plugin::shutdown(), while
    // process detach may bypass normal cleanup entirely. Emit the report from
    // whichever safe shutdown path gets here first, but never duplicate it.
    if (g_summaryPrinted)
      return;
    g_summaryPrinted = true;

    for (const auto& [name, r] : g_features) {
      if (!r.enabled) continue;
      ++enabledCount;
      switch (r.state) {
        case DiagnosticFeatureState::Active: ++active; activeNames.push_back(name); break;
        case DiagnosticFeatureState::Effective: ++effective; break;
        case DiagnosticFeatureState::Skipped: ++skipped; skippedNames.push_back(name); break;
        case DiagnosticFeatureState::Failed: ++failed; failedNames.push_back(name); break;
        default: ++unexercised; pending.push_back(name); break;
      }
    }
  }

  printRuntimeSummary();

  Logger::instance().info("[DIAG] ================= SESSION COVERAGE SUMMARY =================");
  auto line = [](const char* label, unsigned value) {
    std::ostringstream os;
    os << "[DIAG][SUMMARY] " << label << value;
    Logger::instance().info(os.str());
  };
  line("Enabled paths: ", enabledCount);
  line("EFFECTIVE:     ", effective);
  line("ACTIVE only:   ", active);
  line("SKIPPED:       ", skipped);
  line("UNEXERCISED:   ", unexercised);
  line("FAILED:        ", failed);

  auto names = [](const char* state, const std::vector<std::string>& v) {
    if (v.empty()) return;
    std::ostringstream os;
    os << "[DIAG][SUMMARY][" << state << "] ";
    for (std::size_t i = 0; i < v.size(); ++i) {
      if (i) os << ", ";
      os << v[i];
    }
    Logger::instance().info(os.str());
  };
  names("ACTIVE", activeNames);
  names("SKIPPED", skippedNames);
  names("UNEXERCISED", pending);
  names("FAILED", failedNames);

  const char* result =
      failed ? "FAILED" :
      (skipped || unexercised || active) ? "PARTIAL RUNTIME COVERAGE" :
      "ALL ENABLED PATHS OBSERVED (performance benefit unmeasured)";
  Logger::instance().info(std::string("[DIAG][SUMMARY] RESULT: ") + result);
  Logger::instance().info("[DIAG] =============================================================");
}

void Diagnostics::activity(std::string_view name,std::uint64_t inputs,std::uint64_t completed,
    std::uint64_t avoided,std::uint64_t fallbacks,std::uint64_t failures,std::string_view context) {
  if(!enabled_) return;
  std::lock_guard lock(g_mutex);
  auto it=g_features.find(name);if(it==g_features.end()) return;
  auto& r=it->second;r.sampled=true;r.inputs=inputs;r.completed=completed;
  r.avoided=avoided;r.fallbacks=fallbacks;r.failures=failures;
  r.activityContext=context;
}
void Diagnostics::addActivity(std::string_view name,std::uint64_t inputs,std::uint64_t completed,
    std::uint64_t avoided,std::uint64_t fallbacks,std::uint64_t failures) {
  if(!enabled_) return;
  std::lock_guard lock(g_mutex);
  auto it=g_features.find(name);if(it==g_features.end()) return;
  auto& r=it->second;r.sampled=true;r.inputs+=inputs;r.completed+=completed;
  r.avoided+=avoided;r.fallbacks+=fallbacks;r.failures+=failures;
}
bool Diagnostics::runtimeReportDue() {
  const auto interval=g_runtimeIntervalMs.load(std::memory_order_relaxed);
  if(!enabled_ || !interval) return false;
  const auto now=GetTickCount64();auto next=g_nextRuntimeReport.load(std::memory_order_relaxed);
  return now>=next && g_nextRuntimeReport.compare_exchange_strong(next,now+interval,std::memory_order_relaxed);
}
void Diagnostics::printRuntimeSummary() {
  if(!enabled_) return;
  std::vector<std::pair<std::string,FeatureRecord>> snapshot;
  {std::lock_guard lock(g_mutex);snapshot.assign(g_features.begin(),g_features.end());}
  Logger::instance().info("[DIAG][RUNTIME] cumulative coverage; EFFECTIVE = observed execution/work reuse, not measured FPS or correctness of every case. Events and counters have feature-specific units.");
  for(const auto& [name,r]:snapshot) {
    std::ostringstream line;
    line<<"[DIAG][RUNTIME] "<<name<<" requested="<<r.enabled<<" state="
        <<(r.enabled?stateName(r.state):(r.detail.empty()?"CONFIG-OFF":"AUTO-DISABLED"))
        <<" executionReports="<<r.executionReports;
    if(r.sampled) line<<" inputs="<<r.inputs<<" completed="<<r.completed<<" avoidedOrEvents="<<r.avoided
        <<" fallbacks="<<r.fallbacks<<" failures="<<r.failures;
    else line<<" activity=NOT-SAMPLED";
    if(!r.detail.empty()) line<<" detail="<<r.detail;
    if(!r.activityContext.empty()) line<<" sample="<<r.activityContext;
    Logger::instance().info(line.str());
  }
}

} // namespace mws
