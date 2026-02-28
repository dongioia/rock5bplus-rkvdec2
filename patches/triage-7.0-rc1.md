# Patch Triage — Linux 7.0-rc1 for Rock 5B+

**Date**: 2026-02-28
**Base**: v7.0-rc1 (commit 6de23f81a5e0)
**Branch**: rock5b-7.0-rc1

## Summary

| Category | Total | Upstream | Still Needed | Obsolete | Needs Rebase |
|----------|-------|----------|-------------|----------|-------------|
| VPU (RKVDEC2 v9) | 17 | 17 | 0 | 0 | 0 |
| VPU (VP9 experimental) | 2 | 0 | 1 | 1 | 0 |
| DTS (VDPU381 nodes) | 1 | 1 | 0 | 0 | 0 |
| Display (HDMI 2.0) | 5 | 0 | 1 | 3 | 1 |
| NPU | 0 | — | — | — | — |
| Audio | 0 | — | — | — | — |
| GPU | 0 | — | — | — | — |
| Misc | 0 | — | — | — | — |
| **TOTAL** | **25** | **18** | **2** | **4** | **1** |

## Detailed Triage

### patches/vpu/ — RKVDEC2 Driver (Collabora v9 series)

All 17 patches from the Collabora RKVDEC2 v9 series are **upstream** in 7.0-rc1.
Verified via `git log v7.0-rc1 -- drivers/media/platform/rockchip/rkvdec/`.

| Patch | Subject | Status |
|-------|---------|--------|
| v9-01.patch | media: uapi: HEVC: Add v4l2_ctrl_hevc_ext_sps_[ls]t_rps controls | upstream (fa05705107a4) |
| v9-02.patch | media: v4l2-ctrls: Add hevc_ext_sps_[ls]t_rps controls | upstream |
| v9-03.patch | media: visl: Add HEVC short and long term RPS sets | upstream (4cb9cd80b36e) |
| v9-04.patch | media: rkvdec: Switch to using structs instead of writel | upstream (dc6898981f74) |
| v9-05.patch | media: rkvdec: Move cabac tables to their own source file | upstream (2b0ec9006167) |
| v9-06.patch | media: rkvdec: Use structs to represent the HW RPS | upstream (cf29115a68e3) |
| v9-07.patch | media: rkvdec: Move h264 functions to common file | upstream (560438ed7c2c) |
| v9-08.patch | media: rkvdec: Move hevc functions to common file | upstream (34e2b14ae90e) |
| v9-09.patch | media: rkvdec: Add variant specific coded formats list | upstream (ae2070ca8ab2) |
| v9-10.patch | media: rkvdec: Add RCB and SRAM support | upstream (e5640dbb991c) |
| v9-11.patch | media: rkvdec: Support per-variant interrupt handler | upstream (f9c7b7deeffd) |
| v9-12.patch | media: rkvdec: Enable all clocks without naming them | upstream (6a846f7d72c7) |
| v9-13.patch | media: rkvdec: Disable multicore support | upstream (e570307ac987) |
| v9-14.patch | media: rkvdec: Add H264 support for the VDPU381 variant | upstream (e5aa698ea659) |
| v9-15.patch | media: rkvdec: Add H264 support for the VDPU383 variant | upstream (fde24907570d) |
| v9-16.patch | media: rkvdec: Add HEVC support for the VDPU381 variant | upstream (c9a59dc2acc7) |
| v9-17.patch | media: rkvdec: Add HEVC support for the VDPU383 variant | upstream (e3b5b77e3689) |

### patches/vpu/ — VP9 VDPU381 (Experimental)

| Patch | Subject | Status | Notes |
|-------|---------|--------|-------|
| dvab-sarma-vp9-vdpu381.patch | testing: RKVDEC 2 based vp9 hardware decoder | obsolete | Original dvab-sarma patch. Fails to apply on 7.0-rc1 (driver code restructured). Superseded by adapted version. |
| vp9-vdpu381-adapted.patch | VP9 VDPU381 support (adapted) | **still-needed** | Adapted VP9 patch. `git apply --check` passes on 7.0-rc1. Experimental — VP9 VDPU381 is not upstream. |

### patches/dts/ — Device Tree

| Patch | Subject | Status | Notes |
|-------|---------|--------|-------|
| rkvdec2-vdpu381-dts-nodes.patch | Add vdec0/vdec1/SRAM nodes to rk3588-base.dtsi | **upstream** | All nodes (vdec0, vdec0_mmu, vdec1, vdec1_mmu, vdec0_sram, vdec1_sram) are present in 7.0-rc1 rk3588-base.dtsi. Patch fails to apply (context drift) but content is identical. |

