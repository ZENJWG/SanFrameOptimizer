#pragma once
#include <string>
#include <string_view>
#include <atomic>
#include <cstdint>

namespace mws {

enum class DiagnosticFeatureState {
  Unexercised,
  Active,
  Effective,
  Skipped,
  Failed
};

class Diagnostics {
public:
  static Diagnostics& instance();

  void configure(bool enabled, unsigned runtimeIntervalSeconds = 30);
  void reset();
  void registerFeature(std::string_view name, bool enabled);
  void ensureFeature(std::string_view name, bool enabled);
  bool enabled() const noexcept {return enabled_.load(std::memory_order_relaxed);}
  void active(std::string_view name, std::string_view detail = {});
  void effective(std::string_view name, std::string_view detail = {});
  void skipped(std::string_view name, std::string_view detail = {});
  void failed(std::string_view name, std::string_view detail = {});
  void autoDisable(std::string_view name, std::string_view detail = {});
  void printSummary();
  // Snapshot after device creation: configured is not the same as installed.
  std::string optimizationAvailability(bool detailed = false) const;
  // Cumulative snapshots: publish at report boundaries, never lock per draw.
  void activity(std::string_view name, std::uint64_t inputs, std::uint64_t completed,
                std::uint64_t avoided = 0, std::uint64_t fallbacks = 0, std::uint64_t failures = 0,
                std::string_view context = {});
  void addActivity(std::string_view name, std::uint64_t inputs, std::uint64_t completed,
                   std::uint64_t avoided = 0, std::uint64_t fallbacks = 0, std::uint64_t failures = 0);
  bool runtimeReportDue();
  void printRuntimeSummary();

private:
  Diagnostics() = default;
  void setState(std::string_view name, DiagnosticFeatureState state,
                std::string_view detail);

  std::atomic<bool> enabled_{false};
};

} // namespace mws
