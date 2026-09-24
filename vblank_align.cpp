// Vblank alignment for NVIDIA direct mode.
//
// On NVIDIA drivers since 570, a FIFO present to a leased (direct mode)
// display is shown as soon as its rendering completes, not at the next
// vblank. SteamVR hands frames over ~4 ms before vsync and they complete
// ~3 ms before it, so the headset panel switches images partway down the
// scanout: a horizontal line where the lower part of the image lags behind.
// Measured on an RTX 5090 with driver 610.57.04 and a PS VR2 (2026-09-23).
//
// This holds each headset present back so the frame completes inside the
// vertical blanking interval just before the next vsync, where a switch is
// invisible. Vsync times come from the first-pixel-out display event fences
// vrcompositor already waits on. Frames are released a fixed lead before the
// vsync; the driver shows them ~0.4 ms after release, inside the blank.
//
// The lead is fixed on purpose. A loop steering it from measured landings ran
// away: a frame released too close to vsync is held a whole extra frame, which
// the loop read as "release earlier", until frames were late and SteamVR fell
// to ~28 fps. Where frames land is still measured and logged.
//
// Enabled when $HOME/.config/steamvr-linux-fixes/align exists. Its content,
// if any, is the release lead in microseconds before vsync (default 1000).

#include "vblank_align.hpp"
#include "steamvr_linux_fixes.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_alignMutex;
int g_alignEnabled = -1;
int64_t g_targetBeforeVsyncNs = 1000000;

// Vsync (first pixel out) history.
std::vector<int64_t> g_vsyncs;
int64_t g_periodNs = 0;

// How long before the target vsync a frame is released.
int64_t g_leadNs = -1;

VkSwapchainKHR g_hmdSwapchain = VK_NULL_HANDLE;

// Statistics, logged every 600 frames.
std::vector<int64_t> g_landings;
uint64_t g_frames = 0, g_skipped = 0;

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

// Period from recent vsyncs. A wait that missed vsyncs spans several periods,
// so each gap is divided by its multiple of the shortest typical gap.
void UpdatePeriodLocked() {
  if (g_vsyncs.size() < 8)
    return;
  std::vector<int64_t> gaps;
  for (size_t i = 1; i < g_vsyncs.size(); i++)
    gaps.push_back(g_vsyncs[i] - g_vsyncs[i - 1]);
  std::sort(gaps.begin(), gaps.end());
  int64_t base = gaps[gaps.size() / 10];
  if (base < 4000000 || base > 20000000)
    return;
  std::vector<int64_t> folded;
  for (int64_t g : gaps) {
    int64_t n = (g + base / 2) / base;
    if (n >= 1 && n <= 4)
      folded.push_back(g / n);
  }
  std::sort(folded.begin(), folded.end());
  if (!folded.empty())
    g_periodNs = folded[folded.size() / 2];
}

// The first vsync strictly after t, projected from the latest one seen.
int64_t NextVsyncAfterLocked(int64_t t) {
  int64_t last = g_vsyncs.back();
  if (t < last)
    return last;
  int64_t k = (t - last) / g_periodNs + 1;
  return last + k * g_periodNs;
}

void LogStatsLocked() {
  if (g_landings.empty())
    return;
  std::vector<int64_t> l = g_landings;
  std::sort(l.begin(), l.end());
  auto pct = [&](int p) { return l[std::min(l.size() - 1, l.size() * p / 100)] / 1000.0; };
  int visible = 0;
  for (int64_t x : l)
    if (x > 0 || x < -(int64_t)(g_periodNs * 160 / 2200))
      visible++;
  fprintf(stderr,
          "[vblank-align] %llu measured frames, period %.3f ms, lead %.3f ms, skipped %llu; "
          "shown vs vsync (us) p1 %.0f p50 %.0f p99 %.0f; outside blanking %d of %zu\n",
          (unsigned long long)g_frames, g_periodNs / 1e6, g_leadNs / 1e6,
          (unsigned long long)g_skipped, pct(1), pct(50), pct(99),
          visible, l.size());
  g_landings.clear();
}

}  // namespace

bool AlignEnabled() {
  std::lock_guard<std::mutex> lock(g_alignMutex);
  if (g_alignEnabled == -1) {
    const char* home = getenv("HOME");
    std::ifstream f(std::string(home ? home : "") + "/.config/steamvr-linux-fixes/align");
    g_alignEnabled = f.good() ? 1 : 0;
    long us = 0;
    if (g_alignEnabled && (f >> us) && us > 0 && us < 5000)
      g_targetBeforeVsyncNs = us * 1000;
    if (g_alignEnabled)
      fprintf(stderr, "[vblank-align] enabled, releasing frames %lld us before vsync\n",
              (long long)(g_targetBeforeVsyncNs / 1000));
  }
  return g_alignEnabled == 1;
}

void AlignSetHmdSwapchain(VkSwapchainKHR swapchain) {
  std::lock_guard<std::mutex> lock(g_alignMutex);
  g_hmdSwapchain = swapchain;
  g_vsyncs.clear();
  g_periodNs = 0;
  fprintf(stderr, "[vblank-align] headset swapchain %p\n", (void*)swapchain);
}

bool AlignIsHmdSwapchain(VkSwapchainKHR swapchain) {
  std::lock_guard<std::mutex> lock(g_alignMutex);
  return swapchain != VK_NULL_HANDLE && swapchain == g_hmdSwapchain;
}

void AlignOnVsync(int64_t waitBeginNs, int64_t waitEndNs) {
  // Only waits that actually blocked give the vsync time; a wait that began
  // after the fence signalled returns late.
  if (waitEndNs - waitBeginNs < 50000)
    return;
  std::lock_guard<std::mutex> lock(g_alignMutex);
  g_vsyncs.push_back(waitEndNs);
  if (g_vsyncs.size() > 64)
    g_vsyncs.erase(g_vsyncs.begin());
  UpdatePeriodLocked();
}

int64_t AlignComputeRelease(int64_t now) {
  std::lock_guard<std::mutex> lock(g_alignMutex);
  if (g_periodNs <= 0 || g_vsyncs.empty())
    return 0;
  if (g_leadNs < 0)
    g_leadNs = g_targetBeforeVsyncNs;
  int64_t vsync = NextVsyncAfterLocked(now + g_leadNs);
  int64_t release = vsync - g_leadNs;
  if (release - now > g_periodNs) {
    g_skipped++;
    return 0;
  }
  return release;
}

void AlignAfterPresent(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, int64_t presentedAtNs) {
  PFN_vkWaitForPresentKHR wait = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    auto it = g_deviceDispatch.find(device);
    if (it != g_deviceDispatch.end())
      wait = it->second.WaitForPresentKHR;
  }
  if (!wait || presentedAtNs == 0)
    return;
  int64_t start = NowNs();
  if (wait(device, swapchain, presentId, 30000000ull) != VK_SUCCESS)
    return;
  int64_t shown = NowNs();

  std::lock_guard<std::mutex> lock(g_alignMutex);
  if (g_periodNs <= 0 || g_vsyncs.empty() || g_leadNs < 0)
    return;
  // When the wait did not block, the frame was already shown before it began.
  int64_t shownAt = (shown - start > 20000) ? shown : start;
  int64_t vsync = NextVsyncAfterLocked(shownAt - g_periodNs / 2);
  int64_t landing = shownAt - vsync;  // negative: before vsync
  g_landings.push_back(landing);
  if (++g_frames % 75 == 0)  // 600 frames at one measured in eight
    LogStatsLocked();
}
