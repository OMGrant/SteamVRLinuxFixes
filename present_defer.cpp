// Deferred headset presents for vblank alignment.
//
// The first version of vblank alignment held vrcompositor's own thread inside
// vkQueuePresentKHR until the release time. SteamVR reacted by starting every
// frame ~10 ms before vsync instead of ~4 ms, which lengthened pose prediction
// and upset the PS VR2 driver's controller LED sync at 120 Hz (calibration
// failed 34 times instead of once, and the controllers visibly glitched).
//
// Here vkQueuePresentKHR returns at once, so SteamVR keeps its normal timing,
// and a worker thread makes the real present at the release time. Because the
// queue and the swapchain are now used from two threads, every call SteamVR
// makes on them goes through one mutex, and a wait for a present that is still
// held returns at once, as it would have with the driver's immediate flip.

#include "present_defer.hpp"
#include "steamvr_linux_fixes.hpp"
#include "timing_trace.hpp"
#include "vblank_align.hpp"

#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Serialises SteamVR's queue and swapchain calls with the worker's presents.
std::mutex g_queueMutex;

struct Pending {
  VkDevice device;
  VkQueue queue;
  PFN_vkQueuePresentKHR next;
  std::vector<VkSemaphore> waits;
  VkSwapchainKHR swapchain;
  uint32_t imageIndex;
  uint64_t presentId;
  int64_t release;
};

std::mutex g_pendingMutex;
std::condition_variable g_pendingCv;
std::deque<Pending> g_pending;
bool g_workerStarted = false;

// Statistics, logged every 600 deferred presents.
uint64_t g_deferred = 0, g_lateNsTotal = 0, g_heldNsTotal = 0, g_acquireWaits = 0, g_acquireWaitNsTotal = 0;
uint64_t g_waitsAnswered = 0, g_errors = 0;

int64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

void SleepUntil(int64_t ns) {
  timespec ts;
  ts.tv_sec = ns / 1000000000ll;
  ts.tv_nsec = ns % 1000000000ll;
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) != 0) {
  }
}

PFN_vkVoidFunction NextForDevice(VkDevice device, const char* name) {
  std::lock_guard<std::mutex> lock(g_mapMutex);
  auto it = g_deviceDispatch.find(device);
  if (it == g_deviceDispatch.end() || !it->second.GetDeviceProcAddr)
    return nullptr;
  return it->second.GetDeviceProcAddr(device, name);
}

// The device a queue belongs to. A queue the layer did not see handed out
// (vkGetDeviceQueue2) falls back to the only device vrcompositor creates.
VkDevice DeviceForQueue(VkQueue queue) {
  std::lock_guard<std::mutex> lock(g_mapMutex);
  auto it = g_queueToDevice.find(queue);
  if (it != g_queueToDevice.end())
    return it->second;
  return g_deviceDispatch.empty() ? VK_NULL_HANDLE : g_deviceDispatch.begin()->first;
}

bool IsPendingLocked(VkSwapchainKHR swapchain, uint64_t presentId) {
  for (const auto& p : g_pending)
    if (p.swapchain == swapchain && p.presentId == presentId)
      return true;
  return false;
}

// True when a held present still waits on this semaphore. SteamVR must not
// signal it again until the worker has consumed it.
bool PendingWaitsOn(VkSemaphore semaphore) {
  std::lock_guard<std::mutex> lock(g_pendingMutex);
  for (const auto& p : g_pending)
    for (VkSemaphore w : p.waits)
      if (w == semaphore)
        return true;
  return false;
}

uint64_t g_submitWaits = 0;

// Private semaphores that held presents wait on instead of SteamVR's. SteamVR
// re-signals its own render-finished semaphore every frame, so a present that
// kept waiting on it would force SteamVR to wait too. A slot is reused only
// after the present that used it was shown (the worker waits for that), and
// with 8 slots that is ~65 ms back at 120 Hz.
constexpr size_t k_handoffSlots = 8;
std::vector<VkSemaphore> g_handoff;
size_t g_handoffNext = 0;

