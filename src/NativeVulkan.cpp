#include "mws/NativeVulkan.hpp"
#include "mws/ReFXNativeBackendInfo.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/Overlay.hpp"
#include "mws/DrawTelemetry.hpp"
#include "mws/GtaSa10US.hpp"
#include "mws/Logger.hpp"
#include "mws/FxpOptimizer.hpp"
#include "mws/EnginePass.hpp"
#include "mws/OptimizationRuntime.hpp"
#include <TlHelp32.h>
#include <atomic>
#include <mutex>

// Compatibility only: identify the backend and retain ReFX's OSD callbacks.
// No Vulkan policy, command optimization, native pacing or engine scheduling.
namespace {
std::mutex providerMutex;
std::atomic<bool> native{};
std::atomic<bool> ready{};
HMODULE provider{};
std::uint64_t presentCalls{},presentSuccesses{};
void renderOverlay(HWND window) {
  auto& overlay=mws::Overlay::instance();
  if(!overlay.enabled() || !mws::gta::addressIsReadable(0xC97C28,sizeof(void*))) return;
  auto* device=*reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
  if(!device) return;
  D3DPRESENT_PARAMETERS parameters{};parameters.hDeviceWindow=window;
  D3DVIEWPORT9 viewport{};
  if(SUCCEEDED(device->GetViewport(&viewport))) {
    parameters.BackBufferWidth=viewport.Width;parameters.BackBufferHeight=viewport.Height;
  }
  if(SUCCEEDED(device->BeginScene())) {overlay.render(device,parameters);device->EndScene();}
}
}
namespace mws::NativeVulkan {
void configure() {ready.store(true,std::memory_order_release);}
bool attach() {
  if(!ready.load(std::memory_order_acquire)) return false;
  if(native.load(std::memory_order_acquire)) return true;
  std::lock_guard lock(providerMutex);
  if(native.load(std::memory_order_relaxed)) return true;
  const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
  if(snapshot==INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
  if(Module32FirstW(snapshot,&entry)) do {
    HMODULE candidate{};
    if(!GetModuleHandleExW(0,entry.szExePath,&candidate)) continue;
    const auto query=reinterpret_cast<ReFXQueryNativeBackendInfoFn>(GetProcAddress(candidate,"ReFX_QueryNativeBackendInfo"));
    ReFXNativeBackendInfoV1 info{};
    if(!query || !query(sizeof(info),&info) || info.abiVersion!=1 || info.structSize!=sizeof(info)) {
      FreeLibrary(candidate);continue;
    }
    provider=candidate;native.store(true,std::memory_order_release);
    auto& diagnostics=Diagnostics::instance();
    diagnostics.registerFeature("Compatibility.NativeBackend",true);
    diagnostics.effective("Compatibility.NativeBackend","versioned ReFX backend export verified; detection/OSD only");
    diagnostics.autoDisable("Performance.FrameLimiter","native presentation is owned by ReFX; SanFrame does not pace it");
    diagnostics.autoDisable("D3D9.DevicePresent","native GPU presentation is owned by ReFX; D3D9 facade calls are not GPU presents");
    Logger::instance().info("ReFX native Vulkan detected: compatibility/OSD only; no SanFrame Vulkan policy, command optimization or native pacing.");
    break;
  } while(Module32NextW(snapshot,&entry));
  CloseHandle(snapshot);return native.load(std::memory_order_acquire);
}
bool isNativeBackend() {return native.load(std::memory_order_acquire);}
void beforeReplay(HWND window) {if(isNativeBackend()) renderOverlay(window);}
void afterPresent(HWND, bool success) {
  if(!isNativeBackend()) return;
  ++presentCalls;if(success) ++presentSuccesses;
  DrawTelemetry::onFrame();
  auto& diagnostics=Diagnostics::instance();
  if(diagnostics.runtimeReportDue()) {
    diagnostics.activity("Compatibility.NativeBackend",presentCalls,presentSuccesses,0,0,presentCalls-presentSuccesses);
    FxpOptimizer::captureDiagnostics();EnginePass::captureDiagnostics();
    OptimizationRuntime::instance().captureDiagnostics();
    diagnostics.printRuntimeSummary();Logger::instance().flushIfDue();
  }
}
void shutdown() {
  if(isNativeBackend()) Diagnostics::instance().activity("Compatibility.NativeBackend",presentCalls,presentSuccesses,0,0,presentCalls-presentSuccesses);
  std::lock_guard lock(providerMutex);native.store(false,std::memory_order_release);
  ready.store(false,std::memory_order_release);
  if(provider) FreeLibrary(provider);
  provider=nullptr;presentCalls=presentSuccesses=0;
}
}
extern "C" __declspec(dllexport) int __cdecl SanFrameOptimizer_BeforeNativeReplay(void* window) {
  try {
    if(!mws::NativeVulkan::attach()) return 0;
    mws::NativeVulkan::beforeReplay(static_cast<HWND>(window));return 1;
  } catch(...) {return 0;}
}
extern "C" __declspec(dllexport) void __cdecl SanFrameOptimizer_AfterNativePresent(void* window,int success) {
  try {mws::NativeVulkan::afterPresent(static_cast<HWND>(window),success!=0);}catch(...) {}
}
