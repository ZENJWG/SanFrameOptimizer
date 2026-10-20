#include <windows.h>
#include <d3d9.h>
#include "renderer/RenderStream.h"
#include "renderer/GpuTimer.h"
#include <cstdio>
#include <vector>
#include <cstring>
#include <algorithm>
#ifdef FXP_INTEGRATION_TEST
#include "mws/D3D9StateCache.hpp"
#include "mws/RenderSubmission.hpp"
#endif
#define CHECK(x) do { if (!(x)) { std::printf("Failed line %d: %s\n", __LINE__, #x); fxp::render::TestRelease(); return 1; } } while(0)
LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcW(w,m,a,b); }
struct Vertex { float x,y,z; DWORD color; float u,v; };
int main(int argc, char** argv) {
    WNDCLASSW wc{}; wc.lpfnWndProc=&Proc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"FXPHiddenGraphicsTest";
    RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,wc.hInstance,nullptr);
    CHECK(window);
    HMODULE wrapper{};
    IDirect3D9* api{};
    if(argc==2) {
        wchar_t library[32768]{};CHECK(MultiByteToWideChar(CP_ACP,0,argv[1],-1,library,32768));
        wrapper=LoadLibraryExW(library,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);CHECK(wrapper);
        using Create=IDirect3D9*(__stdcall*)(UINT);
        auto create=reinterpret_cast<Create>(GetProcAddress(wrapper,"Direct3DCreate9"));CHECK(create);
        api=create(D3D_SDK_VERSION);std::printf("Testing explicit D3D9 wrapper: %s\n",argv[1]);
    } else api=Direct3DCreate9(D3D_SDK_VERSION);
    CHECK(api);
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth=128; pp.BackBufferHeight=128; pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.hDeviceWindow=window;
    IDirect3DDevice9* device{};
    HRESULT hr=api->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device);
    if (FAILED(hr)) { api->Release(); DestroyWindow(window); std::puts("SKIP: hardware D3D9 device unavailable."); return 77; }
    D3DCAPS9 caps{}; CHECK(SUCCEEDED(device->GetDeviceCaps(&caps)));
    if(caps.VertexShaderVersion<D3DVS_VERSION(3,0)||caps.PixelShaderVersion<D3DPS_VERSION(3,0)) { device->Release();api->Release();DestroyWindow(window);std::puts("SKIP: shader model 3 unavailable.");return 77; }
    IDirect3DTexture9* texture{};
    CHECK(SUCCEEDED(device->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr)));
    D3DLOCKED_RECT tex{}; CHECK(SUCCEEDED(texture->LockRect(0,&tex,nullptr,0)));
    const DWORD checker[]={0xA0FFFFFF,0xE0FFA040,0xC04080FF,0x80FFFFFF};
    memcpy(tex.pBits,checker,8); memcpy(static_cast<char*>(tex.pBits)+tex.Pitch,checker+2,8); texture->UnlockRect(0);
    fxp::render::TriangleArgs triangles[]={
        {{-.8f,-.8f,.3f, -.8f,.8f,.3f, .8f,.8f,.3f},{0,1,0,0,1,0},{255,128,80,190, 80,255,120,240, 255,255,255,220}},
        {{-.8f,-.8f,.3f, .8f,.8f,.3f, .8f,-.8f,.3f},{0,1,1,0,1,1},{255,128,80,190, 255,255,255,220, 120,80,255,200}}
    };
    const std::array originalTriangles{triangles[0],triangles[1]};
    D3DMATRIX identity{}; identity._11=identity._22=identity._33=identity._44=1;
    D3DMATRIX world=identity; world._41=.1f;
    D3DMATRIX projection=identity;
    DWORD tableMode=D3DFOG_NONE, vertexMode=D3DFOG_LINEAR;bool rangeFog{};
    float fogStart{},fogEnd{1};
    struct FogCase { bool enabled,table,range,perspective;D3DFOGMODE mode; };
    std::vector<FogCase> cases{{false,false,false,false,D3DFOG_NONE},{true,false,false,false,D3DFOG_LINEAR},{true,false,false,false,D3DFOG_LINEAR}};
    for(bool perspective:{false,true})for(auto mode:{D3DFOG_LINEAR,D3DFOG_EXP,D3DFOG_EXP2}) {
        cases.push_back({true,false,false,perspective,mode});
        if(caps.RasterCaps&D3DPRASTERCAPS_FOGRANGE)cases.push_back({true,false,true,perspective,mode});
        if(caps.RasterCaps&D3DPRASTERCAPS_FOGTABLE)cases.push_back({true,true,false,perspective,mode});
    }
