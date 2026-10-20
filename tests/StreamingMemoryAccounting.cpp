#include "mws/StreamingMemoryAccounting.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
constexpr std::uint32_t MiB(std::uint32_t value) {
  return value * 1024u * 1024u;
}
} // namespace

int main() {
  using mws::accountGtaStreamingMemory;
  // Stock GTA SA streaming limit, read directly from its global.
  auto counters = accountGtaStreamingMemory(MiB(10), MiB(50), 0);
  check(counters.usedBytes == MiB(10), "stock usage changed");
  check(counters.limitBytes == MiB(50), "stock capacity changed");
  check(counters.peakUsedBytes == MiB(10), "initial peak incorrect");

  // A limit adjuster modifies the GTA global after startup. The next sample
  // must use the new number without consulting any mod configuration.
  counters = accountGtaStreamingMemory(MiB(80), MiB(512),
                                       counters.peakUsedBytes);
  check(counters.limitBytes == MiB(512), "runtime raised GTA limit ignored");
  check(counters.usedBytes == MiB(80), "runtime usage not direct");
  check(counters.peakUsedBytes == MiB(80), "peak not updated");

  // A later lower limit must also become authoritative immediately.
  counters = accountGtaStreamingMemory(MiB(20), MiB(128),
                                       counters.peakUsedBytes);
  check(counters.limitBytes == MiB(128), "runtime lowered GTA limit ignored");
  check(counters.peakUsedBytes == MiB(80), "peak unexpectedly reset");

  // Never apply the historical Improved Streaming 2x multiplier.
  counters = accountGtaStreamingMemory(MiB(45), MiB(256), 0);
  check(counters.usedBytes == MiB(45), "usage scaled without GTA evidence");
  check(counters.limitBytes == MiB(256), "capacity scaled without GTA evidence");

  // A different mod may write a new capacity at any time. The accounting
  // path must preserve *every* game value, with no prior-value heuristics.
  constexpr std::uint32_t observedLimits[] = {
      MiB(50), MiB(64), MiB(100), MiB(128), MiB(256), MiB(512),
      MiB(1024), MiB(2048), 0xFFFFFFFFu
  };
  for (const auto gameLimit : observedLimits) {
    counters = accountGtaStreamingMemory(MiB(25), gameLimit,
                                         counters.peakUsedBytes);
    check(counters.limitBytes == gameLimit,
          "live GTA capacity was inferred, multiplied, clamped or cached");
  }

  // GTA's used counter can briefly exceed its budget. Never alter the
  // capacity to make the percentage appear below 100%.
  counters = accountGtaStreamingMemory(MiB(240), MiB(128), 0);
  check(counters.usedBytes == MiB(240), "overbudget usage was capped");
  check(counters.limitBytes == MiB(128), "overbudget limit was inflated");

  // Preserve unsigned GTA counters and never clamp to a 2 GiB signed limit.
  counters = accountGtaStreamingMemory(0xC0000000u, 0xFFFFFFFFu, 0);
  check(counters.usedBytes == 0xC0000000ull,
        "unsigned usage truncated");
  check(counters.limitBytes == 0xFFFFFFFFull,
        "unsigned limit truncated or doubled");

  std::cout << "Live GTA streaming accounting checks passed\n";
  return 0;
}
