#include "mws/ProperShadersCompatibility.hpp"
#include "mws/EnginePass.hpp"
#include "mws/EnginePassAlgorithms.hpp"
#include "mws/GtaSa10US.hpp"
#include "mws/ExecutableInfo.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/Logger.hpp"
#include "mws/PassProfiler.hpp"
#include "fxp/runtime/ProcessLifetime.h"
#include <safetyhook.hpp>
#include <atomic>

namespace mws::EnginePass {
namespace {
using namespace enginepass;
auto& alphaHook=fxp::processLifetime<SafetyHookInline>();
auto& shadowHook=fxp::processLifetime<SafetyHookInline>();
auto& shadowMid=fxp::processLifetime<SafetyHookMid>();
struct IndexSlot { AlphaList* list{}; AlphaIndex index; };
std::array<IndexSlot,8> indexes;
std::atomic<DWORD> alphaThread{};
uint64_t alphaCalls{},alphaIndexed{},shadowSkipped{},shadowCalls{};
unsigned interval{},presents{};
bool alphaProof{},shadowProof{},shadowGrouping{};
struct ShadowPlan { ShadowGroups groups; unsigned count{}; };
thread_local ShadowPlan* currentShadow{};
using Insert=AlphaNode*(__thiscall*)(AlphaList*,const AlphaData&);
AlphaNode* __fastcall insertHook(AlphaList* list,void*,const AlphaData& data) {
    if (ProperShadersCompatibility::active())
        return alphaHook.original<Insert>()(list, data);
    const DWORD thread = GetCurrentThreadId();
    DWORD owner = alphaThread.load(std::memory_order_relaxed);
    if (!owner) {
        if (alphaThread.compare_exchange_strong(owner, thread, std::memory_order_relaxed)) owner = thread;
    }
    if(owner != thread) return alphaHook.original<Insert>()(list,data);
    ++alphaCalls;
    auto* slot=static_cast<IndexSlot*>(nullptr);
    for(auto& s:indexes) if(s.list==list) {slot=&s;break;}
    if(!slot) for(auto& s:indexes) if(!s.list) {s.list=list;slot=&s;break;}
    AlphaNode* result{};
    if(slot && slot->index.insert(*list,data,result)) {
        ++alphaIndexed;
        if(result && !alphaProof) {alphaProof=true; Diagnostics::instance().effective("Optimization.AlphaListIndex","live alpha insertion used a contiguous search index; linked list remains sorted");}
        return result;
    }
    return alphaHook.original<Insert>()(list,data);
}
void buildShadow(ShadowPlan& p) {
    const auto count=*reinterpret_cast<uint16_t*>(0xC403DC);
    if(count>48) return;
    std::array<ShadowKey,48> keys;
    for(unsigned i=0;i<count;++i) {
        auto* shadow=reinterpret_cast<const char*>(0xC40430+i*0x34);
        std::memcpy(&keys[i].texture,shadow+0x24,4);
        keys[i].type=static_cast<uint8_t>(shadow[0x2E]);
    }
    if(p.groups.build(keys.data(),count)) p.count=count;
}
__declspec(noinline) void renderGroupedShadows() {
    ShadowPlan plan;
    buildShadow(plan);
    auto* previous=currentShadow;
    currentShadow = plan.count ? &plan : nullptr;
    shadowHook.unsafe_ccall<void>();
    currentShadow=previous;
}
void __cdecl renderShadows() {
    mws::PassProfiler::Scope timing(mws::PassProfiler::Pass::Shadows);
    ++shadowCalls;
    // Profiling alone does not need the 128-bucket shadow grouping buffer.
    if (shadowGrouping && !ProperShadersCompatibility::active()) renderGroupedShadows();
    else shadowHook.unsafe_ccall<void>();
}
void shadowLookup(safetyhook::Context& context) {
    auto* p=currentShadow;
    if(!p) return;
    auto* index=reinterpret_cast<unsigned*>(context.esp+0x44);
    const auto i=*index;
    const auto outer=(context.esi-0xC40454)/0x34;
    if(context.esi<0xC40454 || outer>=p->count || i<=outer || i>p->count ||
        context.ebx!=0xC40454+i*0x34) return;
    // The previous accepted element belongs to this group. Jump directly to
    // its next member; all stock casting, water, state and flush code stays.
    const auto next=p->groups.next(i-1);
    if(next<i) return;
    shadowSkipped+=next-i;
    if(next>i && !shadowProof) {shadowProof=true;Diagnostics::instance().effective("Optimization.ShadowGroupIndex","indexed shadow lookup skipped unrelated entries; stock rendering retained");}
    if(next>=p->count) {
        // Match the completed stock loop's registers and locals before its
        // flush/water path, including values not consumed by this hook.
        context.eax=context.ecx=p->count;
        context.ebx=0xC40454+p->count*0x34;
        *index=p->count;
        *reinterpret_cast<uintptr_t*>(context.esp+0x38)=context.ebx;
        context.eip=0x70AE14;return;
    }
    context.ebx=0xC40454+next*0x34; *index=next;
    *reinterpret_cast<uintptr_t*>(context.esp+0x38)=context.ebx;
}
bool signature(uintptr_t address,const char* bytes,size_t size) {
    return gta::addressIsReadable(address,size) && gta::addressIsExecutable(address) && !std::memcmp(reinterpret_cast<void*>(address),bytes,size);
}
bool attach(bool alpha,bool shadow,bool profileShadow=false) {
    bool any{};
    if(alpha && signature(0x733910,"\x8B\x51\x10\x56\x8B\x74\x24\x08\x57\x8D\x79\x14",12)) {
        auto h=safetyhook::InlineHook::create(reinterpret_cast<void*>(0x733910),reinterpret_cast<void*>(&insertHook));
        if(h) {alphaHook=std::move(*h);any=true; Diagnostics::instance().active("Optimization.AlphaListIndex","shared alpha insertion hook installed");}
    }
    const bool shadowEntryValid = signature(0x70A960,"\x55\x8B\xEC\x83\xE4\xF8\x81\xEC\xF0\x00\x00\x00",12);
    const bool indexValid = shadow && shadowEntryValid &&
       signature(0x70AA60,"\x8A\x4E\x0A\x3A\x4B\x0A",6) &&
       signature(0x70ADF5,"\x8B\x44\x24\x44\x0F\xB7\x0D\xDC\x03\xC4\x00",11) &&
       signature(0x70AE14,"\x33\xDB\xA1\x50\xB9\xC4\x00",7) &&
       gta::addressIsReadable(0xC403DC,2) &&
       gta::addressIsReadable(0xC40430,48*0x34);
    if (indexValid) {
        auto mid=safetyhook::MidHook::create(reinterpret_cast<void*>(0x70AA60),&shadowLookup);
        if (mid) { shadowMid=std::move(*mid); shadowGrouping=true; }
    }
    if (shadowEntryValid && (profileShadow || shadowGrouping)) {
        auto h=safetyhook::InlineHook::create(reinterpret_cast<void*>(0x70A960),reinterpret_cast<void*>(&renderShadows));
        if (h) {
            shadowHook=std::move(*h); any=true;
            if (shadowGrouping)
                Diagnostics::instance().active("Optimization.ShadowGroupIndex","stock shadow casting retained; indexed lookup hook installed");
        } else {
            shadowMid.reset(); shadowGrouping=false;
        }
    }
    if (!shadowHook) { shadowMid.reset(); shadowGrouping=false; }
    return any;
}
}
void install(const Config& c) {
    interval=c.optimizationLogInterval;
    if(!c.enabled || !c.optimizationsEnabled) return;
    if (ProperShadersCompatibility::loaded()) {
        ProperShadersCompatibility::report(c);
        return;
    }
    const auto exe=ExecutableInfo::current();
    if(!exe.validPe || reinterpret_cast<uintptr_t>(exe.base)!=0x400000 || exe.imageSize<0x900000) return;
    attach(c.alphaListIndex,c.shadowGroupIndex,c.passTiming);
    if(c.alphaListIndex && !alphaHook) Diagnostics::instance().skipped("Optimization.AlphaListIndex","entry differs from verified stock routine");
    if(c.shadowGroupIndex && !shadowGrouping) Diagnostics::instance().skipped("Optimization.ShadowGroupIndex","stock shadow loop/layout signatures unavailable");
}
void uninstall() {
    captureDiagnostics();
    shadowHook.reset();shadowMid.reset();alphaHook.reset();
    shadowGrouping=false;
    indexes={};alphaThread=0;alphaCalls=alphaIndexed=shadowSkipped=shadowCalls=0;presents=0;alphaProof=shadowProof=false;
}
void captureDiagnostics() {
    if(!alphaHook && !shadowHook) return;
    Diagnostics::instance().activity("Optimization.AlphaListIndex",alphaCalls,alphaIndexed,0,alphaCalls-alphaIndexed);
    Diagnostics::instance().activity("Optimization.ShadowGroupIndex",shadowCalls,shadowCalls,shadowSkipped);
}
void onPresent() {
    if(!alphaHook && !shadowHook) return;
    if(!interval || ++presents<interval) return; presents=0;
    captureDiagnostics();
    Logger::instance().info("[ENGINE-PASS] alphaCalls="+std::to_string(alphaCalls)+" indexed="+std::to_string(alphaIndexed)+" shadowEntriesSkipped="+std::to_string(shadowSkipped));
}
#ifdef SANFRAME_ENGINE_PASS_TEST
bool installReference(bool alpha,bool shadow) {return attach(alpha,shadow);}
thread_local ShadowPlan referencePlan;
void beginShadowReference() {referencePlan={};buildShadow(referencePlan);currentShadow=referencePlan.count?&referencePlan:nullptr;}
void endShadowReference() {currentShadow=nullptr;}
#endif
}
