---
title: Browser Setup
description: Setting up a GPU-accelerated browser on ARM64 boards with Beryllium OS
published: false
date: 2026-05-20T12:00:00.000Z
tags: browser, chromium, gpu, wayland, arm64
editor: markdown
dateCreated: 2026-03-07T12:00:00.000Z
---

# 0. Kernel Premise

Hardware video decode on RK3588 needs a kernel that ships the RKVDEC2 V4L2 stateless drivers along with the recent VP9 VDPU381 reset cleanup that landed in stable on 2026-05-18.

- **Recommended kernel**: `linux-beryllium-rockchip` 7.1.0rc2-2 or newer.
- **Source**: [beryllium-org/linux-beryllium](https://github.com/beryllium-org/linux-beryllium) (branch `7.1-rc2`).
- **Pre-built package (Arch ARM / Beryllium OS)**: [sbc-pkgbuilds/linux-beryllium-rockchip](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/linux-beryllium-rockchip).

This kernel covers:

- VP9 Profile 0 + Profile 2 on RKVDEC2 (VDPU381 + VDPU346, chewitt patchset).
- AV1 hardware decode through hantro-vpu and the Verisilicon IOMMU.
- HEVC and H.264 stateless decode.
- HDMI 2.1 FRL bridge stable on the dw-hdmi-qp + samsung-hdptx stack.

Older kernels (< 7.0) ship no VP9 stateless support, so they fall back to CPU decode regardless of browser flags.

# 1. Introduction

ARM64 boards running Beryllium OS have full GPU acceleration through the Panfrost (OpenGL ES 3.1) and PanVK (Vulkan 1.4) Mesa drivers. Browsers do not take equal advantage of this — and on RK3588 specifically there is a video-decode wrinkle worth understanding before installing anything. This guide covers `Ungoogled Chromium` via Flatpak with GPU flags that work, and pairs it with `mpv` + `yt-dlp` for the codec paths the browser cannot handle on its own.

> Stock Chromium and Firefox on ARM64 do not support V4L2 stateless video decode. A custom RK3588 build of Ungoogled Chromium with V4L2 stateless decode (RKVDEC2 zero-copy MMAP+EXPBUF for H.264 / HEVC / VP9) is published at [github.com/dongioia/rock5bplus-rkvdec2/releases](https://github.com/dongioia/rock5bplus-rkvdec2/releases). The VP9 path still hits a Skia / ANGLE Mali GLSL shader bug ([issues.chromium.org/503755157](https://issues.chromium.org/issues/503755157)) at every resolution, not just 4K. Below 1080p YouTube serves AV1 and the artifact does not appear; from 1080p upwards YouTube generally serves VP9 and the artifact returns. The recommended workaround for any VP9 content is `mpv` + `yt-dlp` — see [section 5](#h-5-youtube-with-hardware-decode).
{.is-info}

# 2. Browser Comparison

- The following table compares the main browser options on ARM64 with Beryllium OS:

| Feature | Ungoogled Chromium (Flatpak) | Firefox / LibreWolf |
|---------|:---:|:---:|
| GPU compositing | Full (ANGLE + Panfrost GLES 3.1) | Partial (WebRender, less optimized on ARM) |
| GPU rasterization | All pages | Limited |
| WebGL | Hardware accelerated | Hardware accelerated |
| WebGPU | Hardware accelerated | Not supported |
| Zero-copy compositing | Supported | Not supported |
| Video decode | Software only | Software only |
| Wayland | Native (Ozone) | Native |
| Flatpak availability | Available | Available |
{.dense}

Chromium's ANGLE backend maps well to Panfrost's OpenGL ES 3.1, providing full hardware-accelerated page rendering, compositing, and WebGL/WebGPU. Firefox's WebRender is less optimized for ARM Mali GPUs.

> **GPU stack in Flatpak**: The Flatpak runtime includes Panthor DRI + PanVK (Vulkan 1.4) + Panfrost (GLES 3.1). With `devices=all` and `sockets=wayland` permissions, the sandbox has full GPU access.
{.is-info}

# 3. Installation

## 3.1 Install Flatpak

- Install Flatpak and add the Flathub repository:

```
sudo pacman -S flatpak
sudo flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
sudo flatpak update --appstream
```

## 3.2 Install Ungoogled Chromium

- Install the browser from Flathub:

```
sudo flatpak install -y flathub io.github.ungoogled_software.ungoogled_chromium
```

# 4. GPU Acceleration Setup

## 4.1 Desktop Entry with GPU Flags

To launch Chromium with full GPU acceleration on Wayland, create a custom desktop entry.

- Create the desktop entry:

```
mkdir -p ~/.local/share/applications
cat > ~/.local/share/applications/ungoogled-chromium-gpu.desktop << 'EOF'
[Desktop Entry]
Type=Application
Name=Ungoogled Chromium (GPU)
Comment=Web Browser with Wayland + Vulkan GPU acceleration
Exec=flatpak run io.github.ungoogled_software.ungoogled_chromium --ignore-gpu-blocklist --enable-zero-copy --ozone-platform=wayland --use-gl=egl --enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan,WaylandWindowDecorations
Icon=io.github.ungoogled_software.ungoogled_chromium
Categories=Network;WebBrowser;
StartupNotify=true
EOF
update-desktop-database ~/.local/share/applications/
```

## 4.2 Flag Reference

- Each flag serves a specific purpose:

| Flag | Purpose |
|------|---------|
| `--ozone-platform=wayland` | Native Wayland - avoids XWayland overhead |
| `--use-gl=egl` | Use EGL directly (required for Wayland) |
| `--ignore-gpu-blocklist` | Allow GPU acceleration on ARM GPUs |
| `--enable-zero-copy` | Zero-copy buffer sharing between GPU and compositor |
| `Vulkan,VulkanFromANGLE,DefaultANGLEVulkan` | ANGLE rendering via PanVK Vulkan 1.4 |
| `WaylandWindowDecorations` | Native window decorations under Wayland |
{.dense}

> **Fallback**: if Vulkan causes crashes on your board, remove the 3 Vulkan features from the `--enable-features` flag and keep only `--use-gl=egl`. This falls back to OpenGL ES via Panfrost, which is still hardware-accelerated.
{.is-warning}

## 4.3 Verify GPU Acceleration

After launching Chromium with the GPU flags:

- Open `chrome://gpu` in the address bar.

You should see:

- **GL_RENDERER**: `Mali-G610` / `panfrost` / `panvk` (not `SwiftShader` or `llvmpipe`)
- **Canvas**: Hardware accelerated
- **Compositing**: Hardware accelerated
- **Rasterization**: Hardware accelerated
- **WebGL**: Hardware accelerated
- **WebGPU**: Hardware accelerated

> If `GL_RENDERER` shows `SwiftShader` or `llvmpipe`, GPU acceleration is not active. Check that Flatpak has `devices=all` permission: `flatpak info --show-permissions io.github.ungoogled_software.ungoogled_chromium`
{.is-warning}

# 5. YouTube with Hardware Decode

For VP9 content of any resolution, and for any 4K / UHD stream, this is the recommended path. `mpv` calls libavcodec's `v4l2request` hwaccel, which talks directly to RKVDEC2 on the kernel side and avoids both libva and the Mali shader bug that plagues Chromium's VP9 path. The kernel decode itself was validated frame-by-frame against the libvpx reference (3000 frames of Big Buck Bunny VP9 Profile 0 1080p, pixel-identical).

> Use `--hwdec=v4l2request-copy`, not the bare `--hwdec=v4l2request`. The `-copy` suffix performs a CPU readback before display, which works on Mali Valhall (Panfrost / Panthor). The zero-copy variant (`--hwdec=v4l2request` without `-copy`) silently falls back to software because the EGL dmabuf import path on the current Mesa panthor driver does not refresh the imported texture between frames. CPU readback adds a small overhead but stays well under software decode — the BBB 720p VP9 clip measured 17% CPU with `-copy` versus 31% software.
{.is-info}

## 5.1 Install mpv, yt-dlp, and a V4L2-aware ffmpeg

- Install `mpv` and `yt-dlp` from the distribution:

```
sudo pacman -S mpv yt-dlp
```

- The default `ffmpeg` package on Arch ARM ships with TLS support but without the V4L2 request hwaccel. RKVDEC2 needs that hwaccel. Install the `ffmpeg-v4l2-requests` PKGBUILD from [sbc-pkgbuilds/ffmpeg-v4l2-requests](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests) (it enables both `--enable-gnutls` and `--enable-v4l2-request`). After installing, confirm:

```
ffmpeg -hwaccels 2>&1 | grep v4l2request
ffmpeg -protocols 2>&1 | grep https
```

Both commands should print a line. If `https` is missing, `mpv` will refuse to open YouTube URLs with `Protocol is either unsupported, or was disabled at compile-time`.

## 5.2 Play from Terminal

- Play a YouTube video with hardware decode:

```
mpv --hwdec=v4l2request-copy 'https://youtube.com/watch?v=VIDEO_ID'
```

`mpv` automatically uses `yt-dlp` to extract the video URL and RKVDEC2 for the actual decode. To make `v4l2request-copy` the default for every invocation, drop one line into `~/.config/mpv/mpv.conf`:

```
hwdec=v4l2request-copy
```

## 5.3 Play from Browser (Bookmarklet)

You can create a bookmarklet to open the current page in mpv directly from Chromium.

- Create a protocol handler script:

```
mkdir -p ~/.local/bin
cat > ~/.local/bin/mpv-handler.sh << 'SCRIPT'
#!/bin/sh
url=$(echo "$1" | sed 's|^mpv://||')
exec mpv --hwdec=v4l2request-copy "$url"
SCRIPT
chmod +x ~/.local/bin/mpv-handler.sh
```

- Create the desktop entry for the `mpv://` protocol:

```
cat > ~/.local/share/applications/mpv-handler.desktop << 'EOF'
[Desktop Entry]
Type=Application
Name=mpv URL Handler
Exec=/home/$USER/.local/bin/mpv-handler.sh %u
MimeType=x-scheme-handler/mpv;
NoDisplay=true
EOF
xdg-mime default mpv-handler.desktop x-scheme-handler/mpv
```

- Add this bookmarklet to your browser bookmarks bar:

```
javascript:void(window.open('mpv://'+location.href))
```

Click the bookmarklet on any YouTube page to open it in mpv with hardware-accelerated decode.

## 5.4 Auto-Redirect from a Userscript

The bookmarklet works one click at a time. For automatic redirect at 1080p and above, combine [Tampermonkey](https://www.tampermonkey.net/) (or Violentmonkey) with this userscript:

```javascript
// ==UserScript==
// @name         YouTube → mpv at 1080p and above
// @match        https://www.youtube.com/watch*
// @run-at       document-idle
// ==/UserScript==
(function () {
  const v = new URL(location.href).searchParams.get('v');
  if (!v) return;

  // Probe the player for its current quality once the metadata loads.
  const probe = setInterval(() => {
    const player = document.querySelector('#movie_player');
    if (!player || typeof player.getPlaybackQuality !== 'function') return;
    const q = player.getPlaybackQuality();  // "hd1080", "hd1440", "hd2160", ...
    if (!q) return;
    clearInterval(probe);
    const hi = /^hd(1080|1440|2160|2880|4320)$/.test(q);
    if (hi) location.href = 'mpv://' + location.href;
  }, 500);
})();
```

This relies on the `mpv://` protocol handler from [section 5.3](#h-53-play-from-browser-bookmarklet). The script checks the player quality once metadata is ready and redirects only at 1080p or above, leaving sub-1080p AV1 content in the browser where it already works.

> If you prefer a context-menu approach with extension support, [ff2mpv](https://github.com/woodruffw/ff2mpv) works well for non-Flatpak Chromium / Firefox. Inside the Flatpak sandbox the native-messaging host setup is fragile (the host script must be reachable from inside the sandbox and `flatpak-spawn` glue varies by browser package), so the userscript route above is the simpler recommendation for the Flatpak Ungoogled Chromium covered by this guide.
{.is-info}

# 6. Troubleshooting

## 6.1 Chromium Shows Software Rendering

If `chrome://gpu` shows software rendering:

- Verify Flatpak GPU permissions:

```
flatpak info --show-permissions io.github.ungoogled_software.ungoogled_chromium
```

The output should include `devices=all` and `sockets=wayland`. If not:

- Override the permissions manually:

```
flatpak override --user --device=all io.github.ungoogled_software.ungoogled_chromium
flatpak override --user --socket=wayland io.github.ungoogled_software.ungoogled_chromium
```

## 6.2 Chromium Crashes with Vulkan Flags

Some boards or Mesa versions may have issues with PanVK. Remove the Vulkan features.

- Use this simpler flag set instead:

```
flatpak run io.github.ungoogled_software.ungoogled_chromium --ignore-gpu-blocklist --enable-zero-copy --ozone-platform=wayland --use-gl=egl --enable-features=WaylandWindowDecorations
```

This still uses GPU acceleration via Panfrost (OpenGL ES) without the Vulkan path.

## 6.3 mpv Does Not Use Hardware Decode

- Verify that RKVDEC2 is available:

```
ls /dev/video-dec0
```

If the device node is missing, check that the kernel module is loaded:

- Check for the RKVDEC2 module:

```
lsmod | grep rockchip_vdec
```

- Confirm `mpv` picks up the `v4l2request-copy` hwaccel:

```
mpv --hwdec=v4l2request-copy --vo=null --frames=1 -v <video> 2>&1 | grep -iE 'hwdec|v4l2'
```

The log should show `Using hardware decoding (v4l2request-copy)` and the VO format should be `nv12` (not `yuv420p`). If it falls back to software, check that the `ffmpeg-v4l2-requests` package is installed (the default Arch ARM `ffmpeg` lacks the `v4l2request` hwaccel) and that `/dev/video0` belongs to the `video` group, with your user a member of it.

## 6.4 Userscript Does Not Redirect

If clicking through a 1080p+ YouTube page does not hand off to `mpv`:

- Open the browser console (F12) on a YouTube watch page and check that the `mpv` protocol handler is registered:

```
location.href = 'mpv://' + location.href
```

If nothing happens, the desktop entry from [section 5.3](#h-53-play-from-browser-bookmarklet) was not registered. Re-run the `xdg-mime default ...` command and confirm with:

```
xdg-mime query default x-scheme-handler/mpv
```

- The Tampermonkey / Violentmonkey toolbar icon should show the script as enabled and matching the current URL. Some YouTube layouts load the player after `document-idle`; if the redirect never fires, change `@run-at` to `document-end` and reload.


# 7. References

- [Chromium GPU Acceleration docs](https://chromium.googlesource.com/chromium/src/+/main/docs/gpu/gpu_testing.md) - Chromium Project
- [Mesa Panfrost driver](https://docs.mesa3d.org/drivers/panfrost.html) - Mesa
- [Mesa PanVK driver](https://docs.mesa3d.org/drivers/panvk.html) - Mesa
- [Flatpak documentation](https://docs.flatpak.org/) - Flatpak
- [mpv manual](https://mpv.io/manual/stable/) - mpv
- [ff2mpv](https://github.com/woodruffw/ff2mpv) - native-messaging bridge between the browser and mpv (alternative to the userscript on non-Flatpak browsers)
- [Tampermonkey](https://www.tampermonkey.net/) - userscript manager used by the auto-redirect script
- [Chromium Skia/ANGLE issue 503755157](https://issues.chromium.org/issues/503755157) - VP9 Mali shader artifact tracked upstream
