# Rock 5B+ Mainline Kernel — Full Hardware Support

Patches, configs and tools for the **Radxa Rock 5B+** on mainline Linux: 4K hardware video decode, HDMI 2.0 4K@60Hz, HDMI audio, GPU overclock, and an RK3588-aware Chromium build.

> **Status (2026-05-24)**: Linux **7.1-rc2** running on Rock 5B+ (`7.1.0-rc1+`, panthor 1.8.0, MCU stable). The production stack lives in [beryllium-org/linux-beryllium `7.1-rc2`](https://github.com/beryllium-org/linux-beryllium/tree/7.1-rc2) — Collabora rockchip-devel squashed onto 7.1-rc2, plus the chewitt VP9 / AV1 / HDMI patch set and `media: rkvdec: fix PM runtime teardown ordering in remove` (Jonas Karlman, [accepted to stable on 2026-05-18](https://lore.kernel.org/all/?q=20260518145414.64514-1-pavone.lawyer@gmail.com)). The pre-built package is at [sbc-pkgbuilds/linux-beryllium-rockchip](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/linux-beryllium-rockchip). **Chromium 147.0.7727.116-3** with the VP9 Mali Valhall artifact bypass is published as a [release](https://github.com/dongioia/rock5bplus-rkvdec2/releases); for any VP9 content the recommended path is `mpv --hwdec=v4l2request-copy` against [`ffmpeg-v4l2-requests`](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests) — the native FFmpeg V4L2 request hwaccel, no libva involvement. Chromium 150 (stable target 2026-06-17) ships the equivalent fix natively via gerrit [CL 7794420 (`d20dfa664`)](https://chromium-review.googlesource.com/c/chromium/src/+/7794420), so the in-tree `kForceLibYUV` workaround will become unnecessary once distros bump. The `dongioia/libva-v4l2-request` fork is **frozen as a historical reference** following Nicolas Dufresne's LKML guidance (2026-05-20) that `libva-v4l2-request` is a dead-end path; new work goes through native V4L2 stateless in FFmpeg / GStreamer / Chromium directly. **GPU overclock currently disabled** — the 1188 MHz GPLL service triggers panthor MCU fatal / kernel panic on the post-2026-04-20 Mesa/firmware combo.

## What works

| Feature | Status | Notes |
|---|---|---|
| H.264 / HEVC / VP9 / AV1 4K decode | ✅ | RKVDEC2 zero-copy (MMAP+EXPBUF). VP9 community, Profile 0 |
| HDMI 2.0 4K@60Hz | ✅ | SCDC scrambling v4 (Ciocaltea, Collabora) |
| HDMI audio + analog (ES8316) | ✅ | PipeWire + UCM tweak |
| GPU Panthor / Mali-G610 | ✅ | Vulkan 1.4 PanVK; 850 MHz default. [1188 MHz GPLL overclock](#gpu-overclock-1188-mhz-disabled) currently disabled (kernel panic on current stack) |
| Dual HDMI, Ethernet 2.5GbE, WiFi 6 (RTL8852BE), Bluetooth, HDMI-RX | ✅ | All mainline |
| NPU (3 cores) | ⚠️ kernel ready | Open-source Rocket+Teflon: basic CNN. Full ops need proprietary RKNN+BSP |
| RGA3 | ❌ | No upstream driver (RGA2 works) |

## Active patches (Linux 7.1-rc2)

Production deployment uses [beryllium-org/linux-beryllium `7.1-rc2`](https://github.com/beryllium-org/linux-beryllium/tree/7.1-rc2): mainline 7.1-rc2 + a single squashed merge of Collabora's `rockchip-devel` tip, then the chewitt patchset and a VP9 reset cleanup on top.

| Series | Author | Purpose |
|---|---|---|
| Collabora `rockchip-devel` (squashed) | [Collabora](https://gitlab.collabora.com/hardware-enablement/rockchip-3588) | HDMI 2.1 FRL bridge, `dw-hdmi-qp` + `samsung-hdptx`, USBDP, PCIe, V4L2 stateless tracing, RGA3, Rocket NPU |
| VP9 VDPU381 + VDPU346 + Profile 2 (6 patches) | [chewitt](https://github.com/chewitt) / Daniel Almeida | RKVDEC2 VP9 stack with Profile 0 + Profile 2 (10-bit / HDR) — replaces the earlier dvab-sarma adaptation |
| AV1 (hantro-vpu) + Verisilicon IOMMU bundle (5 patches) | [chewitt](https://github.com/chewitt) | AV1 hardware decode, IOMMU context restore, tile-info buffer fix, sync_state warn fix |
| HDMI flood fixes (3 patches) | beryllium-org | `dw_hdmi_qp` N-table + ALSA ELD rate-limit |
| `media: rkvdec: fix PM runtime teardown ordering in remove` | [Jonas Karlman](https://lore.kernel.org/all/20260309111126.137-1-jonas@kwiboo.se/) | Removes the VP9 green-chroma motion-block artifact on RKVDEC2 VDPU381 under heavy submission. [Accepted to stable on 2026-05-18](https://lore.kernel.org/all/?q=20260518145414.64514-1-pavone.lawyer@gmail.com) |
| Beryllium defconfig | this repo | Kernel config + build flags |

The historical rc3-era patches (`patches/display/v4-ciocaltea/`, `patches/vpu/vp9-vdpu381-adapted.patch`, `patches/vpu/rkvdec-vdpu381-vp9.{c,h}`) are kept here as reference for anyone rebuilding off mainline 7.0-rc3 directly. The 7.0.y stack documented in earlier revisions of this README is preserved in the [`7.0.y`](https://github.com/beryllium-org/linux-beryllium/tree/7.0.y) branch.

## Build

Apple Silicon Mac (Docker, native arm64) or any aarch64 Linux box. Production build uses the Beryllium 7.1-rc2 branch directly:

```bash
git clone -b 7.1-rc2 https://github.com/beryllium-org/linux-beryllium.git src/linux
cd src/linux
make ARCH=arm64 defconfig                    # uses arch/arm64/configs from the 7.1-rc2 branch
make ARCH=arm64 olddefconfig
../../scripts/build.sh Image 12               # or: make -j$(nproc) Image modules dtbs
```

`scripts/build.sh setup` builds the Docker image once. On macOS the script uses a `git archive` tarball inside the container to dodge HFS+/APFS case-insensitivity collisions (e.g. `ipt_ECN.h` vs `ipt_ecn.h`).

To reproduce the rc3-era stack from this repo instead (smaller patch set, no RGA3/vicap/etc.), check out `v7.0-rc3` and apply the patches under `patches/display/v4-ciocaltea/` + `patches/vpu/`.

## Deploy to Rock 5B+

```bash
BOARD=<ip> USER=<user> KVER=7.1.0-rc1+

scp src/linux/arch/arm64/boot/Image $USER@$BOARD:/tmp/
scp src/linux/arch/arm64/boot/dts/rockchip/rk3588-rock-5b-plus.dtb $USER@$BOARD:/tmp/
ssh $USER@$BOARD "sudo install -m644 /tmp/Image /boot/vmlinuz-linux-custom \
  && sudo install -m644 /tmp/rk3588-rock-5b-plus.dtb /boot/dtbs/rockchip/"

# NEVER extract modules at / on Arch — destroys /lib symlink
rsync -av staging/lib/modules/$KVER/ $USER@$BOARD:/tmp/modules/
ssh $USER@$BOARD "sudo rsync -a /tmp/modules/ /usr/lib/modules/$KVER/ \
  && sudo depmod $KVER \
  && sudo mkinitcpio -k $KVER -g /boot/initramfs-linux-custom.img \
  && sudo grub-mkconfig -o /boot/grub/grub.cfg"
```

## GPU overclock (1188 MHz, disabled)

> ⚠️ **Currently disabled.** After the 2026-04-20 pacman update (Mesa 26.0.5, vulkan-panfrost 26.0.5, xorg-server 21.1.22) the 1188 MHz GPLL service triggers panthor MCU `status=fatal` + page faults at AS0 within ~40 s of boot, on every kernel tested (6.19.1-bredos, 7.0.0-rc3+, 7.0 final). Mesa/firmware/UEFI downgrades did not recover. Disable with `sudo systemctl disable --now gpu-overclock.service && sudo reboot`. The instructions below are kept for future re-enable on a known-stable Mesa+firmware combo.

The RK3588 GPU is capped at 850 MHz on EDK2 UEFI: BL31 hardcodes NPLL via SCMI even though the OPP table lists 900/1000 MHz. Bypass: switch the CRU mux at `0xfd7c0578` (CLKSEL_CON158) from NPLL (3) to GPLL (0 → 1188 MHz) and raise GPU voltage to 1050 mV in DTS.

DTS — set OPP and regulator max:

```dts
/* rk3588-opp.dtsi */
opp-850000000 { opp-microvolt = <1050000 1050000 1050000>; };
/* rk3588-rock-5b-5bp-5t.dtsi */
vdd_gpu_s0: dcdc-reg1 { regulator-max-microvolt = <1050000>; };
```

Userspace mux switch (`/dev/mem`) + systemd unit:

```bash
sudo tee /usr/local/bin/gpu-gpll-overclock.py >/dev/null <<'PY'
#!/usr/bin/env python3
import mmap, struct, os, time
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 4096, offset=0xfd7c0000)
try: open('/sys/class/devfreq/fb000000.gpu/governor','w').write('performance')
except: pass
def go():
    m.seek(0x578)
    if (struct.unpack('<I', m.read(4))[0] >> 5) & 7 != 0:
        m.seek(0x578); m.write(struct.pack('<I', (7<<21) | (0<<5)))
go()
while True: time.sleep(5); go()
PY
sudo chmod +x /usr/local/bin/gpu-gpll-overclock.py

sudo tee /etc/systemd/system/gpu-overclock.service >/dev/null <<'EOF'
[Unit]
Description=GPU GPLL 1188 MHz
After=multi-user.target graphical.target
[Service]
Type=simple
ExecStart=/usr/bin/python3 /usr/local/bin/gpu-gpll-overclock.py
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOF
sudo systemctl enable --now gpu-overclock.service
```

The monitor loop is required because devfreq/SCMI may reset the mux during transitions. The 1188 MHz GPLL setting was stable at 1050 mV pre-2026-04-20 (1000 mV crashed within frames). Modifying BL31/EDK2 directly would mean rebuilding [edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588) UEFI — the userspace mux bypass achieves the same result without reflashing.

## Chromium with hardware video decode

Custom ungoogled-chromium 147.0.7727.116 with V4L2 stateless decode (rkvdec2 zero-copy MMAP+EXPBUF for H.264/HEVC/VP9) and a built-in Mali Valhall artifact bypass for VP9.

**Install the pacman package**:

```bash
wget https://github.com/dongioia/rock5bplus-rkvdec2/releases/download/v147.0.7727.116-3/ungoogled-chromium-147.0.7727.116-3-aarch64.pkg.tar.xz
sudo pacman -U ungoogled-chromium-147.0.7727.116-3-aarch64.pkg.tar.xz
```

**Wayland flags** in `~/.config/chromium-flags.conf` (chromium-launcher reads this; all desktop entries inherit):

```
--ozone-platform=wayland
--enable-features=AcceleratedVideoDecodeLinuxV4L2,AcceleratedVideoDecodeLinuxZeroCopyGL,WaylandWindowDecorations
--disable-features=UseChromeOSDirectVideoDecoder
--use-gl=angle
--use-angle=gles
```

Verify in `chrome://media-internals` while playing a video: `kVideoDecoderName: V4L2VideoDecoder` and `kIsPlatformVideoDecoder: true`.

### VP9 Mali Valhall artifact bypass (default-ON)

`SkYUVAInfo::PlaneConfig::kY_UV` produces a Skia Ganesh GL fragment shader that miscompiles on Mali Valhall + Mesa Panfrost for the two-sampler R8/RG8 plane layout, giving tile-level garbage on every VP9 stream — at any resolution, not only ≥1440p. The earlier hypothesis that the artifact was confined to VP9 ≥1440p is now invalidated; below 1080p the bug is invisible only because YouTube serves AV1 there and the VP9 path is not exercised. Kernel V4L2 decode itself is clean (`ffmpeg -hwaccel v4l2request` 500+ fps on 720p VP9), so the bug lives entirely in the Skia / ANGLE pipeline on Mali Valhall.

The build flips `kForceLibYUV` default-ON in `media/gpu/chromeos/video_decoder_pipeline.cc::PickDecoderOutputFormat`: `viable_candidate` is cleared, the LibYUV ImageProcessor converts NV12 → AR24 on the CPU (~3-8 ms/frame at 1440p on Cortex-A76), and Skia composes a single-plane RGBA `GL_TEXTURE_2D` — no YUVA shader, no artifacts. HW V4L2 decode is preserved.

Opt-out (debug only): `CHROMIUM_RK3588_FORCE_LIBYUV=0`. Upstream tracker: [issues.chromium.org/issues/503755157](https://issues.chromium.org/issues/503755157).

> If you only want a stock browser with GPU compositing (no HW video decode), the [Ungoogled Chromium Flatpak](https://flathub.org/apps/io.github.ungoogled_software.ungoogled_chromium) works with `--ozone-platform=wayland --use-gl=egl --enable-zero-copy --ignore-gpu-blocklist`. Pair it with `mpv --hwdec=v4l2request-copy` and a `mpv://` protocol handler for the VP9 path — see [`docs/bredos-wiki-browser-article.md`](docs/bredos-wiki-browser-article.md) for the full Flatpak + userscript walkthrough.

## Hand VP9 off to mpv (recommended for any browser)

The LibYUV bypass above keeps Chromium usable on VP9, but the bug it works around is in Skia / ANGLE on Mali Valhall and not in the kernel decode itself. The cleaner path — and the only path on stock browsers that did not get the bypass patch — is to let the browser handle navigation and hand the actual VP9 stream off to `mpv`. `mpv` talks directly to RKVDEC2 through libavcodec's `v4l2request` hwaccel, avoiding the Skia shader entirely.

This pattern works the same for the custom RK3588 Chromium build above and for any stock Chromium / Firefox / Flatpak browser.

> **Kernel prerequisite.** mpv hardware decode goes through the same RKVDEC2 driver path as Chromium. Without the VP9 kernel stack (chewitt VP9 Profile 0 + Profile 2 plus the `media: rkvdec: fix PM runtime teardown ordering in remove` cleanup — both included in [`linux-beryllium-rockchip` 7.1.0rc2-2](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/linux-beryllium-rockchip)) `mpv --hwdec=v4l2request-copy` will fall back to software just as Chromium does, and a stock Arch ARM / generic mainline kernel will not work either. The kernel comes first.

### Install mpv + a V4L2-aware FFmpeg

Arch ARM's stock `ffmpeg` ships TLS support but no `v4l2-request` hwaccel. Install Kwiboo's branch instead (packaged by NoDiskNoFun in beryllium-org):

```bash
sudo pacman -S mpv yt-dlp ffmpeg-v4l2-requests
```

If pacman refuses with `unable to satisfy dependency 'libplacebo.so=...' / 'libvpx.so=...'`, the Beryllium prebuilt binary was pinned against older SO names than your system now has. Rebuild locally against current libraries:

```bash
git clone --depth=1 https://github.com/beryllium-org/sbc-pkgbuilds.git
cd sbc-pkgbuilds/ffmpeg-v4l2-requests
makepkg -si    # ~30–60 min on Rock 5B+ A76 cores
```

After install, both checks below must return one line each:

```
ffmpeg -hwaccels  2>&1 | grep v4l2request
ffmpeg -protocols 2>&1 | grep https
```

A one-shot installer that does steps 1–5 below is at [`scripts/yt-mpv-setup.sh`](scripts/yt-mpv-setup.sh):

```bash
curl -fsSL https://raw.githubusercontent.com/dongioia/rock5bplus-rkvdec2/main/scripts/yt-mpv-setup.sh | bash
```

### mpv config

`~/.config/mpv/mpv.conf`:

```ini
hwdec=v4l2request-copy
vo=gpu-next,gpu
gpu-api=vulkan
gpu-context=waylandvk
cache=yes
demuxer-max-bytes=500M
ytdl-format=bestvideo[height<=?1080]+bestaudio/best
```

`v4l2request-copy` is the right value for Mali Valhall. The bare `v4l2request` zero-copy variant silently falls back to software — Mesa panthor's `EGL_EXT_image_dma_buf_import_modifiers` does not refresh the imported texture between frames for the linear NV12 buffers V4L2 CAPTURE exports. CPU readback adds a small overhead but stays well under software decode (BBB 720p VP9: 17 % CPU with `v4l2request-copy` versus 31 % software on Cortex-A76; 4K HEVC: ~68 fps, 2.27× realtime, ~4 s of a single core). If Vulkan misbehaves, fall back to `vo=gpu` + `gpu-context=wayland` (Panfrost GLES).

### `mpv://` protocol handler

Lets the browser launch a YouTube URL straight into mpv with a single bookmarklet click, or — combined with the userscript in the next subsection — automatically at 1080p and above.

```bash
sudo tee /usr/local/bin/open-in-mpv >/dev/null <<'SH'
#!/bin/bash
url="${1#mpv://play/}"; url="${url#mpv://}"
url=$(python3 -c "import sys,urllib.parse;print(urllib.parse.unquote(sys.argv[1]))" "$url" 2>/dev/null || echo "$url")
exec mpv --force-window=immediate "$url" &
SH
sudo chmod +x /usr/local/bin/open-in-mpv

cat > ~/.local/share/applications/open-in-mpv.desktop <<EOF
[Desktop Entry]
Type=Application
Name=Open in mpv
Exec=/usr/local/bin/open-in-mpv %u
NoDisplay=true
MimeType=x-scheme-handler/mpv;
EOF
xdg-mime default open-in-mpv.desktop x-scheme-handler/mpv
```

Bookmarklet (drop in the browser bookmarks bar — clicking it on any YouTube page opens that URL in mpv):

```
javascript:location.href='mpv://play/'+encodeURIComponent(location.href)
```

### Auto-redirect every VP9 stream (userscript)

The reason to hand off to mpv is **codec**, not resolution: the Skia GrYUVtoRGB miscompile on Mali Valhall hits any VP9 frame that goes through the GPU YUVA shader path, regardless of size. (The `kForceLibYUV` bypass in our chromium build avoids it; on stock chromium / Firefox / Flatpak it is unavoidable.) AV1 and H.264 streams render cleanly through the browser, so we want to redirect VP9 only.

Install [Tampermonkey](https://www.tampermonkey.net/) or Violentmonkey, then open this URL — the browser detects the `.user.js` extension and Tampermonkey prompts to install:

```
https://raw.githubusercontent.com/dongioia/rock5bplus-rkvdec2/main/scripts/yt-mpv-vp9.user.js
```

The installable file ([`scripts/yt-mpv-vp9.user.js`](scripts/yt-mpv-vp9.user.js)) sets `@updateURL` to the same path so Tampermonkey auto-updates on subsequent releases. Source (inline for reference):

```javascript
// ==UserScript==
// @name         YouTube → mpv for VP9 streams
// @match        https://www.youtube.com/watch*
// @run-at       document-idle
// @grant        none
// ==/UserScript==
(function () {
  const v = new URL(location.href).searchParams.get('v');
  if (!v) return;
  if (sessionStorage.getItem('ytmpv-skip-' + v) === '1') return;  // back-button safety
  let tries = 0;
  const probe = setInterval(() => {
    const player = document.querySelector('#movie_player');
    if (!player || typeof player.getStatsForNerds !== 'function') {
      if (++tries > 40) clearInterval(probe);  // give up after ~20 s
      return;
    }
    const stats = player.getStatsForNerds() || {};
    const codecs = stats.codecs || '';  // e.g. "vp09.00.40.08 (244) / mp4a.40.2 (140)"
    if (!codecs) return;
    clearInterval(probe);
    const isVP9 = /\bvp09\b/i.test(codecs);
    if (isVP9) {
      sessionStorage.setItem('ytmpv-skip-' + v, '1');  // don't loop if user comes back
      location.href = 'mpv://' + location.href;
    }
  }, 500);
})();
```

Why codec-not-resolution: YouTube negotiates codec based on resolution, network and account preferences — 480p clips can be VP9, 4K clips can be AV1. Resolution alone is the wrong gate. If you specifically want to keep small clips in the browser even when VP9, add `&& player.getPlaybackQuality && /^hd(1080|1440|2160|2880|4320)$/.test(player.getPlaybackQuality())` to the `isVP9` condition.

If you prefer a **bookmarklet** (one-shot, no auto-redirect):

```
javascript:location.href='mpv://'+encodeURIComponent(location.href)
```

The full Flatpak walkthrough — including a `ff2mpv` native-messaging alternative — lives in [`docs/bredos-wiki-browser-article.md`](docs/bredos-wiki-browser-article.md).

## VAAPI driver (libva-v4l2-request fork) — frozen / deprecated path

> ⚠ **Deprecated as of 2026-05-24.** Following Nicolas Dufresne's reply to the LKML announce thread on 2026-05-20, `libva-v4l2-request` is treated upstream as a dead-end: the libva abstraction adds a security surface and feature gaps that the V4L2 stateless API does not, and the V4L2 maintainers' direction is to drive RKVDEC2 directly through the native V4L2 stateless hwaccel in FFmpeg / GStreamer / Chromium. The `dongioia/libva-v4l2-request` fork is kept here as a historical reference (the FFmpeg-ported range coder and the VP9 spec § 6.3.10 `interp_filter` gate were useful learnings) but receives no further development. Use `mpv --hwdec=v4l2request-copy` against [`ffmpeg-v4l2-requests`](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests) instead — same RKVDEC2 driver path, no libva shim.

Historical record:

* Fork: [github.com/dongioia/libva-v4l2-request](https://github.com/dongioia/libva-v4l2-request) — branch `rk3588-vp9`, commit `ed4bc90`.
* Achievement: VP9 Profile 0 1080p decoded pixel-identical to libvpx across 3000 frames (framemd5 verified). `mpv --hwdec=vaapi-copy` ran at ~56 % CPU vs 113 % software on the same clip.
* Block: `mpv --hwdec=vaapi` zero-copy painted a solid blue frame via Mali Valhall EGL dmabuf import (Mesa panthor does not refresh the imported texture between frames for linear NV12 V4L2 CAPTURE buffers). CPU readback paths stayed pixel-correct. The display-path block was never resolved and is now moot because the recommended path no longer touches EGL dmabuf import.
* PKGBUILD never published to beryllium-org. It will not be.

For the equivalent decode pipeline through the supported path, see [Hand VP9 off to mpv](#hand-vp9-off-to-mpv-recommended-for-any-browser) above. Chromium 150 (stable target 2026-06-17) adds the missing piece on the browser side natively via [CL 7794420 (`d20dfa664`)](https://chromium-review.googlesource.com/c/chromium/src/+/7794420), so by mid-June a stock chromium 150 from archlinuxarm or any other aarch64 packager will decode VP9 through V4L2 stateless without any of the local backports here. Per chromiumdash, the commit `first_landed` in `150.0.7829.0` (M150 canary).

## Audio quirks

Hardware works out of the box. Two knobs:

- WirePlumber: `~/.config/wireplumber/wireplumber.conf.d/51-hdmi-rename.conf` renames "Audio interno" to `HDMI 0` / `HDMI 1`.
- ALSA UCM: drop `JackControl` from `/usr/share/alsa/ucm2/Rockchip/rk3588-es8316/HiFi.conf` so analog 3.5 mm stays visible regardless of jack detection. May need re-applying after `alsa-ucm-conf` upgrades.

See [docs/audio-setup.md](docs/audio-setup.md).

## NPU

Mainline (kernel 6.18+) ships the [Rocket](https://docs.kernel.org/accel/rocket/) driver. Just enable it:

```
CONFIG_DRM_ACCEL=y
CONFIG_DRM_ACCEL_ROCKET=m
```

Open-source [Mesa Teflon](https://docs.mesa3d.org/drivers/teflon.html) covers basic TFLite quantized CNN inference (single core, limited ops). YOLO/LLM/speech need proprietary [RKNN-Toolkit2](https://github.com/airockchip/rknn-toolkit2) + vendor BSP kernel — not on Beryllium OS. Full comparison: [docs/bredos-wiki-npu-article.md](docs/bredos-wiki-npu-article.md).

## Changelog

### 2026-05-24 — VAAPI fork frozen, chromium 150 fix confirmed in main

Following Nicolas Dufresne's 2026-05-20 LKML reply, `dongioia/libva-v4l2-request` is deprecated and will not see further work. The supported path is `mpv --hwdec=v4l2request-copy` against `ffmpeg-v4l2-requests` (no libva), and `gst-launch-1.0 ... ! v4l2slvp9dec ! waylandsink` for the browser-free flow. Chromium 150 is confirmed to ship the V4L2 stateless VP9 compressed-header path natively: gerrit [CL 7794420](https://chromium-review.googlesource.com/c/chromium/src/+/7794420) (`d20dfa664`, Jianfeng Liu) merged to `chromium/src` main on 2026-05-06; the follow-up [CL 7821681](https://chromium-review.googlesource.com/c/chromium/src/+/7821681) (`base::span` cleanup) merged on 2026-05-08. chromiumdash `fetch_commit` reports `first_landed: 150.0.7829.0`, deployment `canary: 150.0.7829.0`, `dev: 150.0.7838.2`, `beta: null` (M149 branch cut on 2026-05-04, two days before the merge, so M149-beta does not carry the fix), `stable: null` (M150 stable target 2026-06-17). The userscript [Hand VP9 off to mpv](#auto-redirect-every-vp9-stream-userscript) was rewritten to gate on the active codec via `getStatsForNerds().codecs` (match `vp09.*`) instead of resolution, since the underlying Skia GrYUVtoRGB miscompile on Mali Valhall hits any VP9 stream, not just ≥1080p.

### 2026-05-20 — PHASE 2j-D Mali Valhall EGL dmabuf diagnosis

mpv `--hwdec=vaapi` zero-copy + libva-v4l2-request via Mali Valhall EGL dmabuf import paints solid blue across the entire mpv window despite `/dev/video0` being open and the kernel decode completing — Mesa panthor does not refresh the imported texture between frames for our linear NV12 V4L2 CAPTURE exports. Verified across multiple grim snapshots (3/3 RGB(0,13,128) variance 0). Chromium `use_vaapi=true` rebuild parked indefinitely — same display path, same broken visual. CPU readback paths (`vaapi-copy`, `v4l2request-copy`) stay pixel-correct. Driver commits `c9fec0d`, `4b94a2d`, `ed4bc90` pushed to `dongioia/libva-v4l2-request rk3588-vp9`.

### 2026-05-19 — libva-v4l2-request VP9 pixel-perfect

Forked Bootlin's tree as `dongioia/libva-v4l2-request rk3588-vp9`. Ported the FFmpeg `vpx_rac` range coder + VP9 compressed-header parser, then gated `interp_filter` prob reads on `FILTER_SWITCHABLE` per VP9 spec § 6.3.10 (commit `3dab4cc`). Result: VP9 Profile 0 1080p decodes pixel-identical to libvpx across 3000 frames (framemd5 verified). `mpv --hwdec=vaapi-copy` runs at 56% CPU vs 113% software on the same clip.

### 2026-05-18 — VP9 VDPU381 reset cleanup accepted to stable

`media: rkvdec: fix PM runtime teardown ordering in remove` (Jonas Karlman, v2 with `Cc: <stable@vger.kernel.org>`) [accepted to stable on lore.kernel.org](https://lore.kernel.org/all/?q=20260518145414.64514-1-pavone.lawyer@gmail.com). Removes the SError panic seen on VDPU381 under heavy chromium VP9 submission, and incidentally removes the green-chroma motion-block artifact on RKVDEC2. Beryllium-org `linux-beryllium` PR #4 reconstructed on `7.1-rc2` with this patch and the chewitt VP9 + AV1 + HDMI series.

### 2026-05-16 — Chromium ImageProcessor AR24 picker + skip-GL-rk3588 patches

Internal rebuild on top of `-3`: filter `renderable_fourccs` to AR24 when `kForceLibYUV` is set (`video_decoder_pipeline.cc`), and skip the GL ImageProcessor backend on rk3588 board-DT probe (`image_processor_factory.cc`). HW V4L2 decode confirmed on the new 7.1-rc1 / 7.1-rc2 kernel via `chrome://media-internals`. Binary `bd0a9526…` deployed locally for testing; the publicly released package on the [release page](https://github.com/dongioia/rock5bplus-rkvdec2/releases) is still `-3`.

### 2026-05-11 — Chromium 147.0.7727.116-3 release

Pacman package with a board-aware LibYUV bypass and a slightly larger surface pool cap published as [v147.0.7727.116-3](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/v147.0.7727.116-3).

### 2026-05-10 — Chromium 147.0.7727.116-2 release

Pacman package with VP9 LibYUV bypass default-ON published on the [release page](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/v147.0.7727.116-2). Announced on [warpme/minimyth2#73](https://github.com/warpme/minimyth2/issues/73#issuecomment-4414864582) (Piotr Oniszczuk, JFLim1 — OPi5+ Mali-G610). The previous ANGLE root-cause hypothesis is formally falsified.

### 2026-05-09 — VP9 Mali Valhall fix shipped

`kForceLibYUV` flipped default-ON in the binary; sway launcher activated via `~/.config/chromium-flags.conf`. Upstream bug [503755157](https://issues.chromium.org/issues/503755157) updated.

### 2026-05-08 — Skia YUVA Ganesh GL bisected

Eight-layer bisect (kernel rkvdec → V4L2VideoDecoder → EGL DMABUF binding × 3 styles → standalone Mesa GLES2 C test → SkYUVColorSpace sweep → LibYUV bypass) localized the VP9 ≥1440p artifact to the `GrYUVtoRGBEffect` fragment shader Skia emits for `kY_UV` two-sampler R8/RG8 sources on Mali Valhall + Mesa Panfrost. Not kernel, not V4L2, not EGL attribs, not Mesa, not the YUV→RGB matrix.

### 2026-04-27 — Chromium 147.0.7727.116-1 clean rebuild

Ungoogled-chromium 147 from upstream PKGBUILD plus our RK3588 mods (NV12 forced renderable in `gpu_mojo_media_client_linux.cc`). Pacman package + binary published as [v147.0.7727.116-1](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/v147.0.7727.116-1). Issue [ungoogled-chromium-archlinux#330](https://github.com/ungoogled-software/ungoogled-chromium-archlinux/issues/330) opened. Firefox/LibreWolf/WebKitGTK paths to HW decode all ruled out (rockchip-vaapi+MPP, mpp-dkms, V4L2 stateless missing).

### 2026-04-25 — Linux 7.0 final stable (panthor 1.8)

Rebuilt 7.0.y stack (`#6 SMP PREEMPT Sat Apr 25 10:12:45 UTC 2026`). Panthor 1.8.0 driver replaces 1.7.0 — the rc3→final MCU regression seen on 2026-04-20 is gone, MCU boots clean, no `tick_work`/`queue_run_job` warnings, no page faults at AS0. Kernel `7.0.0+` is now the default GRUB entry; rc3-custom kept as fallback boot option.

### 2026-04-21 — GPU overclock disabled (kernel panic on current stack)

`gpu-overclock.service` (1188 MHz GPLL via CRU mux bypass, 1050 mV) became unstable after the 2026-04-20 pacman update (Mesa 26.0.4→26.0.5, vulkan-panfrost 26.0.5, xorg-server 21.1.22). Symptom on every kernel tried (6.19.1-bredos, 7.0.0-rc3+, 7.0 final): `panthor fb000000.gpu: Failed to boot MCU (status=fatal)` + unhandled page faults at AS0 within ~40 s, MCU unrecoverable. Investigations that did not help: downgrade Mesa/vulkan-panfrost back to 26.0.4, downgrade `linux-firmware{,-other,-whence}` (md5 of MCU FW identical between 20260309 and 20260410), restore the FIT image (EDK2/BL31/OP-TEE) via SD recovery. Service disabled, GPU back to stock OPP. Re-enable only after voltage curve / CRU mux sequence revisit.

### 2026-04-20 — Linux 7.0 final initial build

First rebase of 128 commits onto v7.0 (HDMI QP v4, Frattaroli color-format, Reichel USBDP+DP+PCIe suspend, Pueschel RGA3, Casanova V4L2 stateless tracing, Riesch RK3588 vicap, Cawston Rocket NPU, Beryllium defconfig, VP9 with altref + IOMMU fault fix). Kernel `7.0.0+` deploys clean (51 MB Image, 3538 modules, 6/6 V4L2 devices, HDMI 4K@60Hz). At this point panthor 1.7.0 was hitting MCU fatal vs rc3 — fixed by the panthor 1.8 rebuild on 04-25.

### 2026-04-07 — VP9 VDPU381 fixed + zero-copy HW decode in Chromium

Root cause of the VP9 kernel crash was a **stale `.o`** — the source file was missing from the kernel tree while the Makefile referenced it, so an old object compiled against different headers was linked. Fix: source files added to the tree, clean rebuild. Same day: first working V4L2 zero-copy hardware decode in Chromium on RK3588 mainline (148-canary). Earlier EXPBUF "discovery" was actually a wrong ioctl number (`0xc014560d` vs `0xc0405610`) — EXPBUF was always supported, the Mesa NV12 GBM detour was unnecessary.

### 2026-04-05 — Kernel 7.0-rc3 perf optimization

`KCFLAGS='-march=armv8.2-a+crypto+fp16+dotprod -mtune=cortex-a76'`, `schedutil` default, THP madvise, ZRAM LZ4, ZSWAP, sched-ext + BTF, ARM64 errata trimmed 31→9. Measured: +18.8% SHA256, +17.3% AES, +6.1% memcpy. Branch [`7.0.y`](https://github.com/beryllium-org/linux-beryllium/tree/7.0.y).

<details>
<summary>Older history (7.0-rc1, 6.19.1, 6.19-rc8)</summary>

- **2026-03-10 — Linux 7.0-rc3**: HDMI 2.0 patches updated to v4 series (Ciocaltea), GPU OPP fix added.
- **2026-02-28 — Linux 7.0-rc1**: 18 of 25 patches from the 6.19.1 build went upstream — only VP9 + HPD events still custom. VP9 source needed register field renames (`reg009.dec_mode` → `reg009_dec_mode.dec_mode`, etc.) and per-field block-gating writes to match the upstream RKVDEC2 merge.
- **2026-02-17 — Linux 6.19.1 stable**: rebased + 10 patches added (AV1 CDEF/tx-mode/tile-info, VOP2 mode_valid + ratelimited log, RKVDEC stack fixes, VPU power-domain regulators, HDMI VSI/SPD InfoFrames).
- **2026-02-13 — Linux 6.19-rc8**: initial release — RKVDEC2/VDPU381 v9, HDMI 2.0 scrambling, VP9, NPU config, audio setup, browser guide.

</details>

## Beryllium OS contributions

The BredOS project transitioned to Beryllium OS during 2026; the upstream tracker moved from `BredOS/sbc-pkgbuilds` to `beryllium-org/sbc-pkgbuilds`. The early contributions below pre-date the transition and keep their original BredOS URLs; later items target the Beryllium organisation directly.

| # | Type | Target | Description | Link |
|---|---|---|---|---|
| 1 | PR | sbc-pkgbuilds (BredOS) | `DEBUG_PREEMPT=n` + `PREEMPT_DYNAMIC=n` (~9% MT) | [#16](https://github.com/BredOS/sbc-pkgbuilds/pull/16) |
| 2 | PR | sbc-pkgbuilds (BredOS) | Cleanup: 4 orphaned files | [#17](https://github.com/BredOS/sbc-pkgbuilds/pull/17) |
| 3 | Issue | sbc-pkgbuilds (BredOS) | Kernel 7.0 patch triage | [#18](https://github.com/BredOS/sbc-pkgbuilds/issues/18) |
| 4 | Issue | sbc-pkgbuilds (BredOS) | VOP2 `POST_BUF_EMPTY` IRQ fix | [#19](https://github.com/BredOS/sbc-pkgbuilds/issues/19) |
| 5 | Comment | sbc-pkgbuilds (BredOS) | `dtbs_install` missing `ARCH=arm64` on x86 host | [#8](https://github.com/BredOS/sbc-pkgbuilds/issues/8) |
| 6 | PR | linux-beryllium (Beryllium) | 7.1-rc2 VP9 stack + PM runtime fix | [#4](https://github.com/beryllium-org/linux-beryllium/pull/4) |
| 7 | PR | sbc-pkgbuilds (Beryllium) | `linux-beryllium-rockchip` 7.1.0rc2-2 bump | [#3](https://github.com/beryllium-org/sbc-pkgbuilds/pull/3) |
| 8 | Patch | LKML (linux-media) | `media: rkvdec: fix PM runtime teardown ordering in remove` (Jonas Karlman, Pavone Tested-by) | [lore.kernel.org thread](https://lore.kernel.org/all/?q=20260518145414.64514-1-pavone.lawyer@gmail.com) |
| 9 | Patch | LKML (sound) | `ALSA: pcm_drm_eld: rate-limit ELD parsing errors` (accepted by Takashi Iwai) | [lore.kernel.org thread](https://lore.kernel.org/all/?q=ALSA%3A+pcm_drm_eld%3A+rate-limit) |
| 10 | PR | muffin (upstream) | Multi-GPU primary selection via udev tag | [linuxmint/muffin#811](https://github.com/linuxmint/muffin/pull/811) |
| 11 | Issue | muffin (upstream) | Multi-GPU RK3588 (Mali-C510 + Mali-G610) bug analysis | [linuxmint/muffin#812](https://github.com/linuxmint/muffin/issues/812) |

## Credits

This project rests entirely on the work of the upstream Linux kernel community and the RK3588 enablement effort:

- **[Detlev Casanova](https://gitlab.collabora.com/detlev.casanova)** (Collabora) — RKVDEC2/VDPU381 v9 driver, RPS fix, DTS nodes (merged in Linux 7.0).
- **[Cristian Ciocaltea](https://lore.kernel.org/r/20260119-dw-hdmi-qp-scramb-v3-0-bd8611730fc1@collabora.com/)** (Collabora) — DW HDMI QP bridge, HDMI 2.0 SCDC scrambling, VSI/SPD InfoFrames.
- **[Sebastian Reichel](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Sebastian+Reichel)** (Collabora) — Rock 5B+ DTS, dual HDMI, RK3588 enablement.
- **[Heiko Stuebner](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Heiko+Stuebner)** — Rockchip platform maintainer.
- **[Benjamin Gaignard](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Benjamin+Gaignard)** (Collabora) — Hantro AV1 decoder + bug fixes.
- **[Andy Yan](https://lore.kernel.org/linux-rockchip/?q=Andy+Yan)** (Rockchip) — VOP2 `mode_valid` + bridge connector hook.
- **[Shawn Lin](https://lore.kernel.org/linux-rockchip/?q=Shawn+Lin)** (Rockchip) — VPU power-domain regulator fix.
- **[Arnd Bergmann](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Arnd+Bergmann)** — RKVDEC stack-overflow fixes.
- **[Hans Verkuil](https://git.linuxtv.org/)** — Linux media maintainer; merged the RKVDEC2 series.
- **[Tomeu Vizoso](https://gitlab.freedesktop.org/tomeu)** — Rocket NPU driver (kernel + Mesa).
- **[dvab-sarma](https://github.com/dvab-sarma)** — early community VP9 VDPU381 implementation.
- **[chewitt](https://github.com/chewitt)** (LibreELEC) — the VP9 / AV1 / IOMMU patchset shipping in the 7.1-rc2 production branch (VP9 Profile 0 + Profile 2, AV1 hantro-vpu + Verisilicon IOMMU bundle).
- **[Jonas Karlman (Kwiboo)](https://github.com/Kwiboo)** — FFmpeg `v4l2-request` patches and the `media: rkvdec: fix PM runtime teardown ordering in remove` cleanup that removed the VP9 VDPU381 green-chroma artifact ([lore](https://lore.kernel.org/all/?q=20260518145414.64514-1-pavone.lawyer@gmail.com)).
- **[NoDiskNoFun](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests)** — `ffmpeg-v4l2-requests` Beryllium OS package.
- **[Collabora](https://www.collabora.com/)** — most of the RK3588 mainline enablement; their [mainline status page](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md) is the canonical reference.
- **[Beryllium OS](https://beryllium.gr/)** — Arch Linux ARM distribution + community ([wiki](https://wiki.beryllium.gr/), [Discord](https://discord.com/channels/954056266722979851)).
- **[7Ji](https://github.com/7Ji-PKGBUILDs)** — `linux-aarch64-7ji` kernel packages.
- **[Radxa](https://radxa.com/)** — Rock 5B+ hardware ([docs](https://docs.radxa.com/en/rock5/rock5b)).

## License

Kernel patches keep their original licenses (GPL-2.0). Scripts and configs in this repo are MIT.
