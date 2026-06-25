# Phase C — Zero-Copy Step 2: Standalone no-copy on-screen pipeline (design)

**Date:** 2026-06-25
**Branch:** `spec/vulkanvideo-phaseC-zerocopy`
**Status:** design (pre-plan). Builds on STAGE 1 PASS (`artifacts/phase-c/STAGE1{a,b}-*.md`).
**Prereq met:** Step 1 proved rkvdec NV12 DMA-BUF imports into PanVK byte-exact (1a) and the Mali HW ycbcr sampler converts it to correct RGB with no readback (1b).

## Goal

Decode on rkvdec → DMA-BUF → PanVK HW-YUV sample → **on screen, with no GPU→system-memory readback** — and show it beats the current CPU-copy path (`vulkanh26Xdec ! vulkandownload ! waylandsink`, which memcpys every frame). Standalone (no browser; that is Step 3). This turns the Step-1 single-frame proof into a **streaming, presented** pipeline and resolves the sync + presentation unknowns before any browser work.

## Context (what already works)

- Step 1: single-shot import + sample of one rkvdec frame, pixel-correct (flat-chroma CSC 82 dB; chroma-edge = Mali fixed-function upsampling, validated visually here for the first time on a moving picture).
- The **CPU-copy** present path is already proven end-to-end on the SBC (19/06): `v4l2`/`vulkanh264dec → vulkandownload → waylandsink`, compositor **sway** (`WAYLAND_DISPLAY=wayland-1`, `XDG_RUNTIME_DIR=/run/user/1000`, HDMI-A-1). That is the baseline to beat.
- PanVK (`~/mesa-zc/`, !42353, git-`e5ec9502`) is built with `-Dplatforms=wayland` → it has Wayland WSI (swapchain). System mesa `1:26.0.6-1` stays pinned/untouched.

## Decode source — decision (single-device first)

Two candidate dmabuf sources; Step 2 uses the simpler one, notes the other for Step 3:

- **(chosen) V4L2 (GStreamer `v4l2slh264dec`/`h265dec`) → appsink dmabuf → PanVK.** This is exactly the Step-1 source. Decode is the **kernel** (not Vulkan), so there is only **one Vulkan device (PanVK)** — no Vulkan↔Vulkan cross-device semaphore. The CAPTURE buffer is fully decoded when gstreamer DQBUFs it and emits the appsink sample, so the import is of a *ready* buffer (sync handled below).
- **(deferred to Step 3) Vulkan Video decode ICD (`vulkanh264dec`) → exported `VkImage` dmabuf → PanVK.** This is the genuine two-`VkPhysicalDevice` model (decode ICD + PanVK) and needs cross-device `VK_KHR_external_semaphore_fd` sync. The browser path (WebKitGTK + GStreamer) currently uses this decode; Step 3 decides whether to keep it or switch the browser to the V4L2 decoder. Step 2 deliberately does **not** take on the two-device sync — it isolates the *present* mechanism.

Rationale: the novel, unproven part of Step 2 is **PanVK-sample → present with no readback**, not the decode. Prove that on the simplest reliable source.

## Stage 0 — Kill-switch: can 1080p be zero-copied at all? (do this FIRST)

Step 1 found rkvdec only **dmabuf-exports aligned widths** via gstreamer auto-negotiation: 1280-wide worked at multiple heights; **1920×1080 and 1366×768 fell back to system memory** (no `GstVideoMeta`, packed). 1080p is the primary browser resolution, so if it cannot be zero-copied the whole thesis is capped at ≤1280-width.

**Hypothesis to falsify:** the 1080p fallback is a **gstreamer negotiation choice, not a rkvdec/CMA limit.** The Step-1 probe used default (auto) io-mode; gstreamer may have picked MMAP for the larger buffer. The V4L2 API can be driven directly: `REQBUFS(CAPTURE, memory=MMAP)` then `VIDIOC_EXPBUF` each CAPTURE buffer to an fd (the decode-ICD already does exactly this in `mesa-sree-tree`).

