# Step-3 — in-browser zero-copy (WebKit), A+C — design

**Date:** 2026-06-27. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec; PanVK `~/mesa-zc/` (!42353), system mesa pin `1:26.0.6-1` intact; compositor sway, output HDMI-A-1; browser Epiphany/WebKitGTK-6.0 2.52.4.

## Goal

Hardware-decoded video playing inside WebKitGTK with the decoded frame reaching WebKit's
compositor as a **dmabuf** — no `vulkandownload`, no CPU readback anywhere in the loop. Two
increments:

- **A** — prove the in-browser zero-copy *plumbing* via the GStreamer `v4l2codecs` dmabuf path (no Vulkan in the loop). De-risks WebKit's dmabuf import (DRM modifier, padded NV12, decoder selection) independently.
- **C** — make the **Vulkan Video ICD** itself zero-copy (output VkImage backed by the V4L2 CAPTURE dmabuf, no memcpy) and demonstrate *that* in the same browser. The project deliverable (Mesa ICD, Chromium-convergent) running end-to-end in a real browser consumer.

## Why A+C and not A-only or C-only

WebKitGTK delegates decode to GStreamer `decodebin`, so it can use `v4l2slh264dec` (rank
primary+1, 257) directly — meaning WebKitGTK can plausibly HW-decode H264 *already*, unlike
Chromium whose internal V4L2 path is a walled downstream fork (vault
`strategic-rationale-v4l2-walled-vulkan-bet.md`, HIGH). That is exactly what A measures. It is
**not** wasted Vulkan effort: the ICD's value is for the **Chromium / upstream-convergent**
target where V4L2-direct is a dead end; WebKitGTK is the *hackable test consumer* where we can
demonstrate the ICD. A isolates WebKit's dmabuf variables so that any failure in C is known to
be in our ICD, not WebKit.

The standalone ICD **cannot** zero-copy today: its memory model is posix_memalign + memcpy — it
copies the rkvdec CAPTURE dmabuf into a CPU buffer internally. **Verified HIGH (primary source,
independent review 2026-06-27):** `mesa-sree-tree:/mesa/src/vulkan-v4l2/v4l2vk_vk_device_memory.c`
`v4l2vk_AllocateMemory` — decode-output VkImage memory is `type_index==1` (DEVICE_LOCAL) and
`goto host_alloc` → `posix_memalign(&ptr, 4096, alloc_sz)`; the decode loop (`debug-instrumentation.patch`)
does `memcpy(dst_mem->map + ..., capture_bufs[dq_cap].mmap_addr, copy_size)` with the author's
comment *"Always memcpy even when VkDeviceMemory is EXPBUF'd."* C removes that copy.

## Verified facts (this session, on the board)