### patches/display/ — HDMI 2.0

| Patch | Subject | Status | Notes |
|-------|---------|--------|-------|
| 0000-Add-HDMI-2.0-support-to-DW-HDMI-QP-TX.patch | Cover letter | N/A | Not a valid patch (cover letter for the series). |
| 0001-drm-bridge--Add---detect_ctx-hook-... | drm/bridge: Add ->detect_ctx hook | **obsolete** | Applies cleanly but SHOULD NOT be applied. Upstream took a different approach: 5d156a9c3d5e "drm/bridge: Pass down connector to drm bridge detect hook" (by Andy Yan, Rock-chips). Our detect_ctx approach is superseded. |
| 0002-drm-bridge-connector--Switch-to-using---detect_ctx-hook.patch | Switch to detect_ctx | **obsolete** | Companion to 0001. Superseded by same upstream change. |
| 0003-drm-bridge--dw-hdmi-qp--Add-high-TMDS-clock-ratio-and-scrambling-suppo.patch | High TMDS clock ratio + scrambling | **needs-rebase** | Fails to apply. Upstream has 2d7202c6f38d "replace mode_valid with tmds_char_rate" which partially covers this. Need to verify if scrambling support is complete upstream or if parts of this patch are still needed. |
| 0004-drm-rockchip--dw_hdmi_qp--Do-not-send-HPD-events-for-all-connectors.patch | HPD events optimization | **still-needed** | `git apply --check` passes. Switches from drm_helper_hpd_irq_event (fires for ALL connectors) to drm_connector_helper_hpd_irq_event (specific connector). Upstream still uses the broad approach. Nice optimization but non-critical. |

### patches/npu/, patches/audio/, patches/gpu/, patches/misc/

No patch files in these directories. NPU (rocket), audio (HDMI), and GPU (panthor) drivers were already upstream before 7.0-rc1.

## Patches to Apply

Only 2 patches need to be applied to 7.0-rc1:

1. **`patches/vpu/vp9-vdpu381-adapted.patch`** — VP9 hardware decode for VDPU381 (experimental, by dvab-sarma)
2. **`patches/display/0004-drm-rockchip--dw_hdmi_qp--Do-not-send-HPD-events-for-all-connectors.patch`** — HPD event optimization

### Patches NOT to apply (and why)

- **v9-01 through v9-17**: All upstream
- **dvab-sarma-vp9-vdpu381.patch**: Superseded by adapted version
- **rkvdec2-vdpu381-dts-nodes.patch**: DT nodes upstream
- **0000**: Cover letter
- **0001, 0002**: detect_ctx approach superseded by upstream alternative
- **0003**: Partially upstream, needs rebase — skip for now (TMDS functionality exists upstream)

## Key Finding

**18 of 25 patches are now upstream.** The 7.0-rc1 kernel has native RKVDEC2 H.264/HEVC support with all DT nodes for RK3588. Only the experimental VP9 VDPU381 support and an HPD optimization remain as custom patches.

## Build & Verification Results (2026-02-28)

Both custom patches were applied successfully and the kernel was built, deployed, and verified:

| Patch | Applied | Notes |
|-------|---------|-------|
| vp9-vdpu381-adapted.patch | **OK** | Required fix: VP9 .c/.h source files extracted from original dvab-sarma patch. Struct field names updated for upstream compatibility (reg009→reg009_dec_mode, reg011→reg011_important_en, etc.). Block gating field split into individual bitfields matching H264/HEVC upstream pattern. |
| 0004 HPD events | **OK** | Applied with offset, no modifications needed. |

**Config**: `configs/rock5b_7.0-rc1.config` — based on Panda's BredOS config (clean boot), `DEBUG_PREEMPT=n`, static `PREEMPT=y`.

**Build**: Docker cross-compilation (arm64 native on Apple Silicon), `git archive` tarball approach (avoids macOS case-insensitivity).

**Hardware verification** (all passing):
- Kernel: 7.0.0-rc1 SMP PREEMPT, boot without black screen
- V4L2: 6/6 devices (rkvdec H.264/HEVC/VP9, hantro, AV1, vepu121, hdmi-rx, rga)
- GPU: Panthor v1.7.0 (Mali-G610)
- NPU: Rocket (3 cores)
- WiFi: RTL8852BE (rtw89, interface `wlP2p33s0`)
- HDMI: 1 connected, 1 disconnected
- Bluetooth: RTL8852BU firmware loaded
