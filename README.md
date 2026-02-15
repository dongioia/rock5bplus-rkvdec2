# Rock 5B+ Mainline Kernel — Full Hardware Support

Patches, configs, and tools for full hardware enablement on the **Radxa Rock 5B+** with mainline Linux kernel: **4K hardware video decode**, **HDMI 2.0 4K@60Hz**, **HDMI audio**, **NPU acceleration**, and more.

## What This Provides

Tested and working on **Linux 6.19-rc8** with **BredOS** (Arch Linux ARM):

| Feature | Status | Details |
|---------|--------|---------|
| **H.264 4K decode** | Working | RKVDEC2, ~70 fps @ 4K |
| **HEVC 4K decode** | Working | RKVDEC2, ~68 fps @ 4K |
| **VP9 4K decode** | Working | RKVDEC2, ~69 fps @ 4K (community patch) |
| **H.264 1080p decode** | Working | Hantro VPU121 (upstream) |
| **HDMI 2.0 4K@60Hz** | Working | SCDC scrambling patch (Collabora) |
| **HDMI audio** | Working | LPCM, AC-3, E-AC-3, TrueHD via PipeWire |
| **Analog audio (3.5mm)** | Working | ES8316 codec, jack detect fix |
| **NPU (3 cores)** | Working | Rocket driver + Mesa Teflon, 3.8x speedup |
| **WiFi RTL8852BE** | Working | rtw89 driver, WiFi 6 |
| **GPU Panthor** | Working | Mali-G610, Vulkan |
| **Dual HDMI output** | Working | Upstream DW HDMI QP |
| **Ethernet 2.5GbE** | Working | RTL8125B via PCIe |
| **Bluetooth** | Working | RTL8852BU |

## Hardware

- **Board**: Radxa Rock 5B+ (RS129-D24E0 rev v1.207)
- **SoC**: Rockchip RK3588
- **RAM**: 24GB LPDDR5
- **OS**: BredOS (Arch Linux ARM), UEFI + GRUB

## Patches

### RKVDEC2/VDPU381 Driver (17 patches)