#ifdef FXP_INTEGRATION_TEST
    mws::Config integrated;
    integrated.d3dStateAdaptive = false;
    integrated.d3dOptimizeShaders = true;
    integrated.d3dOptimizeStreamSource = true;
    integrated.d3dOptimizeIndices = true;
    integrated.optimizationLogInterval = 0;
    CHECK(mws::D3D9StateCache::instance().install(device, integrated));
    CHECK(mws::RenderSubmission::install(device, integrated));
#endif
    fxp::gpu::Timer timer;
    const bool timerSupported = timer.initialize(device);
    if (!timerSupported) std::puts("GPU timestamp queries unavailable on test device; rendering checks still run.");
    uint64_t measuredSamples{};
    auto floatBits=[](float value) { DWORD result; memcpy(&result,&value,4);return result; };
    auto setup=[&](bool fog) {
        device->SetTransform(D3DTS_WORLD,&world); device->SetTransform(D3DTS_VIEW,&identity); device->SetTransform(D3DTS_PROJECTION,&projection);
        device->SetTexture(0,texture); device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE);
        device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
        device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);device->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
        device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
        device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
        device->SetRenderState(D3DRS_LIGHTING,FALSE); device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_ZENABLE,FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_FOGENABLE,fog);device->SetRenderState(D3DRS_FOGTABLEMODE,tableMode);device->SetRenderState(D3DRS_FOGVERTEXMODE,vertexMode);device->SetRenderState(D3DRS_RANGEFOGENABLE,rangeFog);
        device->SetRenderState(D3DRS_FOGSTART,floatBits(fogStart));device->SetRenderState(D3DRS_FOGEND,floatBits(fogEnd));device->SetRenderState(D3DRS_FOGDENSITY,floatBits(1.3f));device->SetRenderState(D3DRS_FOGCOLOR,0x406020);
        device->SetVertexShader(nullptr);device->SetPixelShader(nullptr);device->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    };
    auto capture=[&]() {
        IDirect3DSurface9* back{};IDirect3DSurface9* read{};
        std::vector<DWORD> pixels;
        if(SUCCEEDED(device->GetRenderTarget(0,&back)) && SUCCEEDED(device->CreateOffscreenPlainSurface(128,128,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr)) && SUCCEEDED(device->GetRenderTargetData(back,read))) {
            D3DLOCKED_RECT rect{};
            if(SUCCEEDED(read->LockRect(&rect,nullptr,D3DLOCK_READONLY))) {
                pixels.resize(128*128);
                for(unsigned y=0;y<128;++y) memcpy(pixels.data()+y*128,static_cast<char*>(rect.pBits)+y*rect.Pitch,128*4);
                read->UnlockRect();
            }
        }
        if(back)back->Release();if(read)read->Release();return pixels;
    };
    Vertex vertices[6];
    for(unsigned t=0;t<2;++t)for(unsigned c=0;c<3;++c) {
        const auto& in=triangles[t];auto* color=in.rgba+c*4;
        vertices[t*3+c]={in.p[c*3],in.p[c*3+1],in.p[c*3+2],(color[3]<<24)|(color[0]<<16)|(color[1]<<8)|color[2],in.uv[c*2],in.uv[c*2+1]};
    }
    for(unsigned pass=0;pass<cases.size();++pass) {
        const auto& test=cases[pass];const bool fog=test.enabled;
        projection=identity;tableMode=test.table?test.mode:D3DFOG_NONE;vertexMode=test.table?D3DFOG_NONE:test.mode;rangeFog=test.range;
        fogStart=pass<3?0:.2f;fogEnd=pass<3?1:.8f;
        if(test.perspective) {projection={};projection._11=projection._22=1;projection._33=4.0f/3.9f;projection._34=1;projection._43=-.4f/3.9f;}
        for(unsigned t=0;t<2;++t) {
            triangles[t]=originalTriangles[t];
            if(pass>=3)for(unsigned c=0;c<3;++c) {
                const float depth=test.perspective?(c==0?.2f:c==1?.7f:1.3f):(c==0?.1f:c==1?.6f:.9f);
                triangles[t].p[c*3+2]=depth;
                if(test.perspective) {triangles[t].p[c*3]*=depth;triangles[t].p[c*3+1]*=depth;}
            }
        }
        for(unsigned t=0;t<2;++t)for(unsigned c=0;c<3;++c) {
            const auto& in=triangles[t];auto* color=in.rgba+c*4;
            vertices[t*3+c]={in.p[c*3],in.p[c*3+1],in.p[c*3+2],(color[3]<<24)|(color[0]<<16)|(color[1]<<8)|color[2],in.uv[c*2],in.uv[c*2+1]};
        }
        setup(fog);device->Clear(0,nullptr,D3DCLEAR_TARGET,0x203040,1,0);CHECK(SUCCEEDED(device->BeginScene()));
        CHECK(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,vertices,sizeof(Vertex))));CHECK(SUCCEEDED(device->EndScene()));
        auto expected=capture();CHECK(!expected.empty());
        setup(fog);device->Clear(0,nullptr,D3DCLEAR_TARGET,0x203040,1,0);CHECK(SUCCEEDED(device->BeginScene()));
        if(timerSupported)CHECK(timer.begin(pass!=0));
        CHECK(fxp::render::TestDraw(device,triangles,2,&world._11));CHECK(SUCCEEDED(device->EndScene()));
        timer.end();
        auto actual=capture();CHECK(actual.size()==expected.size());
        if(timerSupported) {
            // Readback already completes this draw; timer itself never flushes.
            auto timing=timer.take();
            CHECK(timing.samples==1&&timing.pending==0&&timing.dropped==0);
            CHECK(timing.baselineSamples==(pass==0?1u:0u)&&timing.explosionSamples==(pass==0?0u:1u));
            CHECK(timing.milliseconds>=0&&timing.milliseconds<1000);
            ++measuredSamples;
            std::printf("GPU timestamp scope %u: %.6f ms\n",pass,timing.milliseconds);
        }
        unsigned maxError{},different{};
        for(size_t i=0;i<actual.size();++i)for(unsigned channel=0;channel<3;++channel) {
            unsigned shift=channel*8;int error=std::abs(int((actual[i]>>shift)&255)-int((expected[i]>>shift)&255));
            maxError=std::max(maxError,unsigned(error));different+=error!=0;
        }
        std::printf("Pass %u table=%u range=%u perspective=%u mode=%u: maximum RGB error %u/255, differing channels %u\n",pass,test.table,test.range,test.perspective,unsigned(test.mode),maxError,different);
        CHECK(maxError<=3);
        UINT freq0{},freq1{};device->GetStreamSourceFreq(0,&freq0);device->GetStreamSourceFreq(1,&freq1);CHECK(freq0==1&&freq1==1);
        DWORD value{};device->GetRenderState(D3DRS_FOGENABLE,&value);CHECK(value==DWORD(fog));
        IDirect3DVertexShader9* vs{};device->GetVertexShader(&vs);CHECK(!vs);
        IDirect3DPixelShader9* ps{};device->GetPixelShader(&ps);CHECK(!ps);
        if(pass==1) {
            timer.reset();
#ifdef FXP_INTEGRATION_TEST
            mws::RenderSubmission::onReset();
            mws::D3D9StateCache::instance().onReset();
#endif
            CHECK(SUCCEEDED(device->Reset(&pp))); if(timerSupported)CHECK(timer.initialize(device));
        }
    }
    auto stats=fxp::render::TakeCounters();CHECK(stats.draws==cases.size()&&stats.triangles==2*cases.size()&&stats.renderFallbacks==0);
    // Unsupported states must leave the original draw path available and must
    // not poison eligibility for the next supported batch.
    setup(false); CHECK(SUCCEEDED(device->BeginScene()));
    device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);
    CHECK(!fxp::render::TestDraw(device,triangles,2,&world._11));
    device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT2);
    CHECK(!fxp::render::TestDraw(device,triangles,2,&world._11));
    device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
    CHECK(fxp::render::TestDraw(device,triangles,2,&world._11));
    CHECK(SUCCEEDED(device->EndScene()));
    timer.reset();
    fxp::render::TestRelease();
#ifdef FXP_INTEGRATION_TEST
    mws::RenderSubmission::uninstall();
    mws::D3D9StateCache::instance().uninstall();
#endif
    texture->Release();device->Release();api->Release();DestroyWindow(window);
    if(timerSupported)CHECK(measuredSamples==cases.size());
    std::puts("Hidden D3D9 texture/color/alpha/fog/world-transform differential, state restoration and Reset/recreation passed.");
}
