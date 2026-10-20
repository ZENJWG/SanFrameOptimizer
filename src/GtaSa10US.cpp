#include "mws/GtaSa10US.hpp"
#include "mws/Logger.hpp"
#include <sstream>
#include <algorithm>
#include <cstdint>

namespace {
// VirtualQuery groups adjacent pages with identical attributes. A valid GTA
// model-info table may span several regions (especially with limit adjusters).
// Validate the complete range rather than requiring one VirtualQuery result to
// cover all 80,000 bytes. Never read or hook uncommitted/guarded/inaccessible pages.
bool pageHas(std::uintptr_t address, std::size_t bytes, bool executable) noexcept {
  if (!address || !bytes || bytes > UINTPTR_MAX - address)
    return false;

  const std::uintptr_t end = address + bytes;
  std::uintptr_t cursor = address;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)))
      return false;

    const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    if (cursor < base || mbi.RegionSize == 0 || mbi.RegionSize > UINTPTR_MAX - base)
      return false;
    const auto regionEnd = base + static_cast<std::uintptr_t>(mbi.RegionSize);
    if (regionEnd <= cursor || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
      return false;

    const DWORD protection = mbi.Protect & 0xFFu;
    const bool readable =
        protection == PAGE_READONLY || protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const bool canExecute =
        protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    if (executable ? !canExecute : !readable)
      return false;

    cursor = (std::min)(end, regionEnd);
  }
  return true;
}
}

namespace mws::gta {

StreamingMemorySnapshot streamingMemorySnapshot() noexcept {
  StreamingMemorySnapshot snapshot{};
  if (!addressIsReadable(kStreamingMemoryUsed, sizeof(std::uint32_t)) ||
      !addressIsReadable(kStreamingMemoryAvailable, sizeof(std::uint32_t))) {
    return snapshot;
  }

  const auto used =
      *reinterpret_cast<const volatile std::uint32_t*>(kStreamingMemoryUsed);
  const auto available =
      *reinterpret_cast<const volatile std::uint32_t*>(kStreamingMemoryAvailable);

  // A zero limit occurs before CStreaming initialization and should render as
  // unavailable, not 0/0 MB. Do not reject values above INT32_MAX: mods
  // can legitimately patch the GTA streaming budget above signed-int range.
  // The result is always the live game counter, never an INI-derived capacity.
  if (available == 0u)
    return snapshot;

  snapshot.usedBytes = used;
  snapshot.availableBytes = available;
  snapshot.valid = true;
  return snapshot;
}

bool addressIsExecutable(std::uintptr_t address) noexcept {
  return pageHas(address, 1, true);
}

bool addressIsReadable(std::uintptr_t address, std::size_t bytes) noexcept {
  return pageHas(address, bytes, false);
}


} // namespace mws::gta
