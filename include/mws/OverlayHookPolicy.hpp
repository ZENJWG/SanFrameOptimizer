#pragma once

namespace mws {

// EndScene is observed via a cloned device COM vtable by some streaming ASIs.
// Do not inline-detour it a second time. The existing Present fallback keeps
// the live streaming-memory and Cache64 OSD counters available.
constexpr bool allowEndSceneOverlayHook(
    bool distantStreamingLoaded, bool forcePresentOnly) noexcept {
  return !distantStreamingLoaded && !forcePresentOnly;
}

} // namespace mws
