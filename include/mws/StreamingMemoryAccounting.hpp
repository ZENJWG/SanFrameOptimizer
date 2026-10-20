#pragma once
#include <cstdint>

namespace mws {

// Direct translation of GTA SA's live CStreaming counters for display.
// Never multiply values based on another plugin's INI, guessed accounting
// scale, loaded modules or a hard-coded "expected" streaming limit.
struct StreamingMemoryAccounting {
  std::uint64_t usedBytes{};
  std::uint64_t limitBytes{};
  std::uint64_t peakUsedBytes{};
};

constexpr StreamingMemoryAccounting accountGtaStreamingMemory(
    std::uint32_t gameUsedBytes,
    std::uint32_t gameAvailableBytes,
    std::uint64_t previousPeakBytes) noexcept {
  const std::uint64_t used = gameUsedBytes;
  const std::uint64_t limit = gameAvailableBytes;
  return {used, limit, used > previousPeakBytes ? used : previousPeakBytes};
}

} // namespace mws