- `v4l2slh264dec` / `v4l2slh265dec` advertise `video/x-raw(memory:DMABuf)` NV12 src caps. Rank = primary+1 (257).
- Stage-2 Vulkan bridge `vkh264bridge` registers at rank PRIMARY+2 (258), deliberately shadowing 257 so WebKit picks the Vulkan path. For A the bridge must NOT be registered (or be lowered) so 257 wins.
- WebKitGTK-6.0 2.52.4 has the DMABuf renderer compiled in (GBM + EGLImage + DMA-BUF texture path; strings confirm GBM device/buffer/EGLImage import).
- PanVK exposes `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_memory_fd` (relevant to C's export side / consumer import).

---

## Increment A — WebKitGTK + v4l2codecs dmabuf

### Pipeline
```
rkvdec HW → v4l2slh264dec → video/x-raw(memory:DMABuf) NV12
         → WebKit decodebin → WebKit DMABuf renderer (GBM/EGLImage) → GL compositor CSC → screen
```
No bridge, no `vulkandownload`, no Vulkan.

### Method
- Launch WebKitGTK (Epiphany or the minimal MiniBrowser/test harness already used in Stage-2: `s2-webkit-decode-test.sh` / `s3-realh264-test.sh`) on sway, env WITHOUT `vkh264bridge` registered (default `GST_PLUGIN_PATH`), `VK_ICD_FILENAMES` unset/system (A is non-Vulkan). Clear `~/.cache/gstreamer-1.0/registry.aarch64.bin`.
- **No forced ranks.** The "already free" verdict hinges on WebKit's `decodebin` picking `v4l2slh264dec` (257) over `avdec_h264` by *default* rank. The existing Stage-2/3 scripts force selection with `GST_PLUGIN_FEATURE_RANK` (review finding) — A must run with NO forced ranks to test natural selection. A second run *with* `GST_PLUGIN_FEATURE_RANK=v4l2slh264dec:512` is the fallback only if natural selection fails (and then "already free" is downgraded to "free with a one-line env knob").
- Serve a local H264 `<video>` page (progressive) first; then MSE (`s3-mse-test.sh` shape).
- Clip corpus: existing `~/vvtest/case1.h264`, `demo.h264`, `c1080.h264` (re-muxed to mp4 as needed for `<video>`).

### Gates (all must hold)
1. **HW decode**: `fuser /dev/video0` busy during play; `GST_DEBUG=*decodebin*:5,v4l2*:5` shows `v4l2slh264dec` selected, NOT `avdec_h264`; no SW fallback.
2. **Zero-copy import**: WebKit imports a dmabuf, not a system-memory upload. Evidence: WebKit DMABuf-renderer GBM import active (env `WEBKIT_DEBUG` / log), and the decoder src memory is `memory:DMABuf` (GST log caps). Negative control: the path must NOT show `gldownload` / `vulkandownload` / a sysmem `videoconvert`. **Known WebKit copy trap (review):** when the GStreamer frame is NOT already a dmabuf, WebKit *allocates a GBM buffer and uploads the frame into it* (WebKit bug 260654) — a sysmem copy that silently defeats zero-copy while still "working" on screen. So gate 2 must positively confirm the decoder src is `memory:DMABuf` AND the modifier is GBM-importable, not merely that a picture appears.
3. **Visual**: frame on HDMI, screenshot via `grim`; no green/sheared corruption (the rkvdec padded NV12 modifier is accepted by WebKit's import).
4. **Padded geometry**: confirm coded-vs-visible handled (1088 coded vs 1080 visible @1080p) — WebKit crops to visible without artifact.

### Risks / unknowns A
- WebKit's internal `decodebin` may force its own caps/negotiation that demotes the dmabuf path to a sysmem copy (a `glupload`/`videoconvert` inserted). If so, A is "HW decode but not zero-copy"; record honestly and identify the inserted copy.
- rkvdec NV12 DRM modifier may be rejected by WebKit's GBM import (the `INTERLEAVED_64K`-rejected case from Mesa !42353, or a Rockchip-tiled modifier). If rejected → falls to linear or fails; record which.
- MSE path (fragmented mp4, seek, adaptive switch) may re-negotiate and break the dmabuf path even if progressive works.

### A deliverable
A short report (`artifacts/phase-c/STEP3-A-webkit-v4l2-dmabuf.md`) with the gate evidence and a verdict: is WebKitGTK in-browser zero-copy via v4l2codecs **already free** (yes/no), and if not, exactly what copy is inserted.

---

## Increment C — Vulkan ICD zero-copy export

### Target chain
```
vulkanh264dec (ICD: output VkImage backed by V4L2 CAPTURE dmabuf, no memcpy)
  → bridge: vkGetMemoryFdKHR(output VkImage VkDeviceMemory) → GstDmaBufMemory NV12
  → bridge src caps = video/x-raw(memory:DMABuf)        [vulkandownload removed]
  → WebKit DMABuf renderer import (same as A)
```

### Components
1. **ICD memory model** (`mesa-sree-tree` volume, sree/mesa ICD `libvulkan_v4l2_video.so`):
   the decode-output VkImage's `VkDeviceMemory` must wrap the V4L2 CAPTURE dmabuf rather than a
   posix_memalign CPU buffer. `vkGetMemoryFdKHR` returns that dmabuf fd. Sub-approach decision
   (spike-gated): (2) **V4L2 allocates, Vulkan imports** — ICD imports the existing CAPTURE
   dmabuf fd as the VkImage memory (simplest, V4L2 already owns allocation); vs (1) **Vulkan
   allocates, V4L2 imports** — ICD exports VkDeviceMemory as dmabuf and queues it into V4L2
   CAPTURE as `V4L2_MEMORY_DMABUF`. Default to (2) unless the spike shows the VkImage cannot
   legally wrap an externally-allocated dmabuf for video-decode output.
2. **Bridge — element-class rewrite, not a child swap** (review finding): the current bridge is a
   thin GstBin wrapping `vulkanh264dec ! vulkandownload` and holds **no Vulkan context** — it never
   touches `GstVulkanImageMemory`. To export, the `vulkandownload` child is replaced by a **custom
   GstElement that holds the upstream `GstVulkanDevice`/`VkDevice` context**, peeks the output
   buffer's memory (`gst_is_vulkan_image_memory` → `GstVulkanImageMemory` → its `VkDeviceMemory`),
   calls `vkGetMemoryFdKHR`, and wraps the fd as `GstDmaBufMemory` with the correct `GstVideoMeta`
   (stride/offset/coded-height) and DRM modifier; src caps become `video/x-raw(memory:DMABuf),
   format=NV12`. **Unverified, spike-gated:** whether `vulkanh264dec` even emits `GstVulkanImageMemory`
   downstream (vs an internal pool) — confirm in the spike.
3. **WebKit consumption**: identical to A. If A passed, C's consumer side is proven.

### Risks / unknowns C — three coupled obstacles, all primary-source-confirmed (independent review)
These are NOT footnotes; the review verdict is **SPIKE-REQUIRED-BEFORE-PLAN** for C. Resolve all
three in the spike before committing to the rewrite.

- **(i) CMA write-combine cache coherency — the author already documented why this is hard.**
  The ICD's `host_alloc` comment states the CAPTURE dmabuf is deliberately NOT bound to the
  VkDeviceMemory because *CMA write-combine cache attributes cause stale reads after memcpy*.
  Sub-approach (2) (Vulkan imports the V4L2 dmabuf) must prove the consumer (WebKit GL/EGLImage)
  reads the rkvdec output correctly under that cache attribute — or add the right cache
  maintenance. This is a concrete, pre-identified obstacle, not a hypothetical.
- **(ii) DPB / output-coincide reuse — load-bearing, confirmed.** `v4l2vk_vk_device.c`:
  `cap_idx = setup_slot_index`; the CAPTURE buffer **is** the DPB slot, and the kernel matches
  reference frames by timestamp. Holding/exporting the output buffer for the browser while the
  kernel may reuse that slot as a reference is a real corruption vector. Mitigation = a distinct
  output buffer (extra CAPTURE buffer or copy-to-distinct-dmabuf) — but that **collides with the
  `cap_idx≡slot_index` design and may partly reintroduce the very copy C removes.** The spike
  must measure whether a distinct-output path stays zero-copy or degenerates to a GPU blit.
- **(iii) GStreamer reach + Vulkan context.** A downstream element CAN cast
  `GstVulkanImageMemory` (real type), but `vkGetMemoryFdKHR` needs the `VkDevice` from the
  upstream `GstVulkanDevice` context, which the current thin-bin bridge does not hold. The export
  must live in a custom element holding that context. Also unverified: that `vulkanh264dec`
  actually emits `GstVulkanImageMemory` downstream rather than an internal pool.

Lower-severity C items:
- **Modifier agreement**: the exported fd's reported DRM modifier must match what rkvdec produced
  and what WebKit's GBM import accepts (rkvdec modifier, not a PanVK-native one).
- **ICD bind path**: `vkAllocateMemory`/`vkBindImageMemory` for the output image must route to the
  dmabuf-import path, not posix_memalign. The ICD already implements `v4l2vk_GetMemoryFdKHR`,
  `VkImportMemoryFdInfoKHR`, and a `V4L2_MEMORY_DMABUF` REQBUFS/QBUF path (review) — the surface
  exists; today output `dma_buf_fd == -1`, so the change is to actually back type-1 output memory
  with the CAPTURE dmabuf.

### C SPIKE (gating — must pass before any C implementation or writing-plans for C)
A minimal, throwaway experiment that answers, in order:
1. Does `vulkanh264dec` emit `GstVulkanImageMemory` to a downstream element, and can a custom
   element holding the `GstVulkanDevice` context `vkGetMemoryFdKHR` the output image's memory?
2. Can the ICD back the type-1 decode-output VkDeviceMemory with the V4L2 CAPTURE dmabuf
   (sub-approach 2), and does a consumer read it correctly despite the CMA write-combine attribute?
3. With a distinct-output buffer to dodge the DPB-reuse hazard, does the path stay genuinely
   zero-copy (no GPU blit / no memcpy), or does it degenerate?
Output: a spike report `artifacts/phase-c/STEP3-C-spike.md` with a GO / NO-GO / sub-approach
decision. Only on GO do we write the C implementation plan.

### C deliverable
ICD `.so` rebuilt (isolated, mesa pin untouched), bridge `.so` rebuilt, and an on-screen WebKit
play of an H264 clip with: `fuser /dev/video0` busy, `vulkanh264dec` in the GST graph (NOT
`v4l2slh264dec`, NOT `vulkandownload`), dmabuf reaching WebKit, byte-exact frame-0 (vs ffmpeg,
the established `pixelcheck.py` gate) to prove the export did not corrupt, screenshot. Report
`artifacts/phase-c/STEP3-C-icd-zerocopy.md`.

## Isolation / safety invariants (both increments)
- System mesa pin `1:26.0.6-1` unchanged pre/post (assert in every runner).
- ICD + bridge remain isolated (`~/mesa-zc/`, `GST_PLUGIN_PATH`, `VK_ICD_FILENAMES`); no system install.
- Independent-agent review on every code edit (ICD, bridge, harness) and on this spec, per project protocol — to catch bugs/hallucination before build/deploy.
- mem-search before each increment; systematic-debugging if a gate fails; vault `VulkanVideo/` log + gap-tracker updated after; humanizer on any public outreach.

## Sequencing
1. **A** (cheap, isolates WebKit's dmabuf import; default-rank selection + GBM-import positive check).
2. **C SPIKE** (the gating experiment above — resolves CMA cache coherency, DPB-reuse, and
   GstVulkanImageMemory/context reach). Independent verdict: SPIKE-REQUIRED-BEFORE-PLAN for C.
3. **C implementation** — only on a GO spike verdict, with the sub-approach (1 vs 2) and DPB model
   fixed by the spike. Writing-plans for C is authored *after* the spike, not before.

## Out of scope (YAGNI)
- HEVC in-browser zero-copy (deferred; HEVC ICD exists separately).
- VP9/AV1.
- Chromium integration (the ultimate target, but not this step — no Chromium Vulkan-video CLs exist yet).
- Swapchain recreate / pipelining beyond what WebKit's own compositor handles.
