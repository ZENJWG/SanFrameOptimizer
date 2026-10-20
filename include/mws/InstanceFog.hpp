#pragma once
#include <d3d9.h>
#include <array>
#include <bit>
#include <cmath>

namespace mws::InstanceFog {
// Matches c8/c9/c10 in StaticPropInstancing.hlsl, in both shader stages.
struct State {
    std::array<float, 4> parameters{}; // end, reciprocal span, density, D3DFOGMODE
    std::array<float, 4> control{};    // 0=off, 1=vertex, 2=table Z, 3=table W; range; viewport Z
    std::array<float, 4> color{};
};

inline bool capture(IDirect3DDevice9* device, DWORD rasterCaps, State& result) {
    result = {};
    DWORD enabled{}, table{}, vertex{}, range{}, color{}, bits{};
    const auto get = [&](D3DRENDERSTATETYPE state, DWORD& value) {
        return SUCCEEDED(device->GetRenderState(state, &value));
    };
    if (!get(D3DRS_FOGENABLE, enabled)) return false;
    if (!enabled) return true;
    if (!get(D3DRS_FOGTABLEMODE, table) || !get(D3DRS_FOGVERTEXMODE, vertex) ||
        !get(D3DRS_FOGCOLOR, color)) return false;
    const DWORD mode = table != D3DFOG_NONE ? table : vertex;
    // NONE/NONE can consume a caller-supplied specular-alpha fog factor.
    // That declaration is intentionally outside the instancing subset.
    if (mode != D3DFOG_LINEAR && mode != D3DFOG_EXP && mode != D3DFOG_EXP2) return false;
    result.parameters[3] = static_cast<float>(mode);
    if (mode == D3DFOG_LINEAR) {
        if (!get(D3DRS_FOGSTART, bits)) return false;
        const float start = std::bit_cast<float>(bits);
        if (!get(D3DRS_FOGEND, bits)) return false;
        const float end = std::bit_cast<float>(bits);
        const float span = end - start;
        if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(span) || span <= 0) return false;
        result.parameters[0] = end;
        result.parameters[1] = 1.0f / span;
        if (!std::isfinite(result.parameters[1])) return false;
    } else {
        if (!get(D3DRS_FOGDENSITY, bits)) return false;
        const float density = std::bit_cast<float>(bits);
        if (!std::isfinite(density) || density < 0 || density > 1) return false;
        result.parameters[2] = density;
    }
    if (table != D3DFOG_NONE) {
        if (!(rasterCaps & D3DPRASTERCAPS_FOGTABLE)) return false;
        D3DMATRIX projection{};
        if (FAILED(device->GetTransform(D3DTS_PROJECTION, &projection))) return false;
        for (const auto& row : projection.m)
            for (float value : row) if (!std::isfinite(value)) return false;
        const bool affine = projection._14 == 0 && projection._24 == 0 &&
            projection._34 == 0 && projection._44 == 1;
        const bool perspective = projection._14 == 0 && projection._24 == 0 &&
            projection._34 == 1 && projection._44 == 0;
        // Do not guess eye-relative depth for a custom projection.
        if (!affine && !perspective) return false;
        const bool wFog = perspective && (rasterCaps & D3DPRASTERCAPS_WFOG);
        if (!wFog && !(rasterCaps & D3DPRASTERCAPS_ZFOG)) return false;
        result.control[0] = wFog ? 3.0f : 2.0f;
        D3DVIEWPORT9 viewport{};
        if (FAILED(device->GetViewport(&viewport)) || !std::isfinite(viewport.MinZ) ||
            !std::isfinite(viewport.MaxZ) || viewport.MaxZ < viewport.MinZ) return false;
        // Nondefault viewport depth remapping is outside the verified table-Z
        // subset. Preserve the original draw instead of assuming how the
        // fixed-function driver incorporates this remap into fog depth.
        if (!wFog && (viewport.MinZ != 0 || viewport.MaxZ != 1)) return false;
        result.control[2] = viewport.MinZ;
        result.control[3] = viewport.MaxZ - viewport.MinZ;
    } else {
        if (!(rasterCaps & D3DPRASTERCAPS_FOGVERTEX) || !get(D3DRS_RANGEFOGENABLE, range)) return false;
        if (range && !(rasterCaps & D3DPRASTERCAPS_FOGRANGE)) return false;
        result.control[0] = 1;
        result.control[1] = range ? 1.0f : 0.0f;
    }
    result.color = {((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
        (color & 255) / 255.0f, 0};
    return true;
}

inline bool upload(IDirect3DDevice9* device, const State& state) {
    return SUCCEEDED(device->SetVertexShaderConstantF(8, state.parameters.data(), 1)) &&
        SUCCEEDED(device->SetVertexShaderConstantF(9, state.control.data(), 1)) &&
        SUCCEEDED(device->SetPixelShaderConstantF(8, state.parameters.data(), 1)) &&
        SUCCEEDED(device->SetPixelShaderConstantF(9, state.control.data(), 1)) &&
        SUCCEEDED(device->SetPixelShaderConstantF(10, state.color.data(), 1));
}
}
