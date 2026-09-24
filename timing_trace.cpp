// Timing recorder for vrcompositor: logs when SteamVR presents frames and
// which vsync sources it waits on, so frame timing can be measured against
// the display's refresh. Observes only; every call is passed through as is.
//
// Enabled when $HOME/.config/steamvr-linux-fixes/trace exists. Writes
// $HOME/.cache/steamvr-linux-fixes/vrcompositor-timing.csv after
// TRACE_MAX_EVENTS events.

#include "timing_trace.hpp"
#include "steamvr_linux_fixes.hpp"
#include "vblank_align.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

namespace {

constexpr size_t TRACE_MAX_EVENTS = 80000;

struct TraceEvent {
  uint64_t ns;
  const char* name;
  uint64_t a;
  uint64_t b;
};

std::mutex g_traceMutex;
std::vector<TraceEvent> g_events;
std::unordered_set<VkFence> g_vsyncFences;
int g_enabled = -1;  // -1 unknown, 0 off, 1 recording, 2 written

uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

std::string HomePath(const char* rel) {
  const char* home = getenv("HOME");
  return std::string(home ? home : "") + rel;
}

void WriteLocked() {
  mkdir(HomePath("/.cache").c_str(), 0755);
  mkdir(HomePath("/.cache/steamvr-linux-fixes").c_str(), 0755);
  std::string path = HomePath("/.cache/steamvr-linux-fixes/vrcompositor-timing.csv");
  FILE* f = fopen(path.c_str(), "w");
  if (f) {
    fprintf(f, "ns,event,a,b\n");
    for (const auto& e : g_events)
      fprintf(f, "%llu,%s,%llu,%llu\n", (unsigned long long)e.ns, e.name, (unsigned long long)e.a,
              (unsigned long long)e.b);
    fclose(f);
  }
  fprintf(stderr, "Timing trace written: %zu events to %s\n", g_events.size(), path.c_str());
  g_events.clear();
  g_events.shrink_to_fit();
}

PFN_vkVoidFunction Next(VkDevice device, const char* name) {
  std::lock_guard<std::mutex> lock(g_mapMutex);
  auto it = g_deviceDispatch.find(device);
  if (it == g_deviceDispatch.end() || !it->second.GetDeviceProcAddr)
    return nullptr;
  return it->second.GetDeviceProcAddr(device, name);
}

}  // namespace

bool TraceEnabled() {
  std::lock_guard<std::mutex> lock(g_traceMutex);
  if (g_enabled == -1) {
    struct stat st;
    g_enabled = stat(HomePath("/.config/steamvr-linux-fixes/trace").c_str(), &st) == 0 ? 1 : 0;
    if (g_enabled)
      g_events.reserve(TRACE_MAX_EVENTS);
  }
  return g_enabled == 1;
}

