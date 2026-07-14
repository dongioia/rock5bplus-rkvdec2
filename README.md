# Rock 5B+ Mainline Kernel — Full Hardware Support

Patches, configs and tools for the **Radxa Rock 5B+** on mainline Linux: 4K hardware video decode, HDMI 2.0 4K@60Hz, HDMI audio, GPU overclock, and an RK3588-aware Chromium build.

> **Status (2026-07-12)**: Linux **7.1 final** running on Rock 5B+ (`7.1.0-beryllium+`). The current kernel is built on [Collabora's `rockchip-v7.1`](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/linux/-/tree/rockchip-v7.1) (a pinned 7.1.0 branch carrying the full RK3588 enablement: refactored RKVDEC2 VDPU381/383, Hantro AV1, VOP2, dw-hdmi-qp + FRL, RGA2, IOMMU) plus a small downstream set: the Beryllium defconfig, a board `sync_state` DTS fix, two ASoC log-noise fixes, and the `dw-hdmi-qp` N-coefficient / `dw_dp` `sync_state` work-in-progress. Two things make this build different from earlier ones: **`CONFIG_VSI_IOMMU=y`** binds the AV1 VPU's IOMMU so AV1 decode allocates through it instead of CMA (no more CMA exhaustion under sustained decode — validated, `CmaFree` flat), and the **Rocket NPU** is enabled and probes (`/dev/accel/accel0`). Maximal hardware: HDMI CEC, USB-C DP PHY, UFS, SARADC, SPI-NOR controller, SAI audio, camera interface, ftrace + V4L2 tracepoints. The kernel deploys into its own isolated slot (`/usr/lib/modules/7.1.0-beryllium+`, dedicated `vmlinuz-linux-beryllium` + initramfs) so it never overwrites an existing kernel; a previous kernel stays as a GRUB fallback. **Stock ArchLinuxARM Chromium 150** now plays VP9 / H.264 / HEVC / AV1 in hardware with a clean picture — the VP9 Mali green artifact was fixed upstream in the Chromium 150 ANGLE roll, so no custom build is needed (see [Browser video](#browser-video--stock-chromium-150)). The older custom 147 build with the LibYUV bypass stays published as a [release](https://github.com/dongioia/rock5bplus-rkvdec2/releases) for pre-150 setups, and `mpv --hwdec=v4l2request-copy` against [`ffmpeg-v4l2-requests`](https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests) remains a clean alternative. A companion VAAPI driver fork lives at [dongioia/libva-v4l2-request](https://github.com/dongioia/libva-v4l2-request) (branch `rk3588-vp9`), pixel-perfect on VP9 Profile 0 1080p via the `vaapi-copy` path. **GPU overclock currently disabled** — the 1188 MHz GPLL service triggers panthor MCU fatal / kernel panic on the post-2026-04-20 Mesa/firmware combo.

## What works

| Feature | Status | Notes |
|---|---|---|
| H.264 / HEVC / VP9 / AV1 4K decode | ✅ | RKVDEC2 zero-copy (MMAP+EXPBUF). AV1 on Hantro VPU with the Verisilicon IOMMU bound (`CONFIG_VSI_IOMMU=y`) — no CMA exhaustion. **VP9 on VDPU381 is a downstream restore** (not in the Collabora base — see kernel composition). **VP9 Profile 2 (10-bit)** decodes in HW too, via a patch from [@sky-rk3588](https://github.com/sky-rk3588). In-browser HW decode works on stock Chromium 150 (VP9 green fixed upstream); `mpv --hwdec=v4l2request-copy` is the alternative |
| HDMI 2.0 4K@60Hz | ✅ | SCDC scrambling v4 (Ciocaltea, Collabora) |
| HDMI audio + analog (ES8316) | ✅ | PipeWire + UCM tweak |
| GPU Panthor / Mali-G610 | ✅ | Vulkan 1.4 PanVK; 850 MHz default. [1188 MHz GPLL overclock](#gpu-overclock-1188-mhz-disabled) currently disabled (kernel panic on current stack) |
| Dual HDMI, Ethernet 2.5GbE, WiFi 6 (RTL8852BE), Bluetooth, HDMI-RX | ✅ | All mainline |
| NPU (3 cores) | ✅ kernel | Rocket driver enabled and probing (`/dev/accel/accel0`). Open-source Rocket+Teflon: basic CNN. Full ops need proprietary RKNN+BSP |
| RGA2 | ✅ | mem2mem 2D blit/scale/CSC. RGA3 multi-core still pending upstream (Pengutronix) |

## Kernel composition (Linux 7.1 — `beryllium-complete`)

The current kernel is [Collabora's `rockchip-v7.1`](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/linux/-/tree/rockchip-v7.1) (a pinned 7.1.0 branch, not the rebasing `rockchip-devel`) plus a small downstream set. Most of the RK3588 video and display enablement — refactored RKVDEC2 (VDPU381/383 H.264/HEVC), Hantro AV1, VOP2, `dw-hdmi-qp` + FRL, RGA2, IOMMU including the `vsi-iommu` driver — comes from the base. The base does **not** carry VP9 for VDPU381 (it was never upstreamed); that backend is restored downstream (see below).

| Item | Source | Purpose |
|---|---|---|
| Collabora `rockchip-v7.1` (base) | [Collabora](https://gitlab.collabora.com/hardware-enablement/rockchip-3588) | RKVDEC2 H.264/HEVC (VDPU381/383), Hantro AV1, VOP2, dw-hdmi-qp + HDMI 2.1 FRL, `samsung-hdptx`, USBDP, PCIe, RGA2, IOMMU. **No VP9 on VDPU381** (never upstreamed) |
| VP9 VDPU381 backend (restore) | this repo | `media: rkvdec: add VP9 VDPU381 decoder support` (based on dvab-sarma) + altref/segmap 2K+ fix. Re-adds the VP9 entry to `vdpu381_coded_fmts[]` + the `rkvdec-vdpu381-vp9.c` backend the base lacks. Restores `/dev/video0` VP9 (`VP9F`) + `v4l2slvp9dec`. Validated: HW decode clean in mpv (`--hwdec=v4l2request-copy`) and GStreamer |
| VP9 Profile 2 (10-bit) | [@sky-rk3588](https://github.com/sky-rk3588), tested here | Adds Profile 2 + `NV15` 10-bit output to the VDPU381 VP9 backend: `get_image_fmt()` picks NV12/NV15 from the frame `bit_depth`, stride taken from the buffer `bytesperline`, and `image_fmt` defaults to 8-bit for VP9 so Chromium's zero-copy import stays clean. 10-bit VP9 decodes in HW via `v4l2slvp9dec` / mpv; validated on Rock 5B+ (8-bit unchanged, no IOMMU faults). Being submitted to linux-media |
| VP9 MV/ref stride (portrait) | [@sky-rk3588](https://github.com/sky-rk3588), tested here | Fixes portrait VP9 corruption (every YouTube Short): the MV/ref luma pitch is taken from the buffer `bytesperline` instead of the bitstream display width, plus a 64-byte `bytesperline` align. Byte-identical vs libvpx on Rock 5B+ (8-bit, landscape and H264 unchanged). Folded into Beryllium as [PR #8](https://github.com/beryllium-org/linux-beryllium/pull/8) |
| `CONFIG_VSI_IOMMU=y` (config) | this repo | Binds the AV1 VPU's IOMMU (`fdca0000.iommu`) so the VPU (`fdc70000.video-codec`) allocates via IOVA scatter-gather, not CMA. Validated `CmaFree`-flat under sustained AV1 decode. (Collabora ships `vsi-iommu` as `=m`; the upstream enable lands in mainline 7.2-rc1.) |
| Beryllium defconfig + max-HW fragment | this repo | `beryllium_rk3588_defconfig` + ftrace/V4L2 tracepoints, HDMI CEC, USB-C DP PHY, UFS, SARADC, SPI-NOR, SAI, camera interface, Rocket NPU, `MODULE_ALLOW_BTF_MISMATCH=y` |
| Board `sync_state` DTS fix | this repo | Disable nodes that block `sync_state()` on Rock 5B+ |
| `dw-hdmi-qp` N-coefficients / `dw_dp` `sync_state` (WIP) | this repo | HDMI audio N-table for 497.75 MHz pclk; `dw_dp` no-op `sync_state` callback |
| ASoC log-noise fixes (2) | this repo | `hdmi-codec` / `soc-utils` stop logging benign `-ENOTCONN` on disconnect |

Earlier kernels carried a separate chewitt VP9/AV1 patch stack and a `media: rkvdec: fix PM runtime teardown ordering in remove` cleanup on top of the old monolithic `rkvdec.c`. The `rockchip-v7.1` base refactored RKVDEC2 into per-variant files. Its `vdpu381_coded_fmts[]` lists **only H.264 + HEVC** — VP9 on VDPU381 was never upstreamed, so it is **not** in the base. The squash onto `rockchip-v7.1` therefore dropped VP9 hardware decode on RK3588 (`/dev/video0` stopped advertising `VP9F`). We restore it downstream — our own VP9 VDPU381 backend (`media: rkvdec: add VP9 VDPU381 decoder support`, based on dvab-sarma) plus the altref/segmap fix for 2K+ decode — re-applied on top of the base (table row below). The historical rc3-era patches (`patches/display/v4-ciocaltea/`, `patches/vpu/`) are kept as reference for rebuilding off mainline 7.0-rc3; the 7.0.y stack is preserved in the [`7.0.y`](https://github.com/beryllium-org/linux-beryllium/tree/7.0.y) branch.

## Build the kernel

Two ways to build, depending on where you are. **Path A (cross-build in Docker)** is what this repo automates — fast, reproducible, runs on a Mac or any aarch64 Linux box. **Path B (native on the board)** is for users who just want to rebuild on the Rock 5B+ itself from the packaged PKGBUILD.

Either way the kernel is the same: Collabora `rockchip-v7.1` as the base + the small downstream set from the table above (defconfig, `CONFIG_VSI_IOMMU=y`, the VP9 VDPU381 restore, DTS/ASoC fixes).

### Path A — cross-build in Docker (recommended)

**1. Set up the build container.** The tree must live in a Docker *volume*, never on a macOS host path: HFS+/APFS is case-insensitive and corrupts files like `xt_RATEEST.h` vs `xt_rateest.h`. The helper handles this — it builds an Arch Linux ARM image (`rock5b-dev`) and keeps the kernel checkout in the named volume `linux-beryllium-tree`, mounted at `/work/linux`.

```bash
git clone https://github.com/dongioia/rock5bplus-rkvdec2.git && cd rock5bplus-rkvdec2
./docker/run.sh shell            # first run builds the image + clones the kernel into the volume
```

**2. Build.** Pass the config explicitly — the bare `build-kernel` has a stale default:

```bash
KIMG_TAG=beryllium ./docker/run.sh build-kernel beryllium-mainline.config beryllium
```

This copies `configs/beryllium-mainline.config` → `.config`, runs `olddefconfig`, and builds `Image modules dtbs` with `KCFLAGS='-march=armv8.2-a+crypto+fp16+dotprod+rcpc -mtune=cortex-a76'`. Output: `deploy/kernel-latest.tar.gz`. The config is regenerated from the in-tree `beryllium_rk3588_defconfig` + `configs/fragment-maxhw-trace.config`.

> **Two build gotchas that will bite you.** BTF must be preserved on module strip (`INSTALL_MOD_STRIP="--strip-debug --keep-section=.BTF"`, already set) — without it `nfnetlink`/`r8169` fail BTF validation and networking drops at boot. And after changing any built-in (`=y`) config, run `make clean` first: an incremental build relinks `vmlinux` with new BTF but leaves unchanged modules pointing at the old offsets, so they fail to load. `CONFIG_MODULE_ALLOW_BTF_MISMATCH=y` is in the config as a safety net, but a clean rebuild is the real fix.

> **Iterating on one driver?** A module-only change (e.g. the rkvdec VP9 backend) needs neither a full build nor a deploy of the whole kernel — `make drivers/media/platform/rockchip/rkvdec/` rebuilds just `rockchip-vdec.ko`, which you can copy to the board's `/usr/lib/modules/<kver>/.../` and `depmod -a`. vmlinux is untouched, so there's no BTF risk.

### Path B — native build on the Rock 5B+ (PKGBUILD)

If you'd rather build on the board and install through pacman, use the packaged kernel from sbc-pkgbuilds:

```bash
sudo pacman -S --needed base-devel
git clone https://github.com/beryllium-org/sbc-pkgbuilds.git
cd sbc-pkgbuilds/linux-beryllium-rockchip
makepkg -si                      # builds + installs linux-beryllium-rockchip + headers
```

This is slower (a full kernel build on the A76 cores) but needs no cross-compile setup, and `pacman` handles install + GRUB for you. Skip straight to **Reboot** below.

## Deploy + GRUB fallback (Path A)

`scripts/deploy-kernel-tagged.sh` ships the tarball to the board **without touching the kernel that's currently working** — the single most important property when you're iterating remotely.

```bash
ROCK5B_HOST=<ip-or-host> ./scripts/deploy-kernel-tagged.sh beryllium deploy/kernel-latest.tar.gz
```

What it does, and why each step matters:

1. **Version-isolated slot.** It installs into a dedicated `/boot/vmlinuz-linux-beryllium`, its own `initramfs-linux-beryllium.img`, and a per-version `/usr/lib/modules/<kver>/`. A new kernel never overwrites or mixes modules with the existing one — so the kernel you booted from yesterday stays intact and bootable.
2. **DTB + initramfs.** Copies the matching DTB from the build volume and runs `depmod` + `mkinitcpio` scoped to that version only.
3. **One-shot boot with automatic fallback.** It regenerates `grub.cfg` and sets a *one-shot* GRUB entry into the new kernel. If the new kernel boots fine, great. If it hangs or panics, the next power-cycle falls back to the previous default automatically — no serial console rescue needed.

After you've validated the new kernel, make it the permanent default: point `GRUB_DEFAULT` at its entry and re-run `grub-mkconfig -o /boot/grub/grub.cfg`. Keep the previous entry in the list as a manual fallback.

> Never extract kernel modules at `/` on Arch: `/lib` is a symlink to `usr/lib`, and overwriting it with a real directory breaks the dynamic linker. The deploy script only ever writes the per-version module directory.

### Kernel command line

The board boots with these parameters (`GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`); after editing, regenerate with `sudo grub-mkconfig -o /boot/grub/grub.cfg`:

```
console=ttyS2,1500000n8 console=tty1 console=both rootwait rw init=/sbin/init video=HDMI-A-1:2560x1440@120 cma=512M
```

- **`video=HDMI-A-1:2560x1440@120`** — pin the output to the display's native mode. Without it, mode negotiation on the dw-hdmi-qp + FRL stack can settle on the wrong timing (e.g. 1440p@120 dropping to 60, or "Failed to read FRL config"). Set it to your own panel's real resolution/refresh.
- **`cma=512M`** — the Contiguous Memory Allocator pool. It started as the fix for AV1 CMA exhaustion: the Hantro AV1 VPU allocated decode buffers from CMA and drained the pool under sustained decode, freezing the board. With `CONFIG_VSI_IOMMU=y` the AV1 VPU now allocates through its IOMMU (IOVA scatter-gather, not CMA), so `CmaFree` stays flat and this is no longer required for AV1 — it's kept as headroom for the other CMA users (rkvdec H.264/HEVC capture buffers, the display framebuffer). This kernel's compiled default is 160 MB (`CONFIG_CMA_SIZE_MBYTES=160`); `cma=512M` just raises it. Drop the override, or set `cma=256M`, to reclaim RAM.
- **`console=ttyS2,1500000n8 console=tty1 console=both`** — serial debug console on the 1.5 Mbaud UART plus the HDMI tty. Pair this with the one-shot GRUB fallback above: if a kernel doesn't come up, the serial log tells you why and the next power-cycle boots the previous one.

## Reboot

```bash
ssh <board> sudo reboot
# after it's back, confirm the kernel + hardware decode:
ssh <board> 'uname -r; v4l2-ctl -d /dev/video0 --list-formats-out'   # must list S264, S265, VP9F
```

`VP9F` on `/dev/video0` is the proof the VP9 VDPU381 restore took. If it's missing, you're on a kernel without the restore (or the base before it) and VP9 will fall back to software everywhere.

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

## Browser video — stock Chromium 150

Chromium 150 is now the simple path. The VP9 green artifact on Mali Valhall that plagued earlier versions was **fixed upstream in the ANGLE roll that shipped with Chromium 150** — a chroma UV-plane offset fix, so it was resolution-independent and VP9-specific, not the ">2048px Skia shader" it was long thought to be. The upshot: **stock ArchLinuxARM Chromium 150 plays VP9 / H.264 / HEVC / AV1 in hardware with a clean picture — no custom build, no LibYUV bypass.** This section is the definitive setup.

> **Kernel prerequisite — the caveat that matters.** In-browser HW decode goes through V4L2, so the kernel must expose the codecs:
> - **VP9** on `/dev/video0` (RKVDEC2 VDPU381) — a downstream restore, *not* in the Collabora base. Check: `v4l2-ctl -d /dev/video0 --list-formats-out` must list `VP9F`.
> - **AV1** on `/dev/video4` (Hantro), ideally with `CONFIG_VSI_IOMMU=y` so sustained decode doesn't exhaust CMA. Check for `AV1F`.
> - H.264 / HEVC come with the Collabora base.
>
> On a kernel without `VP9F` / `AV1F` (stock generic mainline), decode silently falls back to software. The kernel comes first — see [Kernel composition](#kernel-composition-linux-71--beryllium-complete).

### 1. Install Chromium 150

```bash
sudo pacman -S chromium     # ArchLinuxARM ships stock 150.0.7871.46, HW decode compiled in
```

The ArchLinuxARM binary also runs as-is on other distros — extract the package into `$HOME` and launch it. It's the community's daily driver on RK3588; Debian's own Chromium builds work too, but some crash on YouTube resolution switches under load.

### 2. The launcher — this is the whole setup

`chromium-launcher` reads `~/.config/chromium-flags.conf` on every start and every `.desktop` entry inherits it, so this one file *is* the definitive setup (no per-launch flags):

```
--ozone-platform-hint=auto
--enable-features=AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL
--ignore-gpu-blocklist
--enable-gpu-rasterization
--enable-zero-copy
```

The flag that actually gates HW decode is **`AcceleratedVideoDecoder`**. The older name `AcceleratedVideoDecodeLinuxV4L2` no longer exists in Chromium 150 and is silently ignored — copy it from an old guide and HW decode stays off. HW decode also needs a **Wayland** session; `--ozone-platform-hint=auto` picks Wayland when the session provides it (on X11, Chromium 150 gives no VPU acceleration).

### 3. Verify

Play any VP9 / AV1 video, then:
- `chrome://gpu` → **Video Decode: Hardware accelerated**.
- `chrome://media-internals` → `kVideoDecoderName: V4L2VideoDecoder`, `kIsPlatformVideoDecoder: true`.

### Caveats

- **10-bit VP9 (Profile 2)** now decodes in hardware in the browser with the experimental [NV15 build](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/chromium-150.0.7871.114-nv15-10bit) (it needs the VP9 Profile 2 kernel to expose `NV15` — not in a Beryllium release yet, so build it from this repo while the kernel PRs are pending). **Stock** Chromium 150 still falls back to software for 10-bit VP9 — it doesn't know the `NV15` fourcc. AV1 and HEVC 10-bit already work in hardware on stock 150.
- Some setups add `--ozone-platform=wayland --use-gl=angle --use-angle=gles` (that is what this project runs), but on some kernel / Mesa combinations forcing `--ozone-platform` or `--use-gl` — rather than letting `--ozone-platform-hint=auto` choose — breaks V4L2 decoder selection. If `chrome://gpu` reports software decode, remove those and retry.
- YouTube serves AV1 (not VP9) whenever the client advertises AV1 hardware decode, so a "VP9" test on YouTube may actually exercise the AV1 VPU. Isolate the rkvdec VP9 path with a local Profile-0 `.webm`.

## Hand video off to mpv (alternative)

With Chromium 150 the in-browser path above just works, so the mpv handoff is no longer required for VP9. It stays useful in two cases: browsers that don't do V4L2 HW decode (Firefox, the Ungoogled Flatpak), and when you want mpv's libplacebo present quality. The browser handles navigation and hands the actual stream off to `mpv`, which talks directly to RKVDEC2 / Hantro through libavcodec's `v4l2request` hwaccel.

This pattern works for any stock Chromium / Firefox / Flatpak browser.

> **Kernel prerequisite.** mpv hardware decode goes through the same RKVDEC2 driver path as Chromium, so it needs the VP9 VDPU381 backend in the kernel. On the current `7.1.0-beryllium+` build that backend is the downstream restore described under [Kernel composition](#kernel-composition-linux-71--beryllium-complete) — the Collabora base alone does **not** expose VP9 on VDPU381. Without it (a stock Arch ARM / generic mainline kernel, or the base before the restore) `mpv --hwdec=v4l2request-copy` falls back to software just as Chromium does. Check with `v4l2-ctl -d /dev/video0 --list-formats-out` — it must list `VP9F`. The kernel comes first.

### Install mpv + a V4L2-aware FFmpeg

Arch ARM's stock `ffmpeg` ships TLS support but no `v4l2-request` hwaccel. Install Kwiboo's branch instead (packaged by NoDiskNoFun in beryllium-org):

```bash
sudo pacman -S mpv yt-dlp
# ffmpeg-v4l2-requests: build from https://github.com/beryllium-org/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests
makepkg -si    # inside the ffmpeg-v4l2-requests directory
```

After install, both checks below must return one line each:

```
ffmpeg -hwaccels  2>&1 | grep v4l2request
ffmpeg -protocols 2>&1 | grep https
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

### Auto-redirect at 1080p and above (userscript)

With [Tampermonkey](https://www.tampermonkey.net/) or Violentmonkey, this snippet hands every 1080p-or-higher YouTube watch page off to mpv automatically; sub-1080p (where YouTube serves AV1 and the browser is fine) stays in the browser:

```javascript
// ==UserScript==
// @name         YouTube → mpv at 1080p and above
// @match        https://www.youtube.com/watch*
// @run-at       document-idle
// ==/UserScript==
(function () {
  const v = new URL(location.href).searchParams.get('v');
  if (!v) return;
  const probe = setInterval(() => {
    const player = document.querySelector('#movie_player');
    if (!player || typeof player.getPlaybackQuality !== 'function') return;
    const q = player.getPlaybackQuality();   // "hd1080", "hd1440", "hd2160", ...
    if (!q) return;
    clearInterval(probe);
    if (/^hd(1080|1440|2160|2880|4320)$/.test(q)) location.href = 'mpv://' + location.href;
  }, 500);
})();
```

The full Flatpak walkthrough — including a `ff2mpv` native-messaging alternative — lives in [`docs/beryllium-wiki-browser-article.md`](docs/beryllium-wiki-browser-article.md).

## VAAPI driver (libva-v4l2-request fork)

[github.com/dongioia/libva-v4l2-request](https://github.com/dongioia/libva-v4l2-request) — branch `rk3588-vp9`, commit `ed4bc90`.

Bootlin's [libva-v4l2-request](https://github.com/bootlin/libva-v4l2-request) tree has been dormant since 2024 and never handled VP9 on RKVDEC2 correctly. This fork rebuilds the VP9 stack (range-coded `compressed_header` parser ported from FFmpeg, `interp_filter` prob-read gate per VP9 spec § 6.3.10) and adds eager V4L2 init at `vaCreateSurfaces` so probe-pattern clients see a populated CAPTURE buffer before the first decode.

| Path | Status | Notes |
|---|---|---|
| `mpv --hwdec=vaapi-copy` 1080p VP9 | ✅ pixel-perfect | 56 % CPU vs 113 % software (3000 frames vs libvpx, framemd5 identical) |
| `mpv --hwdec=vaapi` zero-copy | clean SW fallback | Same Mali Valhall EGL dmabuf cache issue that affects `v4l2request` zero-copy |
| VLC / Chromium native VAAPI | ⚠️ partial | VLC needs `vaPutSurface` (not yet implemented); Chromium aarch64 ships with `use_vaapi=false` |

For most users the V4L2 stateless path above (`mpv --hwdec=v4l2request-copy`) is simpler and avoids libva entirely. The VAAPI fork is interesting for tools that already speak libva, or for future work on Mesa panthor that might unblock the zero-copy display path. The companion PKGBUILD lives in `src/sbc-pkgbuilds/libva-v4l2-request/` (local branch — not yet pushed to beryllium-org).

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

### 2026-07-14 — VP9 portrait stride fix (every YouTube Short) folded into Beryllium

Portrait VP9 decoded to full-frame corruption on the VDPU381 backend: clean keyframe, then every inter frame decayed to garbage. [@sky-rk3588](https://github.com/sky-rk3588) root-caused it. `get_mv_base_addr()` and `config_ref_registers()` took the luma pitch from the bitstream display width, while the buffers use the coded/aligned stride (`bytesperline`). The two match for 16-multiple widths (all landscape), so it only bit portrait content at 1080/2160 wide, i.e. every YouTube Short. The fix uses `bytesperline` for the MV/ref pitch and aligns `bytesperline` to 64 in `rkvdec_fill_decoded_pixfmt()`. Tested here on the Rock 5B+ (same VDPU381 base backend) by hot-swapping the built module and comparing hardware output to libvpx frame-by-frame: portrait VP9 8-bit byte-identical (was full corruption), 10-bit/profile-2 clean at 64.4 dB, landscape unchanged, and an H264 portrait clip byte-identical as a 64-align smoke test. Folded into Beryllium as [linux-beryllium#8](https://github.com/beryllium-org/linux-beryllium/pull/8) on top of the profile-2 patch. Discussion: [minimyth2#73](https://github.com/warpme/minimyth2/issues/73).

### 2026-07-14 — 10-bit VP9 hardware decode in the browser (NV15) — experimental build

10-bit VP9 (Profile 2) now decodes in hardware *in the browser* on RK3588, published as an experimental [Chromium 150 + NV15 build](https://github.com/dongioia/rock5bplus-rkvdec2/releases/tag/chromium-150.0.7871.114-nv15-10bit). This closes the last gap from the entry below — stock Chromium had no `NV15` fourcc, so 10-bit VP9 fell back to software in the browser. The build is stock ArchLinuxARM Chromium 150.0.7871.114 + [@sky-rk3588](https://github.com/sky-rk3588) (Igor Paunovic)'s NV15 end-to-end patch (27 files across media / viz / mojo / ui) + a small ANGLE `dma_buf_utils` hunk needed on GL-Ganesh / ANGLE boards like the Rock 5B+, so ANGLE imports NV15 instead of hitting "Unknown dma_buf format" and going green. Validated on Rock 5B+ (kernel `7.1.0-beryllium-vp9p2+`): 10-bit VP9 Profile 2 decodes on the rkvdec (chrome holds `/dev/video0` through playback), clean picture, zero GPU crashes, no software fallback, same CPU cost as the 8-bit path. It needs the VP9 Profile 2 kernel to expose NV15 — that patch isn't in a Beryllium release yet (the kernel PRs are still pending), so build the kernel from this repo in the meantime; it's also attached to the release as `vp9-profile2-kernel-rkvdec.patch`. Built on [@amazingfate](https://github.com/amazingfate) (Jianfeng Liu)'s upstream Chromium V4L2 stateless-init fixes. Discussion and patches: [minimyth2#73](https://github.com/warpme/minimyth2/issues/73).

### 2026-07-12 — VP9 Profile 2 (10-bit) hardware decode on VDPU381

The RKVDEC2 VDPU381 VP9 backend gained Profile 2 (10-bit) support from a patch by [@sky-rk3588](https://github.com/sky-rk3588), which we reviewed and validated on Rock 5B+. It raises the VP9 profile control to Profile 2, adds an `NV15` 10-bit decoded format selected from the frame `bit_depth` (mirroring the HEVC Main10 path), takes the output stride from the buffer `bytesperline`, and defaults `image_fmt` to 8-bit for VP9 so an 8-bit stream never triggers a mid-stream capture-format reset (which otherwise breaks Chromium's zero-copy import). Result on Rock 5B+: 10-bit VP9 decodes in hardware via `v4l2slvp9dec` and `mpv --hwdec=v4l2request` (byte-clean vs libvpx, no IOMMU faults), with 8-bit unchanged and 8-bit Chromium still clean. In-browser 10-bit is still limited by Chromium not knowing the `NV15` fourcc (an upstream gap); GStreamer and mpv handle it today. The patch is headed for linux-media.

### 2026-07-12 — VP9 Mali green artifact root-caused to Chromium 150 (ANGLE); browser guide rewritten

Isolated what actually fixed the long-standing VP9 green artifact on Mali Valhall. It was **not** the Skia `GrYUVtoRGBEffect` shader (byte-identical across the fix) and **not** resolution-gated — the ">2048px" framing was a conflation with the separate kernel VDPU381 2K+ *decode* bug. Decisive isolation: two Chromium 149 builds green VP9 at 720p **and** 1440p on the current Mesa 26.1.3 / kernel 7.1, while stock Chromium 150 is clean on the same stack, so the fix rode in with the ANGLE roll that shipped in 150. The defect is a chroma (UV) half-resolution plane-offset miscalculation in ANGLE's YUV texture path (sibling commit `210ffede`, "Scale UV plane offsets for YUV textures") — which is why it was resolution-independent and VP9-specific (AV1 was clean throughout). Consequence: **stock ArchLinuxARM Chromium 150 plays VP9 / H.264 / HEVC / AV1 in hardware with a clean picture, no custom build and no LibYUV bypass** — the custom 147 build is now obsolete for VP9. The flag that gates HW decode is `AcceleratedVideoDecoder`; the older `AcceleratedVideoDecodeLinuxV4L2` no longer exists in 150 and is silently ignored. VP9 Profile 2 (10-bit) still falls back to software *in Chromium* (which lacks the `NV15` fourcc); the kernel itself now decodes it in hardware for GStreamer/mpv (see the 10-bit changelog entry above). The browser section above was rewritten around the stock-150 setup.

### 2026-06-28 — Linux 7.1 `beryllium-complete` kernel

Clean rebuild on Collabora's pinned `rockchip-v7.1` (7.1.0 final) for maximal RK3588 hardware support. New since the 7.1-rc2 stack: `CONFIG_VSI_IOMMU=y` binds the AV1 VPU IOMMU (`fdc70000` → IOMMU group 3) so AV1 decode allocates via IOVA scatter-gather instead of CMA — validated `CmaFree` flat (0 cma warnings) under sustained `v4l2slav1dec`, fixing the CMA exhaustion that froze the board under zero-copy AV1. Rocket NPU enabled (`/dev/accel/accel0`). Max-HW config adds HDMI CEC, USB-C DP PHY, UFS, SARADC, SPI-NOR, SAI, camera interface, ftrace + V4L2 tracepoints. The downstream chewitt VP9 / PM-reset stack was dropped — the base refactored RKVDEC2 into per-variant files and supersedes it. Deployed version-isolated as `7.1.0-beryllium+` (`scripts/deploy-kernel-tagged.sh`); boots with eth/HDMI up, 0 BTF failures, modules isolated with no cross-version mixing. The AV1 IOMMU enable is queued for mainline 7.2-rc1 (Benjamin Gaignard); this validates it on 7.1.

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

`KCFLAGS='-march=armv8.2-a+crypto+fp16+dotprod+rcpc -mtune=cortex-a76'`, `schedutil` default, THP madvise, ZRAM LZ4, ZSWAP, sched-ext + BTF, ARM64 errata trimmed 31→9. Measured: +18.8% SHA256, +17.3% AES, +6.1% memcpy. Branch [`7.0.y`](https://github.com/beryllium-org/linux-beryllium/tree/7.0.y).

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