| File | Author | Description |
|------|--------|-------------|
| `patches/vpu/v9-01.patch` .. `v9-17.patch` | [Detlev Casanova](https://gitlab.collabora.com/detlev.casanova) ([Collabora](https://www.collabora.com/)) | VDPU381 variant for the `rkvdec` driver: H.264 + HEVC up to 4K via V4L2 stateless API |
| `patches/vpu/rkvdec-vdpu381-v9.mbox` | [Detlev Casanova](https://gitlab.collabora.com/detlev.casanova) ([Collabora](https://www.collabora.com/)) | Original mbox thread containing the full v9 series |

- **Source**: [v9 series on linux-media](https://lore.kernel.org/linux-media/20260120222018.404741-1-detlev.casanova@collabora.com/) (2026-01-20)
- Individual patches extracted and reordered from the mbox (email threading does not preserve patch order)

### RPS Fix (1 patch)

| File | Author | Description |
|------|--------|-------------|
| `patches/vpu/rkvdec-rps-fix.mbox` | [Detlev Casanova](https://gitlab.collabora.com/detlev.casanova) ([Collabora](https://www.collabora.com/)) | Reference Picture Set fix, applies on top of v9 |

- **Source**: [linux-media mailing list](https://lore.kernel.org/linux-media/) (2026-01-23)

### DTS Nodes for RKVDEC2

| File | Author | Description |
|------|--------|-------------|
| `patches/dts/rkvdec2-vdpu381-dts-nodes.patch` | [Detlev Casanova](https://gitlab.collabora.com/detlev.casanova) ([Collabora](https://www.collabora.com/)) | Device tree nodes for vdec0/vdec1 on RK3588 |

- **Source**: [upstream DTS v3 patch](https://lore.kernel.org/all/20251020212009.8852-2-detlev.casanova@collabora.com/) (2025-10-20)
- Adds 3 named register regions (function, link, cache), IOMMU nodes (`vdec0_mmu`, `vdec1_mmu`), SRAM pools inside `system_sram2`

### HDMI 2.0 / 4K@60Hz (4 patches)

| File | Author | Description |
|------|--------|-------------|
| `patches/display/0001-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `drm/bridge: Add ->detect_ctx hook` |
| `patches/display/0002-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `drm/bridge-connector: Switch to ->detect_ctx` |
| `patches/display/0003-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `dw-hdmi-qp: SCDC scrambling support` — **manually adapted** for 6.19-rc8 (see [docs/hdmi-4k60-setup.md](docs/hdmi-4k60-setup.md)) |
| `patches/display/0004-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `dw_hdmi_qp: HPD events fix` |

- **Source**: [v3 series on drm-misc](https://lore.kernel.org/r/20260119-dw-hdmi-qp-scramb-v3-0-bd8611730fc1@collabora.com/) (2026-01-19)
- Patches 1, 2, 4 applied cleanly via `git am`; patch 3 required manual adaptation (struct differences between `drm-misc-next` and 6.19-rc8, removed `no_hpd` field reference)
- Enables 3840x2160@60Hz (594 MHz TMDS), 1920x1080@120Hz, and all HDMI 2.0 modes
- Tested by: Maud Spierings (v1), Diederik de Haas (v1), this project (v3 on 6.19-rc8)

### NPU / Rocket Driver (kernel config)

No patches needed — the Rocket driver is included in kernel 6.18+. Just enable the config options:

- `CONFIG_DRM_ACCEL=y` — DRM accelerator subsystem (rebuilds `drm.ko`)
- `CONFIG_DRM_ACCEL_ROCKET=m` — Rocket NPU driver

Requires full kernel rebuild (not just the module). See [docs/npu-setup.md](docs/npu-setup.md) for userspace setup with Mesa Teflon and TFLite.

### Audio Setup (PipeWire/WirePlumber configuration)

No kernel patches needed — audio hardware works out of the box. Configuration fixes for PipeWire/WirePlumber device naming and jack detection:

- **WirePlumber rules**: `~/.config/wireplumber/wireplumber.conf.d/51-hdmi-rename.conf` — renames HDMI devices from generic "Audio interno" to "HDMI 0" / "HDMI 1"
- **UCM fix**: Remove `JackControl` from `/usr/share/alsa/ucm2/Rockchip/rk3588-es8316/HiFi.conf` — makes analog output always visible regardless of jack detection
- See [docs/audio-setup.md](docs/audio-setup.md) for details

### VP9 Support (community, experimental)

| File | Author(s) | Description |
|------|-----------|-------------|
| `patches/vpu/dvab-sarma-vp9-vdpu381.patch` | [dvab-sarma](https://github.com/dvab-sarma) (Venkata Atchuta Bheemeswara Sarma Darbha) | Original VP9 VDPU381 decoder patch |
| `patches/vpu/vp9-vdpu381-adapted.patch` | Adapted from dvab-sarma's work | Adaptation of VP9 code to fit the v9 driver framework |
| `patches/vpu/rkvdec-vdpu381-vp9.c` | [dvab-sarma](https://github.com/dvab-sarma), [Boris Brezillon](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Boris+Brezillon) ([Collabora](https://www.collabora.com/)), [Andrzej Pietrasiewicz](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Andrzej+Pietrasiewicz) ([Collabora](https://www.collabora.com/)) | VP9 decoder backend source |
| `patches/vpu/rkvdec-vdpu381-vp9.h` | [dvab-sarma](https://github.com/dvab-sarma) | VP9 decoder header |

- **Source**: [dvab-sarma/android_kernel_rk_opi](https://github.com/dvab-sarma/android_kernel_rk_opi/tree/android-16.0-hwaccel-testing) (branch `android-16.0-hwaccel-testing`)
- VP9 Profile 0 only, up to 4K@30fps, experimental/not production-ready
- The original VP9 backend builds on the rkvdec H.264 backend architecture by Boris Brezillon and Andrzej Pietrasiewicz (Collabora)

## Build

### Prerequisites

- macOS with Docker (Apple Silicon = native arm64, no emulation)
- Or any aarch64 Linux machine

### Quick Build

```bash
# 1. Clone kernel source
git clone --depth=1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git src/linux
# (or use the specific tag/commit you need)

# 2. Apply patches
cd src/linux
for p in ../../patches/vpu/v9-{01..17}.patch; do git apply "$p"; done
git apply ../../patches/vpu/rkvdec-rps-fix.mbox
git apply ../../patches/dts/rkvdec2-vdpu381-dts-nodes.patch
# Optional VP9:
git apply ../../patches/vpu/vp9-vdpu381-adapted.patch
cp ../../patches/vpu/rkvdec-vdpu381-vp9.{c,h} drivers/media/platform/rockchip/rkvdec/

# 3. Configure
cp ../../configs/current_rkvdec2.config .config
make ARCH=arm64 olddefconfig

# 4. Build (via Docker on macOS)
../../scripts/build.sh Image 12

# Or directly on ARM:
make -j$(nproc) Image modules
```

### Docker Build (macOS)

```bash
scripts/build.sh setup    # Build Docker image (first time)
scripts/build.sh Image 12 # Build kernel
scripts/build.sh dtbs 12  # Build device trees only
```

## Deploy to Rock 5B+

```bash
BOARD=<your-board-ip>
USER=<your-board-user>
KVER=<kernel-version>

# Copy kernel and DTB
scp src/linux/arch/arm64/boot/Image $USER@$BOARD:/tmp/
scp src/linux/arch/arm64/boot/dts/rockchip/rk3588-rock-5b-plus.dtb $USER@$BOARD:/tmp/
ssh $USER@$BOARD "sudo cp /tmp/Image /boot/vmlinuz-linux-custom && sudo cp /tmp/rk3588-rock-5b-plus.dtb /boot/dtbs/rockchip/"

# Copy modules (NEVER extract at / on Arch — destroys /lib symlink)
rsync -av staging/lib/modules/$KVER/ $USER@$BOARD:/tmp/modules/
ssh $USER@$BOARD "sudo rsync -a /tmp/modules/ /usr/lib/modules/$KVER/ && sudo depmod $KVER"

# Regenerate initramfs and update GRUB
ssh $USER@$BOARD "sudo mkinitcpio -k $KVER -g /boot/initramfs-linux-custom.img && sudo grub-mkconfig -o /boot/grub/grub.cfg"
```

## FFmpeg with v4l2-request

Stock FFmpeg doesn't support v4l2-request. Use [Kwiboo's fork](https://github.com/Kwiboo/FFmpeg):

```bash
git clone -b v4l2-request-n8.0.1 https://github.com/Kwiboo/FFmpeg.git
cd FFmpeg
./configure --prefix=/usr/local --enable-gpl --enable-version3 --enable-shared \
  --enable-libdrm --enable-v4l2-request --enable-libudev \
  --enable-libx264 --enable-libx265 --enable-libass \
  --disable-debug --disable-doc
make -j$(nproc) && sudo make install
sudo ldconfig
```

### mpv Configuration

```ini
# ~/.config/mpv/mpv.conf
hwdec=v4l2request-copy
vo=gpu
gpu-context=auto
```

## Performance (4K HEVC, complex content)

| Metric | HW Decode (RKVDEC2) | SW Decode (8-core) |
|--------|:---:|:---:|
| FPS | **68** | 44 |
| Speed | **2.27x** realtime | 1.47x realtime |
| CPU time | **~4s** (1 core) | ~43s (all cores) |

## Browser Setup

### Why Ungoogled Chromium over Firefox/LibreWolf

No browser on ARM64 currently supports hardware video decode via V4L2 stateless API (the method used by RKVDEC2 on mainline kernels). However, Chromium and Firefox differ significantly in GPU acceleration:

| Feature | Ungoogled Chromium (Flatpak) | Firefox / LibreWolf |
|---------|:---:|:---:|
| GPU compositing | Full (ANGLE + Panfrost GLES 3.1) | Partial (WebRender, less optimized on ARM) |
| GPU rasterization | All pages | Limited |
| WebGL | Hardware accelerated | Hardware accelerated |
| WebGPU | Hardware accelerated | Not supported |
| Zero-copy compositing | Supported | Not supported |
| Video decode | Software (V4L2 stateless not supported) | Software (V4L2 stateless not supported) |
| Wayland | Native (Ozone) | Native |
| Flatpak (bundled libs) | Available, always up to date | Available |

Chromium's ANGLE backend maps well to Panfrost's OpenGL ES 3.1, providing full hardware-accelerated page rendering, compositing, and WebGL/WebGPU. Firefox's WebRender is less optimized for ARM Mali GPUs. Neither browser supports hardware video decode on mainline RK3588 — use `mpv` + `yt-dlp` for that (see below).

**Note on Vulkan**: Forcing `--use-angle=vulkan` in the Flatpak sandbox falls back to llvmpipe (software Vulkan) since PanVK is not exposed inside the sandbox. The OpenGL ES path via Panfrost is the correct one and provides real hardware acceleration.

### Install

```bash
# Install Flatpak and add Flathub
sudo pacman -S flatpak
sudo flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
sudo flatpak update --appstream

# Install Ungoogled Chromium
sudo flatpak install -y flathub io.github.ungoogled_software.ungoogled_chromium
```

### Desktop entry with GPU flags

```bash
mkdir -p ~/.local/share/applications
cat > ~/.local/share/applications/ungoogled-chromium-gpu.desktop << 'EOF'
[Desktop Entry]
Type=Application
Name=Ungoogled Chromium
Comment=Web Browser with GPU acceleration
Exec=flatpak run io.github.ungoogled_software.ungoogled_chromium --enable-gpu-rasterization --enable-zero-copy --ignore-gpu-blocklist
Icon=io.github.ungoogled_software.ungoogled_chromium
Categories=Network;WebBrowser;
StartupNotify=true
EOF
update-desktop-database ~/.local/share/applications/
```

### Verify GPU acceleration

Open `chrome://gpu` — you should see:
- **GL_RENDERER**: `ANGLE (Mesa, Mali-G610 (Panfrost), OpenGL ES 3.1 Mesa ...)`
- **Canvas, Compositing, Rasterization, WebGL, WebGPU**: Hardware accelerated

### YouTube with hardware decode (bypasses browser)

Since no browser supports V4L2 stateless decode, use `mpv` + `yt-dlp` for hardware-accelerated YouTube playback:

```bash
# Add to ~/.bashrc
alias yt='mpv --ytdl-format="bestvideo[height<=2160]+bestaudio/best"'
alias yt1080='mpv --ytdl-format="bestvideo[height<=1080]+bestaudio/best"'
alias yta='mpv --no-video --ytdl-format="bestaudio/best"'
```

Usage: `yt https://youtube.com/watch?v=...` — plays up to 4K with RKVDEC2 hardware decode.

## Kernel Config Highlights

Key options enabled in `configs/current_rkvdec2.config`:

```
CONFIG_VIDEO_ROCKCHIP_VDEC=m         # RKVDEC2 driver
CONFIG_VIDEO_HANTRO=m                 # Hantro VPU121
CONFIG_DRM_ROCKCHIP=m                 # Display (Panthor, HDMI)
CONFIG_DRM_ACCEL=y                    # DRM accelerator subsystem
CONFIG_DRM_ACCEL_ROCKET=m             # NPU Rocket driver (3 cores, 6 TOPS)
CONFIG_RTW89_8852BE=m                 # WiFi RTL8852BE
CONFIG_ZRAM=m                         # Compressed swap
CONFIG_NLS_ASCII=m                    # Required for UEFI/FAT
```

## Known Limitations

- **VP9**: Community patch, Profile 0 only, experimental
- **NPU**: Only quantized INT8 models via TFLite; no ONNX or PyTorch direct support yet
- **Dual-core VPU**: ABI prepared but no V4L2 scheduler yet
- **RGA3**: No upstream driver (RGA2 works)
- **HDMI audio UCM fix**: May need re-applying after `alsa-ucm-conf` package updates
- **Browser video decode**: No browser supports V4L2 stateless API — video plays in software. Use `yt-dlp` + `mpv` for hardware-accelerated playback
- **HDMI reboot delay**: SCDC i2c nack warnings during shutdown are harmless but slow the reboot on some TVs

## Credits and Acknowledgments

This project builds entirely on the outstanding work of the upstream Linux kernel developers and the RK3588 community. None of this would be possible without their efforts:

### RKVDEC2/VDPU381 Driver

- **[Detlev Casanova](https://gitlab.collabora.com/detlev.casanova)** ([Collabora](https://www.collabora.com/)) — Author of the RKVDEC2/VDPU381 driver patch series (v1 through v9), which adds H.264 and HEVC hardware decoding support for RK3588. The v9 series used in this project was posted on [2026-01-20 to linux-media](https://lore.kernel.org/linux-media/20260120222018.404741-1-detlev.casanova@collabora.com/). Detlev also authored the [DTS nodes](https://lore.kernel.org/all/20251020212009.8852-2-detlev.casanova@collabora.com/) and the [RPS fix](https://lore.kernel.org/linux-media/) used here.

- **[Hans Verkuil](https://git.linuxtv.org/)** — Linux media subsystem maintainer who reviewed and committed the preparatory patches into `media.git/next`.

### VP9 Hardware Decode

- **[dvab-sarma](https://github.com/dvab-sarma)** — Community developer who created the VP9 VDPU381 decoder implementation in the [android_kernel_rk_opi](https://github.com/dvab-sarma/android_kernel_rk_opi/tree/android-16.0-hwaccel-testing) repository. The VP9 code in this project is adapted from their work to fit the v9 driver framework.

### FFmpeg v4l2-request

- **[Jonas Karlman (Kwiboo)](https://github.com/Kwiboo)** — Author and maintainer of the [FFmpeg v4l2-request patches](https://github.com/Kwiboo/FFmpeg), which enable V4L2 stateless hardware decoding in FFmpeg. Essential for using RKVDEC2 with mpv and other FFmpeg-based players.

### Rockchip Mainline Enablement

- **[Sebastian Reichel](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Sebastian+Reichel)** ([Collabora](https://www.collabora.com/)) — Upstream maintainer for Rockchip device trees, authored the Rock 5B+ DTS (`rk3588-rock-5b-plus.dts`), dual HDMI support, and many other RK3588 enablement patches.

- **[Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea)** ([Collabora](https://www.collabora.com/)) — Author of the DW HDMI QP bridge driver (`dw-hdmi-qp.c`), HDMI audio support, HDMI output enablement for RK3588, and the [HDMI 2.0 SCDC scrambling series](https://lore.kernel.org/r/20260119-dw-hdmi-qp-scramb-v3-0-bd8611730fc1@collabora.com/) enabling 4K@60Hz output.

- **[Heiko Stuebner](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Heiko+Stuebner)** — Rockchip platform maintainer in the Linux kernel, responsible for the overall RK3588 mainline integration.

- **[Tomeu Vizoso](https://gitlab.freedesktop.org/tomeu)** — NPU open-source driver development for RK3588 (kernel + Mesa).

### Distribution and Community

- **[BredOS](https://bredos.org/)** — Arch Linux ARM distribution optimized for ARM SBCs, providing the base OS and community support. [Wiki](https://wiki.bredos.org/) | [Discord](https://discord.gg/jwhxuyKXaa)

- **[7Ji](https://github.com/7Ji-PKGBUILDs)** — Maintainer of `linux-aarch64-7ji` kernel packages for Arch Linux ARM, providing pre-built mainline kernels with ARM SBC patches.

- **[Collabora](https://www.collabora.com/)** — Open-source consultancy driving a large portion of the RK3588 mainline enablement effort. Their [RK3588 mainline status page](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md) is an invaluable reference.

- **[Radxa](https://radxa.com/)** — Hardware manufacturer of the Rock 5B+. [Documentation](https://docs.radxa.com/en/rock5/rock5b)

### References

- RKVDEC2 v9 patches: [lore.kernel.org](https://lore.kernel.org/linux-media/20260120222018.404741-1-detlev.casanova@collabora.com/)
- DTS nodes: [lore.kernel.org](https://lore.kernel.org/all/20251020212009.8852-2-detlev.casanova@collabora.com/)
- VP9 community code: [github.com/dvab-sarma](https://github.com/dvab-sarma/android_kernel_rk_opi)
- FFmpeg v4l2-request: [github.com/Kwiboo/FFmpeg](https://github.com/Kwiboo/FFmpeg)
- Collabora RK3588 status: [gitlab.collabora.com](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md)
- PINE64 HW Decoding wiki: [wiki.pine64.org](https://wiki.pine64.org/wiki/Mainline_Hardware_Decoding)
- BredOS wiki: [wiki.bredos.org](https://wiki.bredos.org)

## License

Kernel patches follow their original licenses (GPL-2.0). Scripts and configuration files in this repository are MIT licensed.
