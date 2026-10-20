// Execute isolated routines from the user's local EXE in a test process.
// Never enter the game, resolve imports, or distribute its bytes.
#include <windows.h>
#include "fxp/GtaFxAbi.h"
#include "runtime/Engine.h"
#include "runtime/ParticleMath.h"
#include "renderer/RenderStream.h"
#include <array>
#include <bit>
#include <fstream>
#include <vector>
#include <cstdio>
#include <random>
#include <cmath>
#include <cstring>
#include <string>
#define CHECK(x) do { if (!(x)) { std::printf("Failed line %d: %s\n", __LINE__, #x); fxp::engine::Uninstall(); return 1; } } while(0)
struct Particle {
    void* prev; Particle* next; float life,age; fxp::Vec3 position,velocity; FxSystem_c* system;
    uint32_t color; uint8_t size,rotMult,randomR,randomG,randomB,brightness; int8_t rotZ; bool local;
    float rotation; void* prev2;void* next2;
};
static_assert(sizeof(Particle)==0x44);
struct Movement { fxp::Vec3 position,velocity; float rot[4];bool floating,underwater;char pad[2]; };
Movement injected{};bool hasWater{};float waterLevel{};
unsigned returned{};
void __fastcall ReturnStub(void*,void*,Particle*) { ++returned; }
void __fastcall MovementStub(void*,void*,float,float,float dt,float,bool,Movement* m) {
    m->velocity.x+=dt*2; m->velocity.y-=dt*3;m->position.x+=.1f;
    memcpy(m->rot,injected.rot,sizeof(m->rot));m->floating=injected.floating;m->underwater=injected.underwater;
}
bool __cdecl WaterStub(float,float,float,float* level,bool,void*) { *level=waterLevel;return hasWater; }
__declspec(naked) void TruncateStub() {
    __asm {
        sub esp,12
        fnstcw word ptr [esp+8]
        mov ax,word ptr [esp+8]
        or ax,0C00h
        mov word ptr [esp+10],ax
        fldcw word ptr [esp+10]
        fistp qword ptr [esp]
        fldcw word ptr [esp+8]
        mov eax,dword ptr [esp]
        mov edx,dword ptr [esp+4]
        add esp,12
        ret
    }
}
void Jump(uintptr_t at,void* target) {
    auto* p=reinterpret_cast<unsigned char*>(at);p[0]=0xE9;
    int32_t offset=int32_t(reinterpret_cast<uintptr_t>(target)-at-5);memcpy(p+1,&offset,4);
}
bool hookOverwriteDetected{};
void Log(const char* message) { if(std::strstr(message,"HOOK OVERWRITTEN"))hookOverwriteDetected=true;std::puts(message); }
struct CurveFixture {
    struct Interp { void* vtable;bool loop;int8_t keys,channels;uint8_t pad;uint16_t* times;void** values; } interp;
    std::array<uint16_t,4> times{64,96,128,256};
    std::array<std::array<float,4>,5> floats;
    std::array<std::array<int16_t,4>,5> signedKeys;
    std::array<std::array<uint16_t,4>,5> unsignedKeys;
    std::array<void*,5> pointers;
    int format;
    std::vector<std::array<float,5>> expected;
    CurveFixture(int type,bool loop):format(type) {
        interp={nullptr,loop,4,5,0,times.data(),pointers.data()};
        for(unsigned c=0;c<5;++c)for(unsigned k=0;k<4;++k) {
            floats[c][k]=float(int(c*7+k*3)-10)*.13f;
            signedKeys[c][k]=int16_t(int(c*200+k*900)-1200);
            unsignedKeys[c][k]=uint16_t(c*200+k*900);
        }
        for(unsigned c=0;c<5;++c)pointers[c]=type==0?static_cast<void*>(floats[c].data()):type==1?static_cast<void*>(unsignedKeys[c].data()):static_cast<void*>(signedKeys[c].data());
    }
};
#include "EnginePassReference.h"
int main(int argc,char** argv) {
    // Reserve routine addresses before the CRT allocates the large EXE file buffer.
    constexpr uintptr_t regions[]={0x4A0000,0x6E0000,0x700000,0x730000,0x820000,0x850000,0xA90000,0xC40000};
    if(argc==2) {
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        int length=MultiByteToWideChar(CP_ACP,0,argv[1],-1,nullptr,0);
        std::wstring reference(size_t(length),L'\0');MultiByteToWideChar(CP_ACP,0,argv[1],-1,reference.data(),length);reference.pop_back();
        std::wstring command=L"\""+std::wstring(executable)+L"\" \""+reference+L"\" --child";
        STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
        si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);si.hStdError=GetStdHandle(STD_ERROR_HANDLE);si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        CHECK(CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&si,&process));
        bool reserved=true;
        for(uintptr_t address:regions)reserved=VirtualAllocEx(process.hProcess,reinterpret_cast<void*>(address),0x10000,MEM_RESERVE,PAGE_EXECUTE_READWRITE)==reinterpret_cast<void*>(address)&&reserved;
        if(!reserved){TerminateProcess(process.hProcess,1);CloseHandle(process.hThread);CloseHandle(process.hProcess);CHECK(reserved);}
        ResumeThread(process.hThread);CloseHandle(process.hThread);
        DWORD waited=WaitForSingleObject(process.hProcess,30000);
        if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,1);CloseHandle(process.hProcess);CHECK(waited==WAIT_OBJECT_0);}
        DWORD code{};GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);return int(code);
    }
    for(uintptr_t address:regions) {
        void* allocation=VirtualAlloc(reinterpret_cast<void*>(address),0x10000,MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if(!allocation) {MEMORY_BASIC_INFORMATION info{};VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info));std::printf("Region %08lx occupied by %p (%lx), extent %zu\n",static_cast<unsigned long>(address),info.AllocationBase,info.State,info.RegionSize);}
        CHECK(allocation==reinterpret_cast<void*>(address));
    }
    CHECK(argc==3&&std::strcmp(argv[2],"--child")==0);std::ifstream input(argv[1],std::ios::binary|std::ios::ate);CHECK(input);
    const auto bytes=input.tellg();CHECK(bytes>4096&&bytes<64*1024*1024);
    std::vector<char> file(static_cast<size_t>(bytes));input.seekg(0);input.read(file.data(),bytes);CHECK(input);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());CHECK(dos->e_magic==IMAGE_DOS_SIGNATURE&&dos->e_lfanew>0&&size_t(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS32)<file.size());
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(file.data()+dos->e_lfanew);
    CHECK(nt->Signature==IMAGE_NT_SIGNATURE&&nt->FileHeader.Machine==IMAGE_FILE_MACHINE_I386&&nt->OptionalHeader.ImageBase==0x400000);
    CHECK(nt->OptionalHeader.SizeOfImage<64*1024*1024);
    auto* sections=IMAGE_FIRST_SECTION(nt);
    CHECK(reinterpret_cast<char*>(sections+nt->FileHeader.NumberOfSections)<=file.data()+file.size());
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& section=sections[i];
        CHECK(uint64_t(section.PointerToRawData)+section.SizeOfRawData<=file.size());
        CHECK(uint64_t(section.VirtualAddress)+section.SizeOfRawData<=nt->OptionalHeader.SizeOfImage);
        uintptr_t sectionStart=0x400000+section.VirtualAddress,sectionEnd=sectionStart+section.SizeOfRawData;
        for(uintptr_t region:regions) {
            uintptr_t first=std::max(region,sectionStart),last=std::min(region+0x10000,sectionEnd);
            if(first<last)memcpy(reinterpret_cast<void*>(first),file.data()+section.PointerToRawData+(first-sectionStart),last-first);
        }
    }
    CHECK(memcmp(reinterpret_cast<void*>(0x4A21D0),"\x83\xEC\x2C\x56\x8B\x74\x24\x38\x8B\x56\x28",11)==0);
    std::array<char,16> movementPrefix;memcpy(movementPrefix.data(),reinterpret_cast<void*>(0x4A4A10),16);
    Jump(0x4A4A10,reinterpret_cast<void*>(&MovementStub));Jump(0x6EB690,reinterpret_cast<void*>(&WaterStub));Jump(0x821B40,reinterpret_cast<void*>(&TruncateStub));
    Jump(0x4A93B0,reinterpret_cast<void*>(&ReturnStub));
    FlushInstructionCache(GetCurrentProcess(),nullptr,0);
    // Validate the replacement triangle ABI against the original 27-argument
    // cdecl writer, not just against a matching C++ declaration.
    struct RwVertex { float p[3],normal[3];uint32_t color;float uv[2]; };
    static_assert(sizeof(RwVertex)==36);
    *reinterpret_cast<uintptr_t*>(0xA9AE6C)=0xC4D958;
    *reinterpret_cast<uintptr_t*>(0xA9AE64)=1;
    *reinterpret_cast<unsigned*>(0xA9AE58)=0;
    *reinterpret_cast<unsigned*>(0xA9AE5C)=0;
    fxp::render::TriangleArgs triangle{{1,2,3,4,5,6,7,8,9},{.1f,.2f,.3f,.4f,.5f,.6f},{10,20,30,40,50,60,70,80,90,100,110,120}};
    reinterpret_cast<void(__cdecl*)(fxp::render::TriangleArgs)>(0x4A1410)(triangle);
    auto* written=reinterpret_cast<RwVertex*>(0xC4D958);
    for(unsigned c=0;c<3;++c) {
        CHECK(memcmp(written[c].p,triangle.p+c*3,12)==0&&memcmp(written[c].uv,triangle.uv+c*2,8)==0);
        auto* rgba=triangle.rgba+c*4;
        CHECK(written[c].color==((rgba[3]<<24)|(rgba[0]<<16)|(rgba[1]<<8)|rgba[2]));
    }
    CHECK(*reinterpret_cast<unsigned*>(0xA9AE58)==3&&*reinterpret_cast<unsigned*>(0xA9AE5C)==1);
    constexpr uintptr_t curveAddresses[]={0x4A8470,0x4A8800,0x4A89C0,0x4A8B80};
    using CurveFn=void(__thiscall*)(void*,float*,float);
    std::array<std::unique_ptr<CurveFixture>,8> fixtures;
    for(unsigned i=0;i<8;++i) {
        auto& fixture=fixtures[i];fixture=std::make_unique<CurveFixture>(i%4,i>=4);
        for(unsigned sample=0;sample<1200;++sample) {
            std::array<float,5> result;float t=float(int(sample)-100)*.003125f;
            reinterpret_cast<CurveFn>(curveAddresses[i%4])(&fixture->interp,result.data(),t);
            fixture->expected.push_back(result);
        }
    }
    alignas(16) std::array<char,sizeof(FxSystem_c)> systemBytes{};auto* system=reinterpret_cast<FxSystem_c*>(systemBytes.data());
    std::array<char,0x24> blueprint{};float length=2;memcpy(blueprint.data()+0xC,&length,4);system->m_pBlueprint=blueprint.data();
    system->field_54=std::bit_cast<int>(.7f);
    alignas(16) std::array<char,0x40> bp{};
    using Tick=bool(__thiscall*)(void*,float,Particle*);
    struct Case { Particle input,expected;Movement movement;unsigned timeMult;float dt,level;bool water,dead; };
    std::vector<Case> cases;std::mt19937 rng(911);std::uniform_real_distribution<float> f(-8,8);
    for(unsigned i=0;i<4096;++i) {
        Case c{};c.input.life=.1f+std::abs(f(rng));c.input.age=std::abs(f(rng));
        c.input.position={f(rng),f(rng),f(rng)};c.input.velocity={f(rng),f(rng),f(rng)};
        c.input.system=system;c.input.randomR=uint8_t(i%256);c.input.rotMult=uint8_t((i*13)%256);c.input.rotation=f(rng);
        for(float& r:c.movement.rot)r=f(rng);
        c.movement.floating=(i%4)==0;c.movement.underwater=(i%4)==1;c.water=i%3!=0;c.level=f(rng);
        c.timeMult=i%9==0?0:unsigned(i%5000);c.dt=std::abs(f(rng))*.02f;
        system->m_nTimeMult=uint16_t(c.timeMult);injected=c.movement;hasWater=c.water;waterLevel=c.level;
        c.expected=c.input;c.dead=reinterpret_cast<Tick>(0x4A21D0)(bp.data(),c.dt,&c.expected);cases.push_back(c);
    }
    std::array<Particle,8> batchInput{},batch{},batchExpected{};
    std::array<std::array<char,sizeof(FxSystem_c)>,8> batchSystems{};
    auto prepareBatch=[&]() {
        batch=batchInput;returned=0;injected={};hasWater=false;
        for(unsigned i=0;i<batch.size();++i) {
            memcpy(batchSystems[i].data(),systemBytes.data(),sizeof(FxSystem_c));
            auto* s=reinterpret_cast<FxSystem_c*>(batchSystems[i].data());s->m_nTimeMult=1000;
            s->m_nPlayStatus=(i==1||i==5)?2:0;s->m_nKillStatus=i==3?3:0;
            batch[i].system=s;batch[i].prev=i?&batch[i-1]:nullptr;batch[i].next=i+1<batch.size()?&batch[i+1]:nullptr;
        }
        *reinterpret_cast<Particle**>(bp.data()+0x20)=batch.data();
        *reinterpret_cast<Particle**>(bp.data()+0x24)=batch.data()+7;
        *reinterpret_cast<unsigned*>(bp.data()+0x28)=8;
    };
    for(unsigned i=0;i<batch.size();++i) {
        batchInput[i]=cases[i*5].input;batchInput[i].life=i%3==0?.005f:4;batchInput[i].age=0;
    }
    using EmitterTick=void(__thiscall*)(void*,float);
    prepareBatch();reinterpret_cast<EmitterTick>(0x4A2BC0)(bp.data(),.016f);batchExpected=batch;
    const unsigned expectedReturns=returned,expectedCount=*reinterpret_cast<unsigned*>(bp.data()+0x28);
    // Identical warmed fixture loops through real native routines and their
    // installed replacements. Informational timings, never flaky pass limits.
    LARGE_INTEGER benchmarkFrequency{}; QueryPerformanceFrequency(&benchmarkFrequency);
    auto curveBenchmark = [&]() {
        float output[5]{}; double checksum{};
        LARGE_INTEGER start{}, end{}; QueryPerformanceCounter(&start);
        for (unsigned i=0; i<300000; ++i) {
            const unsigned fixture = i & 7;
            const float t = float(int(i % 1200)-100)*.003125f;
            reinterpret_cast<CurveFn>(curveAddresses[fixture%4])(&fixtures[fixture]->interp,output,t);
            checksum += output[i%5];
        }
        QueryPerformanceCounter(&end);
        return std::pair{double(end.QuadPart-start.QuadPart)*1000.0/double(benchmarkFrequency.QuadPart),checksum};
    };
    auto emitterBenchmark = [&]() {
        LARGE_INTEGER start{}, end{}; QueryPerformanceCounter(&start);
        for(unsigned i=0;i<50000;++i) {
            prepareBatch(); reinterpret_cast<EmitterTick>(0x4A2BC0)(bp.data(),.016f);
        }
        QueryPerformanceCounter(&end);
        return double(end.QuadPart-start.QuadPart)*1000.0/double(benchmarkFrequency.QuadPart);
    };
    curveBenchmark(); emitterBenchmark(); // Warm native code/data before timing.
    const auto stockCurves = curveBenchmark();
    const auto stockEmitterMs = emitterBenchmark();
    CHECK(fxp::engine::Install(true,true,false,&Log));
    fxp::engine::SetGameThread(GetCurrentThreadId());
    struct ForeignCurve { void* interp; std::array<float,5> values; } foreign{&fixtures[0]->interp,{}};
    auto worker=[](void* data)->DWORD {
        auto* sample=static_cast<ForeignCurve*>(data);
        reinterpret_cast<CurveFn>(0x4A8470)(sample->interp,sample->values.data(),.5f);
        return 0;
    };
    HANDLE workerThread=CreateThread(nullptr,0,worker,&foreign,0,nullptr);CHECK(workerThread);
    CHECK(WaitForSingleObject(workerThread,10000)==WAIT_OBJECT_0);CloseHandle(workerThread);
    for(unsigned channel=0;channel<5;++channel)CHECK(std::abs(foreign.values[channel]-fixtures[0]->expected[260][channel])<.00003f);
    float largestCurveError{},largestParticleError{};
    for(unsigned i=0;i<8;++i)for(unsigned sample=0;sample<fixtures[i]->expected.size();++sample) {
        std::array<float,5> actual{};float t=float(int(sample)-100)*.003125f;
        reinterpret_cast<CurveFn>(curveAddresses[i%4])(&fixtures[i]->interp,actual.data(),t);
        for(unsigned channel=0;channel<5;++channel) {
            float error=std::abs(actual[channel]-fixtures[i]->expected[sample][channel]);
            largestCurveError=std::max(largestCurveError,error);CHECK(error<.00003f);
        }
    }
    for(const auto& c:cases) {
        system->m_nTimeMult=uint16_t(c.timeMult);injected=c.movement;hasWater=c.water;waterLevel=c.level;
        auto actual=c.input;bool dead=reinterpret_cast<Tick>(0x4A21D0)(bp.data(),c.dt,&actual);
        CHECK(dead==c.dead);
        auto compare=[&](float a,float b) {float error=std::abs(a-b);largestParticleError=std::max(largestParticleError,error);return error<.00003f;};
        CHECK(compare(actual.age,c.expected.age)&&compare(actual.position.x,c.expected.position.x)&&compare(actual.position.y,c.expected.position.y)&&compare(actual.position.z,c.expected.position.z));
        CHECK(compare(actual.velocity.x,c.expected.velocity.x)&&compare(actual.velocity.y,c.expected.velocity.y)&&compare(actual.velocity.z,c.expected.velocity.z)&&compare(actual.rotation,c.expected.rotation));
    }
    prepareBatch();reinterpret_cast<EmitterTick>(0x4A2BC0)(bp.data(),.016f);
    CHECK(returned==expectedReturns&&*reinterpret_cast<unsigned*>(bp.data()+0x28)==expectedCount);
    for(unsigned i=0;i<batch.size();++i) {
        CHECK(std::abs(batch[i].age-batchExpected[i].age)<.00003f);
        CHECK(std::abs(batch[i].position.x-batchExpected[i].position.x)<.00003f);
        CHECK(std::abs(batch[i].position.y-batchExpected[i].position.y)<.00003f);
        CHECK(std::abs(batch[i].position.z-batchExpected[i].position.z)<.00003f);
        CHECK(batch[i].prev==batchExpected[i].prev&&batch[i].next==batchExpected[i].next);
    }
    CHECK(reinterpret_cast<FxSystem_c*>(batchSystems[3].data())->m_nKillStatus==2);
    auto stats=fxp::engine::TakeCounters();CHECK(stats.curves==9600&&stats.particles==4102&&stats.curveFallbacks==0&&stats.packets==2);
    fixtures[0]->interp.channels = 4;
    std::array<float,5> changed{0,0,0,0,123};
    reinterpret_cast<CurveFn>(curveAddresses[0])(&fixtures[0]->interp,changed.data(),.5f);
    CHECK(changed[4] == 123);
    fixtures[0]->interp.channels = 5;
    fxp::engine::ClearProject(); // Front-cache pointers must be invalidated too.
    reinterpret_cast<CurveFn>(curveAddresses[0])(&fixtures[0]->interp,changed.data(),.5f);
    for(unsigned channel=0;channel<5;++channel)
        CHECK(std::abs(changed[channel]-fixtures[0]->expected[260][channel])<.00003f);
    CHECK(!hookOverwriteDetected);
    auto* monitored=reinterpret_cast<unsigned char*>(0x4A8470);
    const auto savedByte=*monitored;*monitored=0x90;
    fxp::engine::TakeCounters();*monitored=savedByte;
    CHECK(hookOverwriteDetected);
    curveBenchmark(); emitterBenchmark(); // Exclude compilation/first-use work.
    const auto optimizedCurves = curveBenchmark();
    const auto optimizedEmitterMs = emitterBenchmark();
    CHECK(std::abs(optimizedCurves.second-stockCurves.second)<1.0);
    std::printf("Informational native microbenchmark (not gameplay FPS): 300,000 curve calls stock=%.3fms replacement=%.3fms; 50,000 eight-particle emitter calls stock=%.3fms replacement=%.3fms. Includes common fixture/reset overhead.\n",
        stockCurves.first,optimizedCurves.first,stockEmitterMs,optimizedEmitterMs);
    fxp::engine::Uninstall();
    std::printf("Native EXE differential: 48,000 channel values, 4,096 particle cases; largest curve error %.9g, particle error %.9g.\n",largestCurveError,largestParticleError);
    CHECK(TestNewEnginePasses(movementPrefix)==0);
    for(uintptr_t region:regions)VirtualFree(reinterpret_cast<void*>(region),0,MEM_RELEASE);
}
