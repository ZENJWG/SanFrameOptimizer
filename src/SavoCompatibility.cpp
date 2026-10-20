#define SAVO_EXPORTS
#include "SAVO_API.h"
#include "mws/D3D9StateCache.hpp"
#include "mws/EngineOptimizations.hpp"
#include "mws/RenderSubmission.hpp"

// Preserve the SAVO v2 ABI. Entity policies now feed SanFrame's conservative
// bounds cache; camera and occlusion calculations remain owned by GTA.

std::uint32_t SAVO_GetApiVersion() {
  return 2;
}

bool SAVO_RegisterEntity(CEntity* entity, std::uint32_t flags) {
  return mws::EngineOptimizations::registerEntity(entity, flags);
}

void SAVO_UnregisterEntity(CEntity* entity) { mws::EngineOptimizations::unregisterEntity(entity); }
void SAVO_InvalidateEntity(CEntity* entity) { mws::EngineOptimizations::invalidateEntity(entity); }
void SAVO_ForceVisibleForFrames(CEntity* entity, std::uint32_t frames) { mws::EngineOptimizations::forceVisible(entity, frames); }

void SAVO_InvalidateD3DStateCache() {
  mws::RenderSubmission::flush();
  mws::D3D9StateCache::instance().invalidate();
}

bool SAVO_IsD3D9OptimizerInstalled() {
  return mws::D3D9StateCache::instance().installed();
}

bool SAVO_IsD3D9StateSuppressionEnabled() {
  return mws::D3D9StateCache::instance().suppressionEnabled();
}

void SanFrame_FlushRenderQueue() {
  mws::RenderSubmission::flush();
}
