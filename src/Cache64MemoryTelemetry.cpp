#include "mws/Cache64MemoryTelemetry.hpp"
#include <objbase.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include <oleauto.h>
#include <cerrno>
#include <cwchar>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;

// WMI represents CIM_UINT64 as a decimal VT_BSTR. Never cast it to a
// 32-bit SIZE_T or DWORD: the helper can cache tens of GiB.
bool getUint64(IWbemClassObject* object,
               const wchar_t* property,
               std::uint64_t& result) noexcept {
  VARIANT value;
  VariantInit(&value);
  const HRESULT hr = object->Get(property, 0, &value, nullptr, nullptr);
  bool valid = false;
  if (SUCCEEDED(hr) && value.vt == VT_BSTR && value.bstrVal) {
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long number = std::wcstoull(value.bstrVal, &end, 10);
    if (errno == 0 && end != value.bstrVal && *end == L'\0') {
      result = static_cast<std::uint64_t>(number);
      valid = true;
    }
  } else if (SUCCEEDED(hr) && value.vt == VT_UI4) {
    result = value.ulVal;
    valid = true;
  } else if (SUCCEEDED(hr) && value.vt == VT_I4 && value.lVal >= 0) {
    result = static_cast<std::uint64_t>(value.lVal);
    valid = true;
  } else if (SUCCEEDED(hr) && value.vt == VT_UI8) {
    result = value.ullVal;
    valid = true;
  }
  VariantClear(&value);
  return valid;
}

enum class QueryResult { Found, Missing, Error };

QueryResult queryCache64(IWbemServices* service,
                  DWORD ownPid,
                  DWORD& helperPid,
                  std::uint64_t& privateBytes,
                  std::uint64_t& workingSetBytes) noexcept {
  // The actual helper is CreateProcessW-spawned directly by GTA.
  // Match BOTH the exact executable name and GTA's PID, never a
  // same-named helper launched by another game instance.
  const std::wstring statement =
      L"SELECT ProcessId, PrivatePageCount, WorkingSetSize "
      L"FROM Win32_Process WHERE Name='DistantStreaming_AssetCache64.exe' "
      L"AND ParentProcessId=" + std::to_wstring(ownPid);

  BSTR language = SysAllocString(L"WQL");
  BSTR query = SysAllocString(statement.c_str());
  if (!language || !query) {
    SysFreeString(language);
    SysFreeString(query);
    return QueryResult::Error;
  }

  ComPtr<IEnumWbemClassObject> enumeration;
  const HRESULT result = service->ExecQuery(language, query,
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
      nullptr, enumeration.GetAddressOf());
  SysFreeString(language);
  SysFreeString(query);
  if (FAILED(result) || !enumeration) return QueryResult::Error;

  ComPtr<IWbemClassObject> object;
  ULONG returned = 0;
  // Bounded query so an unavailable WMI provider doesn't monopolize the
  // helper sampler. This runs on a low-priority worker, never on Present.
  const HRESULT nextResult = enumeration->Next(400, 1,
      object.GetAddressOf(), &returned);
  if (FAILED(nextResult) || nextResult == WBEM_S_TIMEDOUT) return QueryResult::Error;
  if (returned == 0 || !object) return QueryResult::Missing;

  std::uint64_t pid = 0;
  if (!getUint64(object.Get(), L"ProcessId", pid) ||
      pid == 0 || pid > MAXDWORD ||
      !getUint64(object.Get(), L"PrivatePageCount", privateBytes) ||
      !getUint64(object.Get(), L"WorkingSetSize", workingSetBytes)) return QueryResult::Error;

  const DWORD foundPid = static_cast<DWORD>(pid);
  // Avoid presenting a stale WMI row for an already-exited process.
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
      FALSE, foundPid);
  if (!process) return QueryResult::Missing;
  const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  if (!alive) return QueryResult::Missing;

  helperPid = foundPid;
  return QueryResult::Found;
}
} // namespace

namespace mws {

void Cache64MemoryTelemetry::start(bool enabled) {
  stop();
  if (!enabled) return;
  state_.store(State::Waiting, std::memory_order_release);
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) {
    state_.store(State::Unavailable, std::memory_order_release);
    return;
  }
  worker_ = CreateThread(nullptr, 0, &Cache64MemoryTelemetry::workerEntry,
      this, 0, nullptr);
  if (!worker_) {
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    state_.store(State::Unavailable, std::memory_order_release);
  }
}

void Cache64MemoryTelemetry::stop() {
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
  pid_.store(0, std::memory_order_relaxed);
  privateBytes_.store(0, std::memory_order_relaxed);
  workingSetBytes_.store(0, std::memory_order_relaxed);
  state_.store(State::Waiting, std::memory_order_release);
}

Cache64MemoryTelemetry::Snapshot Cache64MemoryTelemetry::sample() const noexcept {
  Snapshot value{};
  value.state = state_.load(std::memory_order_acquire);
  if (value.state == State::Available) {
    value.pid = pid_.load(std::memory_order_relaxed);
    value.privateBytes = privateBytes_.load(std::memory_order_relaxed);
    value.workingSetBytes = workingSetBytes_.load(std::memory_order_relaxed);
  }
  return value;
}

DWORD WINAPI Cache64MemoryTelemetry::workerEntry(void* context) {
  static_cast<Cache64MemoryTelemetry*>(context)->run();
  return 0;
}

void Cache64MemoryTelemetry::run() noexcept {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) {
    state_.store(State::Unavailable, std::memory_order_release);
    return;
  }

  // CoInitializeSecurity is process-global and might already be owned by
  // GTA/another ASI. Proxy security is set on OUR WMI connection instead.
  const DWORD ownPid = GetCurrentProcessId();

  while (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT) {
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> service;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(locator.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      BSTR ns = SysAllocString(L"ROOT\\CIMV2");
      if (ns) {
        hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0,
            nullptr, nullptr, service.GetAddressOf());
        SysFreeString(ns);
      } else {
        hr = E_OUTOFMEMORY;
      }
    }
    if (SUCCEEDED(hr) && service) {
      hr = CoSetProxyBlanket(service.Get(), RPC_C_AUTHN_WINNT,
          RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    }

    if (FAILED(hr) || !service) {
      state_.store(State::Unavailable, std::memory_order_release);
      if (WaitForSingleObject(stopEvent_, 5000) != WAIT_TIMEOUT) break;
      continue;
    }

    while (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT) {
      DWORD foundPid = 0;
      std::uint64_t privateBytes = 0, workingSetBytes = 0;
      const auto status = queryCache64(service.Get(), ownPid, foundPid,
                       privateBytes, workingSetBytes);
      if (status == QueryResult::Found) {
        pid_.store(foundPid, std::memory_order_relaxed);
        privateBytes_.store(privateBytes, std::memory_order_relaxed);
        workingSetBytes_.store(workingSetBytes, std::memory_order_relaxed);
        state_.store(State::Available, std::memory_order_release);
      } else {
        // A WMI provider failure is different from the helper being absent.
        // Clear the old snapshot in either case.
        state_.store(status == QueryResult::Missing ?
            State::Waiting : State::Unavailable, std::memory_order_release);
      }
      if (WaitForSingleObject(stopEvent_, 2000) != WAIT_TIMEOUT) break;
      // Recreate a failed proxy rather than retrying the same broken WMI
      // connection forever after a provider/service restart.
      if (status == QueryResult::Error) break;
    }
  }
  CoUninitialize();
}

} // namespace mws
