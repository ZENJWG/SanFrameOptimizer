#include "mws/D3D9StateCache.hpp"
#include "mws/SkygfxCompatibility.hpp"
#include "mws/DrawTelemetry.hpp"
#include <Windows.h>
#include <array>
#include <bitset>
#include <cstring>
#include <iostream>
#include <stdexcept>

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
using Constants = std::array<std::array<float, 4>, 256>;
struct Surface { void** vtable{}; unsigned references = 1; };
ULONG STDMETHODCALLTYPE surfaceAddRef(Surface* self) { return ++self->references; }
ULONG STDMETHODCALLTYPE surfaceRelease(Surface* self) { return --self->references; }
struct Device;
struct Block {
  void** vtable{};
  Device* device{};
  Constants vs{}, ps{};
  std::bitset<256> vsMask{}, psMask{};
  void* vertexShader{}; void* pixelShader{};
  bool vertexShaderMask{}, pixelShaderMask{};
};
struct Device {
  void** vtable{};
  Constants vs{}, ps{};
  Block block;
  bool recording{}, fail{}, multithreaded{};
  void* vertexShader{}; void* pixelShader{};
  unsigned vertexBindCalls{}, pixelBindCalls{}, drawCalls{}, copyCalls{};
  Surface* primary{}; Surface* target{};
  unsigned vsCalls{}, psCalls{}, lastRegister{}, lastCount{};
};
HRESULT STDMETHODCALLTYPE placeholder() { return D3D_OK; }
HRESULT STDMETHODCALLTYPE creation(Device* self, D3DDEVICE_CREATION_PARAMETERS* parameters) {
  *parameters = {};
  if (self->multithreaded) parameters->BehaviorFlags = D3DCREATE_MULTITHREADED;
  return D3D_OK;
}
HRESULT constants(Device* self, UINT first, const float* data, UINT count, bool pixel) {
  if (pixel) ++self->psCalls; else ++self->vsCalls;
  self->lastRegister = first; self->lastCount = count;
  if (self->fail || !data || first >= (pixel ? 224u : 256u) || count > (pixel ? 224u : 256u) - first)
    return D3DERR_INVALIDCALL;
  auto& target = self->recording ? (pixel ? self->block.ps : self->block.vs) : (pixel ? self->ps : self->vs);
  for (UINT i = 0; i < count; ++i) {
    std::memcpy(target[first + i].data(), data + i * 4, 16);
    if (self->recording) (pixel ? self->block.psMask : self->block.vsMask).set(first + i);
  }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE vertex(Device* self, UINT first, const float* data, UINT count) {
  return constants(self, first, data, count, false);
}
HRESULT STDMETHODCALLTYPE pixel(Device* self, UINT first, const float* data, UINT count) {
  return constants(self, first, data, count, true);
}
HRESULT STDMETHODCALLTYPE bindVertex(Device* self, IDirect3DVertexShader9* shader) {
  ++self->vertexBindCalls;
  if (self->fail) return D3DERR_INVALIDCALL;
  if (self->recording) { self->block.vertexShader = shader; self->block.vertexShaderMask = true; }
  else self->vertexShader = shader;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE bindPixel(Device* self, IDirect3DPixelShader9* shader) {
  ++self->pixelBindCalls;
  if (self->fail) return D3DERR_INVALIDCALL;
  if (self->recording) { self->block.pixelShader = shader; self->block.pixelShaderMask = true; }
  else self->pixelShader = shader;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE backBuffer(Device* self, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** result) {
  *result = reinterpret_cast<IDirect3DSurface9*>(self->primary);
  if (*result) (*result)->AddRef();
  return *result ? D3D_OK : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE renderTarget(Device* self, DWORD, IDirect3DSurface9** result) {
  *result = reinterpret_cast<IDirect3DSurface9*>(self->target);
  if (*result) (*result)->AddRef();
  return *result ? D3D_OK : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE setTarget(Device* self, DWORD, IDirect3DSurface9* target) {
  if (self->fail) return D3DERR_INVALIDCALL;
  self->target = reinterpret_cast<Surface*>(target);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE draw(Device* self, D3DPRIMITIVETYPE, UINT, UINT) {
  ++self->drawCalls;
  return self->fail ? D3DERR_INVALIDCALL : D3D_OK;
}
HRESULT STDMETHODCALLTYPE drawIndexed(Device* self, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
  ++self->drawCalls;
  return self->fail ? D3DERR_INVALIDCALL : D3D_OK;
}
HRESULT STDMETHODCALLTYPE drawUP(Device* self, D3DPRIMITIVETYPE, UINT, const void*, UINT) {
  ++self->drawCalls;
  return self->fail ? D3DERR_INVALIDCALL : D3D_OK;
}
HRESULT STDMETHODCALLTYPE drawIndexedUP(Device* self, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) {
  ++self->drawCalls;
  return self->fail ? D3DERR_INVALIDCALL : D3D_OK;
}
HRESULT STDMETHODCALLTYPE copySurface(Device* self, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE) {
  ++self->copyCalls;
  return self->fail ? D3DERR_INVALIDCALL : D3D_OK;
}
HRESULT STDMETHODCALLTYPE begin(Device* self) {
  self->recording = true;
  self->block.vertexShaderMask = self->block.pixelShaderMask = false;
  self->block.vsMask.reset(); self->block.psMask.reset();
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE end(Device* self, IDirect3DStateBlock9** result) {
  self->recording = false;
  *result = reinterpret_cast<IDirect3DStateBlock9*>(&self->block);
  return D3D_OK;
}
ULONG STDMETHODCALLTYPE release(Block*) { return 1; }
HRESULT STDMETHODCALLTYPE capture(Block* self) {
  self->vs = self->device->vs; self->ps = self->device->ps;
  self->vertexShader = self->device->vertexShader; self->pixelShader = self->device->pixelShader;
  self->vertexShaderMask = self->pixelShaderMask = true;
  self->vsMask.set(); self->psMask.set();
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE apply(Block* self) {
  if (self->vertexShaderMask) self->device->vertexShader = self->vertexShader;
  if (self->pixelShaderMask) self->device->pixelShader = self->pixelShader;
  for (size_t i = 0; i < 256; ++i) {
    if (self->vsMask.test(i)) self->device->vs[i] = self->vs[i];
    if (self->psMask.test(i)) self->device->ps[i] = self->ps[i];
  }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE create(Device* self, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9** result) {
  capture(&self->block);
  *result = reinterpret_cast<IDirect3DStateBlock9*>(&self->block);
  return D3D_OK;
}
int main(int argc, char** argv) {
  check(argc == 3, "SkyGFX/Proper Shaders fixture paths missing");
  std::array<void*, 119> table;
  table.fill(reinterpret_cast<void*>(&placeholder));
  table[9] = reinterpret_cast<void*>(&creation);
  table[92] = reinterpret_cast<void*>(&bindVertex); table[107] = reinterpret_cast<void*>(&bindPixel);
  table[18] = reinterpret_cast<void*>(&backBuffer); table[38] = reinterpret_cast<void*>(&renderTarget);
  table[37] = reinterpret_cast<void*>(&setTarget); table[34] = reinterpret_cast<void*>(&copySurface);
  table[81] = reinterpret_cast<void*>(&draw); table[82] = reinterpret_cast<void*>(&drawIndexed);
  table[83] = reinterpret_cast<void*>(&drawUP); table[84] = reinterpret_cast<void*>(&drawIndexedUP);
  table[94] = reinterpret_cast<void*>(&vertex); table[109] = reinterpret_cast<void*>(&pixel);
  table[59] = reinterpret_cast<void*>(&create); table[60] = reinterpret_cast<void*>(&begin);
  table[61] = reinterpret_cast<void*>(&end);
  std::array<void*, 6> blockTable;
  blockTable.fill(reinterpret_cast<void*>(&placeholder));
  blockTable[2] = reinterpret_cast<void*>(&release); blockTable[4] = reinterpret_cast<void*>(&capture);
  blockTable[5] = reinterpret_cast<void*>(&apply);
  std::array<void*, 3> surfaceTable{reinterpret_cast<void*>(&placeholder),
      reinterpret_cast<void*>(&surfaceAddRef), reinterpret_cast<void*>(&surfaceRelease)};
  Surface primary{surfaceTable.data()}, offscreen{surfaceTable.data()};
  Device device;
  device.primary = device.target = &primary;
  device.vtable = table.data(); device.block.vtable = blockTable.data(); device.block.device = &device;
  auto* d3d = reinterpret_cast<IDirect3DDevice9*>(&device);
  mws::Config config;
  check(!config.skygfxPassProfiling, "profiling enabled by default");
  config.skygfxPassProfiling = true;
  config.d3dStateAdaptive = false;
  config.d3dStateInvalidateOnPresent = false;
  auto& cache = mws::D3D9StateCache::instance();
  HMODULE sky = nullptr, proper = nullptr;
  int result = 0;
  try {
    check(cache.install(d3d, config), "cache install failed");
    float data[60]{};
    check(SUCCEEDED(d3d->SetVertexShaderConstantF(16, data, 15)), "initial upload failed");
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.vsCalls == 2, "patch active without loaded SkyGFX");
    sky = LoadLibraryA(argv[1]);
    check(sky && mws::isSkygfxLoaded(), "SkyGFX module detection failed");
    cache.onPresent();
    check(cache.stats().skygfxOptimizationActive, "late SkyGFX did not activate patch");
    check(cache.stats().skygfxShaderBindingsActive && cache.stats().skygfxPassProfilingActive, "SkyGFX bindings/profile did not activate");
    auto* shaderA = reinterpret_cast<IDirect3DVertexShader9*>(0x1000);
    auto* shaderB = reinterpret_cast<IDirect3DVertexShader9*>(0x2000);
    auto* pixelA = reinterpret_cast<IDirect3DPixelShader9*>(0x3000);
    d3d->SetVertexShader(shaderA); d3d->SetVertexShader(shaderA);
    check(device.vertexBindCalls == 1, "identical vertex bind not suppressed");
    d3d->SetPixelShader(pixelA); d3d->SetPixelShader(pixelA);
    check(device.pixelBindCalls == 1, "identical pixel bind not suppressed");
    d3d->SetVertexShader(shaderB);
    check(device.vertexShader == shaderB && device.vertexBindCalls == 2, "shader transition lost");
    device.fail = true;
    check(FAILED(d3d->SetVertexShader(shaderA)), "shader bind failure hidden");
    device.fail = false; d3d->SetVertexShader(shaderA);
    check(device.vertexBindCalls == 4, "failed bind polluted cache");
    d3d->SetPixelShader(nullptr); d3d->SetPixelShader(nullptr);
    check(device.pixelShader == nullptr && device.pixelBindCalls == 2, "null shader binding incorrect");
    d3d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    check(cache.stats().skygfxMainDraws == 1 && primary.references == 2, "main draw profiling/refcount incorrect");
    auto* offscreenSurface = reinterpret_cast<IDirect3DSurface9*>(&offscreen);
    d3d->SetRenderTarget(0, offscreenSurface);
    d3d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
    d3d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, nullptr, 12);
    d3d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, nullptr, D3DFMT_INDEX16, nullptr, 12);
    check(cache.stats().skygfxOffscreenDraws == 3, "off-screen draw methods not profiled");
    {
      mws::DrawTelemetry::OverlayScope overlay;
      d3d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
      d3d->StretchRect(offscreenSurface, nullptr, offscreenSurface, nullptr, D3DTEXF_NONE);
    }
    check(cache.stats().skygfxOffscreenDraws == 3 && cache.stats().skygfxCopies == 0, "OSD contaminated profiling");
    d3d->StretchRect(offscreenSurface, nullptr, offscreenSurface, nullptr, D3DTEXF_NONE);
    check(cache.stats().skygfxCopies == 1 && device.copyCalls == 2, "copy profiling changed original calls");
    device.fail = true;
    check(FAILED(d3d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)), "profile hid draw failure");
    device.fail = false;
    check(cache.stats().skygfxProfileFailures == 1 && cache.stats().skygfxOffscreenDraws == 3, "failed draw counted as successful");
    device.primary = nullptr; cache.onReset();
    check(primary.references == 1, "profiler retained reset-blocking primary surface");
    d3d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    check(cache.stats().skygfxUnknownDraws == 1, "missing primary target guessed as reflection");
    device.primary = &primary;
    d3d->SetRenderTarget(0, reinterpret_cast<IDirect3DSurface9*>(&primary));

    d3d->SetVertexShaderConstantF(16, data, 15);
    unsigned calls = device.vsCalls;
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.vsCalls == calls, "identical lighting upload not suppressed");
    data[8] = 1.0f;
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.lastRegister == 18 && device.lastCount == 1, "lighting upload edges not trimmed");
    check(device.vs[18][0] == 1.0f, "trimmed upload changed wrong register");
    data[8] = 2.0f; data[36] = 1.0f;
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.lastRegister == 18 && device.lastCount == 8, "disjoint changes need one enclosing span");
    d3d->SetPixelShaderConstantF(4, data, 4);
    data[4] = 3.0f;
    d3d->SetPixelShaderConstantF(4, data, 4);
    check(device.lastRegister == 5 && device.lastCount == 1, "pixel constants not trimmed");
    calls = device.vsCalls;
    device.fail = true; data[8] = 4.0f;
    check(FAILED(d3d->SetVertexShaderConstantF(16, data, 15)), "driver failure not propagated");
    device.fail = false;
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.vsCalls == calls + 2 && device.vs[18][0] == 4.0f, "failed upload polluted cache");
    cache.onReset();
    unsigned bindCalls = device.vertexBindCalls;
    d3d->SetVertexShader(shaderA);
    check(device.vertexBindCalls == bindCalls + 1, "reset reused stale shader binding");
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.lastRegister == 16 && device.lastCount == 15, "reset reused stale constants");
    cache.invalidate(); // Includes OSD/state-block restoration boundaries.
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.lastCount == 15, "unknown constants were trimmed");
    d3d->BeginStateBlock();
    bindCalls = device.vertexBindCalls;
    d3d->SetVertexShader(shaderA); d3d->SetVertexShader(shaderA);
    check(device.vertexBindCalls == bindCalls + 2, "recording suppressed shader binds");
    calls = device.vsCalls;
    d3d->SetVertexShaderConstantF(16, data, 15);
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.vsCalls == calls + 2 && device.lastCount == 15, "state-block recording suppressed uploads");
    IDirect3DStateBlock9* block = nullptr;
    d3d->EndStateBlock(&block);
    data[8] = 5.0f; d3d->SetVertexShaderConstantF(16, data, 15);
    d3d->SetVertexShader(shaderB);
    block->Apply();
    bindCalls = device.vertexBindCalls;
    d3d->SetVertexShader(shaderA);
    check(device.vertexBindCalls == bindCalls && device.vertexShader == shaderA, "recorded shader-binding delta incorrect");
    data[8] = 4.0f; calls = device.vsCalls;
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.vsCalls == calls && device.vs[18][0] == 4.0f, "recorded state-block cache wrong");
    d3d->CreateStateBlock(D3DSBT_ALL, &block);
    d3d->SetVertexShaderConstantF(16, data, 15);
    block->Apply();
    d3d->SetVertexShaderConstantF(16, data, 15);
    check(device.lastCount == 15, "opaque state-block did not invalidate constants");
    calls = device.vsCalls;
    check(FAILED(d3d->SetVertexShaderConstantF(250, data, 15)) && device.vsCalls == calls + 1,
        "invalid upload was hidden or read outside cache");
    check(FAILED(d3d->SetVertexShaderConstantF(0, nullptr, 1)), "null input failure hidden");
    check(cache.stats().skygfxConstantVectorsSaved > 0, "patch savings not counted");
    proper = LoadLibraryA(argv[2]);
    check(proper != nullptr, "Proper Shaders fixture failed to load");
    Sleep(1010); cache.onPresent();
    check(!cache.stats().skygfxOptimizationActive && !cache.stats().skygfxShaderBindingsActive &&
        !cache.stats().skygfxPassProfilingActive, "Proper Shaders coexistence did not disable patches");
    FreeLibrary(proper); proper = nullptr;
    Sleep(1010); cache.onPresent();
    check(cache.stats().skygfxOptimizationActive, "patch did not recover after compatibility mod unloaded");
    FreeLibrary(sky); sky = nullptr;
    Sleep(1010); cache.onPresent();
    check(!cache.stats().skygfxOptimizationActive, "SkyGFX unload did not disable patch");
    cache.uninstall();
    check(table[94] == reinterpret_cast<void*>(&vertex) && table[92] == reinterpret_cast<void*>(&bindVertex) &&
        table[81] == reinterpret_cast<void*>(&draw) && table[34] == reinterpret_cast<void*>(&copySurface),
        "uninstall did not restore device hooks");
    check(primary.references == 1, "uninstall leaked primary render target");
    sky = LoadLibraryA(argv[1]);
    check(sky != nullptr, "SkyGFX reload failed");
    config.skygfxShaderBindingOptimization = false;
    config.skygfxPassProfiling = false;
    check(cache.install(d3d, config), "disabled binding/profile policy install failed");
    check(!cache.stats().skygfxShaderBindingsActive && !cache.stats().skygfxPassProfilingActive &&
        table[92] == reinterpret_cast<void*>(&bindVertex) && table[81] == reinterpret_cast<void*>(&draw),
        "disabled features still installed hooks");
    bindCalls = device.vertexBindCalls;
    d3d->SetVertexShader(shaderA); d3d->SetVertexShader(shaderA);
    check(device.vertexBindCalls == bindCalls + 2, "disabled binding patch suppressed calls");
    cache.uninstall(); FreeLibrary(sky); sky = nullptr;
    device.multithreaded = true;
    check(!cache.install(d3d, config), "unsafe multithreaded cache installed");
    device.multithreaded = false;
    config.optimizationsEnabled = false;
    check(!cache.install(d3d, config), "master-off installed optimization hooks");
    std::cout << "SkyGFX constant, binding and pass-profile hook regression checks passed\n";
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n'; result = 1;
  }
  cache.uninstall();
  if (proper) FreeLibrary(proper);
  if (sky) FreeLibrary(sky);
  return result;
}
