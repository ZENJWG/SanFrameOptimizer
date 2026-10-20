#include "compiler/Curve.h"
#include "runtime/ParticleMath.h"
#include <cstdio>
#include <array>
#include <random>
#include <cmath>
#define CHECK(x) do { if (!(x)) { std::printf("Failed line %d: %s\n", __LINE__, #x); return 1; } } while(0)
int main() {
    fxp::Curve c;
    const std::array<uint16_t, 3> times{64, 128, 256};
    const std::array<float, 15> data{1,3,7, 2,4,8, 3,5,9, 4,6,10, 5,7,11};
    CHECK(c.compile(times, data, 5, false));
    float output[5]; CHECK(c.evaluate(0, output)); CHECK(output[0] == -1);
    CHECK(c.evaluate(.5f, output)); CHECK(output[4] == 7);
    CHECK(c.evaluate(2, output)); CHECK(output[0] == 7);
    CHECK(c.compile(times, data, 5, true));
    CHECK(c.evaluate(1.5f, output)); CHECK(output[0] == 3);
    CHECK(c.evaluate(-.25f, output)); CHECK(output[0] == -3);
    CHECK(!c.evaluate(NAN, output));
    CHECK(!c.compile(std::array<uint16_t, 3>{0,0,256}, data, 5, false));
    CHECK(!c.evaluate(.5f, output)); // Failed rebuild cannot expose stale keys.
    CHECK(c.compile(times, data, 5, false));
    CHECK(!c.evaluate(.5f, nullptr));
    std::mt19937 rng(1729); std::uniform_real_distribution<float> f(-100, 100);
    for (unsigned run=0; run<2000; ++run) {
        fxp::ParticlePacket p;
        for (unsigned i=0; i<4; ++i) {
            p.age[i]=std::abs(f(rng)); p.life[i]=std::abs(f(rng)); p.dt[i]=std::abs(f(rng))*.001f;
            p.x[i]=f(rng); p.y[i]=f(rng); p.z[i]=f(rng);
            p.vx[i]=f(rng); p.vy[i]=f(rng); p.vz[i]=f(rng);
        }
        const auto before=p; const unsigned dead=p.integrate();
        for (unsigned i=0; i<4; ++i) {
            CHECK(std::abs(p.x[i]-(before.x[i]+before.vx[i]*before.dt[i]))<.0001f);
            CHECK(bool(dead & (1u<<i)) == (before.age[i]+before.dt[i]>=before.life[i]));
        }
    }
    std::puts("Curve endpoints, loops, extrapolation, five-channel SIMD tails and 8,000 packet lanes passed.");
}
