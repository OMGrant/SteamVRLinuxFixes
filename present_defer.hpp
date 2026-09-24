#pragma once

#include <cstdint>
#include <cstring>
#include <vulkan/vulkan.h>

// Deferred headset presents for vblank alignment. See present_defer.cpp.
bool DeferEnabled();

// Queues a headset present for the worker thread to make at the release
// time. Returns false when it cannot (alignment not ready); the caller then
// presents normally.
bool DeferPresent(PFN_vkQueuePresentKHR next, VkQueue queue, const VkPresentInfoKHR* pInfo, uint64_t presentId);

// A present that is not deferred, serialised with the worker.
VkResult PresentLocked(PFN_vkQueuePresentKHR next, VkQueue queue, const VkPresentInfoKHR* pInfo);

// Hooks that serialise SteamVR's queue and swapchain calls with the worker.
PFN_vkVoidFunction DeferGetDeviceProcAddr(const char* pName);
