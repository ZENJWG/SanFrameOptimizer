#include "mws/SkygfxCompatibility.hpp"
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace mws {
bool isSkygfxLoaded() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Module32FirstW(snapshot, &entry)) do {
    std::wstring name = std::filesystem::path(entry.szExePath).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    const auto extension = std::filesystem::path(name).extension().wstring();
    if (name.find(L"skygfx") != std::wstring::npos &&
        (extension == L".asi" || extension == L".dll")) { found = true; break; }
  } while (Module32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return found;
}
}
