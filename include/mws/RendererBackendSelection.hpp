#pragma once

namespace mws {
enum class RendererBackend { D3D9, NativeVulkan, Dxvk, RefxUnverified };

inline RendererBackend selectRendererBackend(bool nativeLive, bool nativePresenting,
    bool dxvkDevice, bool dxvkLoaded, bool refxLoaded) {
  if (nativePresenting || (nativeLive && !dxvkDevice && !dxvkLoaded))
    return RendererBackend::NativeVulkan;
  if (dxvkDevice || (refxLoaded && dxvkLoaded))
    return RendererBackend::Dxvk;
  return refxLoaded ? RendererBackend::RefxUnverified : RendererBackend::D3D9;
}
} // namespace mws
