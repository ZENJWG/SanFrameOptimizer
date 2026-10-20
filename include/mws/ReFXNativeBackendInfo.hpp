#pragma once
#include <cstdint>

// Versioned CPU telemetry ABI. Values come from the actual native Vulkan runtime.
// Fixed layout shared by ReFX and consumers; no Vulkan SDK dependency required.
#pragma pack(push, 8)
struct ReFXNativeBackendInfoV1
{
    std::uint32_t structSize = sizeof(ReFXNativeBackendInfoV1);
    std::uint32_t abiVersion = 1;
    std::uint32_t flags = 0; // 1: live Vulkan device; 2: successful visible presentation, active session
    std::uint32_t apiVersion = 0; // VkPhysicalDeviceProperties::apiVersion
    std::uint32_t driverVersion = 0; // vendor-encoded VkPhysicalDeviceProperties::driverVersion
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t reserved = 0;
    std::uint64_t vulkanDevice = 0;
    std::uint64_t presentedFrames = 0; // successful/suboptimal vkQueuePresentKHR results only
    char gpuName[256] = {};
};
#pragma pack(pop)
static_assert(sizeof(ReFXNativeBackendInfoV1) == 304);
using ReFXQueryNativeBackendInfoFn = int (__cdecl *)(std::uint32_t, ReFXNativeBackendInfoV1 *);
