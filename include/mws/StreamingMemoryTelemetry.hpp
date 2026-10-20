#pragma once
#include <cstdint>

namespace mws {
struct StreamingMemoryTelemetrySnapshot {
  // The live GTA SA 1.0 US CStreaming counters, as the game sees them.
  // Limit adjusters that change ms_memoryAvailable are automatically reflected.
  // No Improved Streaming INI parsing, scaling, clamping or vanilla fallback.
  bool valid{};
  std::uint64_t usedBytes{};
  std::uint64_t limitBytes{};
  std::uint64_t peakUsedBytes{};
};

class StreamingMemoryTelemetry {
public:
  static StreamingMemoryTelemetry& instance();

  // Cheap cached sampler. Re-reads the live GTA globals at least every 250 ms
  // during overlay activity, including if mods change them after startup.
  const StreamingMemoryTelemetrySnapshot& sample() noexcept;
  void reset() noexcept;

private:
  StreamingMemoryTelemetry() = default;
  StreamingMemoryTelemetrySnapshot snapshot_{};
  std::uint64_t lastSampleTickMs_{};
  std::uint64_t lastLogTickMs_{};
  std::uint64_t lastLoggedLimitBytes_{};
};

} // namespace mws
