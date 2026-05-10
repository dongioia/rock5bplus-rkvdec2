# Rock 5B+ Mainline Kernel — Full Hardware Support

Patches, configs and tools for the **Radxa Rock 5B+** on mainline Linux: 4K hardware video decode, HDMI 2.0 4K@60Hz, HDMI audio, GPU overclock, and an RK3588-aware Chromium build.

> **Status (2026-05)**: Linux **7.0-rc3** custom kernel running on Rock 5B+. The RKVDEC2/VDPU381 driver (H.264/HEVC) is upstream in Linux 7.0; only HDMI 2.0 scrambling, VP9 (community), HPD optimization and a GPU OPP fix are still applied. **Chromium 147.0.7727.116-2** with the VP9 Mali Valhall artifact fix is published as a [release](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/v147.0.7727.116-2). A full Linux 7.0 final rebase exists but ships with an upstream Panthor MCU regression and is not the default boot entry.

## What works

| Feature | Status | Notes |
|---|---|---|
| H.264 / HEVC / VP9 / AV1 4K decode | ✅ | RKVDEC2 zero-copy (MMAP+EXPBUF). VP9 community, Profile 0 |
| HDMI 2.0 4K@60Hz | ✅ | SCDC scrambling v4 (Ciocaltea, Collabora) |
| HDMI audio + analog (ES8316) | ✅ | PipeWire + UCM tweak |
| GPU Panthor / Mali-G610 | ✅ | Vulkan 1.4 PanVK; 1188 MHz via [GPLL overclock](#gpu-overclock-1188-mhz) (default 850 MHz) |
| Dual HDMI, Ethernet 2.5GbE, WiFi 6 (RTL8852BE), Bluetooth, HDMI-RX | ✅ | All mainline |
| NPU (3 cores) | ⚠️ kernel ready | Open-source Rocket+Teflon: basic CNN. Full ops need proprietary RKNN+BSP |
| RGA3 | ❌ | No upstream driver (RGA2 works) |

## Active patches (Linux 7.0-rc3)

The RKVDEC2/VDPU381 v9 series, RPS fix, DTS nodes, RKVDEC stack fixes, AV1 fixes, VOP2 fixes and VPU power domain fix are all **upstream in Linux 7.0** and no longer applied. Only the patches below are still needed:

| Patch | Author | Purpose |
|---|---|---|
| `patches/display/v4-ciocaltea/0001..0004-*.mbox` | [Cristian Ciocaltea](https://lore.kernel.org/r/20260119-dw-hdmi-qp-scramb-v3-0-bd8611730fc1@collabora.com/) (Collabora) | DW HDMI QP `detect_ctx` + SCDC scrambling + per-connector HPD → enables 4K@60Hz, 1080p@120Hz, all HDMI 2.0 modes. Patch 0003 needs manual context fixup against rc3 |
| `patches/vpu/vp9-vdpu381-adapted.patch` + `rkvdec-vdpu381-vp9.{c,h}` | [dvab-sarma](https://github.com/dvab-sarma/android_kernel_rk_opi/tree/android-16.0-hwaccel-testing) (community), adapted | VP9 VDPU381 hardware decode (Profile 0). Source files copied into the tree — applying as a patch leaves a stale `.o` and crashes |
| GPU OPP fix (config) | this repo | Mali-G610 frequency table fix; required for the [1188 MHz overclock](#gpu-overclock-1188-mhz) |

Linux config: [`configs/rock5b_7.0-rc3.config`](configs/rock5b_7.0-rc3.config) (netfilter case-insensitive modules disabled for macOS build).

## Build

Apple Silicon Mac (Docker, native arm64) or any aarch64 Linux box:

```bash
# 1. Kernel source
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git src/linux
cd src/linux && git checkout v7.0-rc3

# 2. Apply active patches
for p in ../../patches/display/v4-ciocaltea/000{1,2,3,4}-*.mbox; do git am "$p"; done
git apply ../../patches/vpu/vp9-vdpu381-adapted.patch
cp ../../patches/vpu/rkvdec-vdpu381-vp9.{c,h} drivers/media/platform/rockchip/rkvdec/

# 3. Configure + build (Docker on macOS, or native ARM)
cp ../../configs/rock5b_7.0-rc3.config .config
make ARCH=arm64 olddefconfig
../../scripts/build.sh Image 12     # or: make -j$(nproc) Image modules dtbs
```

`scripts/build.sh setup` builds the Docker image once. On macOS the script uses a `git archive` tarball inside the container to dodge HFS+/APFS case-insensitivity collisions (e.g. `ipt_ECN.h` vs `ipt_ecn.h`).

## Deploy to Rock 5B+

```bash
BOARD=<ip> USER=<user> KVER=7.0.0-rc3-custom

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

## GPU overclock (1188 MHz)

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

The monitor loop is required because devfreq/SCMI may reset the mux during transitions. To revert: `sudo systemctl disable --now gpu-overclock.service`. The 1188 MHz GPLL setting is stable at 1050 mV; 1000 mV crashes within frames. Modifying BL31/EDK2 directly would mean rebuilding [edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588) UEFI — the userspace mux bypass achieves the same result without reflashing.

> Disabled by default on the rock5b after a `pacman -Syu` triggered Panthor MCU faults — re-enable only on a known-stable kernel/firmware combo.

## Chromium with hardware video decode

Custom ungoogled-chromium 147.0.7727.116 with V4L2 stateless decode (rkvdec2 zero-copy MMAP+EXPBUF for H.264/HEVC/VP9) and a built-in Mali Valhall artifact bypass for VP9 ≥1440p.

**Install the pacman package**:

```bash
wget https://github.com/dongioia/rock5bplus-rkvdec2/releases/download/v147.0.7727.116-2/ungoogled-chromium-147.0.7727.116-2-aarch64.pkg.tar.xz
sudo pacman -U ungoogled-chromium-147.0.7727.116-2-aarch64.pkg.tar.xz
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

### VP9 Mali Valhall artifact fix (default-ON)

`SkYUVAInfo::PlaneConfig::kY_UV` produces a Skia Ganesh GL fragment shader whose sample stage miscompiles on Mali Valhall + Mesa Panfrost for asymmetric Y/UV planes (e.g. 2560×1472 + 1280×736), giving tile-level garbage on VP9 ≥1440p — kernel V4L2 decode is fine (`ffmpeg -hwaccel v4l2request` 134 fps clean), the bug lives entirely in the GLES proxy path on Mali Valhall.

The build flips `kForceLibYUV` default-ON in `media/gpu/chromeos/video_decoder_pipeline.cc::PickDecoderOutputFormat`: `viable_candidate` is cleared, the LibYUV ImageProcessor converts NV12 → AR24 on the CPU (~3-8 ms/frame at 1440p on Cortex-A76), and Skia composes a single-plane RGBA `GL_TEXTURE_2D` — no YUVA shader, no artifacts. HW V4L2 decode is preserved.

Opt-out (debug only): `CHROMIUM_RK3588_FORCE_LIBYUV=0`. Upstream tracker: [issues.chromium.org/issues/503755157](https://issues.chromium.org/issues/503755157).

> If you only want a stock browser with GPU compositing (no HW video decode), the [Ungoogled Chromium Flatpak](https://flathub.org/apps/io.github.ungoogled_software.ungoogled_chromium) works with `--ozone-platform=wayland --use-gl=egl --enable-zero-copy --ignore-gpu-blocklist`.

## Media stack (mpv + FFmpeg v4l2-request)

Stock FFmpeg lacks `v4l2-request`. On Beryllium OS:

```bash
sudo pacman -S ffmpeg-v4l2-requests   # Kwiboo's branch, packaged by NoDiskNoFun
```

`~/.config/mpv/mpv.conf`:

```ini
hwdec=v4l2request
vo=gpu-next,gpu
gpu-api=vulkan
gpu-context=waylandvk
cache=yes
demuxer-max-bytes=500M
ytdl-format=bestvideo[height<=?1080]+bestaudio/best
```

If Vulkan misbehaves: `vo=gpu` + `gpu-context=wayland` (Panfrost GLES). 4K HEVC HW decode lands at ~68 fps, 2.27× realtime, ~4 s of one CPU core (vs ~43 s across all cores in software).

### `mpv://` browser handler

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

Bookmarklet: `javascript:location.href='mpv://play/'+encodeURIComponent(location.href)`.

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

### 2026-05-10 — Chromium 147.0.7727.116-2 release

Pacman package with VP9 LibYUV bypass default-ON published on the [release page](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/v147.0.7727.116-2). Announced on [warpme/minimyth2#73](https://github.com/warpme/minimyth2/issues/73#issuecomment-4414864582) (Piotr Oniszczuk, JFLim1 — OPi5+ Mali-G610). The previous ANGLE root-cause hypothesis is formally falsified.

### 2026-05-09 — VP9 Mali Valhall fix shipped

`kForceLibYUV` flipped default-ON in the binary; sway launcher activated via `~/.config/chromium-flags.conf`. Upstream bug [503755157](https://issues.chromium.org/issues/503755157) updated.

### 2026-05-08 — Skia YUVA Ganesh GL bisected

Eight-layer bisect (kernel rkvdec → V4L2VideoDecoder → EGL DMABUF binding × 3 styles → standalone Mesa GLES2 C test → SkYUVColorSpace sweep → LibYUV bypass) localized the VP9 ≥1440p artifact to the `GrYUVtoRGBEffect` fragment shader Skia emits for `kY_UV` two-sampler R8/RG8 sources on Mali Valhall + Mesa Panfrost. Not kernel, not V4L2, not EGL attribs, not Mesa, not the YUV→RGB matrix.

### 2026-04-27 — Chromium 147.0.7727.116-1 clean rebuild

Ungoogled-chromium 147 from upstream PKGBUILD plus our RK3588 mods (NV12 forced renderable in `gpu_mojo_media_client_linux.cc`). Pacman package + binary published as [v147.0.7727.116-1](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/v147.0.7727.116-1). Issue [ungoogled-chromium-archlinux#330](https://github.com/ungoogled-software/ungoogled-chromium-archlinux/issues/330) opened. Firefox/LibreWolf/WebKitGTK paths to HW decode all ruled out (rockchip-vaapi+MPP, mpp-dkms, V4L2 stateless missing).

### 2026-04-20 — Linux 7.0 final built (Panthor MCU regression)

Full rebase of 128 commits onto v7.0 (HDMI QP v4, Frattaroli color-format, Reichel USBDP+DP+PCIe suspend, Pueschel RGA3, Casanova V4L2 stateless tracing, Riesch RK3588 vicap, Cawston Rocket NPU, Beryllium defconfig, VP9 with altref + IOMMU fault fix). Kernel `7.0.0+` deploys clean (51 MB Image, 3538 modules, 6/6 V4L2 devices, HDMI 4K@60Hz). **Upstream regression**: Panthor MCU boots `status=fatal` (`tick_work`, `queue_run_job` warnings, page faults) — reproduced on stock v7.0; our patches don't touch panthor. GRUB stays on `linux-7.0-rc3-custom`; a real `v7.0-rc3..v7.0` bisect over `panthor/`, `scheduler/`, `dma-buf/` is pending.

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

| # | Type | Target | Description | Link |
|---|---|---|---|---|
| 1 | PR | sbc-pkgbuilds | `DEBUG_PREEMPT=n` + `PREEMPT_DYNAMIC=n` (~9% MT) | [#16](https://github.com/BredOS/sbc-pkgbuilds/pull/16) |
| 2 | PR | sbc-pkgbuilds | Cleanup: 4 orphaned files | [#17](https://github.com/BredOS/sbc-pkgbuilds/pull/17) |
| 3 | Issue | sbc-pkgbuilds | Kernel 7.0 patch triage | [#18](https://github.com/BredOS/sbc-pkgbuilds/issues/18) |
| 4 | Issue | sbc-pkgbuilds | VOP2 `POST_BUF_EMPTY` IRQ fix | [#19](https://github.com/BredOS/sbc-pkgbuilds/issues/19) |
| 5 | Comment | sbc-pkgbuilds | `dtbs_install` missing `ARCH=arm64` on x86 host | [#8](https://github.com/BredOS/sbc-pkgbuilds/issues/8) |
| 6 | PR | muffin (upstream) | Multi-GPU primary selection via udev tag | [linuxmint/muffin#811](https://github.com/linuxmint/muffin/pull/811) |
| 7 | Issue | muffin (upstream) | Multi-GPU RK3588 (Mali-C510 + Mali-G610) bug analysis | [linuxmint/muffin#812](https://github.com/linuxmint/muffin/issues/812) |

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
- **[dvab-sarma](https://github.com/dvab-sarma)** — community VP9 VDPU381 implementation.
- **[Jonas Karlman (Kwiboo)](https://github.com/Kwiboo)** — FFmpeg `v4l2-request` patches.
- **[NoDiskNoFun](https://github.com/BredOS/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests)** — `ffmpeg-v4l2-requests` Beryllium OS package.
- **[Collabora](https://www.collabora.com/)** — most of the RK3588 mainline enablement; their [mainline status page](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md) is the canonical reference.
- **[Beryllium OS](https://beryllium.gr/)** — Arch Linux ARM distribution + community ([wiki](https://wiki.beryllium.gr/), [Discord](https://discord.com/channels/954056266722979851)).
- **[7Ji](https://github.com/7Ji-PKGBUILDs)** — `linux-aarch64-7ji` kernel packages.
- **[Radxa](https://radxa.com/)** — Rock 5B+ hardware ([docs](https://docs.radxa.com/en/rock5/rock5b)).

## License

Kernel patches keep their original licenses (GPL-2.0). Scripts and configs in this repo are MIT.
