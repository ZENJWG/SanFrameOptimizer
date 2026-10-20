#include "mws/StreamingMemoryTelemetry.hpp"
#include "mws/StreamingMemoryAccounting.hpp"
#include "mws/GtaSa10US.hpp"
#include "mws/Logger.hpp"
#include <Windows.h>
#include <cstdint>
#include <string>

namespace mws {

StreamingMemoryTelemetry& StreamingMemoryTelemetry::instance() {
  static StreamingMemoryTelemetry telemetry;
  return telemetry;
}

const StreamingMemoryTelemetrySnapshot& StreamingMemoryTelemetry::sample() noexcept {
  const auto now = static_cast<std::uint64_t>(GetTickCount64());
  if (lastSampleTickMs_ != 0 && now - lastSampleTickMs_ < 250)
    return snapshot_;
  lastSampleTickMs_ = now;

  // The GTA engine's live CStreaming globals are the sole authority.
  // Modded capacities appear automatically when they update the GTA value.
  // Neither ImprovedStreaming.ini nor other mod INIs are consulted.
  const auto game = gta::streamingMemorySnapshot();
  if (!game.valid) {
    if (snapshot_.valid) {
      Logger::instance().warn(
          "[GTA Stream] Live CStreaming counters became unavailable; "
          "the OSD will not substitute a mod INI or inferred limit.");
    }
    snapshot_.valid = false;
    return snapshot_;
  }

  const auto accounting = accountGtaStreamingMemory(
      game.usedBytes, game.availableBytes, snapshot_.peakUsedBytes);

  // Keep the OSD current on every sample, but summarize dynamic budget changes
  // at most once per 30 seconds. Compare against the last *logged* limit so a
  // change that settles during the interval is still reported later. Verbose
  // mode retains individual changes for troubleshooting.
  const bool first = !snapshot_.valid;
  const bool changed = accounting.limitBytes != lastLoggedLimitBytes_;
  if (first || (changed && (Logger::instance().debugEnabled() ||
                           now - lastLogTickMs_ >= 30000))) {
    const std::string event =
        snapshot_.valid ? "changed" : "detected";
    Logger::instance().info(
        "[GTA Stream] CStreaming::ms_memoryAvailable " + event +
        ": " + std::to_string(accounting.limitBytes) +
        " bytes (" + std::to_string(accounting.limitBytes / (1024u * 1024u)) +
        " whole MiB); source=game memory 0x008A5A80; multiplier=none");
    lastLogTickMs_ = now;
    lastLoggedLimitBytes_ = accounting.limitBytes;
  }

  snapshot_.usedBytes = accounting.usedBytes;
  snapshot_.limitBytes = accounting.limitBytes;
  snapshot_.peakUsedBytes = accounting.peakUsedBytes;
  snapshot_.valid = true;
  return snapshot_;
}

void StreamingMemoryTelemetry::reset() noexcept {
  snapshot_ = {};
  lastSampleTickMs_ = 0;
  lastLogTickMs_ = 0;
  lastLoggedLimitBytes_ = 0;
}

} // namespace mws
