# Phase C — Zero-Copy Step 2: Standalone no-copy on-screen pipeline (design)

**Date:** 2026-06-25
**Branch:** `spec/vulkanvideo-phaseC-zerocopy`
**Status:** design (pre-plan). **One independent adversarial review folded (verdict HOLD-WITH-CHANGES; all must-fix incorporated).** Builds on STAGE 1 PASS (`artifacts/phase-c/STAGE1{a,b}-*.md`).
**Prereq met:** Step 1 proved rkvdec NV12 DMA-BUF imports into PanVK byte-exact (1a, TRANSFER path) and the Mali HW ycbcr sampler converts it to correct RGB (1b, **compute** path, off-screen, host-readback-validated — single frame).

## Goal

Decode on rkvdec → DMA-BUF → PanVK HW-YUV sample → **on screen, no GPU→system-memory readback** — and show it beats the CPU-copy baseline (`vulkanh26Xdec ! vulkandownload ! waylandsink`, which reads back every frame). Standalone (no browser; Step 3). Turns the Step-1 single-frame, off-screen, compute proof into a **streaming, presented, graphics** pipeline.

## Context (what is and is NOT already proven)

- Step 1 proved, on ONE frame, OFF-SCREEN, via a **compute** shader writing an SSBO that was copied to host: import correctness (1a) and HW ycbcr CSC correctness (1b, flat-chroma 82 dB). It did **not** exercise a graphics pipeline, a swapchain, `vkQueuePresentKHR`, streaming, or motion. Those are all net-new in Step 2.
- The **CPU-copy** present path is proven end-to-end on the SBC (19/06): `vulkanh264dec → vulkandownload → waylandsink`, compositor **sway** (`WAYLAND_DISPLAY=wayland-1`, `XDG_RUNTIME_DIR=/run/user/1000`, HDMI-A-1). Baseline to beat. Note it does readback **and** re-upload (waylandsink re-imports for composition).
- PanVK (`~/mesa-zc/`, !42353, git-`e5ec9502`) has Wayland WSI — **verified in source**: `KHR_swapchain`, `KHR_swapchain_mutable_format`, `KHR_wayland_surface` advertised (`panvk_vX_physical_device.c`), `wsi_device_init`/`wsi_common_create_swapchain_image` present. System mesa `1:26.0.6-1` stays pinned.
- `vulkansink` exists on the board (a candidate present element, vs a custom Vulkan loop — see Presentation).

## Decode source — decision (single-device first)

- **(chosen) V4L2 (`v4l2slh264dec`/`h265dec`) → appsink dmabuf → PanVK.** Decode is the **kernel**, so the only `VkPhysicalDevice` is PanVK → **no Vulkan↔Vulkan timeline semaphore.** This is NOT the same as "no sync": rkvdec (arm-smmu-v3 iommu group 8) and Mali-G610 (panthor, its own integrated MMU) are in **distinct IOMMU/MMU domains**, so the rkvdec→Mali cross-IOMMU + cache-coherency boundary is real and must be handled at import (see Sync). Step-1a crossed it once via the TRANSFER path; Step 2 crosses it streaming via the SAMPLING path.
- **(deferred to Step 3) Vulkan-Video decode ICD → exported VkImage dmabuf → PanVK** = the genuine two-`VkPhysicalDevice` model needing `VK_KHR_external_semaphore_fd` cross-device sync. **Step 2 does NOT exercise cross-device sync** — so it does not de-risk the browser path's hardest sync unknown; Step 3 must take that on first (see OQ-S2-4).

Rationale: the one genuinely novel Step-2 unknown is **PanVK graphics-sample → present, no readback**, identical regardless of dmabuf origin. Prove it on the simplest reliable source.

## Stage 0 — Kill-switch: can 1080p be zero-copied? (DONE → PASS)

