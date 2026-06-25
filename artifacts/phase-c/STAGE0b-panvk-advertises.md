# Stage-0b result — PanVK advertises the import (OQ2, import side) → STAGE 0 PASS

**Date:** 2026-06-25. **PanVK:** Mesa 26.2.0-devel (git-e5ec9502, !42353), built isolated in the
`dev-server` container (M4, ~1 min), deployed to the SBC `~/mesa-zc/` (system mesa 26.0.6 untouched).
**Query:** `scripts/vvtest/zc_modifier_query.c` (vkGetPhysicalDeviceFormatProperties2 +
VkDrmFormatModifierPropertiesListEXT, + vkGetPhysicalDeviceImageFormatProperties2).

## Result: clean PASS
- PanVK loads on the SBC as `Mali-G610 MC4`, driverID `MESA_PANVK`, apiVersion 1.4.354.
- Import extensions present: `VK_EXT_image_drm_format_modifier` (rev2), `external_memory_dma_buf`,
  `external_memory_fd`, `sampler_ycbcr_conversion` (rev14).
- For `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` (NV12), PanVK advertises **exactly one** DRM modifier:

  | modifier | planes | sampled | ycbcr-linfilter | midpoint | cosited |
  |---|---|---|---|---|---|
  | `0x0` (LINEAR) | 1 | yes | **yes** | yes | yes |

- `vkGetPhysicalDeviceImageFormatProperties2(NV12, LINEAR, SAMPLED, DRM_MODIFIER tiling)` = **SUPPORTED**.

## Why this closes Stage 0
- rkvdec exports LINEAR NV12 (Stage 0a). PanVK's ONLY importable NV12 modifier is LINEAR. **They match.**
- The LINEAR modifier carries the **ycbcr-conversion** feature (`ycbcrLinFilter=1`) → the HW-YUV
  sampler path (spec 1b) is supported; **no SW-CSC fallback needed**.
- `planes=1` confirms the non-disjoint single-`vkBindImageMemory` model (spec F2).
- Both midpoint + cosited chroma siting supported → siting can be matched to the stream.

## Bearing on the thesis
Zero-copy via PanVK on RK3588 is **feasible** at the import boundary — the kill-switch did not kill it.
Remaining for Stage 1: actually import the real rkvdec dmabuf (strict_import pitch 1280 at create
time), then the dual-check (1a non-converting per-plane byte-exact, 1b HW-YUV PSNR).
