#pragma once
#include <Windows.h>

namespace mws::NativeVulkan {
void configure();
// Detection and OSD compatibility only; never changes native Vulkan policy.
bool attach();
bool isNativeBackend();
void beforeReplay(HWND window);
void afterPresent(HWND window, bool success);
void shutdown();
}