**RESULT (2026-06-25, `artifacts/phase-c/STAGE0-step2-1080p-dmabuf.md`): PASS — 1080p IS zero-copyable.** The Step-1 "1080p → system memory" was a gst negotiation artifact: the v4l2codecs decoder logs `GstVideoMeta support required, copying frames` and copies the **padded** 1080p hardware buffer to a packed system buffer **when downstream lacks GstVideoMeta support**. Proven: 1080p `! fakesink` (meta-unaware) copies; 1080p `! fakevideosink` (meta-aware) keeps the hardware dmabuf (0 copies). RULED OUT: CMA (CmaFree unchanged across decode), DPB/pool (baseline 1080p copies identically), and the `capture-io-mode` lever (no such property). **Imposes on Step-2: the dmabuf consumer MUST advertise `GST_VIDEO_META_API_TYPE` (one call, C-side) or zero-copy is lost at padded resolutions.** Resolves OQ-S2-1; refutes the ≤1280 carry-over. The original investigation method (below) is retained for record.

### Stage-0 method (as designed)

Step 1 found: under **default** gst negotiation, rkvdec CAPTURE is already dmabuf-backed at 720p (`STAGE0a`, no forced io-mode), and at **1280×1088** too — but **1920×1080 and 1366×768 came back as system memory, no `GstVideoMeta`**. So the variable is **width/stride, not an io-mode choice.** (Correcting the prior draft: `v4l2slh264dec` has **no `capture-io-mode` property** — verified by `gst-inspect-1.0` on the board; it is the `v4l2codecs` stateless element, not legacy mem2mem. Forcing io-mode is not a real lever.)

Stage-0 must DISTINGUISH the cause, because two are plausible and they have opposite consequences:

1. **gst/stride emission** — re-probe 1920×1080 on default negotiation and measure `n_memory`, `bytesperline`/stride, plane offsets, and whether `GstVideoMeta` is emitted. If rkvdec pads 1080p stride (1920→2048) or returns `n_memory>1`, gst may drop the dmabuf/VideoMeta path even though the buffer is exportable. All Step-1 dmabuf cases had stride==width unpadded — 1080p may simply be padded.
2. **CMA exhaustion (real hardware bound)** — the board has **`CmaTotal: 65536 kB` (64 MB), no `cma=` cmdline** (verified `/proc/meminfo` + cmdline). If rkvdec CAPTURE is CMA-backed, an 8–16-buffer pool of ~3.1 MB 1080p NV12 frames (+ OUTPUT side + GPU/compositor) can exhaust CMA → driver falls back to system pages. **Measure whether rkvdec CAPTURE is CMA-backed** (`/sys/kernel/debug/dma_buf/bufinfo` / which dma-heap, needs root) and whether raising `cma=` or shrinking the pool changes the 1080p outcome.

