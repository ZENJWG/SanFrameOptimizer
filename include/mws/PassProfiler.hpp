#pragma once
#include <d3d9.h>
#include "mws/Config.hpp"
#include <cstdint>

namespace mws::PassProfiler {
enum class Pass : unsigned { Frame, BuildingsPC, BuildingsDayNight, Shadows, Count };
// Call after the actual D3D9 device is known, and before a Reset.
void install(IDirect3DDevice9* device, const Config& config);
void reset();       // release default-pool queries before Reset
void shutdown();
void onPresent();   // non-blocking readback; frame-to-frame interval
class Scope {
public:
    explicit Scope(Pass pass) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Pass pass_;
    std::int64_t start_{};
    bool sampled_{};
    bool gpu_{};
};
}
