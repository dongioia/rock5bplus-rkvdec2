# Step-2 Increment-2 (sub-gate 2b) — on-screen ZERO-COPY present → PASS

**Date:** 2026-06-25. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec; PanVK `~/mesa-zc/` (!42353), system mesa `1:26.0.6-1` intact; compositor **sway** on output **HDMI-A-1** (has a 120 Hz mode).
**Code:** `scripts/vvtest/zc_swapchain_test.c` + `zc_sc.vert` (+ reuses `zc_present.frag`) + `zc-swapchain-run.sh`. SDL2 for the Wayland window. Two independent reviews folded.

## Result: on-screen zero-copy present works
Pipeline (no `vulkandownload`, no readback anywhere in the loop):
```
rkvdec (meta-aware dmabuf) -> PanVK import -> fragment-shader VkSamplerYcbcrConversion
   -> Wayland SWAPCHAIN image -> vkQueuePresentKHR
```
- swapchain 1280×720, `B8G8R8A8_UNORM`, 5 images, **FIFO**, TRANSFER_SRC supported.
- **30 frames presented, per-frame readback: NONE**, ~117 fps (= FIFO vsync at the monitor's 120 Hz mode), no VUID errors.
- **Window confirmed `visible=True` on HDMI-A-1** (`swaymsg -t get_tree`; `app_id=zc_swapchain_test`) — the present is genuinely on the physical monitor (not just `vkQueuePresentKHR` returning SUCCESS). User confirms the picture visually.
- mesa pin unchanged before/after.

## Design (carry-forward from the Increment-1 review, applied)
- **Synchronous per frame:** import → render → present → `vkQueueWaitIdle` → destroy import (image+mem+view) + close fd + unref GstSample. One frame in flight → **no buffer-recycle race, no per-frame object leak** (the reviewer's streaming hazards avoided by construction). Frame-invariant objects (render pass, pipeline, sampler, conversion, DSL) hoisted out of the loop.
- **Visible-extent present:** `uvscale` push constant `{vis_w/coded_w, vis_h/coded_h}` crops rkvdec coded padding (e.g. 1080 of 1088) so padding rows are not scanned out (review carry-forward #1).
- fd ownership: `dup` per frame handed to Vulkan import (Vulkan owns it); GstSample retains the original, unref'd only after `vkQueueWaitIdle`.

## Correctness basis (honest scope)
- **CONTENT** correctness is inherited from **sub-gate 2a** (`STEP2-inc1-*.md`): the import + fragment-ycbcr render path is byte-identical, proven byte-exact (flat-chroma 80 dB @720p / 76 dB @1080p, independently reviewed). 2b reuses that exact path into a swapchain image.
- **2b proves the on-screen PRESENT plumbing**: window mapped+visible, frames presented, no readback. It does NOT itself re-read the swapchain pixels (a one-time swapchain readback gate is a nice-to-have — the swapchain has TRANSFER_SRC for it; skipped because 2a already proves the identical render and adds a BGRA-format/scale wrinkle).
- Independent reviews: the Increment-1 review found+fixed the CSF fragment→copy barrier (reused-correct here); the 2b review verified the swapchain/sync/crop against the Khronos sync examples, **falsified** a suspected OOD-acquire semaphore leak (spec: acquire leaves the semaphore unaffected on OOD), and caught a false header comment (claimed a readback gate that didn't run) + a fatal SUBOPTIMAL-acquire — both fixed.

## Carry-forward (streaming hardening — NOT blockers for 2b)
1. Full swapchain **recreate** on OOD/SUBOPTIMAL (today: skip/accept; the synchronous `vkQueueWaitIdle` keeps the binary-semaphore 1:1 pairing valid on the happy path, but a real compositor reconfigure needs recreate).
2. **Pipelining** (currently synchronous, 1 frame in flight; per-frame fences + a sync ring would overlap decode/render/present for higher throughput).
3. **A/B measurement vs the `vulkandownload` CPU-copy baseline** (the spec's quantitative no-copy-vs-copy: CPU%, off-FIFO throughput) — not yet done; the present FPS is FIFO-capped.
4. Optional self-proving one-time swapchain readback gate.

## Bearing
Phase-C thesis demonstrated end-to-end **outside the browser**: on-screen zero-copy hardware video present on RK3588 via PanVK, no CPU readback. Step 3 = in-browser (WebKit), per the Phase-C spec.
