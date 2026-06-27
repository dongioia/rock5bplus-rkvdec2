# Step-3 Increment A — diagnosis: WebKitGTK + v4l2codecs default path → ROOT CAUSE = GstVideoMeta ALLOCATION gap

**Date:** 2026-06-27. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec; WebKitGTK-6.0 **2.52.4** (Epiphany 50.4), GStreamer v4l2codecs; system mesa pin `1:26.0.6-1` intact pre/post. Default plugin ranks, NO Vulkan ICD, NO vkh264bridge.

## Question
Can WebKitGTK do **zero-copy HW video** "for free" via the GStreamer `v4l2codecs` dmabuf path (no Vulkan)? (User challenge: would that mean the Vulkan work was redundant?)

## Verdict: NO — the default path errors. But for a precise, fixable reason, NOT a WebKit limitation.

`<video src=h264.mp4>` in Epiphany with default ranks → media element fails:
overlay `rs=0 t=0.00 0x0 ERR4` (`MEDIA_ERR_SRC_NOT_SUPPORTED`); `/dev/video0` never busy; reproduced on demo.mp4 (720p) and c1080.mp4 (1080p).

## Root cause (primary evidence, systematic-debugging)

```
ERROR v4l2codecs-h264dec gstv4l2codech264dec.c:477:gst_v4l2_codec_h264_dec_decide_allocation:
      <v4l2slh264dec0> DMABuf caps negotiated without the mandatory support of VideoMeta
  videodecoder: <v4l2slh264dec0> didn't get downstream ALLOCATION hints
  WARN  videodecoder: Subclass failed to decide allocation
  ERROR v4l2codecs-h264dec: Failed to negotiate with downstream
  WARN  h264decoder: Failed to process SPS / Failed to handle codec data
  videodecoder: flow error not-negotiated
  queue2-0: streaming stopped, reason not-negotiated (-4) → GstMessageError on bus
  webkitcommon GStreamerCommon.cpp:1103: Got message: error → MediaError ERR4
```

`v4l2slh264dec` **mandates** that a DMABuf consumer advertise the **`GstVideoMeta` API** in its
ALLOCATION query — the rkvdec hardware buffers are padded (stride ≠ width, coded height ≠ visible),
so the meta carries the real stride/offset. WebKit 2.52's GL video sink
(`glupload ! glcolorconvert ! webkitglvideosink`) negotiates the dmabuf caps fine but does **not**
propose `GstVideoMeta` in its ALLOCATION query → `decide_allocation` aborts → `not-negotiated` →
the whole pipeline errors → `ERR4`.

This is the **exact same class** as the Step-2 Stage-0 finding: our standalone zero-copy pipeline
fixed it with a meta-aware pad-probe that adds `GST_VIDEO_META_API_TYPE` to the ALLOCATION query
(see `STEP2-inc1-*.md`, `scripts/vvtest/`). WebKit's pipeline lacks that.

## Falsified hypotheses (recorded — avoid re-investigating)
- **"WebKit doesn't support v4l2 stateless"** — FALSE. `GStreamerRegistryScanner.cpp` includes ALL video decoders ≥ `GST_RANK_MARGINAL`; `v4l2slh264dec` (257) is selected by decodebin; H264 advertised supported. No v4l2 blocklist, no enable knob.
- **"WebKit's sink can't negotiate DMA_DRM"** — FALSE. `glupload`/`glcolorconvert`/`webkitglvideosink` all advertise `video/x-raw(memory:DMABuf), format=DMA_DRM, drm-format={…NV12 + modifiers…}`. DMA_DRM is fully supported.
- **"The decoder never links"** — FALSE. `linked v4l2slh264dec0:src and decodepad1:proxypad7, successful`. The "pad has no peer" line was a transient query during setup.
- **Decoder/clip broken** — FALSE. Standalone `filesrc ! qtdemux ! h264parse ! v4l2slh264dec ! fakesink` decodes demo.mp4 cleanly: real NV12 1280×720 frames, GstVideoMeta present, `video0` busy.

## Why Stage-2 (the Vulkan bridge) worked anyway
The bridge fed WebKit **system-memory NV12** (`vulkandownload`). System buffers don't trigger the
v4l2 dmabuf meta requirement, and `glupload` uploads system NV12 fine. So WebKit eats system NV12,
but not the raw rkvdec dmabuf directly — until the meta gap is closed.

## Implications
- In-browser **zero-copy is real work, not a config flip** — the user's skepticism resolved correctly: HW-decode-ish yes, zero-copy no, blocked on the meta ALLOCATION gap.
- The gap is **fixable with the meta-aware technique we already own** (Step-2). Two routes:
  - **A-shim**: an autopluggable GStreamer element between `v4l2slh264dec` and WebKit's sink that intercepts the ALLOCATION query and adds `GstVideoMeta` API support (zero-copy preserved — same dmabuf fd). Closest to Step-2's `meta_probe`, packaged for decodebin.
  - **WebKit patch**: make WebKit's GL-sink ALLOCATION query advertise `GstVideoMeta` (upstream WebKit fix; larger, but the "right" place).
- This unblocks BOTH A and C: the C (Vulkan ICD) bridge, when it outputs dmabuf, will hit the *same* WebKit meta gap — so solving the meta side is shared groundwork.

## Harnesses (board `~/vvtest/`, host `scripts/vvtest/`)
`s3a-webkit-default-rank.sh` (progressive, default rank), `s3a2-diag.sh` (busy-seconds + overlay),
`s3c-negotiation-diag.sh` (rich caps/v4l2/bus debug → `/tmp/s3c_gst.log`). Sandbox off
(`WEBKIT_FORCE_SANDBOX=0`) needed for the decode-subprocess GST log.
