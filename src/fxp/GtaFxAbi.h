#pragma once
// Minimal GTA SA 1.0 US FX / RenderWare ABI types used by SanFrame FXP.
//
// These are memory-layout views, NOT Plugin-SDK replacement implementations.
// They intentionally avoid including Plugin-SDK's bundled SafetyHook, which
// conflicts with the separately pinned SafetyHook library used by SanFrame.
//
// Offsets checked against DK22Pac/plugin-sdk revision 15f15b60 (32-bit).
// Do not use these layouts on another GTA executable without version checks.

#include <cstddef>
#include <cstdint>

static_assert(sizeof(void*) == 4, "GTA SA classic requires a 32-bit build");

struct RwV3d {
    float x, y, z;
};

struct RwMatrix {
    RwV3d right;
    std::uint32_t flags;
    RwV3d up;
    std::uint32_t pad1;
    RwV3d at;
    std::uint32_t pad2;
    RwV3d pos;
    std::uint32_t pad3;
};

struct RwRaster;
struct RwCamera;
class FxManager_c;
class FxEmitterBP_c;

struct FxSystem_c {
    std::uint8_t link[0x08];
    void* m_pBlueprint;            // 0x08
    RwMatrix* m_pParentMatrix;     // 0x0C
    RwMatrix m_localMatrix;        // 0x10
    std::uint8_t m_nPlayStatus;    // 0x50
    std::uint8_t m_nKillStatus;    // 0x51
    std::uint8_t rest52[0x02];
    std::int32_t field_54;        // 0x54
    std::uint8_t rest58[0x08];
    std::uint16_t m_nTimeMult;    // 0x60
    std::uint8_t rest62[0x104 - 0x62];
};

static_assert(sizeof(RwV3d) == 12);
static_assert(sizeof(RwMatrix) == 0x40);
static_assert(offsetof(RwMatrix, right) == 0x00);
static_assert(offsetof(RwMatrix, up) == 0x10);
static_assert(offsetof(RwMatrix, at) == 0x20);
static_assert(offsetof(RwMatrix, pos) == 0x30);
static_assert(offsetof(FxSystem_c, m_pBlueprint) == 0x08);
static_assert(offsetof(FxSystem_c, m_pParentMatrix) == 0x0C);
static_assert(offsetof(FxSystem_c, m_localMatrix) == 0x10);
static_assert(offsetof(FxSystem_c, m_nPlayStatus) == 0x50);
static_assert(offsetof(FxSystem_c, m_nKillStatus) == 0x51);
static_assert(offsetof(FxSystem_c, field_54) == 0x54);
static_assert(offsetof(FxSystem_c, m_nTimeMult) == 0x60);
static_assert(sizeof(FxSystem_c) == 0x104);
