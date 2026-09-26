# SteamVR Linux Fixes

## About this fork

This fork adds one feature to SteamVR Linux Fixes: **vblank alignment for NVIDIA direct mode**. It was offered upstream as [PR #3](https://github.com/BnuuySolutions/SteamVRLinuxFixes/pull/3) and declined as out of scope for a layer meant to stay small and temporary, so it lives here.

**The problem.** On NVIDIA's proprietary driver, a FIFO present to a leased (direct mode) headset display is shown as soon as its rendering finishes instead of at the next vblank. SteamVR hands frames over a few milliseconds before vsync, so the panel switches images partway through its scanout. In the headset this is a horizontal line where the lower part of the image lags behind the upper part while you turn your head. The upstream maintainer's explanation: when any connected display is VRR-capable, the NVIDIA driver treats every display surface like a VRR one and presents immediately. The same line shows up in the NVIDIA forum thread "Tearing in VR since 570.x".

**What this fork does.** It holds each headset frame and presents it just before the next vsync, so the image switch lands in the vertical blank where it can't be seen. SteamVR's own timing is left alone: its present call returns at once and a worker thread does the real present. The README section "Vblank alignment for NVIDIA (opt-in)" below has the details and the measurements.

**Turning it on.** It is off unless you ask for it:

```bash
mkdir -p ~/.config/steamvr-linux-fixes
echo 1200 > ~/.config/steamvr-linux-fixes/align
```

Then restart SteamVR. Delete the file to turn it off.

**Alternatives without this fork.** Hiding VRR from the NVIDIA driver entirely (the `nvidia-modeset` option `conceal_vrr_caps=1`) should avoid the problem too, at the cost of VRR on your desktop monitors. The upstream maintainer has mentioned a possible future layer that patches the driver's behaviour directly.

Tested on an RTX 5090 with driver 610.57.04 and a PS VR2 (Ignition and PSVR2 Toolkit) at 90 and 120 Hz, on KDE Plasma with Wayland.

A Vulkan layer that patches SteamVR's vrcompositor to address issues for wired headsets (Vive, Index, Beyond, PSVR2, etc).

## What this does fix
- Patches vrcompositor to allow all refresh rates to work correctly. For example, PSVR2 could only use 90Hz and not 120Hz
- Uses `VK_KHR_present_wait` to wait on the latest frame that was just rendered. This fixes the view lagging behind
- Enables `VK_PRESENT_MODE_FIFO_LATEST_READY_EXT` if supported (NVIDIA only), which ensures the latest frame is always presented
- Addresses issue with swapchain missing `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, which causes no image to show in HMD on Mesa 26
- Fixes a crash that can happen if SteamVR decides to allocate a 0x0 Vulkan texture. Launching Resonite on AMD was one way to cause this issue 

## What this does NOT fix
- Encoding issues on Steam Link
- ALVR issues
- Frame rate issues
- Any issues with SteamVR that this layer does not promise to fix

## Vblank alignment for NVIDIA (opt-in)

On NVIDIA's proprietary driver (seen on 570 and later, measured on 610.57.04 with an RTX 5090), a FIFO present to a direct-mode (DRM-leased) display is shown as soon as its rendering completes instead of at the next vblank. SteamVR hands frames over about 4 ms before vsync, so the panel switches images partway through its scanout. In the headset this is a horizontal line where the lower part of the image lags behind the upper part while you turn your head. It is the same problem as the "Tearing in VR since 570.x" reports on the NVIDIA forums, and it happens with or without VRR.

The layer can hold each headset frame and present it just before the next vsync, so the switch lands in the vertical blanking interval where it cannot be seen:

- Vsync times come from the first-pixel-out display event fences vrcompositor already waits on.
- `vkQueuePresentKHR` returns immediately, and a worker thread makes the real present at the release time. SteamVR's frame timing is unchanged. (Holding vrcompositor's own thread instead made SteamVR start frames much earlier, which broke the PS VR2 controllers' LED sync at 120 Hz.)
- Every queue and swapchain call vrcompositor makes goes through one mutex, so the worker never touches them concurrently.
- The release lead is fixed. Steering it from measured landings is unstable: a frame released too close to vsync is held a whole extra frame.

Enable it by creating the file below, containing the lead in microseconds (1200 works at 90 and 120 Hz on a PS VR2), then restart SteamVR:

```bash
mkdir -p ~/.config/steamvr-linux-fixes
echo 1200 > ~/.config/steamvr-linux-fixes/align
```

Delete the file to turn it off. The layer logs statistics to `vrcompositor-linux.txt` every 600 frames: where frames landed against vsync, how long they were held, and how many landed outside the blanking interval.

For diagnosis, creating `~/.config/steamvr-linux-fixes/trace` records present, present-wait and vsync timings to `~/.cache/steamvr-linux-fixes/vrcompositor-timing.csv`.

## How to use

If you are on Arch, you can just install [steamvr-linux-fixes-layer-bin](https://aur.archlinux.org/packages/steamvr-linux-fixes-layer-bin) from AUR.

If you are using the pre-built release binaries, download and extract the release archive, then run `sudo ./install.sh` to install it (or `sudo ./install.sh --uninstall` to remove it).

If you are building from source, you can install it system-wide using CMake as an implicit Vulkan layer (see below).

## Building from source and installing

1. Make sure you have Vulkan headers and CMake.
2. Clone this repository recursively with `git clone --recursive https://github.com/BnuuySolutions/SteamVRLinuxFixes/`
3. Go into the repository directory, build, and install with CMake:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
   cmake --build build
   sudo cmake --install build
   ```
4. Launch SteamVR and confirm the layer is logging stuff by looking at `~/.steam/steam/logs/vrcompositor-linux.txt` or `~/.steam/steam/logs/vrstartup-linux.txt`. SteamVR should now be at least slightly better than how it was before.
