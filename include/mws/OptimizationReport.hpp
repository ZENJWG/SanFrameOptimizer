#pragma once
#include "mws/Config.hpp"
#include <cstdint>
#include <vector>

namespace mws::OptimizationReport {
struct Row {
  const char* group;
  const char* key;
  const char* name;
  const char* inputUnit;
  std::uint64_t input;
  std::uint64_t saved;
  bool requested;
  bool active;
  bool adaptiveOff = false;
};
using Rows = std::vector<Row>;
void configure(const Config& config, bool properShaders);
void configureDiagnostics(const Config& config);
void onPresent();
void finish();
}
namespace mws {
void appendStateOptimizerReport(OptimizationReport::Rows&, const Config&);
namespace RenderWareBatcher { void appendOptimizerReport(OptimizationReport::Rows&, const Config&); }
namespace RenderSubmission { void appendOptimizerReport(OptimizationReport::Rows&, const Config&); }
namespace EngineOptimizations { void appendOptimizerReport(OptimizationReport::Rows&, const Config&); }
}
