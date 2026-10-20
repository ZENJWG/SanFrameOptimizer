#pragma once
#include <Windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstddef>

namespace mws::gta {

// GTA SA Classic 1.0 US data used to identify the game presentation device.
inline constexpr std::uintptr_t kRsGlobal = 0x00C17040;
struct RsPlatformSpecificLite {
  HWND window;             // +0x00
  HINSTANCE instance;      // +0x04
  DWORD fullScreen;        // +0x08
};

struct RsGlobalTypeLite {
  const char* appName;      // +0x00
  std::int32_t maximumWidth;// +0x04
  std::int32_t maximumHeight;//+0x08
  std::int32_t maxFPS;      // +0x0C
  std::int32_t quit;        // +0x10
  void* ps;                 // +0x14
};

inline RsGlobalTypeLite& rsGlobal() noexcept {
  return *reinterpret_cast<RsGlobalTypeLite*>(kRsGlobal);
}
// GTA SA 1.0 US CStreaming counters (plugin-sdk / gta-reversed mappings).
// GTA's active ms_memoryAvailable is the engine's streaming budget, not a
// value from a mod INI. Limit adjusters that patch this global are reflected
// automatically; do not use vanilla 50 MiB as a fallback or double the value.
inline constexpr std::uintptr_t kStreamingMemoryAvailable = 0x008A5A80;
inline constexpr std::uintptr_t kStreamingMemoryUsed      = 0x008E4CB4;

struct StreamingMemorySnapshot {
  std::uint32_t usedBytes{};
  std::uint32_t availableBytes{};
  bool valid{};
};

StreamingMemorySnapshot streamingMemorySnapshot() noexcept;

bool addressIsExecutable(std::uintptr_t address) noexcept;
bool addressIsReadable(std::uintptr_t address, std::size_t bytes) noexcept;

} // namespace mws::gta
