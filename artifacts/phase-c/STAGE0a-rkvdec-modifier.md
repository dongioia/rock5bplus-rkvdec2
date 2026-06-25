# Stage-0a result — rkvdec exported modifier (OQ2, export side)

**Date:** 2026-06-25. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec `/dev/video0`.
**Method:** `scripts/vvtest/zc_modifier_probe.py` (GStreamer GI) + `v4l2-ctl --list-formats`.

## Finding: LINEAR NV12, single allocation

- rkvdec CAPTURE offers exactly one pixelformat: `NV12` (no Rockchip-tiled variant).
- Negotiated caps: plain `video/x-raw, format=NV12` — **no `drm-format` / modifier token** → `DRM_FORMAT_MOD_LINEAR`.
- The CAPTURE `GstMemory` is already **dmabuf-backed** (`is_dmabuf_memory=True`), even on the default (system-caps) path — export is trivial; the SRC pad also advertises `video/x-raw(memory:DMABuf)`.
- `n_memory = 1` → **single allocation, non-disjoint** (both planes in one buffer).

### Exact layout (1280x720), for `pPlaneLayouts`
| plane | aspect | offset | rowPitch |
|---|---|---|---|
| 0 (Y) | PLANE_0 | 0 | 1280 |
| 1 (UV) | PLANE_1 | 921600 | 1280 |

`total_size = 1843200` (Y 921600 + UV 460800 = 1382400 used; 460800 trailing scratch, benign).

## Bearing on the spec
- Confirms F2: bind non-disjoint, single `vkBindImageMemory`, plane offsets via `pPlaneLayouts`.
- Linear is outside the modifiers `!42353` rejects (`INTERLEAVED_64K`/`16x16`).
- "linear unproven" → **measured** on the export side. Import side (PanVK advertises `(NV12, LINEAR, SAMPLED + ycbcr)`) = Stage 0b, needs the `!42353` Mesa build.
- Prior GL "external-only" (claude-mem #5564) was a GL-sampler quirk, not tiling — does not contradict this linear-export finding.
