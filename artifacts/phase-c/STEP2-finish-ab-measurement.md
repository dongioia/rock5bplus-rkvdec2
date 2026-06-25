# Step-2 finish — A/B measurement: zero-copy vs CPU-copy (vulkandownload) → quantified

**Date:** 2026-06-25. **Board:** Rock 5B+, rkvdec; PanVK `~/mesa-zc/` (!42353), system mesa `1:26.0.6-1` intact.
**Code:** `scripts/vvtest/zc_measure.c` + `zc-measure-run.sh`. Independent-reviewed (verdict MERGE-AFTER-FIXES; all fixes folded).

## Method
Stream the clip; per frame, import the rkvdec NV12 dmabuf and do ONE of:
- **copy** (models `vulkandownload`): `vkCmdCopyImageToBuffer` NV12 → host buffer, then a **full-frame consumer read** (every cache line of both planes). What the status-quo path pays to put the decoded frame in system memory.
- **zerocopy**: ycbcr fragment-sample render to an offscreen RGBA target — GPU only, **no readback**.
Both: import + submit + fence-wait + destroy per frame; gst HW decode common to both. CPU via `getrusage` (utime+stime, whole process), wall via `clock_gettime`. Off-screen (no FIFO). 3 runs/mode.

## Result (3× runs, tight ranges)
| res | copy CPU ms/frame | zerocopy CPU ms/frame | CPU reduction | fps (both, decode-bound) |
|---|---|---|---|---|
| 720p (demo.h264, 120f) | 2.68–2.75 (util ~77%) | 0.52–0.59 (util ~16%) | **~80%** (copy ≈ 5×) | ~287 |
| 1080p (c1080.h264, 120f) | 5.66–5.77 (util ~59%) | 0.80–0.84 (util ~8.5%) | **~85%** (copy ≈ 7×) | ~103 |

- The CPU delta (≈2.15 ms/frame @720p, ≈4.9 ms/frame @1080p) scales ~with pixel area (2.3×) → it **is** the readback+consume cost.
- **Throughput is decode-bound and equal in both modes** (rkvdec is the bottleneck; the per-frame GPU op isn't). Per-frame `vkWaitForFences` serializes both, so this harness does NOT measure pipelined throughput — only that zero-copy isn't the bottleneck and costs far less CPU.

## Honesty caveats (conservative — real win is larger)
- The measured CPU delta is **host-side only**: PanVK's `copyImageToBuffer` is itself a GPU compute pass (verified in PanVK source), so the readback's **memory-bandwidth** cost is NOT in `getrusage`. The numbers are a **lower bound**.
- The "consumer read" only **reads** the buffer; a real `vulkandownload` consumer also **memcpy/re-uploads** it — more CPU still. So real-world zero-copy advantage > the 80–85% shown.
- Numbers are indicative (3 runs, one board); the direction and order of magnitude are the result, not exact percentages.

## Step-2 COMPLETE
Zero-copy via PanVK on RK3588 is: **feasible** (Stage 1), **pixel-correct** (1a byte-exact import, 1b/2a HW-YUV CSC 80 dB), **on-screen** (2b: window visible, 30 frames, no readback), and **quantified beneficial** (this: ~80–85% less per-frame video CPU vs vulkandownload, scaling with resolution). System mesa pin untouched throughout; isolated `~/mesa-zc/` deploy.

Optional hardening NOT done (not required to claim Step-2; documented for later): frame pipelining (overlap decode/render/present), swapchain recreate on OOD/SUBOPTIMAL, a real pipelined-throughput benchmark.

## Next
Step-3 — in-browser zero-copy (WebKit): the next phase, its own spec + review cycle, with the "no WebKit patch" decision made with this Step-1/2 information.
