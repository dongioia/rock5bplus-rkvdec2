---
title: Browser Setup
description: GPU-accelerated browser with in-browser hardware video decode on RK3588 boards with Beryllium OS
published: true
date: 2026-07-12T12:00:00.000Z
tags: browser, chromium, gpu, wayland, arm64, hardware-decode
editor: markdown
dateCreated: 2026-03-07T12:00:00.000Z
---

# 0. Kernel Premise

In-browser hardware video decode on RK3588 goes through the kernel's V4L2 stateless decoders, so the kernel must expose the codecs before any browser flag matters:

- **VP9** on `/dev/video0` (RKVDEC2 VDPU381). Check: `v4l2-ctl -d /dev/video0 --list-formats-out` must list `VP9F`.
- **AV1** on `/dev/video4` (Hantro VPU), ideally with the Verisilicon IOMMU bound (`CONFIG_VSI_IOMMU=y`) so sustained decode does not exhaust CMA. Check for `AV1F`.
- **H.264 / HEVC** come with the Collabora RK3588 base.

- **Recommended kernel**: `linux-beryllium-rockchip` 7.1 or newer, built on Collabora `rockchip-v7.1` with the downstream VP9 VDPU381 restore.
- **Source**: [beryllium-org/linux-beryllium](https://github.com/beryllium-org/linux-beryllium).

On a kernel without `VP9F` / `AV1F` (a stock generic mainline kernel), decode silently falls back to CPU regardless of browser flags. **The kernel comes first.**

> The Collabora `rockchip-v7.1` base does not carry VP9 on VDPU381 (it was never upstreamed). A downstream restore re-adds it — see the [rock5bplus-rkvdec2](https://github.com/dongioia/rock5bplus-rkvdec2) kernel composition.
{.is-info}

# 1. Introduction

On RK3588 with Beryllium OS, **stock Chromium 150 decodes VP9 / H.264 / HEVC / AV1 in hardware, in-browser, with a clean picture.** The long-standing VP9 green artifact on Mali Valhall was a chroma-plane bug in Chromium's ANGLE YUV texture path; it was fixed upstream in the ANGLE roll that shipped with Chromium 150, so there is no longer any need for a custom browser build or a LibYUV CPU-conversion bypass.

This guide covers the stock Chromium 150 setup — install, launcher flags, and verification. It also keeps the `mpv` handoff (section 5) as an alternative for browsers that do not do V4L2 decode (Firefox, the Flatpak) or when you want mpv's libplacebo present quality.

> Earlier versions of this guide recommended a custom Ungoogled Chromium build plus an mpv handoff for all VP9 content, because the VP9 path hit a Mali GLSL bug. That bug is fixed in Chromium 150 — the custom build is now only of historical interest. Upstream tracker: [issue 503755157](https://issues.chromium.org/issues/503755157).
{.is-info}

# 2. Browser Comparison

| Feature | Stock Chromium 150 (pacman) | Ungoogled Chromium (Flatpak) | Firefox / LibreWolf |
|---------|:---:|:---:|:---:|
| V4L2 hardware video decode | **Yes** (VP9/H264/HEVC/AV1) | No | No |
| GPU compositing | Full (ANGLE + Panfrost) | Full | Partial (WebRender) |
| GPU rasterization | Yes | Yes | Limited |
| WebGL / WebGPU | Yes / Yes | Yes / Yes | Yes / No |
| Wayland | Native (Ozone) | Native | Native |
{.dense}

The ArchLinuxARM `chromium` package is built with `use_v4l2_codec=true` + `use_av1_hw_decoder=true`, so the V4L2 decode path is compiled in and reachable on generic Linux. The Flatpak Ungoogled build and Firefox do GPU compositing but not V4L2 video decode — pair those with the mpv handoff for hardware video.

# 3. Installation

## 3.1 Stock Chromium 150 (recommended)

```
sudo pacman -S chromium
```

ArchLinuxARM ships stock Chromium 150 with V4L2 hardware decode compiled in. The same binary also runs on other distros (Ubuntu / Debian on RK3588) — extract the package into `$HOME` and launch it.

## 3.2 Ungoogled Chromium via Flatpak (optional — GPU compositing only)

For a sandboxed browser with GPU compositing but **without** V4L2 video decode (pair it with the mpv handoff in section 5):

```
sudo pacman -S flatpak
sudo flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
sudo flatpak install -y flathub io.github.ungoogled_software.ungoogled_chromium
```

# 4. Chromium 150 hardware video decode

## 4.1 The launcher — this is the whole setup

`chromium-launcher` reads `~/.config/chromium-flags.conf` on every start, and every `.desktop` entry inherits it, so this one file is the definitive setup — no per-launch flags:

```
--ozone-platform-hint=auto
--enable-features=AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL
--ignore-gpu-blocklist
--enable-gpu-rasterization
--enable-zero-copy
```

## 4.2 Flag Reference

| Flag | Purpose |
|------|---------|
| `--enable-features=AcceleratedVideoDecoder` | Gates V4L2 hardware decode. The older name `AcceleratedVideoDecodeLinuxV4L2` no longer exists in Chromium 150 and is silently ignored — copy it from an old guide and HW decode stays off |
| `AcceleratedVideoDecodeLinuxGL`, `AcceleratedVideoDecodeLinuxZeroCopyGL` | The Linux GL import paths for the decoded NV12 frame |
| `--ozone-platform-hint=auto` | Picks Wayland when the session provides it. HW decode needs Wayland; X11 gives none |
| `--ignore-gpu-blocklist` | Allow GPU acceleration on ARM Mali |
| `--enable-gpu-rasterization` | GPU page rasterization |
| `--enable-zero-copy` | Zero-copy buffer sharing between GPU and compositor |
{.dense}

> Some setups also add `--ozone-platform=wayland --use-gl=angle --use-angle=gles`. That works on some stacks, but forcing `--ozone-platform` or `--use-gl` — instead of letting `--ozone-platform-hint=auto` choose — breaks V4L2 decoder selection on some kernel / Mesa combinations. If `chrome://gpu` reports software decode, remove those two.
{.is-warning}

## 4.3 Verify Hardware Decode

Play any VP9 or AV1 video, then check:

- `chrome://gpu` → **Video Decode: Hardware accelerated**.
- `chrome://media-internals` → `kVideoDecoderName: V4L2VideoDecoder` and `kIsPlatformVideoDecoder: true`.

> **10-bit VP9 (Profile 2)** now decodes in hardware with the experimental NV15 build — see [4.4](#44-10-bit-vp9-profile-2--experimental-nv15-build). Stock Chromium 150 still falls back to software for 10-bit VP9 (it doesn't know the `NV15` fourcc); AV1 and HEVC 10-bit already work in hardware on stock 150.
{.is-info}

> **YouTube codec note.** YouTube serves AV1 (not VP9) whenever the client advertises AV1 hardware decode, so a "VP9" test on YouTube may actually exercise the AV1 VPU (`/dev/video4`). To isolate the rkvdec VP9 path, test a local Profile-0 `.webm`.
{.is-info}

## 4.4 10-bit VP9 Profile 2 — experimental NV15 build

Stock Chromium 150 decodes 8-bit VP9 in hardware but falls back to software for **10-bit VP9 (Profile 2)**, because it doesn't know the packed 10-bit `NV15` format the RK3588 decoder outputs. An experimental build closes that gap: [Chromium 150 + NV15](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/chromium-150.0.7871.114-nv15-10bit).

It is stock Chromium 150.0.7871.114 plus sky-rk3588 (Igor Paunovic)'s NV15 end-to-end patch and a small ANGLE `dma_buf_utils` hunk — the hunk is needed on the Rock 5B+ GL/ANGLE render path so ANGLE imports NV15 instead of showing green. On the Rock 5B+ it decodes 10-bit VP9 Profile 2 on the rkvdec with a clean picture and no software fallback, at the same CPU cost as 8-bit.

Requirements:
- The **VP9 Profile 2 kernel** — the rkvdec driver has to advertise `NV15`. That is sky-rk3588 (Igor)'s profile-2 driver patch, **which isn't in a Beryllium release yet — the kernel PRs are still pending**. For now, build the kernel yourself from [rock5bplus-rkvdec2](https://github.com/dongioia/rock5bplus-rkvdec2#build-the-kernel); the patch is also attached to the [release](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/chromium-150.0.7871.114-nv15-10bit) as `vp9-profile2-kernel-rkvdec.patch`. Without it, 10-bit VP9 falls back to software even with this build.
- The same Wayland launcher as above.

Install:
```bash
sudo pacman -U chromium-150.0.7871.114-nv15-v2angle.pkg.tar.xz
```

To confirm it is really hardware and not a silent software fallback, play a 10-bit VP9 clip and check that chromium holds the decoder:
```bash
fuser /dev/video0   # should list chromium's pid during playback
```
`chrome://gpu` can read "accelerated" even after the GPU process fell back, so the device hold is the reliable check.

Experimental — built from patches not yet in upstream Chromium. 8-bit VP9 and every other codec are unchanged. Discussion and patches: [minimyth2#73](https://github.com/warpme/minimyth2/issues/73).

# 5. Hand video off to mpv (alternative)

With stock Chromium 150 the in-browser path above just works, so the mpv handoff is no longer required for VP9. It stays useful for browsers that don't do V4L2 decode (Firefox, the Flatpak Ungoogled Chromium) and when you want mpv's libplacebo present quality. `mpv` calls libavcodec's `v4l2request` hwaccel, which talks directly to RKVDEC2 / Hantro on the kernel side.

> Use `--hwdec=v4l2request-copy`, not the bare `--hwdec=v4l2request`. The `-copy` suffix does a CPU readback before display, which works on Mali Valhall (Panfrost / Panthor). The zero-copy variant silently falls back to software because the current Mesa panthor EGL dmabuf import does not refresh the imported texture between frames. CPU readback adds a small overhead but stays well under software decode — a 720p VP9 clip measured 17% CPU with `-copy` versus 31% software.
{.is-info}

## 5.1 Install mpv, yt-dlp, and a V4L2-aware ffmpeg

- Install `mpv` and `yt-dlp`:

```
sudo pacman -S mpv yt-dlp
```

- The default `ffmpeg` on Arch ARM ships TLS support but no V4L2 request hwaccel, which RKVDEC2 needs. Install the `ffmpeg-v4l2-requests` PKGBUILD from [sbc-pkgbuilds/ffmpeg-v4l2-requests](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests) (enables both `--enable-gnutls` and `--enable-v4l2-request`). Confirm:

```
ffmpeg -hwaccels 2>&1 | grep v4l2request
ffmpeg -protocols 2>&1 | grep https
```

Both should print a line. If `https` is missing, `mpv` refuses YouTube URLs with `Protocol is either unsupported, or was disabled at compile-time`.

## 5.2 Play from Terminal

```
mpv --hwdec=v4l2request-copy 'https://youtube.com/watch?v=VIDEO_ID'
```

`mpv` uses `yt-dlp` to extract the stream and RKVDEC2 for the decode. To make it the default, drop one line into `~/.config/mpv/mpv.conf`:

```
hwdec=v4l2request-copy
```

## 5.3 Play from Browser (Bookmarklet)

- Create a protocol-handler script:

```
mkdir -p ~/.local/bin
cat > ~/.local/bin/mpv-handler.sh << 'SCRIPT'
#!/bin/sh
url=$(echo "$1" | sed 's|^mpv://||')
exec mpv --hwdec=v4l2request-copy "$url"
SCRIPT
chmod +x ~/.local/bin/mpv-handler.sh
```

- Register the `mpv://` protocol:

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

- Add this bookmarklet to your bookmarks bar; click it on any page to open it in mpv:

```
javascript:void(window.open('mpv://'+location.href))
```

## 5.4 Auto-Redirect from a Userscript

For automatic handoff at 1080p and above, combine [Tampermonkey](https://www.tampermonkey.net/) (or Violentmonkey) with this userscript:

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

This relies on the `mpv://` protocol handler from [section 5.3](#h-53-play-from-browser-bookmarklet). It redirects only at 1080p or above. Since stock Chromium 150 now plays these in hardware in-browser, the userscript is optional — use it only if you specifically prefer mpv's libplacebo output.

# 6. Troubleshooting

## 6.1 Chromium Shows Software Decode

If `chrome://gpu` reports "Video Decode: Software only":

- Confirm the feature flag is `AcceleratedVideoDecoder`, **not** the stale `AcceleratedVideoDecodeLinuxV4L2` (silently ignored in Chromium 150).
- Confirm you are in a **Wayland** session — X11 gives no VPU decode on Chromium 150.
- Confirm the kernel exposes the codec: `v4l2-ctl -d /dev/video0 --list-formats-out` lists `VP9F` (and `/dev/video4` lists `AV1F`).
- If you added `--use-gl` or `--ozone-platform`, remove them and rely on `--ozone-platform-hint=auto`.

## 6.2 Flatpak Shows Software Rendering / Vulkan Crash

For the Flatpak Ungoogled Chromium (compositing only), check GPU permissions:

```
flatpak info --show-permissions io.github.ungoogled_software.ungoogled_chromium
```

The output should include `devices=all` and `sockets=wayland`. If not:

```
flatpak override --user --device=all io.github.ungoogled_software.ungoogled_chromium
flatpak override --user --socket=wayland io.github.ungoogled_software.ungoogled_chromium
```

If Vulkan (PanVK) crashes on your board or Mesa version, drop the Vulkan features and keep OpenGL ES via Panfrost:

```
flatpak run io.github.ungoogled_software.ungoogled_chromium --ignore-gpu-blocklist --enable-zero-copy --ozone-platform=wayland --use-gl=egl --enable-features=WaylandWindowDecorations
```

## 6.3 mpv Does Not Use Hardware Decode

Confirm RKVDEC2 is present and picked up:

```
ls /dev/video0
lsmod | grep rockchip_vdec
mpv --hwdec=v4l2request-copy --vo=null --frames=1 -v <video> 2>&1 | grep -iE 'hwdec|v4l2'
```

The log should show `Using hardware decoding (v4l2request-copy)` and the VO format should be `nv12` (not `yuv420p`). If it falls back to software, check that `ffmpeg-v4l2-requests` is installed (stock Arch ARM `ffmpeg` lacks the `v4l2request` hwaccel) and that your user is in the `video` group.

## 6.4 Userscript Does Not Redirect

Confirm the `mpv://` handler is registered:

```
xdg-mime query default x-scheme-handler/mpv
```

If empty, re-run the `xdg-mime default mpv-handler.desktop x-scheme-handler/mpv` command from [section 5.3](#h-53-play-from-browser-bookmarklet). Some YouTube layouts load the player after `document-idle`; if the redirect never fires, change `@run-at` to `document-end` and reload.

# 7. References

- [Chromium GPU Acceleration docs](https://chromium.googlesource.com/chromium/src/+/main/docs/gpu/gpu_testing.md) — Chromium Project
- [Mesa Panfrost driver](https://docs.mesa3d.org/drivers/panfrost.html) — Mesa
- [Mesa PanVK driver](https://docs.mesa3d.org/drivers/panvk.html) — Mesa
- [rock5bplus-rkvdec2](https://github.com/dongioia/rock5bplus-rkvdec2) — RK3588 kernel composition, VP9 VDPU381 restore, and the browser-video guide this page mirrors
- [mpv manual](https://mpv.io/manual/stable/) — mpv
- [Tampermonkey](https://www.tampermonkey.net/) — userscript manager for the optional auto-redirect
- [Chromium issue 503755157](https://issues.chromium.org/issues/503755157) — the VP9 Mali chroma-plane artifact, fixed upstream in Chromium 150
