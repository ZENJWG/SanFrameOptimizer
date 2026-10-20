#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace mws {

// Windows process-memory usage for the DistantStreaming x64 helper spawned by
// THIS GTA process. WMI exposes 64-bit byte counters even to a Win32 ASI.
// No pipe calls, injection, cache tampering, or work on the render thread.
class Cache64MemoryTelemetry {
public:
  enum class State : unsigned char { Waiting, Available, Unavailable };
  struct Snapshot {
    State state{State::Waiting};
    DWORD pid{};
    std::uint64_t privateBytes{};
    std::uint64_t workingSetBytes{};
  };

  void start(bool enabled);
  void stop();
  Snapshot sample() const noexcept;

private:
  static DWORD WINAPI workerEntry(void* self);
  void run() noexcept;

  HANDLE stopEvent_{};
  HANDLE worker_{};
  std::atomic<State> state_{State::Waiting};
  std::atomic<DWORD> pid_{0};
  std::atomic<std::uint64_t> privateBytes_{0};
  std::atomic<std::uint64_t> workingSetBytes_{0};
};
} // namespace mws
