#include "mws/DrawTelemetry.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <tuple>

namespace {
std::atomic<IDirect3DDevice9*> device{};
std::atomic<std::uint64_t> calls{}, primitives{};
thread_local unsigned overlayDepth{};
mws::DrawTelemetry::Sample current;
std::uint64_t baselineCalls{}, baselinePrimitives{}, frames{};
ULONGLONG windowStart{};

bool exchange(void** slot, void* replacement) {
  DWORD protection{};
  if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) return false;
  InterlockedExchangePointer(slot, replacement);
  DWORD unused{};
  VirtualProtect(slot, sizeof(void*), protection, &unused);
  return true;
}

template<int Slot, class Signature> struct Hook;
template<int Slot, class T, class R, class... Args>
struct Hook<Slot, R(__stdcall T::*)(Args...)> {
  using Fn = R(STDMETHODCALLTYPE*)(T*, Args...);
  struct Patch { void** table{}; Fn original{}; };
  static inline std::array<Patch, 32> patches{};
  static inline std::atomic<unsigned> count{};
  static bool install(T* self) {
    auto** table = *reinterpret_cast<void***>(self);
    const auto used = count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < used; ++i) if (patches[i].table == table) return true;
    if (used == patches.size()) return false;
    // Publish the chain before exposing the replacement to other threads.
    patches[used] = {table, reinterpret_cast<Fn>(table[Slot])};
    count.store(used + 1, std::memory_order_release);
    if (exchange(&table[Slot], reinterpret_cast<void*>(&invoke))) return true;
    count.store(used, std::memory_order_release);
    return false;
  }
  static void restore() {
    const auto used = count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < used; ++i) {
      const auto& patch = patches[i];
      if (patch.table[Slot] == reinterpret_cast<void*>(&invoke))
        exchange(&patch.table[Slot], reinterpret_cast<void*>(patch.original));
    }
    count = 0;
  }
  static R STDMETHODCALLTYPE invoke(T* self, Args... args) {
    auto** table = *reinterpret_cast<void***>(self);
    Fn original{};
    const auto used = count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < used; ++i)
      if (patches[i].table == table) { original = patches[i].original; break; }
    if (!original) return D3DERR_INVALIDCALL;
    const auto result = original(self, args...);
    if (SUCCEEDED(result) && self == device.load(std::memory_order_relaxed) && overlayDepth == 0) {
      const auto a = std::tuple{args...};
      UINT countPrimitives{};
      if constexpr (Slot == 81) countPrimitives = std::get<2>(a);
      if constexpr (Slot == 82) countPrimitives = std::get<5>(a);
      if constexpr (Slot == 83) countPrimitives = std::get<1>(a);
      if constexpr (Slot == 84) countPrimitives = std::get<3>(a);
      calls.fetch_add(1, std::memory_order_relaxed);
      primitives.fetch_add(countPrimitives, std::memory_order_relaxed);
    }
    return result;
  }
};
template<int Slot, class T, class R, class... Args>
struct Hook<Slot, R(__stdcall T::*)(Args...) noexcept> : Hook<Slot, R(__stdcall T::*)(Args...)> {};
using DP = Hook<81, decltype(&IDirect3DDevice9::DrawPrimitive)>;
using DIP = Hook<82, decltype(&IDirect3DDevice9::DrawIndexedPrimitive)>;
using DPUP = Hook<83, decltype(&IDirect3DDevice9::DrawPrimitiveUP)>;
using DIPUP = Hook<84, decltype(&IDirect3DDevice9::DrawIndexedPrimitiveUP)>;
}

namespace mws::DrawTelemetry {
bool bind(IDirect3DDevice9* next) {
  if (!next) return false;
  if (device.load() == next && current.available) return true;
  const bool ok = DP::install(next) && DIP::install(next) && DPUP::install(next) && DIPUP::install(next);
  device.store(ok ? next : nullptr);
  current = {};
  current.available = ok;
  baselineCalls = calls.load();
  baselinePrimitives = primitives.load();
  frames = 0;
  windowStart = GetTickCount64();
  return ok;
}
void shutdown() {
  device.store(nullptr);
  // Submission/state hooks above us must already have been removed.
  DIPUP::restore(); DPUP::restore(); DIP::restore(); DP::restore();
  current = {};
  calls.store(0); primitives.store(0);
  windowStart = 0; frames = 0;
}
void onFrame() {
  if (!current.available) return;
  ++frames;
  const auto now = GetTickCount64();
  const auto elapsed = now - windowStart;
  if (elapsed < 1000) return;
  const auto totalCalls = calls.load(std::memory_order_relaxed);
  const auto totalPrimitives = primitives.load(std::memory_order_relaxed);
  current.callsPerFrame = static_cast<double>(totalCalls - baselineCalls) / static_cast<double>(frames);
  current.callsPerSecond = static_cast<double>(totalCalls - baselineCalls) * 1000.0 / static_cast<double>(elapsed);
  current.primitivesPerFrame = static_cast<double>(totalPrimitives - baselinePrimitives) / static_cast<double>(frames);
  current.ready = true;
  baselineCalls = totalCalls; baselinePrimitives = totalPrimitives;
  frames = 0; windowStart = now;
}
Sample sample() { return current; }
bool isOverlayDraw() noexcept { return overlayDepth != 0; }
OverlayScope::OverlayScope() { ++overlayDepth; }
OverlayScope::~OverlayScope() { --overlayDepth; }
}
