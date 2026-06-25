# Step-2 Increment-1 — meta-aware padded import + graphics-path ycbcr (sub-gate 2a) → PASS

**Date:** 2026-06-25. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec; PanVK `~/mesa-zc/` (!42353, git-`e5ec9502`), system mesa `1:26.0.6-1` intact.
**Artifact code:** `scripts/vvtest/zc_present_test.c` + `zc_present.vert`/`.frag` + `zc-present-run.sh`. Two independent adversarial reviews (bug-find + final); both folded.

## What this increment proves (both PASS)

| clip | coded | visible | FLAT PSNR | max\|d\| | notes |
|---|---|---|---|---|---|
| case1 | 1280×720 | 1280×720 | 80.27 dB | 3 | px[0,0]+center byte-exact |
| hd1080 | 1920×**1088** | 1920×1080 | 76.54 dB | 3 | **padded** UV offset 2088960 |

1. **Meta-aware consumer** (`meta_probe`: a GStreamer query pad-probe adds `GST_VIDEO_META_API_TYPE` to the ALLOCATION query) makes the v4l2codecs decoder keep the **padded hardware dmabuf** instead of copying — the C implementation of the Step-2 Stage-0 finding. **1080p zero-copy import now works** (plain appsink got a copied system buffer; meta-aware gets the real dmabuf).
2. **Padded-layout import validated** — 1080p decodes to coded 1920×1088 (height padded 1080→1088), UV at offset 2088960 ≠ visible-packed; imported + sampled byte-correct. This is the genuine padded geometry generalization Stage-1 could not reach (non-meta sinks masked it).
3. **Graphics (fragment-shader) ycbcr path** (sub-gate 2a) — render the imported NV12 through `VkSamplerYcbcrConversion` in a fragment shader into an offscreen RGBA color attachment, read back, flat-chroma PSNR. This was the reviewer's #1 risk (Stage-1 only proved the COMPUTE path). Correct at both resolutions.

## Bug found + fixed (by independent review)
**Missing CSF barrier** between `vkCmdEndRenderPass` and `vkCmdCopyImageToBuffer`. PanVK CSF (verified in source) does NOT auto-sync the fragment STORE against the downstream copy, and its `copyImageToBuffer` is a **compute** meta-dispatch that samples the RT. Without a barrier the copy raced the tiler → tile-granular partial reads (proven from the dump: top rows alpha=0/unstored, tile-aligned good/bad boundary at row 352, byte-exact bottom half — manifested as a "sheared color-bars + white" image). **Fix:** `rt2copy` image barrier `COLOR_ATTACHMENT_OUTPUT`/`COLOR_ATTACHMENT_WRITE` → `TRANSFER|COMPUTE_SHADER`/`TRANSFER_READ|SHADER_READ` (old==new==`TRANSFER_SRC_OPTIMAL`, execution+memory dep only). Plus a coded-vs-visible fix (VideoMeta height is coded 1088; ref/compare use caps visible 1080).

## Carry-forward to Increment-2 (swapchain present 2b + streaming) — NOT blockers for this commit
1. **2b correctness:** the 2a gate compares only the VISIBLE region, so it cannot catch **coded padding rows being scanned out**. 2b must present/blit at visible extent (crop the source rect) or prove padding isn't displayed.
2. **Streaming:** `gate_2a` leaks all per-frame Vulkan objects (one-shot, fine); and `import_image`'s generic `CHECK` error paths don't `close(f->fd)` (Stage-1a did). For the loop: hoist frame-invariant objects (sampler, conversion, DSL, pipeline, render pass) out; destroy/pool per-frame (RT image+mem, framebuffer, views, descriptor pool, staging); close fd on every error path.
3. **Robustness:** assert the expected padded geometry (stride≠width / coded≠visible at 1080p) rather than only inferring success from `gst_is_dmabuf_memory`; verify the GstSample ref can be released once the dmabuf is imported.

## Next
Increment-2 = swapchain present (sub-gate 2b, on sway) + streaming loop + sync (fence/FOREIGN_EXT acquire) + no-copy/throughput measurement vs the vulkandownload baseline, per the Step-2 spec.
