# Stage-2 Gate 1b + 1c Results

**Date**: 2026-06-21  
**SBC**: rock5b (RK3588 Rock 5B+, kernel 7.1.0-rc1+)  
**ICD**: `~/vvtest/v4l2vk_icd.aarch64.json` + `libvulkan_v4l2_video.so` (B0 fix, commit e5a0d50)  
**Branch**: `spec/vulkanvideo-phaseB0`

---

## Gate 1b — Vulkan Feed in WebKit (Epiphany/GStreamer)

**VERDICT: PARTIAL (harness-gap blocker, not ICD blocker)**

### What was run

Harness: `scripts/vvtest/s2-webkit-decode-test.sh vulkan`  
Evidence: `benchmark/stage2-20260621/webkit-vulkan.out`

### marker_parse.classify() result (original harness)

```
decoder:        v4l2slh264dec   ← wrong decoder selected
hw:             True
negotiated:     False
videometa_fail: True
fuser_video0:   none
```

### Root cause

`vulkanh264dec` has GStreamer rank `none (0)`. `v4l2slh264dec` has rank `primary+1 (257)`.
WebKit's `decodebin` auto-selects by rank and picks `v4l2slh264dec` first.
`v4l2slh264dec` then fails on `VideoMeta` negotiation (same VideoMeta wall as v4l2-direct gate).
**`vulkanh264dec` is never tried.**

The harness clears the registry cache and sets `VK_ICD_FILENAMES` (both confirmed working), but
does **not** set `GST_PLUGIN_FEATURE_RANK` to elevate `vulkanh264dec` above `v4l2slh264dec`.

### Secondary finding: WebKit scanner excludes vulkanh264dec

Even with `GST_PLUGIN_FEATURE_RANK="vulkanh264dec:512,v4l2slh264dec:0"`, WebKit falls back to
`avdec_h264` (SW). This is because:
- `vulkanh264dec` SRC caps: `video/x-raw(memory:VulkanImage)` — Vulkan memory type
- WebKit's GStreamer registry scanner does not enumerate decoders that output
  `memory:VulkanImage` (it needs DMABuf or system memory for its GL pipeline)
- A `vulkandownload` element would be needed between decoder and WebKit sink, but WebKit
  builds its own GStreamer pipeline internally and does not insert `vulkandownload`

### S2.2 kill-gate context

S2.2 probe (same day, `benchmark/stage2-20260621/vulkan-feed-probe.log`) confirmed:
- Variant B: `vulkanh264dec ! vulkandownload ! videoconvert ! glimagesink` → PLAYING + EOS ✓
- Variant C: `vulkanh264dec ! vulkandownload ! fakesink` → PLAYING + EOS ✓
- Variant A: `vulkanh264dec ! vulkansink` → cross-device link error (expected)

The ICD works. The browser pipeline cannot automatically insert `vulkandownload`.

### Harness bug

**The harness is missing `GST_PLUGIN_FEATURE_RANK` to force decoder selection** and is missing
knowledge that WebKit's pipeline cannot handle `memory:VulkanImage` without `vulkandownload`.
Gate 1b as specified (browser + auto decoder selection) requires either:
1. A WebKit patch to add Vulkan memory support and `vulkandownload` insertion, OR
2. A custom Epiphany/WPE build with `use-h264-video-decoder` override, OR
3. A side-channel approach that pre-builds the pipeline (not via decodebin)

**This is an architectural limitation of this Stage-2 approach, not an ICD bug.**

---

## Gate 1c — Authoritative Frame-0 Byte-Exact (standalone, decoupled from browser)

**VERDICT: PASS — byte-exact**

### Pipeline used

```bash
export VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json
# Clear registry
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin

# Step 1: ffmpeg reference
ffmpeg -y -loglevel error -i ~/vvtest/case1.mp4 \
  -pix_fmt nv12 -f rawvideo ~/vvtest/ref_case1.nv12

# Step 2: Vulkan ICD decode via gst-launch
gst-launch-1.0 filesrc location=~/vvtest/case1.h264 \
  ! h264parse ! vulkanh264dec ! vulkandownload \
  ! 'video/x-raw,format=NV12,width=1280,height=720' \
  ! filesink location=~/vvtest/out_vulkan.nv12

# Step 3: Frame-0 extract (1280x720 NV12 = 1382400 bytes)
head -c 1382400 ~/vvtest/ref_case1.nv12 > ~/vvtest/ref_f0.nv12
head -c 1382400 ~/vvtest/out_vulkan.nv12 > ~/vvtest/out_f0.nv12
```

### File sizes (geometry check)

```
ref_case1.nv12:  41472000 bytes (30 frames × 1382400) ✓
out_vulkan.nv12: 41472000 bytes (30 frames × 1382400) ✓
ref_f0.nv12:      1382400 bytes ✓
out_f0.nv12:      1382400 bytes ✓
```

No geometry mismatch (coded stride == visible width 1280; case1 is non-cropped).

### nv12_tool compare output

```
len_a: 1382400
len_b: 1382400
byte_exact: True
RESULT: PASS (byte-exact)
```

Evidence: `benchmark/stage2-20260621/gate1c-nv12compare.out`

---

## Overall

```
STAGE2_GATE=PARTIAL
  Gate 1c: PASS (byte-exact frame-0; ICD decode is pixel-correct)
  Gate 1b: PARTIAL (vulkanh264dec not selected by WebKit decodebin;
             architectural boundary: memory:VulkanImage not handled
             by WebKit's internal GStreamer pipeline)
```

### Next steps for full gate 1b pass

1. Implement `vulkandownload` insertion in WebKit's GStreamer media pipeline, OR
2. Use a custom WebKit build with explicit `vulkanh264dec` allowlisting, OR
3. Reframe gate 1b as a display-path test via standalone `waylandsink` (B0 already validated
   display end-to-end via `waylandsink` on real HDMI output)

The ICD is functionally correct (gate 1c byte-exact, S2.2 PASS). The browser integration
gap is in WebKit's pipeline construction, not in the decoder.
