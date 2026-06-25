# Phase-C Stage-1 — resume (start here next session)

**Status:** **STAGE 1 PASS (complete).** 1a import byte-exact (`STAGE1a-import-bytexact.md`, 3 geometries) + 1b HW-YUV sample CSC-correct (`STAGE1b-hwyuv-sample.md`, flat-chroma PSNR 82/79 dB @ 720p/1088). Zero-copy via PanVK feasible AND pixel-correct. `scripts/vvtest/zc_import_test.c` (both gates, 3× independently reviewed) + `zc.comp` + `zc-run.sh`. **Next = Step 2** (standalone no-copy on-screen pipeline, own spec). Carry-overs: (a) PanVK import bug `drmFormatModifierPlaneCount=2` workaround (upstream-report); (b) DMA-BUF domain bound — rkvdec dmabuf only for aligned widths (1280 ok; 1920/1366 → system mem) → Step-2 must confirm 1080p (primary browser res) reaches dmabuf; (c) edge chroma reconstruction is Mali HW fixed-function (validated visually in Step 2, not CPU-PSNR).
**Status (orig):** Stage 0 PASS (kill-switch). Zero-copy via PanVK on RK3588 = feasible.
**Plan:** `docs/superpowers/specs/2026-06-25-vulkanvideo-rk3588-phaseC-zerocopy-spike.md` §Stage-1 (twice indep-reviewed). This note = the actionable delta.

## What's already proven / ready
- rkvdec exports **LINEAR NV12**, single alloc, layout **Y{offset 0, pitch 1280}, UV{offset 921600, pitch 1280}** (1280x720). `zc_modifier_probe.py`.
- **!42353 PanVK deployed + working on the SBC**: `VK_ICD_FILENAMES=$HOME/mesa-zc/panfrost_icd.json` → Mali-G610, Mesa 26.2.0-devel (git-e5ec9502). Advertises NV12 modifier `0x0` LINEAR, planes=1, sampled=1, **ycbcrLinFilter=1**, midpoint+cosited; import tuple SUPPORTED. `zc_modifier_query.c`.
- Build recipe banked (container, ~1min): see MEMORY Phase-C line + `STAGE0b-*.md`. To rebuild PanVK: `dev-server` container `/work/mesa-pin` (pinned e5ec9502), `ninja -C bdir3 install` → `/work/mesa-zc-out`, deploy → SBC.

## Stage-1 = new artifact `scripts/vvtest/zc_import_test.c` (C/Vulkan)
Build on SBC: `cc zc_import_test.c -o zc_import_test $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0) -lvulkan`.

### KEY de-risk (from Stage-0a): fd plumbing is EASY
The **default** `v4l2slh265dec ! appsink` buffer is **already dmabuf-backed** — measured `is_dmabuf_memory=True, fd=10, n_memory=1` on the PLAIN path (NO `video/x-raw(memory:DMABuf)` caps filter needed; that filter STALLED). So: single program links gstreamer + vulkan, decode→`appsink` pull first buffer→`gst_dmabuf_memory_get_fd(mem0)` + `gst_buffer_get_video_meta` (stride/offset)→Vulkan-import **same process** (fd valid). No SCM_RIGHTS, no synthetic gbm buffer.

### Import recipe (review-hardened — all in the spec, condensed)
- `VkImportMemoryFdInfoKHR`(fd, EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF) → `VkDeviceMemory`. **Single `vkBindImageMemory`** (non-disjoint — planes=1 confirmed).
- Image: `tiling=DRM_FORMAT_MODIFIER_EXT`, chain `VkImageDrmFormatModifierExplicitCreateInfoEXT`(modifier=0, pPlaneLayouts Y{0,1280,...}/UV{921600,1280,...}) + `VkExternalMemoryImageCreateInfo`(DMA_BUF) + **`VkImageFormatListCreateInfo`**(NV12,R8_UNORM,R8G8_UNORM) [mandatory w/ MUTABLE]; flags `MUTABLE_FORMAT | EXTENDED_USAGE`.
- **strict_import**: a `VK_ERROR_INITIALIZATION_FAILED` at image-create = pitch mismatch (NOT modifier-reject); log pPlaneLayouts, retry with PanVK-reported pitch. (LINEAR pitch=1280=align16(1280) likely fine.)
- 1a (geometry, copy path): per-plane R8/R8G8 views → `vkCmdCopyImageToBuffer` → byte-exact vs **independent** canonical-linear NV12 (`nv12_tool.py`/ffmpeg, NOT re-derived from the same GstVideoMeta).
- 1b (the path that matters): `VkSamplerYcbcrConversion`(model BT.709 for 720p / BT.601 per VUI, range limited, siting matched — PanVK supports midpoint+cosited) combined-sampler view → sample to RGB → PSNR vs CPU NV12→RGB at identical params.

### Tips
- Enable `VK_LAYER_KHRONOS_validation` if present (debug import/view VUs).
- ffmpeg ref for the SAME frame: `ffmpeg -i hevc_case1.mp4 -frames:v 1 -pix_fmt nv12 ...` (already used; `ref_hevc_f0.nv12` may still be on SBC).
- PanVK device select: only our ICD loaded via VK_ICD_FILENAMES → pds[0] is it.

## After Stage 1 → Step 2 (standalone no-copy display pipeline), Step 3 (in-browser, WebKit-patch decision). Each its own spec.