// Consumes SteamVR's wait semaphores now with an empty submit that signals a
// private semaphore, and returns that semaphore. Called with g_queueMutex held.
VkSemaphore HandOffWaitsLocked(VkDevice device, VkQueue queue, const VkSemaphore* waits, uint32_t count) {
  static auto createSemaphore = (PFN_vkCreateSemaphore)NextForDevice(device, "vkCreateSemaphore");
  static auto submit = (PFN_vkQueueSubmit)NextForDevice(device, "vkQueueSubmit");
  if (!createSemaphore || !submit)
    return VK_NULL_HANDLE;
  if (g_handoff.empty()) {
    VkSemaphoreCreateInfo info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (size_t i = 0; i < k_handoffSlots; i++) {
      VkSemaphore sem = VK_NULL_HANDLE;
      if (createSemaphore(device, &info, nullptr, &sem) != VK_SUCCESS)
        return VK_NULL_HANDLE;
      g_handoff.push_back(sem);
    }
  }
  VkSemaphore sem = g_handoff[g_handoffNext];
  g_handoffNext = (g_handoffNext + 1) % k_handoffSlots;
  std::vector<VkPipelineStageFlags> stages(count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = count;
  si.pWaitSemaphores = waits;
  si.pWaitDstStageMask = stages.data();
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &sem;
  if (submit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  return sem;
}

// Blocks until no headset present is held.
void DrainPending(bool countAsAcquire) {
  std::unique_lock<std::mutex> lock(g_pendingMutex);
  if (g_pending.empty())
    return;
  int64_t start = NowNs();
  g_pendingCv.wait(lock, [] { return g_pending.empty(); });
  if (countAsAcquire) {
    g_acquireWaits++;
    g_acquireWaitNsTotal += NowNs() - start;
  }
}

void Worker() {
  for (;;) {
    Pending p;
    {
      std::unique_lock<std::mutex> lock(g_pendingMutex);
      g_pendingCv.wait(lock, [] { return !g_pending.empty(); });
      p = g_pending.front();
    }
    if (p.release > NowNs())
      SleepUntil(p.release);

    VkResult r;
    {
      std::lock_guard<std::mutex> q(g_queueMutex);
      VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
      pid.swapchainCount = 1;
      pid.pPresentIds = &p.presentId;
      VkPresentInfoKHR info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      info.pNext = &pid;
      info.waitSemaphoreCount = (uint32_t)p.waits.size();
      info.pWaitSemaphores = p.waits.data();
      info.swapchainCount = 1;
      info.pSwapchains = &p.swapchain;
      info.pImageIndices = &p.imageIndex;
      int64_t at = NowNs();
      r = p.next(p.queue, &info);
      Trace("deferred_present", (uint64_t)p.swapchain, p.presentId);
      // Measure where the frame landed against vsync, for the log. It waits
      // for the frame to be shown while holding the queue lock, so only one
      // frame in eight is measured to keep SteamVR's submits from waiting.
      static uint64_t measured = 0;
      if (r == VK_SUCCESS && (measured++ % 8) == 0)
        AlignAfterPresent(p.device, p.swapchain, p.presentId, at);
      std::lock_guard<std::mutex> lock(g_pendingMutex);
      g_lateNsTotal += at > p.release ? at - p.release : 0;
    }

    std::lock_guard<std::mutex> lock(g_pendingMutex);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
      g_errors++;
      fprintf(stderr, "[vblank-align] deferred present returned %d\n", r);
    }
    g_pending.pop_front();
    if (++g_deferred % 600 == 0) {
      fprintf(stderr,
              "[vblank-align] deferred %llu presents: held avg %.3f ms, released late avg %.3f ms, "
              "acquire waited %llu times (avg %.3f ms), submit waited %llu, waits answered early %llu, errors %llu\n",
              (unsigned long long)g_deferred, g_heldNsTotal / 1e6 / 600, g_lateNsTotal / 1e6 / 600,
              (unsigned long long)g_acquireWaits, g_acquireWaits ? g_acquireWaitNsTotal / 1e6 / g_acquireWaits : 0.0,
              (unsigned long long)g_submitWaits, (unsigned long long)g_waitsAnswered, (unsigned long long)g_errors);
      g_heldNsTotal = g_lateNsTotal = g_acquireWaits = g_acquireWaitNsTotal = g_waitsAnswered = g_submitWaits = 0;
    }
    g_pendingCv.notify_all();
  }
}

// ---- Hooks that serialise SteamVR's queue and swapchain use ----

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* pSubmits,
                                                  VkFence fence) {
  static auto next = (PFN_vkQueueSubmit)NextForDevice(DeviceForQueue(queue), "vkQueueSubmit");
  for (uint32_t i = 0; i < count; i++)
    for (uint32_t j = 0; j < pSubmits[i].signalSemaphoreCount; j++)
      if (PendingWaitsOn(pSubmits[i].pSignalSemaphores[j])) {
        g_submitWaits++;
        DrainPending(false);
      }
  std::lock_guard<std::mutex> q(g_queueMutex);
  Trace("queue_submit", count, 0);
  return next(queue, count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits,
                                                   VkFence fence) {
  static auto next = (PFN_vkQueueSubmit2)NextForDevice(DeviceForQueue(queue), "vkQueueSubmit2");
  for (uint32_t i = 0; i < count; i++)
    for (uint32_t j = 0; j < pSubmits[i].signalSemaphoreInfoCount; j++)
      if (PendingWaitsOn(pSubmits[i].pSignalSemaphoreInfos[j].semaphore)) {
        g_submitWaits++;
        DrainPending(false);
      }
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(queue, count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits,
                                                      VkFence fence) {
  static auto next = (PFN_vkQueueSubmit2KHR)NextForDevice(DeviceForQueue(queue), "vkQueueSubmit2KHR");
  for (uint32_t i = 0; i < count; i++)
    for (uint32_t j = 0; j < pSubmits[i].signalSemaphoreInfoCount; j++)
      if (PendingWaitsOn(pSubmits[i].pSignalSemaphoreInfos[j].semaphore)) {
        g_submitWaits++;
        DrainPending(false);
      }
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(queue, count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* pInfo,
                                                      VkFence fence) {
  static auto next = (PFN_vkQueueBindSparse)NextForDevice(DeviceForQueue(queue), "vkQueueBindSparse");
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(queue, count, pInfo, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueueWaitIdle(VkQueue queue) {
  static auto next = (PFN_vkQueueWaitIdle)NextForDevice(DeviceForQueue(queue), "vkQueueWaitIdle");
  DrainPending(false);
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkDeviceWaitIdle(VkDevice device) {
  static auto next = (PFN_vkDeviceWaitIdle)NextForDevice(device, "vkDeviceWaitIdle");
  DrainPending(false);
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(device);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                          VkSemaphore semaphore, VkFence fence, uint32_t* pIndex) {
  static auto next = (PFN_vkAcquireNextImageKHR)NextForDevice(device, "vkAcquireNextImageKHR");
  // Take a free image without waiting; the swapchain almost always has one.
  // Only when none is free can the held present be what the acquire needs,
  // and then it is released first (never while holding the queue lock, or
  // the worker could not present the image the acquire is waiting for).
  if (AlignIsHmdSwapchain(swapchain)) {
    {
      std::lock_guard<std::mutex> q(g_queueMutex);
      VkResult r = next(device, swapchain, 0, semaphore, fence, pIndex);
      if ((r != VK_NOT_READY && r != VK_TIMEOUT) || timeout == 0)
        return r;
    }
    DrainPending(true);
  }
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(device, swapchain, timeout, semaphore, fence, pIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pInfo,
                                                           uint32_t* pIndex) {
  static auto next = (PFN_vkAcquireNextImage2KHR)NextForDevice(device, "vkAcquireNextImage2KHR");
  if (pInfo && AlignIsHmdSwapchain(pInfo->swapchain)) {
    VkAcquireNextImageInfoKHR now = *pInfo;
    now.timeout = 0;
    {
      std::lock_guard<std::mutex> q(g_queueMutex);
      VkResult r = next(device, &now, pIndex);
      if ((r != VK_NOT_READY && r != VK_TIMEOUT) || pInfo->timeout == 0)
        return r;
    }
    DrainPending(true);
  }
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(device, pInfo, pIndex);
}

VKAPI_ATTR void VKAPI_CALL Hook_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                      const VkAllocationCallbacks* pAllocator) {
  static auto next = (PFN_vkDestroySwapchainKHR)NextForDevice(device, "vkDestroySwapchainKHR");
  DrainPending(false);
  std::lock_guard<std::mutex> q(g_queueMutex);
  next(device, swapchain, pAllocator);
}

// A wait for a present the worker still holds returns at once: with the
// driver's immediate flip it would already have been shown by now, and
// SteamVR only uses the answer to pace its next frame.
VKAPI_ATTR VkResult VKAPI_CALL Hook_vkWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                        uint64_t presentId, uint64_t timeout) {
  static auto next = (PFN_vkWaitForPresentKHR)NextForDevice(device, "vkWaitForPresentKHR");
  Trace("wait_present_begin", (uint64_t)swapchain, presentId);
  {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    if (IsPendingLocked(swapchain, presentId)) {
      g_waitsAnswered++;
      Trace("wait_present_end", (uint64_t)swapchain, presentId & 0xffffffffffffull);
      return VK_SUCCESS;
    }
  }
  VkResult r;
  {
    std::lock_guard<std::mutex> q(g_queueMutex);
    r = next(device, swapchain, presentId, timeout);
  }
  Trace("wait_present_end", (uint64_t)swapchain, ((uint64_t)(uint32_t)r << 48) | (presentId & 0xffffffffffffull));
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkWaitForPresent2KHR(VkDevice device, VkSwapchainKHR swapchain,
                                                         const VkPresentWait2InfoKHR* pInfo) {
  static auto next = (PFN_vkWaitForPresent2KHR)NextForDevice(device, "vkWaitForPresent2KHR");
  if (pInfo) {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    if (IsPendingLocked(swapchain, pInfo->presentId)) {
      g_waitsAnswered++;
      return VK_SUCCESS;
    }
  }
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(device, swapchain, pInfo);
}

}  // namespace

bool DeferEnabled() {
  return AlignEnabled();
}

VkResult PresentLocked(PFN_vkQueuePresentKHR next, VkQueue queue, const VkPresentInfoKHR* pInfo) {
  if (!DeferEnabled())
    return next(queue, pInfo);
  // Only a present to the headset swapchain must wait for a held one, to keep
  // its frames in order; the mirror window's present only needs the lock.
  for (uint32_t i = 0; i < pInfo->swapchainCount; i++)
    if (AlignIsHmdSwapchain(pInfo->pSwapchains[i])) {
      DrainPending(false);
      break;
    }
  std::lock_guard<std::mutex> q(g_queueMutex);
  return next(queue, pInfo);
}

bool DeferPresent(PFN_vkQueuePresentKHR next, VkQueue queue, const VkPresentInfoKHR* pInfo, uint64_t presentId) {
  if (pInfo->swapchainCount != 1)
    return false;
  int64_t now = NowNs();
  int64_t release = AlignComputeRelease(now);
  if (release == 0)
    return false;

  Pending p;
  p.device = DeviceForQueue(queue);
  p.queue = queue;
  p.next = next;
  if (pInfo->waitSemaphoreCount > 0) {
    std::lock_guard<std::mutex> q(g_queueMutex);
    VkSemaphore handoff = HandOffWaitsLocked(p.device, queue, pInfo->pWaitSemaphores, pInfo->waitSemaphoreCount);
    if (handoff == VK_NULL_HANDLE)
      return false;
    p.waits.push_back(handoff);
  }
  p.swapchain = pInfo->pSwapchains[0];
  p.imageIndex = pInfo->pImageIndices[0];
  p.presentId = presentId;
  p.release = release;

  std::lock_guard<std::mutex> lock(g_pendingMutex);
  if (!g_workerStarted) {
    std::thread(Worker).detach();
    g_workerStarted = true;
    fprintf(stderr, "[vblank-align] deferred presents: SteamVR's present call returns at once\n");
  }
  g_heldNsTotal += release > now ? release - now : 0;
  g_pending.push_back(std::move(p));
  if (pInfo->pResults)
    pInfo->pResults[0] = VK_SUCCESS;
  g_pendingCv.notify_all();
  return true;
}

PFN_vkVoidFunction DeferGetDeviceProcAddr(const char* pName) {
  if (!DeferEnabled())
    return nullptr;
  struct Entry {
    const char* name;
    PFN_vkVoidFunction fn;
  };
  static const Entry entries[] = {
      {"vkQueueSubmit", (PFN_vkVoidFunction)Hook_vkQueueSubmit},
      {"vkQueueSubmit2", (PFN_vkVoidFunction)Hook_vkQueueSubmit2},
      {"vkQueueSubmit2KHR", (PFN_vkVoidFunction)Hook_vkQueueSubmit2KHR},
      {"vkQueueBindSparse", (PFN_vkVoidFunction)Hook_vkQueueBindSparse},
      {"vkQueueWaitIdle", (PFN_vkVoidFunction)Hook_vkQueueWaitIdle},
      {"vkDeviceWaitIdle", (PFN_vkVoidFunction)Hook_vkDeviceWaitIdle},
      {"vkAcquireNextImageKHR", (PFN_vkVoidFunction)Hook_vkAcquireNextImageKHR},
      {"vkAcquireNextImage2KHR", (PFN_vkVoidFunction)Hook_vkAcquireNextImage2KHR},
      {"vkDestroySwapchainKHR", (PFN_vkVoidFunction)Hook_vkDestroySwapchainKHR},
      {"vkWaitForPresentKHR", (PFN_vkVoidFunction)Hook_vkWaitForPresentKHR},
      {"vkWaitForPresent2KHR", (PFN_vkVoidFunction)Hook_vkWaitForPresent2KHR},
  };
  for (const auto& e : entries)
    if (strcmp(pName, e.name) == 0)
      return e.fn;
  return nullptr;
}
