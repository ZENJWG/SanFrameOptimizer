#include <Windows.h>
#include "mws/Plugin.hpp"

static DWORD WINAPI InitThread(void*) {
  // Live detours and callbacks require the ASI's code to remain mapped.
  HMODULE pinned{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&InitThread), &pinned)) return 0;
  mws::Plugin::instance().initialize();
  return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    mws::setPluginModule(module);
    if (HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr))
      CloseHandle(thread);
  }

  // Do not perform MinHook removal, logging or worker teardown
  // from DLL_PROCESS_DETACH. DllMain runs under the Windows loader lock, and
  // complex cleanup here can deadlock or obscure the real process-exit path.
  // During process termination Windows reclaims these resources anyway.
  return TRUE;
}
