# Stage-1a result — rkvdec DMA-BUF imported into PanVK, byte-exact (import geometry)

**Date:** 2026-06-25. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec `/dev/video0`.
**PanVK:** Mesa 26.2.0-devel (git-`e5ec9502`, !42353), isolated `~/mesa-zc/` (system mesa `1:26.0.6-1` untouched, verified pre/post every run).
**Artifact:** `scripts/vvtest/zc_import_test.c` (+ `zc-run.sh` runner). Independent-reviewed (general-purpose agent) before this result was accepted; findings folded (below).

## Gate 1a = PASS, byte-exact, across 3 geometries

Per-plane `vkCmdCopyImageToBuffer` (PLANE_0/PLANE_1 aspects) of the imported image → byte-compare vs an **independent** ffmpeg SW-decode NV12 (`ffmpeg -i <clip> -frames:v 1 -pix_fmt nv12`, NOT re-derived from the GstVideoMeta used for the import layout → no F1 false-green).

| clip | res | profile | offset[1] | bytes | result |
|---|---|---|---|---|---|
| case1.h264 | 1280×720 | Constrained Baseline | 921600 | 1382400 | byte-exact (PSNR ∞) |
| re720.h264 | 1280×720 | High | 921600 | 1382400 | byte-exact |
| t1088.h264 | 1280×1088 | High | **1392640** | 2088960 | byte-exact |

- t1088's offset[1]=1392640 ≠ case1's 921600 with a different total size → proves the import layout is read from `GstVideoMeta` (stride/offset), **not hardcoded**. Geometry generalizes within the dmabuf domain.
- Y-distinct 206–218 (real frame content, not blank); `memcmp` over the full buffer is the actual gate.

## What this proves (Stage-1, OQ14 partial)
- Cross-IOMMU DMA-BUF import (rkvdec V4L2 domain → Mali) **succeeds** and is **pixel-faithful**: PanVK's copy path reads the exact rkvdec bytes == independent ffmpeg decode.
- Single non-disjoint `vkBindImageMemory`, explicit LINEAR DRM-modifier layout, dedicated import (`allocationSize = memReq.size`, ≤ dmabuf size — benign trailing scratch).
- `UNDEFINED → TRANSFER_SRC` barrier preserves content for LINEAR (no retiling). For Step-2 (long-running) switch to a `VK_QUEUE_FAMILY_FOREIGN_EXT` acquire to be portable; here it is correct-for-LINEAR.

## PanVK bug found (advertise/consume mismatch) — upstream-reportable
- The LINEAR modifier advertises `drmFormatModifierPlaneCount = 1` for NV12 (one memory plane), so a spec-conformant app supplies `pPlaneLayouts` length 1 (VU 02265).
- But the import path consumes **2**: `get_plane_count` (`panvk_image.c:121`) → `vk_format_get_plane_count(NV12)=2` → `image->plane_count=2` → layout loop (`:482`) reads `pPlaneLayouts[0]` **and** `[1]`. **Confirmed by driver instrumentation** (logged `plane=0` and `plane=1`).
- With count=1 the (loader/layer) safe-copy of the pNext chain is length 1, so the driver reads `[1]` from a 1-long copy → garbage (`off=65, pitch=3.8e9`) → `"WSI offset not properly aligned"` (`pan_mod.c` linear handler).
- **Workaround in the spike:** declare count=2, supply Y+UV. Works with the Khronos validation layer ON and OFF.
- **Anti-hallucination note:** declaring count=2 is technically non-conformant vs the advertised 1, BUT the validation layer present on the SBC emitted **no VUID** for it (verified: full stderr scan shows only `+validation`, no `VUID`/`02265`). So earlier assertions (mine *and* the reviewer's) that it "trips VUID-02265 every run" were **unverified/false** — the code comment is corrected to match observed behavior.

## DMA-BUF domain bound (important for the zero-copy thesis)
rkvdec only **dmabuf-exports aligned resolutions**:
- 1280×720 and 1280×1088 → dmabuf (VideoMeta present, stride==width, offset==stride*height, unpadded).
- 1920×1080 and 1366×768 → **system memory**, no VideoMeta (packed) → **not importable, no zero-copy**.
- Encoding-independent (re-encoded High-profile 720p still dmabuf); width-dependent (1280 ok at multiple heights; 1920/1366 fall back).
- Consequence: every dmabuf case observed has stride==width and offset==stride*height (no padding). The "pitch≠width masks stride bugs" risk is real in principle but appears **unreachable** in rkvdec's dmabuf path on this board — Step-2 should re-check on the resolutions it actually targets (notably whether 1080p can be coaxed into dmabuf, since it is a primary browser resolution).

## Review findings folded into the code
- count=2 documented precisely as a PanVK bug workaround (not "benign"); VU claim corrected to observed (no VUID emitted).
- fd `close()` added on pre-`vkAllocateMemory` error paths (real leak when Step-2 loops this; harmless at process-exit here).
- `ZC_NOVALIDATE` env to skip the layer (mimics production no-layer pipeline).
- stale `-lm` header-comment build line fixed.
- Mesa SHA: the tree carries !42353 in rebased form at `e5ec9502`; the literal `78e55592` is not an ancestor — cite by-feature / `e5ec9502`.

## Next: Stage-1b (the check that matters for zero-copy)
`VkSamplerYcbcrConversion` (BT.709 limited for 720p / per-VUI, siting matched) combined-sampler view on the **same import** → sample to RGB → PSNR vs CPU NV12→RGB at identical params. To be written, **independently reviewed before deploy**, then run.
