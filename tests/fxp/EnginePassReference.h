// Included by the isolated executable harness after CurveFixture and Jump.
#include "runtime/OperatorPlans.h"
#include "mws/EnginePass.hpp"
#include "mws/EnginePassAlgorithms.hpp"
double __cdecl ReferencePow(double base,double exponent) {return std::pow(base,exponent);}
__declspec(naked) void PowStub() {
    __asm {
        sub esp,16
        fstp qword ptr [esp+8]
        fstp qword ptr [esp]
        call ReferencePow
        add esp,16
        ret
    }
}
unsigned shadowRecorded{},shadowPointer{};
unsigned shadowEndEax{},shadowEndEcx{},shadowEndEbx{},shadowEndIndex{};
std::array<unsigned,48> shadowTrace;
void __cdecl RecordShadow() {shadowTrace[shadowRecorded++]=(shadowPointer-0xC40454)/0x34;}
__declspec(naked) void ShadowBodyStub() {
    __asm {
        mov shadowPointer,ebx
        pushad
        call RecordShadow
        popad
        mov eax,070ADF5h
        jmp eax
    }
}
__declspec(naked) void ShadowExitStub() {
    __asm {
        mov shadowEndEax,eax
        mov shadowEndEcx,ecx
        mov shadowEndEbx,ebx
        mov eax,dword ptr [esp+044h]
        mov shadowEndIndex,eax
        add esp,048h
        pop edi
        pop esi
        pop ebx
        ret
    }
}
__declspec(naked) void RunShadowLookup(unsigned) {
    __asm {
        push ebx
        push esi
        push edi
        sub esp,048h
        mov eax,dword ptr [esp+058h]
        mov dword ptr [esp+044h],eax
        imul eax,034h
        add eax,0C40454h
        mov esi,eax
        mov ebx,eax
        mov dword ptr [esp+038h],eax
        mov eax,070AA60h
        jmp eax
    }
}
int TestNewEnginePasses(const std::array<char,16>& movementPrefix) {
    memcpy(reinterpret_cast<void*>(0x4A4A10),movementPrefix.data(),movementPrefix.size());
    Jump(0x822130,reinterpret_cast<void*>(&PowStub));
    struct Info {void** table;uint16_t type;bool particleTime;char pad;CurveFixture::Interp interpolation;};
    struct Manager {int count;void** infos;int8_t movement,render;char pad[2];};
    using Value=void(__thiscall*)(void*,float,float,float,float,bool,void*);
    CurveFixture frictionCurve(2,false),windCurve(2,false),sizeCurve(1,false);
    frictionCurve.interp.channels=windCurve.interp.channels=1;sizeCurve.interp.channels=4;
    for(unsigned i=0;i<4;++i) {frictionCurve.signedKeys[0][i]=int16_t(900+i*40);windCurve.signedKeys[0][i]=int16_t(100+i*30);}
    void* frictionTable[]={nullptr,nullptr,reinterpret_cast<void*>(0x4A55E0)};
    void* windTable[]={nullptr,nullptr,reinterpret_cast<void*>(0x4A5AA0)};
    void* sizeTable[]={nullptr,nullptr,reinterpret_cast<void*>(0x4A6030)};
    Info friction{frictionTable,0x2000,false,0,frictionCurve.interp};
    Info wind{windTable,0x2000,false,0,windCurve.interp};
    Info ignored{frictionTable,0x1000,false,0,frictionCurve.interp};
    Info size{sizeTable,0x4000,false,0,sizeCurve.interp};
    void* infos[]={&ignored,&friction,&wind,&friction,&ignored,&size};
    Manager manager{6,infos,0,4,{}};
    fxp::Vec3 direction{.25f,-.5f,.7f};float speed=.6f;
    *reinterpret_cast<fxp::Vec3**>(0xA9AEAC)=&direction;
    *reinterpret_cast<float**>(0xA9AEB0)=&speed;
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    auto timed=[&](auto work) {LARGE_INTEGER a{},b{};QueryPerformanceCounter(&a);work();QueryPerformanceCounter(&b);return double(b.QuadPart-a.QuadPart)*1000/double(frequency.QuadPart);};
    auto operatorWork=[&] {for(unsigned i=0;i<100000;++i) {Movement m{};m.velocity={3,-5,7};reinterpret_cast<Value>(0x4A55E0)(&friction,.4f,.3f,.016f,2,false,&m);}};
    operatorWork();const double stockOperator=timed(operatorWork);
    struct Sample {Movement input,expected,windExpected;float time,delta;bool particleTime;};
    std::array<Sample,256> samples;
    std::mt19937 rng(4134);std::uniform_real_distribution<float> number(-20,20);
    for(unsigned i=0;i<samples.size();++i) {
        auto& s=samples[i];memset(&s.input,0xCC,sizeof(Movement));
        s.input.position={number(rng),number(rng),number(rng)};
        s.input.velocity={number(rng),number(rng),number(rng)};
        s.time=.25f+float(i/16)*.05f;s.delta=.016f;
        s.particleTime=(i&1)!=0;friction.particleTime=wind.particleTime=s.particleTime;
        s.expected=s.input;s.windExpected=s.input;
        reinterpret_cast<Value>(0x4A55E0)(&friction,s.time,.3f,s.delta,2,false,&s.expected);
        reinterpret_cast<Value>(0x4A5AA0)(&wind,s.time,.3f,s.delta,2,false,&s.windExpected);
    }
    CHECK(fxp::operators::Install(true,false,&Log));fxp::operators::BeginFrame();
    float largest{};
    for(const auto& s:samples) for(unsigned kind=0;kind<2;++kind) {
        friction.particleTime=wind.particleTime=s.particleTime;
        auto actual=s.input;const auto& expected=kind?s.windExpected:s.expected;
        reinterpret_cast<Value>(kind?0x4A5AA0:0x4A55E0)(kind?&wind:&friction,s.time,.3f,s.delta,2,false,&actual);
        const float* a=&actual.velocity.x;const float* b=&expected.velocity.x;
        for(unsigned c=0;c<3;++c) {const float error=std::abs(a[c]-b[c]);largest=std::max(largest,error);CHECK(error<.00003f);}
        CHECK(!memcmp(&actual.position,&s.input.position,sizeof(fxp::Vec3)));
        CHECK(!memcmp(actual.rot,s.input.rot,20));
    }
    auto cacheStats=fxp::operators::TakeStats();CHECK(cacheStats.cacheHits>200);
    friction.particleTime=wind.particleTime=false;
    struct WorkerData {void* info;Movement result;} workerData{&friction,samples[0].input};
    const auto worker=[](void* p)->DWORD {
        auto& data=*static_cast<WorkerData*>(p);
        reinterpret_cast<Value>(0x4A55E0)(data.info,.25f,.3f,.016f,2,false,&data.result);return 0;
    };
    auto handle=CreateThread(nullptr,0,worker,&workerData,0,nullptr);CHECK(handle);
    CHECK(WaitForSingleObject(handle,10000)==WAIT_OBJECT_0);CloseHandle(handle);
    CHECK(!memcmp(&workerData.result,&samples[0].expected,sizeof(Movement)));
    operatorWork();const double cachedOperator=timed(operatorWork);
    fxp::operators::TakeStats();
    // Changing wind inputs and project lifetime must invalidate cache hits.
    auto changed=samples[0].input;
    reinterpret_cast<Value>(0x4A5AA0)(&wind,.25f,.3f,.016f,2,false,&changed);
    fxp::operators::TakeStats();direction.x=.9f;speed=.8f;
    reinterpret_cast<Value>(0x4A5AA0)(&wind,.25f,.3f,.016f,2,false,&changed);
    CHECK(fxp::operators::TakeStats().cacheHits==0);
    fxp::operators::ClearProject();
    reinterpret_cast<Value>(0x4A55E0)(&friction,.25f,.3f,.016f,2,false,&changed);
    CHECK(fxp::operators::TakeStats().cacheHits==0);
    fxp::operators::Uninstall();
    std::array<Movement,256> movementExpected;
    std::array<std::array<char,0xA4>,256> renderExpected;
    auto planWork=[&] {for(unsigned i=0;i<100000;++i) {Movement m{};m.velocity={3,-5,7};reinterpret_cast<Value>(0x4A4A10)(&manager,.4f,.3f,.016f,2,false,&m);}};
    planWork();const double stockPlan=timed(planWork);
    for(unsigned i=0;i<samples.size();++i) {
        movementExpected[i]=samples[i].input;renderExpected[i].fill(char(0xCC));
        reinterpret_cast<Value>(0x4A4A10)(&manager,samples[i].time,.3f,.016f,2,false,&movementExpected[i]);
        reinterpret_cast<Value>(0x4A4A80)(&manager,samples[i].time,.3f,.016f,2,false,renderExpected[i].data());
    }
    CHECK(fxp::operators::Install(false,true,&Log));fxp::operators::BeginFrame();
    for(unsigned i=0;i<samples.size();++i) {
        auto movement=samples[i].input;std::array<char,0xA4> render;render.fill(char(0xCC));
        reinterpret_cast<Value>(0x4A4A10)(&manager,samples[i].time,.3f,.016f,2,false,&movement);
        reinterpret_cast<Value>(0x4A4A80)(&manager,samples[i].time,.3f,.016f,2,false,render.data());
        CHECK(!memcmp(&movement,&movementExpected[i],sizeof(Movement)));
        CHECK(render==renderExpected[i]);
    }
    const auto planStats=fxp::operators::TakeStats();CHECK(planStats.planCalls==512 && planStats.planHits>500);
    planWork();const double compiledPlan=timed(planWork);
    // Type mutation must invalidate an existing plan without dropping operations.
    ignored.type=0x2000;auto actual=samples[0].input;
    reinterpret_cast<Value>(0x4A4A10)(&manager,.25f,.3f,.016f,2,false,&actual);
    fxp::operators::Uninstall();auto expected=samples[0].input;
    reinterpret_cast<Value>(0x4A4A10)(&manager,.25f,.3f,.016f,2,false,&expected);
    CHECK(!memcmp(&actual,&expected,sizeof(actual)));
    // An extension callback must use the original, live manager traversal.
    void* customTable[]={nullptr,nullptr,reinterpret_cast<void*>(&MovementStub)};
    ignored.table=customTable;expected=samples[0].input;
    reinterpret_cast<Value>(0x4A4A10)(&manager,.25f,.3f,.016f,2,false,&expected);
    CHECK(fxp::operators::Install(false,true,&Log));fxp::operators::BeginFrame();actual=samples[0].input;
    reinterpret_cast<Value>(0x4A4A10)(&manager,.25f,.3f,.016f,2,false,&actual);
    CHECK(!memcmp(&actual,&expected,sizeof(actual)));
    CHECK(fxp::operators::TakeStats().fallbacks==1);fxp::operators::Uninstall();
    using namespace mws::enginepass;
    AlphaList list;std::array<AlphaNode,200> pool;
    auto reset=[&] {list={};list.head.next=&list.tail;list.tail.prev=&list.head;
        list.freeHead.next=pool.data();list.freeTail.prev=&pool.back();
        for(unsigned i=0;i<pool.size();++i) {pool[i]={};pool[i].prev=i?&pool[i-1]:&list.freeHead;pool[i].next=i+1<pool.size()?&pool[i+1]:&list.freeTail;}};
    using Insert=AlphaNode*(__thiscall*)(AlphaList*,const AlphaData&);
    std::array<float,200> distances;for(auto& d:distances)d=float(rng()%21);
    std::vector<std::vector<uintptr_t>> alphaExpected;
    auto alphaRun=[&](bool compare) {
        reset();
        for(unsigned i=0;i<distances.size();++i) {
            AlphaData data{reinterpret_cast<void*>(i+1),reinterpret_cast<void*>(2),distances[i]};
            auto* node=reinterpret_cast<Insert>(0x733910)(&list,data);
            if(node!=&pool[i]) return false;
            std::vector<uintptr_t> order;
            for(auto* n=list.head.next;n!=&list.tail;n=n->next)order.push_back(reinterpret_cast<uintptr_t>(n->data.object));
            if(compare) {if(order!=alphaExpected[i]) return false;}else alphaExpected.push_back(order);
        }
        AlphaData data{};return reinterpret_cast<Insert>(0x733910)(&list,data)==nullptr;
    };
    CHECK(alphaRun(false));
    auto alphaWork=[&] {for(unsigned trial=0;trial<1000;++trial) {reset();for(unsigned i=0;i<200;++i) {AlphaData data{reinterpret_cast<void*>(i+1),nullptr,distances[i]};reinterpret_cast<Insert>(0x733910)(&list,data);}}};
    alphaWork();const double stockAlpha=timed(alphaWork);
    // Compare the unmodified stock shadow comparison/advance loop. Only the
    // drawing body is substituted with a trace recorder; no D3D/game startup.
    std::array<char,7> shadowExit;memcpy(shadowExit.data(),reinterpret_cast<void*>(0x70AE14),7);
    Jump(0x70AA76,reinterpret_cast<void*>(&ShadowBodyStub));
    Jump(0x70AE14,reinterpret_cast<void*>(&ShadowExitStub));
    *reinterpret_cast<uint16_t*>(0xC403DC)=48;
    for(unsigned i=0;i<48;++i) {
        auto* p=reinterpret_cast<char*>(0xC40430+i*0x34);
        const uintptr_t texture=(rng()%9)*16;memcpy(p+0x24,&texture,4);p[0x2E]=char(rng()%3);
    }
    std::array<std::vector<unsigned>,48> shadowExpected;
    for(unsigned i=0;i<48;++i) {shadowRecorded=0;RunShadowLookup(i);shadowExpected[i].assign(shadowTrace.begin(),shadowTrace.begin()+shadowRecorded);}
    auto shadowWork=[&] {for(unsigned trial=0;trial<1000;++trial) for(unsigned i=0;i<48;++i) {shadowRecorded=0;RunShadowLookup(i);}};
    shadowWork();const double stockShadow=timed(shadowWork);
    memcpy(reinterpret_cast<void*>(0x70AE14),shadowExit.data(),7);
    CHECK(mws::EnginePass::installReference(true,true));CHECK(alphaRun(true));
    alphaWork();const double indexedAlpha=timed(alphaWork);
    Jump(0x70AE14,reinterpret_cast<void*>(&ShadowExitStub));
    mws::EnginePass::beginShadowReference();
    for(unsigned i=0;i<48;++i) {
        shadowRecorded=0;RunShadowLookup(i);
        CHECK(std::vector<unsigned>(shadowTrace.begin(),shadowTrace.begin()+shadowRecorded)==shadowExpected[i]);
        CHECK(shadowEndEax==48 && shadowEndEcx==48 && shadowEndIndex==48 && shadowEndEbx==0xC40454+48*0x34);
    }
    shadowWork();const double indexedShadow=timed(shadowWork);
    mws::EnginePass::endShadowReference();
    for(unsigned scene=0;scene<100;++scene) {
        const unsigned count=1+rng()%48;*reinterpret_cast<uint16_t*>(0xC403DC)=uint16_t(count);
        for(unsigned i=0;i<count;++i) {
            auto* p=reinterpret_cast<char*>(0xC40430+i*0x34);
            const uintptr_t texture=(rng()%17)*16;memcpy(p+0x24,&texture,4);p[0x2E]=char(rng()%5);
        }
        for(unsigned i=0;i<count;++i) {shadowRecorded=0;RunShadowLookup(i);shadowExpected[i].assign(shadowTrace.begin(),shadowTrace.begin()+shadowRecorded);}
        mws::EnginePass::beginShadowReference();
        for(unsigned i=0;i<count;++i) {
            shadowRecorded=0;RunShadowLookup(i);
            CHECK(std::vector<unsigned>(shadowTrace.begin(),shadowTrace.begin()+shadowRecorded)==shadowExpected[i]);
            CHECK(shadowEndEax==count && shadowEndEcx==count && shadowEndIndex==count && shadowEndEbx==0xC40454+count*0x34);
        }
        mws::EnginePass::endShadowReference();
    }
    mws::EnginePass::uninstall();
    std::printf("Native engine-pass differential: 512 cached operator cases (max error %.9g), 512 exact movement/render plans, 200 alpha insertions, 48 shadow group traces.\n",largest);
    std::printf("Informational new-pass microbenchmarks stock/replacement ms: friction 100k %.3f/%.3f; movement plan 100k %.3f/%.3f; alpha 1000x200 %.3f/%.3f; shadow lookup 1000x48 %.3f/%.3f. Friction uses the same CRT pow stub on both sides; shadow drawing is a trace stub. Not gameplay FPS.\n",stockOperator,cachedOperator,stockPlan,compiledPlan,stockAlpha,indexedAlpha,stockShadow,indexedShadow);
    return 0;
}