void Trace(const char* name, uint64_t a, uint64_t b) {
  if (!TraceEnabled())
    return;
  uint64_t ns = NowNs();
  std::lock_guard<std::mutex> lock(g_traceMutex);
  if (g_enabled != 1)
    return;
  g_events.push_back({ns, name, a, b});
  if (g_events.size() >= TRACE_MAX_EVENTS) {
    WriteLocked();
    g_enabled = 2;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                        uint64_t presentId, uint64_t timeout) {
  static auto next = (PFN_vkWaitForPresentKHR)Next(device, "vkWaitForPresentKHR");
  Trace("wait_present_begin", (uint64_t)swapchain, presentId);
  VkResult r = next(device, swapchain, presentId, timeout);
  Trace("wait_present_end", (uint64_t)swapchain, ((uint64_t)(uint32_t)r << 48) | (presentId & 0xffffffffffffull));
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkRegisterDisplayEventEXT(VkDevice device, VkDisplayKHR display,
                                                              const VkDisplayEventInfoEXT* pDisplayEventInfo,
                                                              const VkAllocationCallbacks* pAllocator,
                                                              VkFence* pFence) {
  static auto next = (PFN_vkRegisterDisplayEventEXT)Next(device, "vkRegisterDisplayEventEXT");
  VkResult r = next(device, display, pDisplayEventInfo, pAllocator, pFence);
  if (r == VK_SUCCESS && pFence) {
    std::lock_guard<std::mutex> lock(g_traceMutex);
    g_vsyncFences.insert(*pFence);
  }
  Trace("register_display_event", pDisplayEventInfo ? pDisplayEventInfo->displayEvent : 0,
        pFence ? (uint64_t)*pFence : 0);
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences,
                                                    VkBool32 waitAll, uint64_t timeout) {
  static auto next = (PFN_vkWaitForFences)Next(device, "vkWaitForFences");
  bool vsync = false;
  {
    std::lock_guard<std::mutex> lock(g_traceMutex);
    for (uint32_t i = 0; i < fenceCount && !vsync; i++)
      vsync = g_vsyncFences.count(pFences[i]) != 0;
  }
  if (!vsync) {
    int64_t b = (int64_t)NowNs();
    VkResult r2 = next(device, fenceCount, pFences, waitAll, timeout);
    int64_t e = (int64_t)NowNs();
    if (e - b > 200000)
      Trace("wait_fence", (uint64_t)fenceCount, (uint64_t)(e - b));
    return r2;
  }
  int64_t begin = (int64_t)NowNs();
  Trace("wait_vsync_fence_begin", (uint64_t)pFences[0], 0);
  VkResult r = next(device, fenceCount, pFences, waitAll, timeout);
  if (vsync) {
    int64_t end = (int64_t)NowNs();
    Trace("wait_vsync_fence_end", (uint64_t)pFences[0], (uint64_t)(uint32_t)r);
    if (r == VK_SUCCESS) {
      AlignOnVsync(begin, end);
      NoteVsyncForHitches(begin, end);
    }
  }
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkGetSwapchainCounterEXT(VkDevice device, VkSwapchainKHR swapchain,
                                                             VkSurfaceCounterFlagBitsEXT counter,
                                                             uint64_t* pCounterValue) {
  static auto next = (PFN_vkGetSwapchainCounterEXT)Next(device, "vkGetSwapchainCounterEXT");
  VkResult r = next(device, swapchain, counter, pCounterValue);
  Trace("swapchain_counter", (uint64_t)swapchain, (r == VK_SUCCESS && pCounterValue) ? *pCounterValue : ~0ull);
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkGetPastPresentationTimingGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount, VkPastPresentationTimingGOOGLE* pTimings) {
  static auto next = (PFN_vkGetPastPresentationTimingGOOGLE)Next(device, "vkGetPastPresentationTimingGOOGLE");
  VkResult r = next(device, swapchain, pCount, pTimings);
  Trace("past_presentation_timing", (uint64_t)swapchain, pCount ? *pCount : 0);
  return r;
}

// Logs whenever vrcompositor misses vsyncs: the gap between two vsyncs it
// actually waited for is more than 1.5 periods. Works with alignment on or
// off, so hitches can be compared between the two.
void NoteVsyncForHitches(int64_t begin, int64_t end) {
  static int64_t last = 0, period = 0;
  static uint64_t count = 0;
  if (end - begin < 50000)  // only waits that blocked give the vsync time
    return;
  if (last) {
    int64_t gap = end - last;
    if (period == 0 || gap < period * 13 / 10)
      period = period ? (period * 15 + gap) / 16 : gap;
    else if (gap > period * 3 / 2 && gap < 2000000000ll) {
      time_t now = time(nullptr);
      char when[16];
      strftime(when, sizeof(when), "%H:%M:%S", localtime(&now));
      fprintf(stderr, "[hitch] %s compositor missed %lld vsync(s): %.1f ms between vsyncs (#%llu)\n", when,
              (long long)(gap / period - 1), gap / 1e6, (unsigned long long)++count);
    }
  }
  last = end;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pInfo,
                                                    uint64_t timeout) {
  static auto next = (PFN_vkWaitSemaphores)Next(device, "vkWaitSemaphores");
  int64_t b = (int64_t)NowNs();
  VkResult r = next(device, pInfo, timeout);
  int64_t e = (int64_t)NowNs();
  if (e - b > 200000)
    Trace("wait_semaphores", pInfo ? pInfo->semaphoreCount : 0, (uint64_t)(e - b));
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkWaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfo* pInfo,
                                                       uint64_t timeout) {
  static auto next = (PFN_vkWaitSemaphoresKHR)Next(device, "vkWaitSemaphoresKHR");
  int64_t b = (int64_t)NowNs();
  VkResult r = next(device, pInfo, timeout);
  int64_t e = (int64_t)NowNs();
  if (e - b > 200000)
    Trace("wait_semaphores", pInfo ? pInfo->semaphoreCount : 0, (uint64_t)(e - b));
  return r;
}

PFN_vkVoidFunction TraceGetDeviceProcAddr(const char* pName) {
  // Vblank alignment needs the vsync fence hooks too.
  if (!TraceEnabled() && !AlignEnabled())
    return nullptr;
  if (strcmp(pName, "vkWaitForPresentKHR") == 0)
    return (PFN_vkVoidFunction)Hook_vkWaitForPresentKHR;
  if (strcmp(pName, "vkRegisterDisplayEventEXT") == 0)
    return (PFN_vkVoidFunction)Hook_vkRegisterDisplayEventEXT;
  if (strcmp(pName, "vkWaitForFences") == 0)
    return (PFN_vkVoidFunction)Hook_vkWaitForFences;
  if (TraceEnabled() && strcmp(pName, "vkWaitSemaphores") == 0)
    return (PFN_vkVoidFunction)Hook_vkWaitSemaphores;
  if (TraceEnabled() && strcmp(pName, "vkWaitSemaphoresKHR") == 0)
    return (PFN_vkVoidFunction)Hook_vkWaitSemaphoresKHR;
  if (strcmp(pName, "vkGetSwapchainCounterEXT") == 0)
    return (PFN_vkVoidFunction)Hook_vkGetSwapchainCounterEXT;
  if (strcmp(pName, "vkGetPastPresentationTimingGOOGLE") == 0)
    return (PFN_vkVoidFunction)Hook_vkGetPastPresentationTimingGOOGLE;
  return nullptr;
}
