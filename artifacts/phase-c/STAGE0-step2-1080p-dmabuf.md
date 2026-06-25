# Step-2 Stage-0 result — 1080p IS zero-copyable (gst negotiation, not hardware) → GATE PASS

**Date:** 2026-06-25. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec `/dev/video0`, gst 1.28.4.
**Question:** Step-1 found 1920×1080 (and 1366) rkvdec CAPTURE came back as **system memory** (no dmabuf, no GstVideoMeta) under default gst negotiation, while 1280×720/1088 were dmabuf. Is 1080p (primary browser res) un-zero-copyable, or was that an artifact?

## Result: artifact. 1080p hardware dmabuf IS available — the decoder was *copying*.

Root cause, from the gst v4l2codecs decoder log:
```
WARN gst_v4l2_codec_h264_dec_decide_allocation: GstVideoMeta support required, copying frames.
```
The 1080p rkvdec hardware CAPTURE buffer is **padded** (alignment/scratch ⇒ stride/offset ≠ packed), so it needs `GstVideoMeta` to describe it. When downstream does **not** advertise `GstVideoMeta` support, the v4l2codecs decoder **copies** the padded hardware buffer into a packed system buffer (the 3110400 = 1920·1080·1.5 exact-packed buffer Step-1 saw, with no rkvdec scratch). At 720p the hardware buffer happens to be unpadded (stride==width), so no meta is needed and the dmabuf passes through even to a meta-unaware sink — which is why 720p "worked" and 1080p didn't.

### Evidence (copies counted via `GST_DEBUG=2 | grep "copying frames"`)
| pipeline | copies |
|---|---|
| 720p `! fakesink` (meta-unaware) | 0 |
| 1080p `! fakesink` (meta-unaware) | **1** (copies) |
| 1080p `! fakevideosink` (meta-aware) | **0** (keeps hw dmabuf) |
| 720p `! fakevideosink` | 0 |

### Causes RULED OUT
- **CMA exhaustion:** `CmaFree` = 51616 kB **before and after** 1080p decode (unchanged) → rkvdec CAPTURE is not consuming CMA; no CMA pressure. (Board CMA total 64 MB, no `cma=`.)
- **DPB/pool size:** baseline-profile 1080p (`-refs 1 -bf 0`, small DPB) behaves **identically** to high-profile 1080p (both copied) → not a per-frame-count/pool effect.
- **Stride padding breaking gst:** the *system* fallback buffer is exact-packed; the *hardware* buffer is padded but valid — padding is the reason meta is required, not a failure.
- **Forced `capture-io-mode=dmabuf`:** invalid — `v4l2slh264dec` (v4l2codecs stateless) has **no such property** (gst-inspect on board); that lever does not exist. Explicit `video/x-raw(memory:DMABuf)` capsfilter **stalls at any resolution** (incl. 720p) — a gst negotiation deadlock, not a fix.

## Gate decision: PASS
Zero-copy is **resolution-general**, including 1080p. The Step-1 "1080p → system memory" was a consequence of the plain appsink being meta-unaware, **not** a CMA or hardware bound. This resolves **OQ-S2-1** and **refutes the carry-over "zero-copy capped to ≤1280-width" concern**.

## Requirement this imposes on Step-2 implementation
The dmabuf consumer (appsink, or the custom Vulkan present sink) **MUST advertise `GstVideoMeta` support** in its allocation-query response, or the decoder copies and zero-copy is lost at padded resolutions. In C this is one call in a downstream allocation-query handler / pad probe:
`gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL)`.
(`zc_import_test.c` uses a plain appsink → at 1080p it currently gets the copied system buffer and import would fail "mem0 not dmabuf"; the first Step-2 task adds meta support, then imports the **padded** 1080p hardware dmabuf — which also validates the import's stride/offset handling on a genuinely padded layout, the geometry-generalization Step-1 could not reach because non-meta sinks masked it.)

Note: forcing meta support in Python via a query pad-probe was attempted and hit PyGI limitations (VideoMeta API GType not registerable without instantiation); trivial in C. The gate is settled by the `fakevideosink` (meta-aware) result above, which needs no custom code.

## Next (Step-2 impl, task 1)
Add `GstVideoMeta` support to the consumer; import the padded 1080p hardware dmabuf into PanVK (1a-at-1080p, padded); then the present path (sub-gates 2a offscreen / 2b swapchain) per the spec.
