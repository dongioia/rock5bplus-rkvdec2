---
title: Cinnamon Wayland with GPU Acceleration
description: Switching Cinnamon from X11 to Wayland with hardware-accelerated rendering on RK3588 boards
published: false
date: 2026-03-07T15:00:00.000Z
tags: cinnamon, wayland, gpu, panthor, rk3588
editor: markdown
dateCreated: 2026-03-07T15:00:00.000Z
---

# 1. Introduction

Cinnamon supports Wayland sessions starting from version `6.4`. On RK3588 boards, switching from X11 to Wayland requires extra configuration because the SoC exposes two separate DRM devices: one for display output and one for GPU rendering. Without proper setup, Cinnamon's compositor (`Muffin`) falls back to `llvmpipe` software rendering, resulting in poor desktop performance.

This guide walks you through enabling a fully GPU-accelerated Cinnamon Wayland session on BredOS.

> **Known limitation (Muffin 6.6.x):** Muffin does not yet support the `mutter-device-preferred-primary` udev tag from upstream Mutter. The compositor itself uses the GPU correctly for desktop compositing via GBM, but Wayland EGL clients (GTK OpenGL apps) may still fall back to `llvmpipe`. Vulkan applications (mpv, browsers, vkmark) are not affected. This has been confirmed on Rock 5B+ and Orange Pi 5 Plus. See [section 7.8](#h-78-wayland-egl-clients-use-llvmpipe-known-muffin-limitation) for details and workarounds. Other Wayland compositors (GNOME, KDE) handle multi-GPU correctly on the same hardware.
{.is-warning}

# 2. Prerequisites

## 2.1 Required Packages

- Verify that all required packages are installed:

```
sudo pacman -S --needed cinnamon muffin wayland xorg-xwayland libinput pipewire mesa libdrm
```

## 2.2 Kernel and GPU Driver

This guide assumes you already have the Panthor GPU driver enabled. If you are still using Panfork (the default on BredOS for RK35xx), follow the [Setup Panthor on Mali GPUs with RK3588](/en/how-to/how-to-setup-panthor) guide first, then return here.

- Verify that the `panthor` module is loaded:

```
lsmod | grep panthor
```

You should see `panthor` in the output. If not, load it manually:

- Load the module:

```
sudo modprobe panthor
```

> Panthor requires a BredOS BSP kernel 6.1-rkr3 or mainline kernel `6.12` or later. If the module is not available, update your kernel.
{.is-warning}

## 2.3 User Permissions

Your user must belong to the `render` and `video` groups to access the GPU render node. Without these permissions, applications silently fall back to software rendering.

- Check your current groups:

```
groups
```

If `render` or `video` are missing from the output, add your user to both groups:

```
sudo usermod -aG render,video $USER
```

**Log out and log back in for the group change to take effect.**

- Verify that the GPU render node exists and is accessible:

```
ls -la /dev/dri/
```

You should see at least `card0`, `card1`, and `renderD128`. The `renderD128` device should be owned by group `render` with permission `crw-rw----`.

> If `/dev/dri/renderD128` is missing entirely, the Panthor driver failed to initialize. Check `dmesg | grep -i panthor` for errors.
{.is-warning}

# 3. Understanding the Dual-GPU Setup

On RK3588 boards, the kernel exposes two DRI card devices. This is the root cause of most GPU acceleration issues with Cinnamon Wayland.

- The following table shows the role of each device:

| Device | Driver | Role |
|--------|--------|------|
| `/dev/dri/card0` | `rockchip-drm` | Display controller (HDMI/DP output). No 3D rendering. |
| `/dev/dri/card1` | `panthor` | GPU (Mali-G610). Handles all 3D rendering. |
| `/dev/dri/renderD128` | `panthor` | GPU render node (used by applications for 3D). |
{.dense}

The problem: `Muffin` (Cinnamon's compositor, a fork of GNOME's Mutter) tries to use `card0` for rendering by default. Since `rockchip-drm` has no 3D capabilities, it falls back to `llvmpipe` (CPU-based software rendering).

The solution: tell Muffin to use the `panthor` render node for GPU acceleration while keeping `rockchip-drm` for display output.

- Verify your device layout:

```
ls -l /dev/dri/
```

- Check which driver is behind each card:

```
udevadm info -q property -n /dev/dri/card0 | grep DRIVER
udevadm info -q property -n /dev/dri/card1 | grep DRIVER
```

# 4. Configure GPU Selection

## 4.1 Create a Udev Rule

The most reliable method is a `udev` rule that tells Muffin which device to prefer for rendering.

- Create the udev rule file:

```
sudo nano /etc/udev/rules.d/61-mutter-panthor.rules
```

- Add the following content:

```
# RK3588: mark Panthor render node as preferred GPU for Mutter/Muffin
SUBSYSTEM=="drm", KERNEL=="card1", DRIVERS=="panthor", TAG+="mutter-device-preferred-primary"
```

- Reload udev rules:

```
sudo udevadm control --reload
sudo udevadm trigger
```

- Verify that the tag was applied:

```
udevadm info -q all -n /dev/dri/card1 | grep mutter
```

You should see `mutter-device-preferred-primary` in the output.

## 4.2 Set Environment Variables

In addition to the udev rule, set environment variables that help Muffin and Mesa select the correct GPU.

- Create the environment configuration file:

```
sudo nano /etc/environment.d/90-rk3588-gpu.conf
```

- Add the following content:

```
# RK3588 GPU selection for Mutter/Muffin Wayland
MUTTER_ALLOW_HYBRID_GPUS=1

# Enable OpenGL 3.3 (Panfrost defaults to 3.1, some apps require 3.3+)
PAN_MESA_DEBUG=gl3
```

The `PAN_MESA_DEBUG=gl3` variable enables experimental OpenGL 3.3 support on the Mali-G610 GPU. Without it, Panfrost exposes only OpenGL 3.1, which prevents applications like `kitty` from launching. If a specific application shows rendering artifacts, launch it without the flag:

```
PAN_MESA_DEBUG= application-name
```

> Do not set `WLR_DRM_DEVICES` - that variable is for wlroots-based compositors (Sway, Hyprland), not for Muffin. Setting it has no effect on Cinnamon.
{.is-warning}

## 4.3 Remove Incorrect Configuration

If you previously created a `/etc/environment.d/gpu-wayland.conf` with variables like `WLR_DRM_DEVICES`, `MESA_LOADER_DRIVER_OVERRIDE`, or `MUTTER_DEBUG_FORCE_EGL_STREAM`, remove or rename it. These variables do not apply to Muffin and may cause unexpected behavior.

- Check for stale configuration:

```
ls /etc/environment.d/
```

- Remove any incorrect files:

```
sudo rm /etc/environment.d/gpu-wayland.conf
```

# 5. Select the Wayland Session

## 5.1 From the Login Screen

Most display managers (LightDM, GDM, SDDM) allow choosing the session type from the login screen.

- Look for a gear icon or session selector on the login screen
- Select `Cinnamon (Wayland)` instead of `Cinnamon`
- Log in normally

## 5.2 Verify the Session Type

After logging in:

- Confirm you are running a Wayland session:

```
echo $XDG_SESSION_TYPE
```

The output should be `wayland`.

# 6. Configure mpv for Hardware Video Playback

On RK3588, `mpv` can use V4L2 Request API for hardware video decoding and Vulkan for rendering output. This bypasses the Wayland EGL path entirely, ensuring smooth playback regardless of the Muffin limitation described in the introduction.

- Install mpv and yt-dlp:

```
sudo pacman -S --needed mpv yt-dlp
```

- Create or edit the mpv configuration file:

```
nano ~/.config/mpv/mpv.conf
```

- Add the following content:

```
hwdec=v4l2request
vo=gpu-next,gpu
gpu-api=vulkan
gpu-context=waylandvk
ytdl-format=bestvideo[height<=?1080]+bestaudio
```

| Option | Purpose |
|--------|---------|
| `hwdec=v4l2request` | Hardware video decoding via V4L2 stateless API (zero-copy DMA-BUF) |
| `vo=gpu-next,gpu` | Video output via libplacebo (falls back to gpu if needed) |
| `gpu-api=vulkan` | Use PanVK Vulkan driver (works correctly, bypasses EGL) |
| `gpu-context=waylandvk` | Vulkan WSI for Wayland (native, no XWayland) |
| `ytdl-format=...` | Limit YouTube to 1080p to avoid overloading the hardware decoder |
{.dense}

- Test playback:

```
mpv --fs https://www.youtube.com/watch?v=LXb3EKWsInQ
```

You should see `Using hardware decoding (v4l2request)` and `VO: [gpu-next]` in the output. The video should play smoothly without dropped frames.

> Without the Vulkan configuration, mpv defaults to OpenGL via the Wayland EGL path, which falls back to `llvmpipe` software rendering on Cinnamon. This causes severe frame drops (hundreds of dropped frames per minute). Always use the Vulkan configuration above.
{.is-warning}

# 7. Verify GPU Acceleration

## 7.1 Check the Renderer

On Wayland, you must use `eglinfo` to check GPU acceleration. The `glxinfo` command goes through XWayland and will show `llvmpipe` even when the compositor is GPU-accelerated (see [section 8.1](#h-81-glxinfo-still-shows-llvmpipe)).

- Install `eglinfo` if not present:

```
sudo pacman -S --needed mesa-utils
```

- Check the GBM and Device renderers:

```
eglinfo -B 2>/dev/null | grep -A5 "GBM platform"
```

```
eglinfo -B 2>/dev/null | grep -A10 "Device #0"
```

You should see `Mali-G610 MC4 (Panfrost)` in both outputs, not `llvmpipe`. The renderer name `Panfrost` is correct even when using the `panthor` kernel driver — Mesa's OpenGL driver is named Panfrost for all Mali Valhall GPUs.

- Check the Wayland platform:

```
eglinfo -B 2>/dev/null | grep -A10 "Wayland platform"
```

> Due to the Muffin limitation described in the introduction, the Wayland platform may show `llvmpipe` even when the compositor is using the GPU. This is a known issue. Verify GPU acceleration using the GBM and Device platform checks above instead.
{.is-info}

- Check Vulkan (should always work):

```
vulkaninfo --summary 2>/dev/null | grep -A3 "GPU"
```

You should see `Mali-G610 MC4` with the `panvk` driver.

## 7.2 Check Compositing Status

- Open Cinnamon's System Settings, navigate to `General` and check that `Compositing` is enabled
- Alternatively, check from terminal:

```
dconf read /org/cinnamon/muffin/compositing-manager
```

The output should be `true`.

# 8. Troubleshooting

If you encounter issues, start by generating a full system report. This collects all hardware and software information in one shot and makes it easy to get help on the [BredOS Discord](https://discord.gg/beSUnWGVH2):

```
sudo sys-report
```

This uploads the report to `termbin.com` and prints a URL you can share. To save locally instead:

```
sudo sys-report -l
```

## 8.1 glxinfo Still Shows llvmpipe

This is expected on Wayland. The `glxinfo` command uses the GLX protocol which goes through XWayland. Even with a fully GPU-accelerated Wayland session, `glxinfo` may report `llvmpipe` because XWayland may not have access to the GPU render node.

- Use `eglinfo` instead to verify the Wayland renderer (see [section 7.1](#h-71-check-the-renderer) and [section 8.8](#h-88-wayland-egl-clients-use-llvmpipe-known-muffin-limitation))
- To fix `glxinfo` specifically for X11 applications running under XWayland, try:

```
DRI_PRIME=1 glxinfo -B
```

## 8.2 Panthor Loaded but No GPU Rendering

If `lsmod | grep panthor` shows the module is loaded but applications still use `llvmpipe`, work through these checks in order:

**Check 1: Render node exists**

```
ls -la /dev/dri/
```

- If `renderD128` is missing, Panthor failed to initialize. Check the kernel log:

```
dmesg | grep -i panthor
```

Common causes:
- Missing firmware: check `dmesg | grep -i firmware | grep -i mali`. The `mali-G610-firmware` package must be installed and the firmware files must be in `/lib/firmware/arm/mali/arch10.8/`.
- Device tree overlay not enabled: follow the [Setup Panthor](/en/how-to/how-to-setup-panthor) guide to enable the `rockchip-rk3588-panthor-gpu` DTBO if you are on kernel 6.1-rkr3.

**Check 2: User permissions**

```
ls -la /dev/dri/renderD128
groups
```

If `renderD128` exists but your user is not in the `render` group, see [section 2.3](#h-23-user-permissions).

**Check 3: Mesa detects the GPU**

```
eglinfo -B 2>/dev/null | grep -A5 "Device platform"
```

- If the output shows empty devices or only `llvmpipe`, Mesa is not finding the Panthor driver. Verify that you are using the standard `mesa` package (not `mesa-panfork-git`):

```
pacman -Q mesa
```

- If it shows `mesa-panfork-git`, replace it:

```
sudo pacman -S mesa
```

**Check 4: No conflicting libMali**

```
pacman -Q | grep -i mali
```

- You should see `mali-G610-firmware` only. If any `libmali-valhall-g610` package is installed, it conflicts with Mesa's open source driver:

```
sudo pacman -R libmali-valhall-g610
```

**Check 5: Environment variable overrides**

- Stale or incorrect environment variables can force Mesa to use the wrong driver:

```
env | grep -iE "mesa|gallium|dri|gpu|libgl|egl"
```

If any of `MESA_LOADER_DRIVER_OVERRIDE`, `LIBGL_ALWAYS_SOFTWARE`, `GALLIUM_DRIVER`, or `__GLX_VENDOR_LIBRARY_NAME` are set, unset them or remove them from your environment configuration files.

## 8.3 Vulkan Errors (VK_ERROR_INCOMPATIBLE_DRIVER)

If you see errors like `ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)` when running graphical applications:

- This means Mesa is trying to use the Zink driver (OpenGL-over-Vulkan) but no Vulkan driver is available. Install the Vulkan packages:

```
sudo pacman -S --needed vulkan-icd-loader vulkan-panfrost
```

- Verify that PanVK is detected:

```
vulkaninfo --summary 2>/dev/null | grep -A3 "GPU"
```

You should see `Mali-G610` or `panvk` in the output.

> This error does not prevent GPU-accelerated OpenGL from working. If Panthor is correctly set up, Mesa uses the native Gallium driver for OpenGL without going through Zink/Vulkan. However, installing PanVK is recommended for applications that require Vulkan.
{.is-info}

## 8.4 Cinnamon Falls Back to Software Rendering

If you have confirmed that Panthor works (the checks in [section 8.2](#h-82-panthor-loaded-but-no-gpu-rendering) all pass) but Cinnamon's compositor still uses software rendering:

- Verify the udev rule is applied (see [section 4.1](#h-41-create-a-udev-rule)):

```
udevadm info -q all -n /dev/dri/card1 | grep mutter
```

- Check that `MUTTER_ALLOW_HYBRID_GPUS=1` is set (see [section 4.2](#h-42-set-environment-variables))

- Check Muffin's log for errors:

```
journalctl --user -b | grep -i muffin
```

- Make sure compositing is not disabled:

```
dconf read /org/cinnamon/muffin/compositing-manager
```

If the output is `false` or empty, enable it:

```
dconf write /org/cinnamon/muffin/compositing-manager true
```

## 8.5 Black Screen or Login Loop


If the Wayland session fails to start:

- Switch to a TTY with `Ctrl+Alt+F2` and log in
- Check the session log:

```
journalctl --user -b -u cinnamon-session
```

- As a workaround, fall back to the X11 session from the login screen and verify your configuration

## 8.6 Screen Tearing or Poor Performance

If the session starts but performance is poor:

- Verify compositing is enabled (see [section 7.2](#h-72-check-compositing-status))
- Check that no Flatpak or Snap version of Cinnamon is overriding the system session
- Try adding `CLUTTER_PAINT=disable-clipped-redraws:disable-culling` to `/etc/environment.d/90-rk3588-gpu.conf` if you see rendering artifacts

## 8.7 Reverting to X11

If Wayland does not work correctly, you can always switch back to X11 from the login screen by selecting the `Cinnamon` session (without the "Wayland" label).

## 8.8 Wayland EGL Clients Use llvmpipe (Known Muffin Limitation)

If the GBM and Device platforms in `eglinfo` correctly show `Mali-G610 MC4 (Panfrost)` but the Wayland platform shows `llvmpipe`, this is a known limitation in Muffin `6.6.x`.

**Explanation:** Muffin uses the GPU correctly for its own desktop compositing (via GBM). However, it advertises the wrong DRM device to Wayland clients via the `wl_drm` protocol. It announces `card0` (rockchip-drm, display controller with no 3D capabilities) instead of `card1`/`renderD128` (panthor, the GPU). All Wayland EGL clients inherit this wrong device and fall back to `llvmpipe`.

GNOME's Mutter solves this with the [`meta_is_udev_device_preferred_primary()`](https://gitlab.gnome.org/GNOME/mutter/-/blob/main/src/backends/meta-udev.c) function, which reads the `mutter-device-preferred-primary` udev tag. Muffin has not yet ported this function from upstream Mutter. The udev rule from [section 4.1](#h-41-create-a-udev-rule) is still recommended (other compositors and future Muffin versions will use it), but it has no effect on current Muffin versions.

- Verify the limitation by checking what DRM devices Muffin has open:

```
ls -la /proc/$(pgrep -f 'cinnamon --replace' -o)/fd 2>/dev/null | grep dri
```

You should see both `card0` and `renderD128` open, confirming Muffin is using the GPU for compositing.

**What works despite this limitation:**

| Component | Status | Reason |
|-----------|--------|--------|
| Desktop compositing | GPU-accelerated | Muffin uses GBM directly |
| Vulkan apps (mpv, vkmark, browsers) | GPU-accelerated | PanVK bypasses EGL entirely |
| Wayland EGL apps (GTK OpenGL) | Software (llvmpipe) | Wrong DRM device advertised |
{.dense}

**Workarounds:**

- Use Vulkan rendering where possible (see [section 6](#h-6-configure-mpv-for-hardware-video-playback) for mpv)
- Use GNOME or KDE Wayland instead, which handle multi-GPU correctly on the same hardware
- Track the upstream issue: [linuxmint/muffin](https://github.com/linuxmint/muffin/issues)

# 9. Summary

- The following table summarizes the changes needed:

| What | File | Content |
|------|------|---------|
| Udev rule | `/etc/udev/rules.d/61-mutter-panthor.rules` | Tag `card1` as `mutter-device-preferred-primary` |
| Environment | `/etc/environment.d/90-rk3588-gpu.conf` | `MUTTER_ALLOW_HYBRID_GPUS=1` and `PAN_MESA_DEBUG=gl3` |
| mpv config | `~/.config/mpv/mpv.conf` | Vulkan rendering + V4L2 hardware decoding |
| Session | Login screen | Select `Cinnamon (Wayland)` |
{.dense}

# 10. References

- [Muffin source code](https://github.com/linuxmint/muffin) - Linux Mint
- [Mutter multi-GPU support](https://gitlab.gnome.org/GNOME/mutter) - GNOME
- [Mutter `meta_is_udev_device_preferred_primary` implementation](https://gitlab.gnome.org/GNOME/mutter/-/blob/main/src/backends/meta-udev.c) - GNOME (missing in Muffin)
- [Mesa Panfrost driver documentation](https://docs.mesa3d.org/drivers/panfrost.html) - Mesa
- [Setup Panthor on Mali GPUs with RK3588](https://wiki.bredos.org/en/how-to/how-to-setup-panthor) - BredOS Wiki
- [BredOS sys-report](https://github.com/BredOS/sys-report) - System diagnostics tool
- [kitty OpenGL 3.3 workaround](https://github.com/kovidgoyal/kitty/issues/2790#issuecomment-969195133) - PAN_MESA_DEBUG=gl3
- [Armbian RK3588 GPU acceleration discussion](https://forum.armbian.com/topic/56374-expected-default-graphics-acceleration-for-rk3588/) - Armbian Forum
