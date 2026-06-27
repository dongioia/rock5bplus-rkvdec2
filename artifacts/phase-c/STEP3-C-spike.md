# Step-3 C SPIKE — orientation (primary source) → ENCOURAGING, experiment pending

**Date:** 2026-06-27. ICD source: `mesa-sree-tree:/work/mesa-sree/mesa/src/vulkan-v4l2/`
(access: `docker run --rm -v mesa-sree-tree:/work/mesa-sree <dev-image>`; rebuild via
`scripts/vvtest/icd-rebuild.sh`). System mesa pin untouched.

Goal of C: make the Vulkan ICD's `vulkanh264dec` output the rkvdec CAPTURE dmabuf with NO memcpy,
so the bridge can drop `vulkandownload` and WebKit imports the decoder's dmabuf directly (the
GstVideoMeta meta-bridge groundwork from Increment A carries over).

## The three spike questions — primary-source status

### Q2 — CMA write-combine (the documented blocker): LIKELY DOES NOT APPLY to zero-copy
`v4l2vk_vk_device_memory.c` lines 160-183: decode-output memory (type-1) sets `dma_buf_fd = -1`
and `goto host_alloc` → `posix_memalign` + (at decode time) memcpy from the V4L2 MMAP CAPTURE
buffer. Author comment (verbatim, lines ~168-176):
> The EXPBUF'd CAPTURE buffer fds are available via the V4L2 context for future DMA-BUF export
> (GetMemoryFdKHR) but are [not used]. ... CMA-backed DMA-BUF mmaps on ARM have write-combine cache
> attributes that cause stale reads after memcpy.

**Key insight:** the documented hazard is a **CPU-read** problem (stale reads after a CPU memcpy
out of a write-combine mapping). The zero-copy path **never CPU-reads** the buffer — the GPU
consumer (WebKit EGLImage / PanVK sampler) imports the dmabuf and the GPU reads it. So the
author's reason for the memcpy fallback does not obviously apply to a GPU-consumer zero-copy
export. **Spike must confirm** the EGLImage import of the EXPBUF'd CAPTURE dmabuf reads correct
pixels (byte-exact vs ffmpeg).

### Q3 — GetMemoryFdKHR / export reach: INFRASTRUCTURE EXISTS
`v4l2vk_GetMemoryFdKHR` implemented (line 318): `*pFd = fcntl(mem->dma_buf_fd, F_DUPFD_CLOEXEC, 0)`,
guarded by `dma_buf_fd >= 0` (line 327). Import path (`VkImportMemoryFdInfoKHR`, line 130) and an
EXPBUF/`V4L2_MEMORY_DMABUF` flow also present. So the export API is real; the change is to back the
type-1 decode-output `VkDeviceMemory` with the EXPBUF'd CAPTURE dmabuf fd (instead of -1) so
`GetMemoryFdKHR` returns it. Still UNVERIFIED: whether `vulkanh264dec` (GStreamer vulkan plugin)
emits `GstVulkanImageMemory` that a downstream bridge element can reach to call the export — this is
the GStreamer-side half (Increment-A reviewer flagged it; needs the probe-element spike
`s3c-spike-vkmem-probe.c`).

### Q4 — DPB / output-coincide: STILL THE HARD ONE (unread this pass)
`v4l2vk_vk_device.c` `cap_idx`/`setup_slot_index` not yet read this session (the run captured the
memory file). Independent review (spec) reported `cap_idx = setup_slot_index` and a timestamp-based
reference-matching comment → the CAPTURE buffer IS the DPB slot; exporting/holding it for the
browser may collide with reference reuse. MUST read `v4l2vk_vk_device.c` and determine
DPB_AND_OUTPUT_COINCIDE vs distinct-output, and whether a distinct output buffer stays zero-copy.

## Spike verdict so far: GO-LEANING, experiment required
Two of three unknowns look favorable from primary source (export infra exists; the CMA blocker is
CPU-side and likely irrelevant to GPU zero-copy). The remaining work is the actual experiment:
1. Read `v4l2vk_vk_device.c` DPB/cap_idx model (Q4).
2. Behind an env flag, back type-1 output `VkDeviceMemory` with the EXPBUF'd CAPTURE dmabuf;
   `GetMemoryFdKHR` returns it. Rebuild ICD (isolated).
3. `s3c-spike-vkmem-probe.c`: confirm `vulkanh264dec` emits reachable `GstVulkanImageMemory` +
   export succeeds.
4. Test: EGLImage import of the exported fd reads byte-exact pixels (Q2 CPU-vs-GPU resolution);
   multi-frame GOP byte-check for DPB corruption (Q4).
Only on a clean experiment → write the C implementation plan.

## Not done this session
The experiment (steps 1-4) is a focused next effort — the hardest part of the project (the author
deliberately avoided this path) and deserves a dedicated run, not a marathon-session tail.
