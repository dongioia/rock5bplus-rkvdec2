---
title: Browser Setup
description: Setting up a GPU-accelerated browser on ARM64 boards with BredOS
published: false
date: 2026-03-07T12:00:00.000Z
tags: browser, chromium, gpu, wayland, arm64
editor: markdown
dateCreated: 2026-03-07T12:00:00.000Z
---

# 1. Introduction

ARM64 boards running BredOS have full GPU acceleration capabilities via the Panfrost (OpenGL ES 3.1) and PanVK (Vulkan 1.4) Mesa drivers. However, not all browsers take advantage of this equally. This guide covers setting up `Ungoogled Chromium` via Flatpak with optimal GPU flags for a smooth browsing experience.

> No browser on ARM64 currently supports hardware video decode via V4L2 stateless API. For hardware-accelerated video playback, see section [5. YouTube with Hardware Decode](#h-5-youtube-with-hardware-decode).
{.is-info}

# 2. Browser Comparison

- The following table compares the main browser options on ARM64 with BredOS:

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

No browser on ARM64 supports V4L2 stateless decode, which is the hardware video decode method used on mainline RK3588 kernels. To play YouTube videos with hardware acceleration, use `mpv` + `yt-dlp`.

## 5.1 Install mpv and yt-dlp

- Install the required packages:

```
sudo pacman -S mpv yt-dlp
```

## 5.2 Play from Terminal

- Play a YouTube video with hardware decode:

```
mpv 'https://youtube.com/watch?v=VIDEO_ID'
```

`mpv` automatically uses `yt-dlp` to extract the video URL and RKVDEC2 for hardware decode.

## 5.3 Play from Browser (Bookmarklet)

You can create a bookmarklet to open the current page in mpv directly from Chromium.

- Create a protocol handler script:

```
mkdir -p ~/.local/bin
cat > ~/.local/bin/mpv-handler.sh << 'SCRIPT'
#!/bin/sh
url=$(echo "$1" | sed 's|^mpv://||')
exec mpv "$url"
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


# 7. References

- [Chromium GPU Acceleration docs](https://chromium.googlesource.com/chromium/src/+/main/docs/gpu/gpu_testing.md) - Chromium Project
- [Mesa Panfrost driver](https://docs.mesa3d.org/drivers/panfrost.html) - Mesa
- [Mesa PanVK driver](https://docs.mesa3d.org/drivers/panvk.html) - Mesa
- [Flatpak documentation](https://docs.flatpak.org/) - Flatpak
- [mpv manual](https://mpv.io/manual/stable/) - mpv
