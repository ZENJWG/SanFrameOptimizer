#pragma once
#include <cstdint>

#ifdef SAVO_EXPORTS
#define SAVO_API extern "C" __declspec(dllexport)
#else
#define SAVO_API extern "C" __declspec(dllimport)
#endif

class CEntity;

enum SAVO_Flags : std::uint32_t {
    SAVO_DEFAULT           = 0,
    SAVO_BYPASS_OCCLUSION  = 1u << 0,
    SAVO_DYNAMIC           = 1u << 1,
    SAVO_EXTERNAL          = 1u << 2,
    SAVO_ALWAYS_RETEST     = 1u << 3
};

// ABI v2. Nonzero entity flags bypass the static-bounds cache. Registration is
// bounded and may return false on a slot collision. Unregister before deletion.
// ForceVisible affects the screen test, not GTA's separate occlusion decisions.
SAVO_API std::uint32_t SAVO_GetApiVersion();
SAVO_API bool SAVO_RegisterEntity(CEntity* entity, std::uint32_t flags);
SAVO_API void SAVO_UnregisterEntity(CEntity* entity);
SAVO_API void SAVO_InvalidateEntity(CEntity* entity);
SAVO_API void SAVO_ForceVisibleForFrames(CEntity* entity, std::uint32_t frameCount);

// Renderer mods bypassing the device vtable must flush BEFORE such work, then
// invalidate the state cache AFTER it. Invoke rendering APIs on the game thread.
SAVO_API void SanFrame_FlushRenderQueue();
SAVO_API void SAVO_InvalidateD3DStateCache();
SAVO_API bool SAVO_IsD3D9OptimizerInstalled();
SAVO_API bool SAVO_IsD3D9StateSuppressionEnabled();