Stage-0 experiment (cheap, no PanVK): for 1080p,
1. force gstreamer CAPTURE `io-mode=dmabuf` (`v4l2slh264dec capture-io-mode=dmabuf`) and re-check `is_dmabuf_memory`; **and/or**
2. drive V4L2 by hand (or via the ICD's EXPBUF path) and confirm `VIDIOC_EXPBUF` returns valid fds for 1080p CAPTURE buffers, with plane offsets/strides.

**Gate:** 1080p CAPTURE surfaces a dmabuf fd (either path) → zero-copy is resolution-general, proceed. If **no** path exports 1080p dmabuf → document the real hardware/CMA bound; zero-copy is limited to the widths that do, and Step 3's browser value is correspondingly scoped. Either outcome is a finding, not a failure.

## Architecture (chosen path)

```
gst: filesrc ! parse ! v4l2sl{h264,h265}dec (CAPTURE dmabuf) ! appsink
        |  per frame: GstSample -> dmabuf fd + GstVideoMeta (offsets/strides)
        v
PanVK (single VkDevice): import fd -> VkImage (LINEAR NV12, explicit layout,
        drmFormatModifierPlaneCount=2 workaround) -> ycbcr-sample into the
        swapchain image (graphics or blit) -> vkQueuePresentKHR (Wayland)
```

No `vulkandownload`, no host buffer in the per-frame loop.

## Step-2 design

### Presentation
- PanVK Wayland **swapchain** (`VK_KHR_swapchain` + `VK_KHR_wayland_surface`), surface format `B8G8R8A8`/`R8G8B8A8_UNORM`, present mode FIFO (vsync, tear-free). Run under **sway** like the baseline.
- Per frame: import the rkvdec dmabuf as the NV12 sampled image (Step-1 recipe), then a **graphics pass** (fullscreen triangle, the Step-1 ycbcr combined sampler) renders into the acquired swapchain image, then present. (A `vkCmdBlitImage` from a sampled ycbcr image is not allowed — ycbcr needs a sampler — so it is a draw, not a blit.)
- Imported NV12 image is **per-frame** (new fd each appsink sample). Recreate or recycle `VkImage`/`VkDeviceMemory` per frame; measure whether per-frame image creation is a bottleneck (if so, pool by fd/buffer-index — V4L2 reuses a small CAPTURE buffer set, so a fd→VkImage cache is the optimization).

### Sync (single-device, but not trivial)
- **Decode→sample:** the appsink buffer is already DQBUF'd (decode complete) before we import — so a CPU-side guarantee exists. For correctness across the import boundary, acquire the image with the same `UNDEFINED→SHADER_READ_ONLY` barrier proven in 1b (LINEAR preserves). If tearing/staleness appears, import the dmabuf's implicit fence as a wait (`VK_KHR_external_semaphore_fd` / sync_file) — but expected unnecessary for the gst-DQBUF'd path.
- **Keep the dmabuf alive while in flight:** the GstSample must be ref-held until the frame's GPU work completes (fence), or V4L2 may recycle the CAPTURE buffer under the GPU. This is the main streaming-correctness hazard (Step 1 was single-shot and sidestepped it). Hold a ref per in-flight frame, release on fence signal.

### Measurement (the point of Step 2)
Compare the no-copy path vs the CPU-copy baseline on the same clip:
- **Proof of no-copy:** the frame loop contains zero `vkCmdCopyImageToBuffer`/host memcpy of pixel data (code-audited) — contrast `vulkandownload`.
- **Throughput:** sustained FPS at 720p and (if Stage 0 passes) 1080p; CPU utilization (the copy path burns CPU on the per-frame memcpy). Expect lower CPU and/or higher FPS.
- **Correctness on a moving picture:** capture a presented frame (or render to an exportable image) and confirm it matches the Step-1b CSC result (flat-chroma); confirm no progressive corruption over N frames (the buffer-recycle hazard).

## Verification gates

| Gate | Pass criterion |
|---|---|
| Stage 0 | 1080p rkvdec CAPTURE surfaces a dmabuf fd (forced io-mode or EXPBUF); else documented bound |
| Present correctness | on-screen output visually correct, moving; a captured frame matches 1b flat-chroma CSC |
| No-copy | frame loop has zero pixel readback/host copy (audited) vs `vulkandownload` baseline |
| Throughput | sustained real-time FPS; CPU lower than the copy baseline (numbers recorded) |
| Stability | no progressive corruption over ≥1000 frames (dmabuf-lifetime / recycle correctness) |
| Isolation | `pacman -Q mesa` == `1:26.0.6-1` before/after; deploy stays in `~/mesa-zc/` |

## Plan-B / risks (self-adversary)

- **1080p not dmabuf-able (Stage 0 fail):** zero-copy capped to supported widths; still a valid result for those, and Step 3 scopes accordingly. Investigate CMA size / `io-mode` before concluding it is hardware.
- **Per-frame VkImage import too slow:** pool fd→VkImage by V4L2 buffer index (small fixed set).
- **Buffer recycle races (corruption after N frames):** the dmabuf-lifetime hazard; fix by ref-holding the GstSample per in-flight frame + fence-gated release. This is the most likely real bug.
- **Edge chroma reconstruction on motion:** Step 1 showed edges are Mali fixed-function (un-CPU-matchable); on a moving picture this is just normal HW upsampling — validate visually, do not gate on CPU PSNR.
- **Present path needs a compositor:** sway must be up (baseline already uses it). Headless fallback = render to an exportable dmabuf and inspect, but on-screen is the goal.

## Out of scope (Step 2)
The browser (Step 3); the Vulkan-Video-ICD two-device decode + cross-device semaphore; the "no WebKit patch" decision; Main10/10-bit; codecs beyond H.264/HEVC 8-bit 4:2:0.

## Open questions
- **OQ-S2-1:** is the 1080p dmabuf fallback gstreamer or hardware? (Stage 0 answers.)
- **OQ-S2-2:** per-frame import cost vs a fd→VkImage pool — needed for real-time?
- **OQ-S2-3:** does the gst-DQBUF'd buffer need an explicit fence import, or is CPU-side readiness enough for tear-free present?
- **OQ-S2-4 (informs Step 3):** for the browser, keep the Vulkan-Video-ICD decode (two-device sync) or switch to V4L2 decode (single-device, as proven here)?

## References
- `artifacts/phase-c/STAGE1a-import-bytexact.md`, `STAGE1b-hwyuv-sample.md` (Step 1 results, import recipe, PanVK quirks).
- `scripts/vvtest/zc_import_test.c` (import + ycbcr-sample building blocks to lift into the present loop).
- Phase-C spike spec `2026-06-25-vulkanvideo-rk3588-phaseC-zerocopy-spike.md` (3-step decomposition; Option A V4L2-in-PanVK rejected).
- Baseline present path: MEMORY Phase-B0 "Display end-to-end validated on SBC (19/06)" (sway, waylandsink, CPU-copy).
