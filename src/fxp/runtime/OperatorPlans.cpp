#include "OperatorPlans.h"
#include "ProcessLifetime.h"
#include "HookWatch.h"
#include <safetyhook.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace fxp::operators {
namespace {
using Value = void(__thiscall*)(void*, float, float, float, float, bool, void*);
enum { Friction, Wind, Movement, Render, Count };
auto& hooks = processLifetime<std::array<SafetyHookInline, Count>>();
std::array<HookWatch,Count> watches;
engine::Logger log{};
Stats stats;
DWORD thread{};
uint64_t epoch{1};
bool cacheProof{}, planProof{};
struct Manager { int count; void** infos; int8_t movement, render; char padding[2]; };
static_assert(sizeof(Manager) == 12);
struct Info { void** table; uint16_t type; bool particleTime; uint8_t pad; };
static_assert(sizeof(Info) == 8);
struct Vec { float x,y,z; };
struct MovementData { Vec position, velocity; float rotation[4]; bool floating, underwater; char pad[2]; };
struct Key {
    void* info{};
    std::array<uint32_t,4> arguments{};
    bool constant{};
    std::array<std::byte,16> interpolation{};
    Vec wind{}; float speed{};
};
struct Cached { uint64_t epoch{}; Key key{}; Vec value{}; };
bool sameKey(const Key& a,const Key& b) {
    return a.info==b.info && a.arguments==b.arguments && a.constant==b.constant &&
        a.interpolation==b.interpolation && !std::memcmp(&a.wind,&b.wind,sizeof(Vec)) &&
        std::bit_cast<uint32_t>(a.speed)==std::bit_cast<uint32_t>(b.speed);
}
// Separate operator domains prevent neighboring friction/wind objects from
// evicting each other on every particle because of an unlucky pointer hash.
std::array<std::array<Cached,256>,2> cache;
struct Binding { void* object{}; void** table{}; Value function{}; uint16_t type{}; };
struct Plan {
    void* manager{}; Manager metadata{}; bool render{};
    unsigned count{}, bindings{};
    std::array<Binding,64> validation{}, operations{};
};
std::array<Plan,64> plans;
thread_local bool dispatching{};
bool own() { return thread && thread == GetCurrentThreadId(); }
void proof(bool& flag, const char* text) { if (!flag && log) { flag=true; log(text); } }

template<int Kind> void __fastcall cachedValue(void* info, void*, float time, float mult,
    float delta, float length, bool constant, void* output) {
    auto original = hooks[Kind].original<Value>();
    if (!own() || !info || !output || static_cast<Info*>(info)->particleTime ||
        !std::isfinite(time) || !std::isfinite(mult) || !std::isfinite(delta) || !std::isfinite(length) || length == 0) {
        original(info,time,mult,delta,length,constant,output); return;
    }
    ++stats.cacheCalls;
    Key key{}; key.info=info; key.arguments={std::bit_cast<uint32_t>(time),std::bit_cast<uint32_t>(mult),std::bit_cast<uint32_t>(delta),std::bit_cast<uint32_t>(length)};
    key.constant=constant;
    std::memcpy(key.interpolation.data(),static_cast<char*>(info)+8,16);
    if constexpr (Kind == Wind) {
        auto* direction=*reinterpret_cast<Vec**>(0xA9AEAC);
        auto* speed=*reinterpret_cast<float**>(0xA9AEB0);
        if (!direction || !speed) { original(info,time,mult,delta,length,constant,output); return; }
        key.wind=*direction; key.speed=*speed;
    }
    auto& entry=cache[Kind][(reinterpret_cast<uintptr_t>(info)>>3)&255];
    auto* movement=static_cast<MovementData*>(output);
    if(!std::isfinite(movement->velocity.x) || !std::isfinite(movement->velocity.y) || !std::isfinite(movement->velocity.z)) {
        original(info,time,mult,delta,length,constant,output);return;
    }
    if (entry.epoch != epoch || !sameKey(entry.key,key)) {
        MovementData scratch{};
        if constexpr (Kind == Friction) scratch.velocity={1,1,1};
        original(info,time,mult,delta,length,constant,&scratch);
        // Never cache nonfinite factors; preserve stock exceptional arithmetic.
        if (!std::isfinite(scratch.velocity.x) || !std::isfinite(scratch.velocity.y) || !std::isfinite(scratch.velocity.z)) {
            original(info,time,mult,delta,length,constant,output); return;
        }
        entry={epoch,key,scratch.velocity};
    } else {
        ++stats.cacheHits;
        proof(cacheProof,"EXECUTING: system-timed FX operator cache reused a friction/wind result.");
    }
    if constexpr (Kind == Friction) {
        movement->velocity.x*=entry.value.x; movement->velocity.y*=entry.value.y; movement->velocity.z*=entry.value.z;
    } else {
        movement->velocity.x+=entry.value.x; movement->velocity.y+=entry.value.y; movement->velocity.z+=entry.value.z;
    }
}

Plan* getPlan(Manager* manager, bool render) {
    if (!manager || manager->count < 0 || manager->count > 64 || !manager->infos ||
        manager->movement < 0 || manager->render < manager->movement || manager->render > manager->count) return nullptr;
    auto& p=plans[((reinterpret_cast<uintptr_t>(manager)>>4)^unsigned(render))&63];
    const unsigned first=render ? unsigned(manager->render) : unsigned(manager->movement);
    const unsigned last=render ? unsigned(manager->count) : unsigned(manager->render);
    bool valid=p.manager==manager && p.render==render && p.metadata.count==manager->count &&
        p.metadata.infos==manager->infos && p.metadata.movement==manager->movement && p.metadata.render==manager->render;
    for (unsigned i=0; valid && i<p.bindings; ++i) {
        auto* info=static_cast<Info*>(manager->infos[first+i]);
        const auto& b=p.validation[i];
        valid=info && info->table && b.object==info && b.table==info->table && b.type==info->type &&
            b.function==reinterpret_cast<Value>(info->table[2]);
    }
    if (valid) { ++stats.planHits; return &p; }
    p={}; p.manager=manager; p.metadata=*manager; p.render=render;
    for (unsigned i=first; i<last; ++i) {
        auto* info=static_cast<Info*>(manager->infos[i]);
        if (!info || !info->table || !info->table[2]) { p.manager=nullptr; return nullptr; }
        Binding b{info,info->table,reinterpret_cast<Value>(info->table[2]),info->type};
        // Custom callbacks can mutate/reenter the manager while it is being
        // traversed. Preserve their stock traversal contract rather than
        // treating an arbitrary extension as an immutable dispatch plan.
        const auto function=reinterpret_cast<uintptr_t>(b.function);
        if(function<0x4A0000 || function>=0x4AB000) {p.manager=nullptr;return nullptr;}
        p.validation[p.bindings++]=b;
        if (info->type & (render ? 0xC000 : 0x2000)) p.operations[p.count++]=b;
    }
    return &p;
}
void initializeRender(void* output) {
    auto* p=static_cast<char*>(output);
    // Exactly the stock initializer's writes. Other fields deliberately remain
    // untouched (including secondary color and flat-matrix payload).
    uint32_t color=0xFFFFFFFF; std::memcpy(p,&color,4);
    for (int offset : {8,20,24,44}) { uint32_t zero=0; std::memcpy(p+offset,&zero,4); }
    for (int offset : {12,16}) { float size=.1f; std::memcpy(p+offset,&size,4); }
    for (int offset : {28,36}) { float value=.5f; std::memcpy(p+offset,&value,4); }
    for (int offset : {32,40}) { float value=-.5f; std::memcpy(p+offset,&value,4); }
    for (int offset : {52,54,55,120,121,122}) p[offset]=0;
    p[123]=static_cast<char>(-1);
}
template<bool Rendering> void __fastcall process(Manager* manager, void*, float time, float mult,
    float delta, float length, bool constant, void* output) {
    auto original=hooks[Rendering ? Render : Movement].original<Value>();
    auto* p=own() && output && !dispatching ? getPlan(manager,Rendering) : nullptr;
    if (!p) { ++stats.fallbacks; original(manager,time,mult,delta,length,constant,output); return; }
    ++stats.planCalls;
    if constexpr (Rendering) initializeRender(output);
    else {
        auto* m=static_cast<MovementData*>(output);
        for (float& r:m->rotation) r=0;
        m->floating=m->underwater=false;
    }
    // Nested passes use the stock traversal, so they cannot overwrite the
    // active bounded slot. Avoid copying a kilobyte of bindings per particle.
    struct Guard {Guard(){dispatching=true;} ~Guard(){dispatching=false;}} guard;
    const auto count=p->count;
    for (unsigned i=0;i<count;++i) {
        const auto b=p->operations[i];
        b.function(b.object,time,mult,delta,length,constant,output);
    }
    proof(planProof,"EXECUTING: compiled FX operator plan dispatched an ordered movement/render pass.");
}
}
bool Install(bool caching, bool planning, engine::Logger logger) {
    log=logger;
    struct Entry { uintptr_t address; const char* bytes; size_t size; void* replacement; bool needed; };
    Entry entries[] = {
        {0x4A55E0,"\x8A\x41\x06\x83\xEC\x40\x84\xC0",8,reinterpret_cast<void*>(&cachedValue<Friction>),caching},
        {0x4A5AA0,"\x8A\x41\x06\x83\xEC\x40\x84\xC0",8,reinterpret_cast<void*>(&cachedValue<Wind>),caching},
        {0x4A4A10,"\x33\xC0\x53\x56\x8B\x74\x24\x20",8,reinterpret_cast<void*>(&process<false>),planning},
        {0x4A4A80,"\x53\x56\x8B\x74\x24\x20\xB0\xFF",8,reinterpret_cast<void*>(&process<true>),planning}
    };
    for (unsigned i=0;i<Count;++i) if (entries[i].needed) {
        const auto& e=entries[i];
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(e.address),&region,sizeof(region)) ||
           region.State!=MEM_COMMIT || (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
           e.address+e.size>reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize ||
           std::memcmp(reinterpret_cast<void*>(e.address),e.bytes,e.size)) { Uninstall(); return false; }
        auto h=safetyhook::InlineHook::create(reinterpret_cast<void*>(e.address),e.replacement,safetyhook::InlineHook::StartDisabled);
        if (!h) { Uninstall(); return false; } hooks[i]=std::move(*h);
    }
    for (auto& h:hooks) if (h && !h.enable()) { Uninstall(); return false; }
    for(unsigned i=0;i<Count;++i) if(hooks[i]) watches[i].capture(entries[i].address);
    return true;
}
void BeginFrame() { if(!hooks[Friction] && !hooks[Movement]) return; thread=GetCurrentThreadId(); ++epoch; }
void ClearProject() { for(auto& p:plans) p.manager=nullptr; ++epoch; }
void Uninstall() { for(auto& h:hooks) h.reset(); watches={}; ClearProject(); thread=0; stats={}; cacheProof=planProof=false; }
Stats TakeStats() { for(auto& watch:watches) watch.check(log); auto result=stats; stats={}; return result; }
}
