#pragma once
#include <algorithm>
#include <cstdint>
#include <xmmintrin.h>

namespace fxp {
struct Vec3 { float x, y, z; };
// Four-lane SoA work packet. Stable game particle pointers remain the facade.
struct alignas(16) ParticlePacket {
    float age[4], life[4], dt[4], x[4], y[4], z[4];
    float vx[4], vy[4], vz[4];
    ParticlePacket() noexcept : age{}, life{}, dt{}, x{}, y{}, z{}, vx{}, vy{}, vz{} {}
    // Emitter gathering fills every live lane; only absent lanes need zeroing.
    struct Uninitialized {};
    explicit ParticlePacket(Uninitialized) noexcept {}
    void clearTail(unsigned first) noexcept {
        for (unsigned i = first; i < 4; ++i)
            age[i] = life[i] = dt[i] = x[i] = y[i] = z[i] = vx[i] = vy[i] = vz[i] = 0;
    }
    unsigned integrate() {
        auto delta = _mm_load_ps(dt);
        auto ages = _mm_add_ps(_mm_load_ps(age), delta);
        _mm_store_ps(age, ages);
        unsigned dead = unsigned(_mm_movemask_ps(_mm_cmpge_ps(ages, _mm_load_ps(life))));
        _mm_store_ps(x, _mm_add_ps(_mm_load_ps(x), _mm_mul_ps(_mm_load_ps(vx), delta)));
        _mm_store_ps(y, _mm_add_ps(_mm_load_ps(y), _mm_mul_ps(_mm_load_ps(vy), delta)));
        _mm_store_ps(z, _mm_add_ps(_mm_load_ps(z), _mm_mul_ps(_mm_load_ps(vz), delta)));
        return dead;
    }
};
inline float advanceRotation(float angle, const float* r, uint8_t random, uint8_t multiplier, float dt) {
    const bool a = r[0] > 0 || r[1] > 0, b = r[2] > 0 || r[3] > 0;
    if (!a && !b) return angle;
    float f; unsigned pair; float sign;
    if (a && b) {
        pair = random < 128 ? 0 : 2;
        f = (random < 128 ? float(random) : float(random) - 128) / 128;
        sign = random < 128 ? 1.0f : -1.0f;
    } else {
        pair = a ? 0 : 2; f = random * (1.0f / 255); sign = a ? 1.0f : -1.0f;
    }
    return angle + sign * (r[pair] + (r[pair + 1] - r[pair]) * f) * multiplier * dt * (1.0f / 255);
}
}
