# Rock 5B+ Mainline Kernel — Full Hardware Support

Patches, configs, and tools for full hardware enablement on the **Radxa Rock 5B+** with mainline Linux kernel: **4K hardware video decode**, **HDMI 2.0 4K@60Hz**, **HDMI audio**, **NPU acceleration**, and more.

> **Current**: Linux 7.0-rc1 custom kernel built, tested, and deployed on Rock 5B+. The RKVDEC2/VDPU381 driver (H.264/HEVC) is [upstream in Linux 7.0](https://lore.kernel.org/linux-media/) — only VP9 (community patch) and an HPD optimization remain as custom patches. All hardware verified working: 6/6 V4L2 devices, Panthor GPU, NPU, WiFi, HDMI.

## Changelog

### 2026-02-28 — Linux 7.0-rc1

Built and deployed **Linux 7.0-rc1** custom kernel. 18 of 25 patches from the 6.19.1 build are now upstream — only 2 custom patches needed.

**Patch triage** (full details in [`patches/triage-7.0-rc1.md`](patches/triage-7.0-rc1.md)):

| Category | Total | Upstream | Still Needed | Obsolete |
|----------|-------|----------|-------------|----------|
| VPU (RKVDEC2 v9) | 17 | 17 | 0 | 0 |
| VPU (VP9 experimental) | 2 | 0 | 1 | 1 |
| DTS (VDPU381 nodes) | 1 | 1 | 0 | 0 |
| Display (HDMI 2.0) | 5 | 0 | 1 | 3+1 |
| **TOTAL** | **25** | **18** | **2** | **5** |

**Upstream in 7.0-rc1** (no longer applied):
- RKVDEC2/VDPU381 v9 driver — all 17 patches ([Detlev Casanova](https://gitlab.collabora.com/detlev.casanova), [Collabora](https://www.collabora.com/))
- DTS nodes for vdec0/vdec1/SRAM (Detlev Casanova, Collabora)

**Still applied (custom patches)**:
- **VP9 VDPU381 support** ([dvab-sarma](https://github.com/dvab-sarma), adapted — see below)
- **HDMI HPD events optimization** ([Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea), Collabora)

**No longer needed** (superseded by upstream):
- HDMI `detect_ctx` patches (0001, 0002) — upstream took a different approach: commit `5d156a9c3d5e` "drm/bridge: Pass down connector to drm bridge detect hook" ([Andy Yan](https://lore.kernel.org/linux-rockchip/?q=Andy+Yan), Rockchip)
- HDMI TMDS/scrambling (0003) — partially upstream via `2d7202c6f38d` "replace mode_valid with tmds_char_rate", needs rebase, skipped

**VP9 patch adaptation for 7.0-rc1** — the upstream RKVDEC2 merge restructured the driver code significantly. The VP9 adapted patch (`vp9-vdpu381-adapted.patch`) applies the Makefile/header changes, but the source files (`rkvdec-vdpu381-vp9.c`, `.h`) from dvab-sarma's original patch required manual fixes to compile against the upstream register API:

1. **Register struct field renames** — the upstream merge renamed register struct fields from short names to descriptive names. All 16 references in the VP9 source were updated:

   | Old field | New field |
   |-----------|-----------|
   | `reg009.dec_mode` | `reg009_dec_mode.dec_mode` |
   | `reg011.buf_empty_en` | `reg011_important_en.buf_empty_en` |
   | `reg011.dec_clkgate_e` | `reg011_important_en.dec_clkgate_e` |
   | `reg011.dec_timeout_e` | `reg011_important_en.dec_timeout_e` |
   | `reg018.y_hor_virstride` | `reg018_y_hor_stride.y_hor_virstride` |
   | `reg019.uv_hor_virstride` | `reg019_uv_hor_stride.uv_hor_virstride` |
   | `reg020.y_virstride` | `reg020_y_stride.y_virstride` |
   | `stream_len` | `reg016_stream_len` |
   | `timeout_threshold` | `reg032_timeout_threshold` |

2. **Block gating struct split** — the old `swreg_block_gating_e` field (a single 20-bit value `0xfffef`) was split into individual named bitfields in the upstream header. Replaced the single write with per-field assignments matching the upstream H.264/HEVC pattern:
   ```c
   // Old (dvab-sarma):
   regs->common.reg026_block_gating_en.swreg_block_gating_e = 0xfffef;

   // New (adapted for upstream):
   regs->common.reg026_block_gating_en.inter_auto_gating_e = 1;
   regs->common.reg026_block_gating_en.filterd_auto_gating_e = 1;
   // ... (all gating enables set to 1, except busifd = 0)
   ```

3. **Unused variable cleanup** — removed unused `s8 delta` variable to eliminate compiler warning.

**Config strategy**: Start from Panda's BredOS config (`panda_bredos_6.19.1.config`) for clean boot (`DRM_SIMPLEDRM=y`, `SYSFB_SIMPLEFB=y`), override with `DEBUG_PREEMPT=n` and static `PREEMPT=y` for performance.

**Build method**: `git archive` tarball extracted inside Docker container (Linux ext4) — avoids macOS HFS+/APFS case-insensitivity conflicts with kernel headers like `ipt_ECN.h` vs `ipt_ecn.h`.

**Hardware verification** (all passing):
- 6/6 V4L2 devices: rkvdec (H.264/HEVC/VP9), hantro, AV1, vepu121, hdmi-rx, rga
- Panthor GPU v1.7.0 (Mali-G610)
- NPU Rocket (3 cores)
- WiFi RTL8852BE (rtw89)
- HDMI output, Bluetooth

### 2026-02-17 — Linux 6.19.1 stable

Rebased all patches onto **Linux 6.19.1 stable** (from 6.19-rc8). Added 10 new patches for AV1 bug fixes, VOP2 display stability, RKVDEC stack safety, VPU power domain regulators, and HDMI InfoFrame support.

**New patches:**
- AV1 CDEF computation fix ([Benjamin Gaignard](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Benjamin+Gaignard), Collabora)
- AV1 tx mode bit mapping fix (Benjamin Gaignard, Collabora)
- AV1 tile info buffer size fix (Benjamin Gaignard, Collabora)
- VOP2 `mode_valid` callback ([Andy Yan](https://lore.kernel.org/linux-rockchip/?q=Andy+Yan), Rockchip)
- VOP2 `drm_err_ratelimited` log fix ([Hsieh Hung-En](https://lore.kernel.org/linux-rockchip/?q=Hsieh+Hung-En))
- RKVDEC stack fix for VDPU383 H.264 ([Arnd Bergmann](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Arnd+Bergmann))
- RKVDEC stack fix for VP9 count table (Arnd Bergmann)
- Power domain `need_regulator` fix for rkvdec0/1 and venc0/1 ([Shawn Lin](https://lore.kernel.org/linux-rockchip/?q=Shawn+Lin), Rockchip)
- DTS power domain labels and `domain-supply` references (adapted from Shawn Lin)
- HDMI VSI & SPD InfoFrame support (adapted from [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) v2, Collabora)

**Carried forward from 6.19-rc8** (applied cleanly to 6.19.1):
- RKVDEC2/VDPU381 v9 — 17 patches (Detlev Casanova, Collabora)
- RPS fix (Detlev Casanova, Collabora)
- DTS nodes for rkvdec2 (Detlev Casanova, Collabora)
- HDMI 2.0 SCDC scrambling v3 — 4 patches (Cristian Ciocaltea, Collabora)
- VP9 VDPU381 support (dvab-sarma, community)
- Custom boot logo

### 2026-02-13 — Linux 6.19-rc8

Initial release with RKVDEC2/VDPU381 v9 driver, HDMI 2.0 scrambling, VP9 community patch, NPU config, audio setup, and browser guide.

## What This Provides

Tested and working on **Linux 7.0-rc1** and **Linux 6.19.1 stable** with **BredOS** (Arch Linux ARM):

| Feature | Status | Details |
|---------|--------|---------|
| **H.264 4K decode** | Working | RKVDEC2, ~70 fps @ 4K |
| **HEVC 4K decode** | Working | RKVDEC2, ~68 fps @ 4K |
| **VP9 4K decode** | Working | RKVDEC2, ~69 fps @ 4K (community patch) |
| **AV1 decode** | Working | Hantro VPU121 (with bug fixes) |
| **H.264 1080p decode** | Working | Hantro VPU121 (upstream) |
| **HDMI 2.0 4K@60Hz** | Working | SCDC scrambling + VSI/SPD InfoFrames |
| **HDMI audio** | Working | LPCM, AC-3, E-AC-3, TrueHD via PipeWire |
| **Analog audio (3.5mm)** | Working | ES8316 codec, jack detect fix |
| **NPU (3 cores)** | Working | Rocket driver + Mesa Teflon, 3.8x speedup |
| **WiFi RTL8852BE** | Working | rtw89 driver, WiFi 6 |
| **GPU Panthor** | Working | Mali-G610 MP4, Vulkan 1.4 (PanVK) |
| **Dual HDMI output** | Working | Upstream DW HDMI QP |
| **Ethernet 2.5GbE** | Working | RTL8125B via PCIe |
| **Bluetooth** | Working | RTL8852BU |
| **HDMI-RX** | Working | Capture via `/dev/video0` |

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
- **Upstream status**: merged in Linux 7.0 ([media updates GIT PULL](https://lore.kernel.org/linux-media/), 2026-02-11)
- **Adaptation**: applied cleanly on both 6.19-rc8 and 6.19.1 via `git am`

### RPS Fix (1 patch)

| File | Author | Description |
|------|--------|-------------|
| `patches/vpu/rkvdec-rps-fix.mbox` | [Detlev Casanova](https://gitlab.collabora.com/detlev.casanova) ([Collabora](https://www.collabora.com/)) | Reference Picture Set fix, applies on top of v9 |

- **Source**: [linux-media mailing list](https://lore.kernel.org/linux-media/) (2026-01-23)
- **Adaptation**: applied cleanly via `git am`

### DTS Nodes for RKVDEC2

| File | Author | Description |
|------|--------|-------------|
| `patches/dts/rkvdec2-vdpu381-dts-nodes.patch` | [Detlev Casanova](https://gitlab.collabora.com/detlev.casanova) ([Collabora](https://www.collabora.com/)) | Device tree nodes for vdec0/vdec1 on RK3588 |

- **Source**: [upstream DTS v3 patch](https://lore.kernel.org/all/20251020212009.8852-2-detlev.casanova@collabora.com/) (2025-10-20)
- Adds 3 named register regions (function, link, cache), IOMMU nodes (`vdec0_mmu`, `vdec1_mmu`), SRAM pools inside `system_sram2`
- **Adaptation**: applied cleanly via `git apply`

### RKVDEC Stack Fixes (2 patches)

| File | Author | Description |
|------|--------|-------------|
| `patches/vpu/rkvdec-stack-fix-1.mbox` | [Arnd Bergmann](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Arnd+Bergmann) | `noinline_for_stack` for VDPU383 H.264 `set_field_order_cnt` and new `set_dec_params` helper |
| `patches/vpu/rkvdec-stack-fix-2.mbox` | [Arnd Bergmann](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Arnd+Bergmann) | `noinline_for_stack` for VP9 `rkvdec_init_v4l2_vp9_count_tbl` |

- **Source**: [linux-media mailing list](https://lore.kernel.org/linux-media/) (2026-01)
- Prevents kernel stack overflow on functions with large local variables by marking them `noinline_for_stack`, which forces the compiler to allocate separate stack frames
- Patch 1 also refactors the H.264 DPB flag iteration into a dedicated `set_dec_params()` function
- **Adaptation**: applied cleanly via `git am` on top of the v9 series

### VPU Power Domain Fix (2 patches)

| File | Author | Description |
|------|--------|-------------|
| (applied directly) | [Shawn Lin](https://lore.kernel.org/linux-rockchip/?q=Shawn+Lin) ([Rockchip](https://www.rock-chips.com/)) | Set `need_regulator=true` for `RK3588_PD_RKVDEC0`, `RK3588_PD_RKVDEC1`, `RK3588_PD_VENC0`, `RK3588_PD_VENC1` in `pm-domains.c` |
| (applied directly) | Adapted from [Shawn Lin](https://lore.kernel.org/linux-rockchip/?q=Shawn+Lin) ([Rockchip](https://www.rock-chips.com/)) | Add `pd_rkvdec0`/`pd_rkvdec1`/`pd_venc0`/`pd_venc1` labels to `rk3588-base.dtsi` and `domain-supply = <&vdd_vdenc_s0>` references in `rk3588-rock-5b-plus.dts` |

- **Source**: [linux-rockchip mailing list](https://lore.kernel.org/linux-rockchip/) (2026-02)
- The `pm-domains.c` change is a single-line-per-domain fix: changing the last parameter of `DOMAIN_RK3588()` from `false` to `true` enables regulator supply management for VPU power domains
- **Adaptation**: the DTS portion was adapted for Rock 5B+ specifically — the original patch targets `rk3588-evb1-v10.dts`; we added the same `domain-supply` properties to `rk3588-rock-5b-plus.dts` and created the necessary `pd_*` labels in `rk3588-base.dtsi` to make the references work

### HDMI 2.0 / 4K@60Hz (4 patches)

| File | Author | Description |
|------|--------|-------------|
| `patches/display/0001-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `drm/bridge: Add ->detect_ctx hook` |
| `patches/display/0002-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `drm/bridge-connector: Switch to ->detect_ctx` |
| `patches/display/0003-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `dw-hdmi-qp: SCDC scrambling support` — **manually adapted** (see below) |
| `patches/display/0004-*.patch` | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | `dw_hdmi_qp: HPD events fix` |

- **Source**: [v3 series on drm-misc](https://lore.kernel.org/r/20260119-dw-hdmi-qp-scramb-v3-0-bd8611730fc1@collabora.com/) (2026-01-19)
- Enables 3840x2160@60Hz (594 MHz TMDS), 1920x1080@120Hz, and all HDMI 2.0 modes
- Tested by: Maud Spierings (v1), Diederik de Haas (v1), this project (v3 on 6.19-rc8 and 6.19.1)
- **Adaptation**: patches 1, 2, 4 applied cleanly via `git am`; patch 3 required manual adaptation — the v3 series targets `drm-misc-next` where `struct drm_connector` has a `no_hpd` field not present in 6.19.x stable; the reference was removed and the `drm_bridge_helper_reset_crtc()` call was adjusted for the stable API. See [docs/hdmi-4k60-setup.md](docs/hdmi-4k60-setup.md)

### HDMI VSI & SPD InfoFrames (adapted from v2 series)

| File | Author | Description |
|------|--------|-------------|
| (applied directly) | [Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea) ([Collabora](https://www.collabora.com/)) | Vendor-Specific and Source Product Description InfoFrame support for DW HDMI QP |

- **Source**: [v2 series on linux-rockchip](https://lore.kernel.org/linux-rockchip/20260129-dw-hdmi-qp-iframe-v2-0-0157ad05232c@collabora.com/) (2026-01-29)
- Adds `dw_hdmi_qp_write_pkt()` and `dw_hdmi_qp_write_infoframe_data()` helpers, VSI/SPD configuration callbacks, and new register definitions (`PKTSCHED_VSI_FIELDRATE`, `PKTSCHED_VSI_TX_EN`, `PKTSCHED_SPDI_TX_EN`)
- **Adaptation**: the original v2 series (5 patches) also reworks AVI, DRM, and Audio InfoFrame handlers with a unified `dw_hdmi_qp_write_infoframe()` helper; since those reworks depend on `drm-misc-next` API changes not present in 6.19.x, only the VSI and SPD patches (1 and 2) were adapted, using the existing infoframe infrastructure in our tree

### AV1 Bug Fixes (3 patches)

| File | Author | Description |
|------|--------|-------------|
| (applied directly) | [Benjamin Gaignard](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Benjamin+Gaignard) ([Collabora](https://www.collabora.com/)) | Fix CDEF enable computation — if all CDEF parameters are zero, `av1_enable_cdef` must be unset even when `V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF` is set |
| (applied directly) | [Benjamin Gaignard](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Benjamin+Gaignard) ([Collabora](https://www.collabora.com/)) | Fix tx mode bit mapping — adds a mapping function between AV1 spec tx modes (4x4, largest, select) and hardware tx modes (4x4, 8x8, 16x16, 32x32, select) |
| (applied directly) | [Benjamin Gaignard](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Benjamin+Gaignard) ([Collabora](https://www.collabora.com/)) | Fix tile info buffer size — allocate `AV1_MAX_TILES * 16` bytes (4 fields x 4 bytes each) instead of `AV1_MAX_TILES` to avoid writing into non-allocated memory |

- **Source**: [linux-media mailing list](https://lore.kernel.org/linux-media/) (2025-12 and 2026-01)
- Fixes: `727a400686a2c` ("media: verisilicon: Add Rockchip AV1 decoder")
- Reported-by: Jianfeng Liu (CDEF fix, via [GStreamer issue #4786](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/4786))
- **Adaptation**: applied cleanly via `git am` — these patches target `drivers/media/platform/verisilicon/` which is unchanged between 6.19-rc8 and 6.19.1

### VOP2 Display Fixes (2 patches)

| File | Author | Description |
|------|--------|-------------|
| (applied directly) | [Andy Yan](https://lore.kernel.org/linux-rockchip/?q=Andy+Yan) ([Rockchip](https://www.rock-chips.com/)) | Add `mode_valid` callback to VOP2 CRTC — filters modes exceeding `vp->data->max_output.width` |
| (applied directly) | [Hsieh Hung-En](https://lore.kernel.org/linux-rockchip/?q=Hsieh+Hung-En) | Replace `DRM_DEV_ERROR` with `drm_err_ratelimited` in VOP2 timeout handlers |

- **Source**: [linux-rockchip mailing list](https://lore.kernel.org/linux-rockchip/) (2026-01)
- The mode_valid callback prevents the display pipeline from attempting unsupported resolutions. The ratelimited logging prevents dmesg flooding during transient display timeouts (e.g., port_mux or layer config)
- **Adaptation**: both patches applied cleanly to `rockchip_drm_vop2.c` and `rockchip_vop2_reg.c`; no modifications needed

### VP9 Support (community, experimental)

| File | Author(s) | Description |
|------|-----------|-------------|
| `patches/vpu/dvab-sarma-vp9-vdpu381.patch` | [dvab-sarma](https://github.com/dvab-sarma) (Venkata Atchuta Bheemeswara Sarma Darbha) | Original VP9 VDPU381 decoder patch (for pre-upstream driver) |
| `patches/vpu/vp9-vdpu381-adapted.patch` | Adapted from dvab-sarma's work | Adaptation of VP9 code to fit the v9/upstream driver framework (Makefile, headers) |
| `patches/vpu/rkvdec-vdpu381-vp9.c` | [dvab-sarma](https://github.com/dvab-sarma), [Boris Brezillon](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Boris+Brezillon) ([Collabora](https://www.collabora.com/)), [Andrzej Pietrasiewicz](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Andrzej+Pietrasiewicz) ([Collabora](https://www.collabora.com/)) | VP9 decoder backend source |
| `patches/vpu/rkvdec-vdpu381-vp9.h` | [dvab-sarma](https://github.com/dvab-sarma) | VP9 decoder header |

- **Source**: [dvab-sarma/android_kernel_rk_opi](https://github.com/dvab-sarma/android_kernel_rk_opi/tree/android-16.0-hwaccel-testing) (branch `android-16.0-hwaccel-testing`)
- VP9 Profile 0 only, up to 4K@30fps, experimental/not production-ready
- The original VP9 backend builds on the rkvdec H.264 backend architecture by Boris Brezillon and Andrzej Pietrasiewicz (Collabora)
- **7.0-rc1 adaptation**: the adapted patch applies the Makefile entry and header integration; the `.c` and `.h` source files are extracted from the original dvab-sarma patch and require register API fixes (struct field renames + block gating bitfield split) to compile against the upstream driver. See the [7.0-rc1 changelog](#2026-02-28--linux-70-rc1) for the full list of changes

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

## Build

### Prerequisites

- macOS with Docker (Apple Silicon = native arm64, no emulation)
- Or any aarch64 Linux machine

### Quick Build — Linux 7.0-rc1 (recommended)

```bash
# 1. Clone kernel source
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git src/linux
cd src/linux
git checkout v7.0-rc1

# 2. Apply patches (only 2 needed — RKVDEC2 H.264/HEVC is upstream!)
git apply ../../patches/vpu/vp9-vdpu381-adapted.patch
cp ../../patches/vpu/rkvdec-vdpu381-vp9.{c,h} drivers/media/platform/rockchip/rkvdec/
git apply ../../patches/display/0004-drm-rockchip--dw_hdmi_qp--Do-not-send-HPD-events-for-all-connectors.patch

# 3. Configure (Panda's BredOS config base + performance overrides)
cp ../../configs/rock5b_7.0-rc1.config .config
make ARCH=arm64 olddefconfig

# 4. Build (via Docker on macOS — use tarball to avoid case-sensitivity issues)
git archive HEAD | gzip > /tmp/linux-source.tar.gz
# Then extract inside Docker container and build with make -j12
```

### Quick Build — Linux 6.19.1 stable

```bash
# 1. Clone kernel source (6.19.1 stable)
git clone --depth=1 --branch v6.19.1 \
  https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git src/linux

# 2. Apply patches (all 25+ patches needed)
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

Stock FFmpeg doesn't support v4l2-request. On **BredOS**, install the pre-built package from the BredOS repository:

```bash
sudo pacman -S ffmpeg-v4l2-requests
```

This package ([source](https://github.com/BredOS/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests)) is based on [Kwiboo's FFmpeg v4l2-request-n8.0.1](https://github.com/Kwiboo/FFmpeg/tree/v4l2-request-n8.0.1) branch. It replaces the stock `ffmpeg` package and supports V4L2 stateless hardware decoding for H.264, HEVC, VP9, AV1, VP8, and MPEG-2.

<details>
<summary>Manual build (other distros or custom needs)</summary>

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

</details>

### mpv Configuration

```ini
# ~/.config/mpv/mpv.conf

# Hardware decode — zero-copy DMA-BUF via RKVDEC2
hwdec=v4l2request

# Video output — Vulkan via PanVK 1.4 (libplacebo renderer)
vo=gpu-next,gpu
gpu-api=vulkan
gpu-context=waylandvk

# Cache for streaming
cache=yes
demuxer-max-bytes=500M
demuxer-max-back-bytes=200M

# yt-dlp integration
script-opts=ytdl_hook-ytdl_path=yt-dlp
ytdl-format=bestvideo[height<=?1080]+bestaudio/best
```

> **Fallback**: if `gpu-next` + Vulkan causes artifacts, use `vo=gpu` with `gpu-context=wayland` (OpenGL ES via Panfrost).

### Open in mpv (from browser)

Set up a `mpv://` protocol handler to play video URLs directly from the browser:

```bash
# 1. Install handler script
sudo tee /usr/local/bin/open-in-mpv << 'SCRIPT'
#!/bin/bash
url="$1"
url="${url#mpv://play/}"
url="${url#mpv://}"
url=$(python3 -c "import sys, urllib.parse; print(urllib.parse.unquote(sys.argv[1]))" "$url" 2>/dev/null || echo "$url")
[ -z "$url" ] && exit 1
exec mpv --force-window=immediate "$url" &>/dev/null &
SCRIPT
sudo chmod +x /usr/local/bin/open-in-mpv

# 2. Register protocol handler
cat > ~/.local/share/applications/open-in-mpv.desktop << 'EOF'
[Desktop Entry]
Type=Application
Name=Open in mpv
Exec=/usr/local/bin/open-in-mpv %u
Icon=mpv
NoDisplay=true
MimeType=x-scheme-handler/mpv;
EOF
xdg-mime default open-in-mpv.desktop x-scheme-handler/mpv

# 3. Add bookmarklet to browser bookmark bar:
#    Name: "mpv"
#    URL:  javascript:location.href='mpv://play/'+encodeURIComponent(location.href)
```

Click the bookmarklet on any YouTube/video page to play it in mpv with hardware decode.

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

**GPU stack in Flatpak**: The Flatpak runtime includes Panthor DRI + PanVK (Vulkan 1.4) + Panfrost (GLES 3.1). With `devices=all` and `sockets=wayland` permissions, the sandbox has full GPU access. On Wayland, Chromium gets native GPU access (no ANGLE translation layer on X11).

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
Name=Ungoogled Chromium (GPU)
Comment=Web Browser with Wayland + Vulkan GPU acceleration
Exec=flatpak run io.github.ungoogled_software.ungoogled_chromium --ignore-gpu-blocklist --enable-zero-copy --ozone-platform=wayland --use-gl=egl --enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan,WaylandWindowDecorations
Icon=io.github.ungoogled_software.ungoogled_chromium
Categories=Network;WebBrowser;
StartupNotify=true
EOF
update-desktop-database ~/.local/share/applications/
```

Flag breakdown:

| Flag | Purpose |
|------|---------|
| `--ozone-platform=wayland` | Native Wayland — avoids XWayland + ANGLE overhead |
| `--use-gl=egl` | EGL directly (required for Wayland) |
| `--enable-zero-copy` | Zero-copy buffer sharing GPU ↔ compositor |
| `Vulkan,VulkanFromANGLE,DefaultANGLEVulkan` | ANGLE rendering via PanVK Vulkan 1.4 |
| `WaylandWindowDecorations` | Native window decorations on Wayland |

> **Fallback**: if Vulkan causes crashes, remove the 3 Vulkan features and keep only `--use-gl=egl` (falls back to OpenGL ES via Panfrost).

### Verify GPU acceleration

Open `chrome://gpu` — you should see:
- **GL_RENDERER**: Mali-G610 / panfrost / panvk (not SwiftShader or llvmpipe)
- **Canvas, Compositing, Rasterization, WebGL, WebGPU**: Hardware accelerated

### YouTube with hardware decode (bypasses browser)

No browser supports V4L2 stateless decode on ARM. Use `mpv` + `yt-dlp` for hardware-accelerated playback — either via the [mpv:// bookmarklet](#open-in-mpv-from-browser) or from the terminal:

```bash
mpv 'https://youtube.com/watch?v=...'
```

mpv automatically uses yt-dlp and RKVDEC2 hardware decode.

## Kernel Config Highlights

Key options enabled in `configs/rock5b_7.0-rc1.config` (and `configs/current_rkvdec2.config` for 6.19.1):

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

## Customization

### Custom Boot Logo

The default Tux penguin is replaced with a custom 80x80 224-color logo compiled into the kernel. To use your own:

```bash
# Convert any image to 80x80 PPM with max 224 colors
magick your-image.png -resize 80x80 -background white -flatten -colors 224 -compress none PPM:logo.ppm

# Replace in kernel source and rebuild
cp logo.ppm src/linux/drivers/video/logo/logo_linux_clut224.ppm
scripts/build.sh Image 12
```

### Custom fastfetch Logo

```bash
# Copy your image to the board
scp your-logo.png $USER@$BOARD:~/.config/fastfetch/logo.png

# The config in ~/.config/fastfetch/config.jsonc uses chafa rendering
# IMPORTANT: the "modules" array must be present, or no system info is displayed
```

## Known Limitations

- **VP9**: Community patch, Profile 0 only, experimental
- **NPU**: Only quantized INT8 models via TFLite; no ONNX or PyTorch direct support yet
- **Dual-core VPU**: ABI prepared but no V4L2 scheduler yet
- **RGA3**: No upstream driver (RGA2 works)
- **GPU max clock**: 850 MHz (firmware PVTM limit; OPP table lists 900/1000 MHz but the voltage regulator cannot supply enough)
- **HDMI audio UCM fix**: May need re-applying after `alsa-ucm-conf` package updates
- **Browser video decode**: No browser supports V4L2 stateless API — video plays in software. Use `yt-dlp` + `mpv` for hardware-accelerated playback

## Credits and Acknowledgments

This project builds entirely on the outstanding work of the upstream Linux kernel developers and the RK3588 community. None of this would be possible without their efforts:

### RKVDEC2/VDPU381 Driver

- **[Detlev Casanova](https://gitlab.collabora.com/detlev.casanova)** ([Collabora](https://www.collabora.com/)) — Author of the RKVDEC2/VDPU381 driver patch series (v1 through v9), which adds H.264 and HEVC hardware decoding support for RK3588. The v9 series used in this project was posted on [2026-01-20 to linux-media](https://lore.kernel.org/linux-media/20260120222018.404741-1-detlev.casanova@collabora.com/). Detlev also authored the [DTS nodes](https://lore.kernel.org/all/20251020212009.8852-2-detlev.casanova@collabora.com/) and the [RPS fix](https://lore.kernel.org/linux-media/) used here. The driver has been [merged upstream in Linux 7.0](https://lore.kernel.org/linux-media/).

- **[Hans Verkuil](https://git.linuxtv.org/)** — Linux media subsystem maintainer who reviewed and committed the RKVDEC2 patches into `media.git/next`.

### AV1 Bug Fixes

- **[Benjamin Gaignard](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Benjamin+Gaignard)** ([Collabora](https://www.collabora.com/)) — Author of the Rockchip AV1 decoder (Hantro VPU121) and the three bug fix patches for CDEF computation, tx mode bit mapping, and tile info buffer sizing.

### RKVDEC Stack Fixes

- **[Arnd Bergmann](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Arnd+Bergmann)** — Author of the `noinline_for_stack` fixes for VDPU383 H.264 and VP9 decoder functions, preventing kernel stack overflow.

### VP9 Hardware Decode

- **[dvab-sarma](https://github.com/dvab-sarma)** — Community developer who created the VP9 VDPU381 decoder implementation in the [android_kernel_rk_opi](https://github.com/dvab-sarma/android_kernel_rk_opi/tree/android-16.0-hwaccel-testing) repository. The VP9 code in this project is adapted from their work to fit the v9 driver framework.

### VOP2 Display Fixes

- **[Andy Yan](https://lore.kernel.org/linux-rockchip/?q=Andy+Yan)** ([Rockchip](https://www.rock-chips.com/)) — Author of the VOP2 `mode_valid` callback that filters unsupported display resolutions.

- **[Hsieh Hung-En](https://lore.kernel.org/linux-rockchip/?q=Hsieh+Hung-En)** — Author of the VOP2 `drm_err_ratelimited` fix that prevents log flooding during display timeouts.

### VPU Power Domain Fix

- **[Shawn Lin](https://lore.kernel.org/linux-rockchip/?q=Shawn+Lin)** ([Rockchip](https://www.rock-chips.com/)) — Author of the power domain regulator fix for RKVDEC and VENC domains, ensuring proper power supply management for video codec operation.

### FFmpeg v4l2-request

- **[Jonas Karlman (Kwiboo)](https://github.com/Kwiboo)** — Author and maintainer of the [FFmpeg v4l2-request patches](https://github.com/Kwiboo/FFmpeg), which enable V4L2 stateless hardware decoding in FFmpeg. Essential for using RKVDEC2 with mpv and other FFmpeg-based players.

- **[NoDiskNoFun](https://github.com/BredOS/sbc-pkgbuilds/tree/main/ffmpeg-v4l2-requests)** — Packaged `ffmpeg-v4l2-requests` for the BredOS repository, making V4L2-request FFmpeg available as a simple `pacman -S` install for BredOS users.

### Rockchip Mainline Enablement

- **[Sebastian Reichel](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Sebastian+Reichel)** ([Collabora](https://www.collabora.com/)) — Upstream maintainer for Rockchip device trees, authored the Rock 5B+ DTS (`rk3588-rock-5b-plus.dts`), dual HDMI support, and many other RK3588 enablement patches.

- **[Cristian Ciocaltea](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/?qt=author&q=Cristian+Ciocaltea)** ([Collabora](https://www.collabora.com/)) — Author of the DW HDMI QP bridge driver (`dw-hdmi-qp.c`), HDMI audio support, HDMI output enablement for RK3588, the [HDMI 2.0 SCDC scrambling series](https://lore.kernel.org/r/20260119-dw-hdmi-qp-scramb-v3-0-bd8611730fc1@collabora.com/) enabling 4K@60Hz output, and the [HDMI VSI & SPD InfoFrame series](https://lore.kernel.org/linux-rockchip/20260129-dw-hdmi-qp-iframe-v2-0-0157ad05232c@collabora.com/).

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
- HDMI VSI/SPD InfoFrames v2: [lore.kernel.org](https://lore.kernel.org/linux-rockchip/20260129-dw-hdmi-qp-iframe-v2-0-0157ad05232c@collabora.com/)
- VP9 community code: [github.com/dvab-sarma](https://github.com/dvab-sarma/android_kernel_rk_opi)
- FFmpeg v4l2-request: [github.com/Kwiboo/FFmpeg](https://github.com/Kwiboo/FFmpeg)
- Collabora RK3588 status: [gitlab.collabora.com](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md)
- PINE64 HW Decoding wiki: [wiki.pine64.org](https://wiki.pine64.org/wiki/Mainline_Hardware_Decoding)
- BredOS wiki: [wiki.bredos.org](https://wiki.bredos.org)

## License

Kernel patches follow their original licenses (GPL-2.0). Scripts and configuration files in this repository are MIT licensed.
