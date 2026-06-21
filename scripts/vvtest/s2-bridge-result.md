# Stage-2 Gate 1b Bridge Result

**Date**: 2026-06-21  
**Branch**: spec/vulkanvideo-phaseB0  
**Gate**: 1b — WebKit (Epiphany) HW-decodes H264 through our Vulkan ICD

## Verdict: GATE_1B=PASS (Step 3)

## marker_parse.classify output

```
{'decoder': 'vulkanh264dec', 'hw': True, 'negotiated': True, 'videometa_fail': False}
```

- `decoder=vulkanh264dec` ✓
- `hw=True` ✓
- `negotiated=True` ✓
- `FUSER_VIDEO0=t=6s` ✓ (hardware decode confirmed at t=6s)
- No `not-negotiated` errors ✓

## Screenshot

`benchmark/stage2-20260621/webkit-vulkan-bridge-step3-screenshot.png`

Note: screenshot taken at t=9s with video playing. Overlay shows `tick rs=4 t=...` (readyState=4, video
playing). The HTTP range-request error (code=9) is a Python http.server limitation (returns 200 not 206);
it does not block decode — HW was busy and video played.

## Step 1 result (rank-bump only, without wrapper element): FAILED

Raised `GST_PLUGIN_FEATURE_RANK=vulkanh264dec:512,vulkandownload:512,v4l2slh264dec:0,v4l2slvideo0h264dec:0`.

Root cause: decodebin marks `memory:VulkanImage` SRC pads as "final/exposed" rather than seeking
converters. `vulkandownload` (even at rank 512) is not tried as a converter element after `vulkanh264dec`
because decodebin's `autoplug-continue` signal from the VulkanImage pad returns 1 (final). Rank bump
changes priority in the decoder list but doesn't fix the autoplug converter chain for opaque memory types.

Additionally: when `WAYLAND_DISPLAY` is set (Epiphany is a Wayland app), GStreamer's vulkan plugin
calls `vkCreateInstance` at plugin_init time; with ONLY our ICD the instance failed on some code paths
producing `GST_WARN: Failed to create vulkan instance: Incompatible driver`. The element still registered
correctly once the cache was cleared, but the combined issue with decodebin meant the rank bump was
insufficient.

## Step 3 result: PASS

Built `gstvkh264bridge.c` — a minimal C `GstBin` that wraps `vulkanh264dec ! vulkandownload` internally
and exposes:
- SRC: `video/x-h264` (accepts stream-format byte-stream/avc/avc3)
- SINK: `video/x-raw, format=NV12`
- Rank: `PRIMARY+2` = 258

Compiled on SBC with `gcc -shared -fPIC` + `pkg-config gstreamer-1.0`. Loaded via `GST_PLUGIN_PATH=$HOME/vvtest`.

decodebin auto-plugs `vkh264bridge` correctly because it exposes opaque NV12 system-memory SRC caps.
The internal Vulkan context propagates within the bin: `vulkanh264dec` creates the Vulkan instance,
`vulkandownload` uses it via the same Vulkan instance context (GstContext sharing within the bin).

## Evidence

- `benchmark/stage2-20260621/webkit-vulkan-bridge.out` — Step 1 run (v4l2slvideo0h264dec fallback, FAIL)
- `benchmark/stage2-20260621/webkit-vulkan-bridge-step3.out` — Step 3 run (vkh264bridge, PASS)
- `benchmark/stage2-20260621/webkit-vulkan-bridge-step3-screenshot.png` — Screenshot at t=9s

## Concern: HTTP range request (code=9)

Python `http.server` returns HTTP 200 for range requests; WebKit expects 206 partial content for seeking.
WebKit logs: `R4: Received unexpected 200 HTTP status code for range request`.
This causes code=9 (MEDIA_ERR_NETWORK) to be fired but the video still plays from start. The gate
criterion (decoder=vulkanh264dec, hw=true, negotiated=true, video0 busy, readyState≥2) is met.
A production HTTP server (nginx/python-aiohttp) serving 206 would eliminate this error.

## Files changed

- `scripts/vvtest/s2-webkit-decode-test.sh` — added rank env + GST_PLUGIN_PATH for vulkan feed
- `scripts/vvtest/gstvkh264bridge.c` — new C GstBin wrapper element
- `scripts/vvtest/s2-bridge-result.md` — this file
- `benchmark/stage2-20260621/` — evidence directory (logs + screenshot)
