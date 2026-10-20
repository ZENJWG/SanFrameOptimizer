#include <safetyhook.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
struct Particle { float age{}, life{4}; };
struct Emitter {
    __declspec(noinline) bool update(float dt, Particle* particle) {
        particle->age += dt;
        return particle->age >= particle->life;
    }
};
SafetyHookInline hook;
unsigned intercepted{};
bool __fastcall Redirect(Emitter* emitter, void*, float dt, Particle* particle) {
    ++intercepted;
    return hook.unsafe_thiscall<bool>(emitter, dt * 2, particle);
}
int main() {
    auto member = &Emitter::update;
    static_assert(sizeof(member) == sizeof(void*));
    void* address{}; std::memcpy(&address, &member, sizeof(address));
    auto created = safetyhook::InlineHook::create(address, reinterpret_cast<void*>(&Redirect), safetyhook::InlineHook::StartDisabled);
    if (!created) return 1;
    hook = std::move(*created);
    Emitter emitter; Particle particle;
    if (emitter.update(1, &particle) || particle.age != 1 || intercepted) return 1;
    if (!hook.enable()) return 1;
    particle.age = 0;
    if (!emitter.update(2, &particle) || std::abs(particle.age - 4) > .00001f || intercepted != 1) return 1;
    if (!hook.disable()) return 1;
    particle.age = 0;
    if (emitter.update(1, &particle) || particle.age != 1 || intercepted != 1) return 1;
    std::puts("x86 thiscall float/pointer arguments, fastcall detour, boolean return, disabled preparation and restoration passed.");
}
