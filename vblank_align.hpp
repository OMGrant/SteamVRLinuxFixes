#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

// Vblank alignment for NVIDIA direct mode. See vblank_align.cpp.
bool AlignEnabled();

// The release time for a present made now, without waiting. Returns 0 when
// alignment is not ready or the frame would be held longer than one period.
int64_t AlignComputeRelease(int64_t nowNs);
void AlignSetHmdSwapchain(VkSwapchainKHR swapchain);
bool AlignIsHmdSwapchain(VkSwapchainKHR swapchain);

// Called when vrcompositor's wait on a first-pixel-out fence returns.
void AlignOnVsync(int64_t waitBeginNs, int64_t waitEndNs);

// Waits for a present to be shown and records where it landed against vsync.
void AlignAfterPresent(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, int64_t presentedAtNs);