Fallback export path (if gst won't surface 1080p dmabuf but the buffer is exportable): drive V4L2 by hand — `REQBUFS(CAPTURE, memory=MMAP)` → STREAMON → decode → `VIDIOC_EXPBUF` each CAPTURE buffer to an fd, check the fd's plane layout is LINEAR NV12 at the 1080p stride. (The decode-ICD reportedly does this — **asserted, not re-verified: the ICD C source is not in this repo and `mesa-sree` is not mounted in `dev-server`; only inferred from `debug-instrumentation.patch` field names + a memcpy comment.**)

**Gate:** 1080p rkvdec CAPTURE surfaces an importable LINEAR NV12 dmabuf (gst default, or manual EXPBUF) → zero-copy is resolution-general. If **no** path exports 1080p dmabuf, document the real bound (CMA vs stride-align) — zero-copy is limited to the widths that do, and Step-3's browser value is scoped accordingly. **Either way Step 2 still proceeds at 1280-width** to prove the present mechanism; only the 1080p throughput row is dropped.

## Architecture (chosen path)

```
gst: filesrc ! parse ! v4l2sl{h264,h265}dec (CAPTURE dmabuf) ! appsink
        |  per frame: GstSample -> dmabuf fd + GstVideoMeta + (sync_file fence)
        v
PanVK (single VkDevice): import fd -> VkImage (LINEAR NV12, explicit layout,
        drmFormatModifierPlaneCount=2 workaround) -> FOREIGN_EXT acquire ->
        ycbcr-sample in a FRAGMENT shader, render into the acquired swapchain
        image -> vkQueuePresentKHR (Wayland/sway)
```

No `vulkandownload`, no host pixel buffer in the per-frame loop.

## Step-2 design

### Presentation (staged sub-gates — Stage 1 proved compute, NOT this)
- **Sub-gate 2a (isolate the sampler from WSI):** fragment-shader ycbcr combined sampler (the 1b conversion) → render to an **offscreen** RGBA color attachment → read back once → confirm it matches the 1b flat-chroma CSC. Proves the *graphics* ycbcr path (render pass + immutable ycbcr sampler in a frag stage) independent of presentation. Only then:
- **Sub-gate 2b (WSI):** same draw into an **acquired swapchain image** → present, under sway. Surface = Wayland. **Query `vkGetPhysicalDeviceSurfaceFormatsKHR` + `…PresentModesKHR` first** and assert a UNORM B/RGBA format + the intended present mode exist (`KHR_swapchain_mutable_format` is available if a format alias is needed).
- ycbcr requires a **sampler**, so this is a `vkCmdDraw`, **not** `vkCmdBlitImage` (blit-convert of a ycbcr image is illegal). 
- Imported NV12 image is **per-frame** (new fd per appsink sample). V4L2 recycles a small fixed CAPTURE set, so cache `fd → VkImage/VkDeviceMemory` by V4L2 buffer index rather than create-per-frame; measure whether per-frame creation is even a bottleneck before optimizing.
- Custom Vulkan loop preferred over `vulkansink` for the spike (full control of import + acquire); `vulkansink` noted as a fallback if WSI plumbing is the blocker.

### Sync (single Vulkan device, but cross-IOMMU — fence-first)
- **Default = import the dmabuf's implicit fence** (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` → `VK_KHR_external_semaphore_fd`) and wait on it, **plus** a `VK_QUEUE_FAMILY_FOREIGN_EXT` acquire barrier (`UNDEFINED→SHADER_READ_ONLY`) on the imported image — coherency for *texture reads* across rkvdec-SMMU→Mali-MMU is not guaranteed by CPU-side DQBUF alone. ("DQBUF readiness is enough, no fence" is the **optimization to prove**, not the baseline — inverted from the prior draft.)
- **dmabuf lifetime:** ref-hold the GstSample per in-flight frame, release on the frame's fence — else V4L2 recycles the CAPTURE buffer under the GPU (corruption).
- **Pool-depth starvation (distinct hazard):** constrain `in_flight_frames < (capture_pool_size − decoder_min_free)`; if in-flight ≥ pool size the decoder starves (no free CAPTURE buffer) → stall/deadlock, not corruption. Set REQBUFS count explicitly and cap swapchain depth + in-flight accordingly.

### Measurement (the point of Step 2)
- **No-copy proof (two signals, audit is not enough):** (a) app command stream has zero `vkCmdCopyImageToBuffer`/host pixel memcpy; **and** (b) an *independent* signal that PanVK inserts no internal staging — GPU memory-bandwidth / perf counter (`PANVK_DEBUG`/perfetto): zero-copy ≈ 1× frame-read bandwidth, the readback path ≈ 2× + a CPU memcpy. (Internal copy is unlikely — STAGE1a/b: LINEAR sampled directly on the Valhall texture unit, no retile — but prove it, don't assume.)
- **Throughput:** measure with **MAILBOX or an offscreen render-to-exportable target to UNCLAMP FPS** (FIFO/vsync pins both paths to refresh and hides the delta). Report **CPU utilization as the primary win** (the memcpy is CPU-side). State the baseline honestly: `vulkandownload`→`waylandsink` = readback **+** re-upload, so the fair copy-cost being removed is the readback.
- **Correctness on motion:** capture a presented/offscreen frame mid-stream, confirm it matches 1b flat-chroma CSC; confirm no progressive corruption over ≥1000 frames (lifetime/recycle correctness).

## Verification gates

| Gate | Pass criterion |
|---|---|
| Stage 0 | 1080p rkvdec CAPTURE surfaces an importable dmabuf (default or EXPBUF); cause distinguished (gst/stride vs CMA); else documented bound + proceed at 1280 |
| Sub-gate 2a | fragment-shader ycbcr → offscreen RGBA matches 1b flat-chroma CSC (graphics path, no WSI) |
| Sub-gate 2b | on-screen output visually correct + moving, under sway; surface formats/present modes pre-checked |
| No-copy | frame loop has zero readback/host copy (audited) **and** independent bandwidth/perf signal shows no internal staging |
| Throughput | sustained real-time; FPS measured off-FIFO; CPU lower than the readback baseline (numbers recorded) |
| Stability | no progressive corruption AND no decoder starvation over ≥1000 frames (lifetime + pool-depth) |
| Isolation | `pacman -Q mesa` == `1:26.0.6-1` before/after; deploy stays in `~/mesa-zc/` |

## Plan-B / risks (self-adversary, ordered by likelihood)

1. **Fragment-shader ycbcr + render-pass + swapchain is unvalidated on PanVK** (Stage 1 was compute) — the single largest new surface. Mitigation: sub-gate 2a isolates it before WSI.
2. **Buffer-recycle corruption** — ref-hold GstSample + fence-gated release.
3. **Pool-depth starvation/deadlock** — bound in-flight < pool − min-free.
4. **1080p not dmabuf-able** — CMA (64 MB) exhaustion or stride-pad; distinguish in Stage 0; fall back to ≤1280 with the bound documented.
5. **Cross-IOMMU coherency on sampling** — fence + FOREIGN_EXT acquire by default; the TRANSFER-path success of 1a does not guarantee the texture-read path.
6. **Edge chroma reconstruction on motion** — Mali fixed-function (un-CPU-matchable, STAGE1b); validate visually, never gate on CPU PSNR.
7. **Present needs a compositor** — sway up (baseline already uses it); headless fallback = render to exportable dmabuf and inspect.

## Out of scope (Step 2)
The browser (Step 3); the Vulkan-Video-ICD two-device decode + cross-device semaphore (Step 2's single-device sync conclusions do NOT transfer to it); the "no WebKit patch" decision; Main10/10-bit; codecs beyond H.264/HEVC 8-bit 4:2:0.

## Open questions
- **OQ-S2-1:** is the 1080p dmabuf fallback stride/gst emission or CMA exhaustion? (Stage 0 distinguishes; is rkvdec CAPTURE CMA-backed?)
- **OQ-S2-2:** per-frame VkImage import cost vs a fd→VkImage pool keyed by V4L2 buffer index — needed for real-time?
- **OQ-S2-3:** is the dmabuf implicit fence required, or is gst-DQBUF readiness + acquire barrier enough for tear-free, coherent sampling? (Default fence-first; prove the cheaper path.)
- **OQ-S2-4 (informs Step 3, residual NOT de-risked here):** Step 2 proves single-device V4L2→PanVK present; the browser currently decodes via the Vulkan-Video-ICD (two devices). **Step 2 leaves the cross-device fd-export + semaphore sync unproven — Step 3's first task must be exactly that.** Decide then: keep ICD decode (two-device sync) or switch the browser to V4L2 decode (single-device, as proven here).

## References
- `artifacts/phase-c/STAGE1a-import-bytexact.md`, `STAGE1b-hwyuv-sample.md` (import recipe, PanVK quirks, CSC validation, dmabuf-domain bound).
- `scripts/vvtest/zc_import_test.c` (import + ycbcr-sample building blocks — note: **compute**, to be re-expressed as a fragment/render-pass path for present).
- Phase-C spike spec `2026-06-25-vulkanvideo-rk3588-phaseC-zerocopy-spike.md` (3-step decomposition; Option A V4L2-in-PanVK rejected).
- Baseline present path: MEMORY Phase-B0 "Display end-to-end validated on SBC (19/06)" (sway, waylandsink, CPU-copy).
- Independent review (2026-06-25): hardware-verified PanVK WSI present; `v4l2slh264dec` has no io-mode; CMA=64 MB; rkvdec/Mali distinct IOMMU domains; Stage-1 compute-only.
