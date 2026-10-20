#include "mws/GpuMemoryTelemetry.hpp"
#include <dxgi1_4.h>
#include <objbase.h>

namespace {

bool adapterHasMonitor(IDXGIAdapter1* adapter, HMONITOR target) noexcept {
  if (!target) return false;
  for (UINT index = 0; ; ++index) {
    IDXGIOutput* output = nullptr;
    if (FAILED(adapter->EnumOutputs(index, &output)) || !output)
      break;
    DXGI_OUTPUT_DESC desc{};
    const bool matched = SUCCEEDED(output->GetDesc(&desc)) &&
        desc.Monitor == target;
    output->Release();
    if (matched) return true;
  }
  return false;
}

// The adapter's WDDM budget/usage is 64-bit. DXGI_ADAPTER_DESC::DedicatedVideoMemory
// is SIZE_T (32-bit in GTA SA), and MUST NOT be used as a >4 GiB denominator.
IDXGIAdapter3* findAdapter(HMONITOR monitor, std::uint64_t pciId) noexcept {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
      reinterpret_cast<void**>(&factory))) || !factory)
    return nullptr;

  IDXGIAdapter3* selected = nullptr;
  unsigned selectedScore = 0;
  IDXGIAdapter3* single = nullptr;
  UINT count = 0;
  for (UINT index = 0; ; ++index) {
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(factory->EnumAdapters1(index, &adapter)) || !adapter)
      break;

    DXGI_ADAPTER_DESC1 desc{};
    const bool gotDesc = SUCCEEDED(adapter->GetDesc1(&desc));
    const std::uint64_t id = (static_cast<std::uint64_t>(desc.VendorId) << 32) |
        static_cast<std::uint64_t>(desc.DeviceId);
    const bool monitorMatch = adapterHasMonitor(adapter, monitor);
    const bool pciMatch = gotDesc && pciId != 0 && id == pciId;
    IDXGIAdapter3* adapter3 = nullptr;
    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
        reinterpret_cast<void**>(&adapter3))) && adapter3) {
      ++count;
      if (count == 1) {
        single = adapter3;
        single->AddRef();
      }
      // Hybrid systems can render on a discrete GPU while the integrated GPU
      // owns the monitor. Prefer the D3D9 adapter identity; use the monitor to
      // break ties between identical GPUs, or when no PCI match exists.
      const unsigned score = (pciMatch ? 2u : 0u) + (monitorMatch ? 1u : 0u);
      if (score > selectedScore) {
        if (selected) selected->Release();
        selected = adapter3;
        selected->AddRef();
        selectedScore = score;
      }
      adapter3->Release();
    }
    adapter->Release();
  }
  if (!selected && count == 1) {
    selected = single;
    selected->AddRef();
  }
  if (single) single->Release();
  factory->Release();
  return selected;
}

} // namespace

namespace mws {

void GpuMemoryTelemetry::start(bool enabled) {
  stop();
  if (!enabled) return;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) return;
  active_.store(true, std::memory_order_release);
  worker_ = CreateThread(nullptr, 0, &GpuMemoryTelemetry::workerEntry,
      this, 0, nullptr);
  if (!worker_) {
    active_.store(false, std::memory_order_release);
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
}

void GpuMemoryTelemetry::stop() {
  if (worker_) {
    SetEvent(stopEvent_);
    WaitForSingleObject(worker_, INFINITE);
    CloseHandle(worker_);
    worker_ = nullptr;
  }
  if (stopEvent_) {
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
  active_.store(false, std::memory_order_release);
  valid_.store(false, std::memory_order_release);
}

void GpuMemoryTelemetry::setDeviceAdapter(IDirect3DDevice9* device) noexcept {
  if (!device) return;
  D3DDEVICE_CREATION_PARAMETERS cp{};
  if (FAILED(device->GetCreationParameters(&cp))) return;

  IDirect3D9* d3d = nullptr;
  if (FAILED(device->GetDirect3D(&d3d)) || !d3d) return;
  const HMONITOR monitor = d3d->GetAdapterMonitor(cp.AdapterOrdinal);
  D3DADAPTER_IDENTIFIER9 id{};
  std::uint64_t pci = 0;
  if (SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id)))
    pci = (static_cast<std::uint64_t>(id.VendorId) << 32) | id.DeviceId;
  d3d->Release();

  // These atomics are only written at device creation/reset.
  monitor_.store(reinterpret_cast<std::uintptr_t>(monitor), std::memory_order_release);
  pciId_.store(pci, std::memory_order_release);
}

GpuMemoryTelemetry::Snapshot GpuMemoryTelemetry::sample() const noexcept {
  Snapshot out{};
  out.valid = valid_.load(std::memory_order_acquire);
  if (!out.valid) return out;
  out.localUsageBytes = localUsage_.load(std::memory_order_relaxed);
  out.localBudgetBytes = localBudget_.load(std::memory_order_relaxed);
  out.nonLocalUsageBytes = nonLocalUsage_.load(std::memory_order_relaxed);
  out.nonLocalBudgetBytes = nonLocalBudget_.load(std::memory_order_relaxed);
  return out;
}

DWORD WINAPI GpuMemoryTelemetry::workerEntry(void* context) {
  static_cast<GpuMemoryTelemetry*>(context)->run();
  return 0;
}

void GpuMemoryTelemetry::run() noexcept {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = SUCCEEDED(apartment);

  IDXGIAdapter3* adapter = nullptr;
  std::uintptr_t boundMonitor = 0;
  std::uint64_t boundPci = 0;
  DWORD failedAttempts = 0;

  while (WaitForSingleObject(stopEvent_, 1000) == WAIT_TIMEOUT) {
    const auto monitor = monitor_.load(std::memory_order_acquire);
    const auto pci = pciId_.load(std::memory_order_acquire);
    if (!monitor && !pci) {
      valid_.store(false, std::memory_order_release);
      continue;
    }
    if (monitor != boundMonitor || pci != boundPci || (!adapter && (++failedAttempts % 5 == 1))) {
      if (adapter) adapter->Release();
      adapter = findAdapter(reinterpret_cast<HMONITOR>(monitor), pci);
      boundMonitor = monitor;
      boundPci = pci;
      valid_.store(false, std::memory_order_release);
      if (adapter) failedAttempts = 0;
    }
    if (!adapter) continue;

    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
    const HRESULT l = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    const HRESULT n = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
    if (FAILED(l) || FAILED(n)) {
      valid_.store(false, std::memory_order_release);
      adapter->Release();
      adapter = nullptr;
      continue;
    }

    localUsage_.store(local.CurrentUsage, std::memory_order_relaxed);
    localBudget_.store(local.Budget, std::memory_order_relaxed);
    nonLocalUsage_.store(nonLocal.CurrentUsage, std::memory_order_relaxed);
    nonLocalBudget_.store(nonLocal.Budget, std::memory_order_relaxed);
    valid_.store(true, std::memory_order_release);
  }
  if (adapter) adapter->Release();
  if (uninitialize) CoUninitialize();
}

} // namespace mws
