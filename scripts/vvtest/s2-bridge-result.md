# Stage-2 Gate 1b Bridge Result

**Date**: 2026-06-21  
**Branch**: spec/vulkanvideo-stage2  
**Gate**: 1b — WebKit (Epiphany) HW-decodes H264 through our Vulkan ICD

## Verdict: GATE_1B=PASS (clean)

### marker_parse.classify output (fixed run — 206 server)

```
{'decoder': 'vulkanh264dec', 'hw': True, 'negotiated': True, 'videometa_fail': False}
```

- `decoder=vulkanh264dec` ✓  (not avdec_h264, not v4l2slh264dec)
- `hw=True` ✓
- `negotiated=True` ✓  (zero not-negotiated errors)
- `videometa_fail=False` ✓
- `avdec_h264=0` ✓  (software fallback not used)
- `VERR_9/code=9/unexpected-200 count=0` ✓  (206 server eliminated range error)

### Sustained-playback evidence

**fuser /dev/video0 per-second timeline:**
```
t=1s:idle t=2s:idle t=3s:idle t=4s:idle t=5s:idle
t=6s:busy t=7s:busy t=8s:busy t=9s:busy t=10s:busy t=11s:busy t=12s:busy
```
rkvdec stays busy for 7 consecutive seconds (t=6s–t=12s). FUSER_VIDEO0=t=6s.

**Screenshot at t=9s**: `benchmark/stage2-20260621/webkit-vulkan-bridge-fixed.png`
- Overlay text: `tick rs=4 t=4.30 1280x720`
- `currentTime=4.30` ✓  (> 0.5s assertion — sustained playback confirmed)
- `readyState=4` (HAVE_ENOUGH_DATA) ✓
- Video dimensions: 1280x720 ✓
- Frame: SMPTE colour-bar test pattern (visibly decoded, non-black) ✓

**curl 206 verification:**
```
HTTP/1.0 206 Partial Content
Content-Range: bytes 0-1023/420130
Accept-Ranges: bytes
```

## Step 1 result (rank-bump only, without wrapper element): FAILED

Raised `GST_PLUGIN_FEATURE_RANK=vulkanh264dec:512,vulkandownload:512,v4l2slh264dec:0,v4l2slvideo0h264dec:0`.

Root cause: decodebin marks `memory:VulkanImage` SRC pads as "final/exposed" rather than seeking
converters. `vulkandownload` (even at rank 512) is not tried as a converter element after `vulkanh264dec`
because decodebin's `autoplug-continue` signal from the VulkanImage pad returns 1 (final). Rank bump
changes priority in the decoder list but does not fix the autoplug converter chain for opaque memory types.

Additionally: when `WAYLAND_DISPLAY` is set (Epiphany is a Wayland app), GStreamer's vulkan plugin
calls `vkCreateInstance` at plugin_init time; with only our ICD the instance failed on some code paths
producing `GST_WARN: Failed to create vulkan instance: Incompatible driver`. The element still registered
correctly once the cache was cleared, but the combined issue with decodebin meant the rank bump was
insufficient.

## Step 3 result: PASS

Built `gstvkh264bridge.c` — a minimal C `GstBin` that wraps `vulkanh264dec ! vulkandownload` internally
and exposes:
- SINK: `video/x-h264` (accepts stream-format byte-stream/avc/avc3)
- SRC: `video/x-raw, format=NV12`
- Rank: `PRIMARY+2` = 258

Compiled on SBC with `gcc -shared -fPIC` + `pkg-config gstreamer-1.0`. Loaded via `GST_PLUGIN_PATH=$HOME/vvtest`.

decodebin auto-plugs `vkh264bridge` correctly because it exposes opaque NV12 system-memory SRC caps.
The internal Vulkan context propagates within the bin: `vulkanh264dec` creates the Vulkan instance,
`vulkandownload` uses it via the same Vulkan instance context (GstContext sharing within the bin).

## Gate 1c scope note

Gate 1c byte-exactness is STANDALONE decode (`vulkanh264dec`→`vulkandownload`→NV12 vs ffmpeg reference). In-browser display correctness (Epiphany/WebKit) is VISUAL-ONLY via the SMPTE-bars screenshot, NOT `pixelcheck`-byte-verified through WebKit.

## Guard fix applied (final review)

`gstvkh264bridge.c` `plugin_init` now guards registration on child-factory availability: probes
`gst_element_factory_find("vulkanh264dec")` and `gst_element_factory_find("vulkandownload")`
at startup; returns FALSE (no register) if either is absent. Recompiled and redeployed on SBC
(see `benchmark/stage2-20260621/bridge-guard-recompile.out`). The TODO comment has been removed.

## Evidence files

| File | Description |
|------|-------------|
| `benchmark/stage2-20260621/webkit-vulkan-bridge-fixed.out` | Fixed run log (206 server, no VERR_9) |
| `benchmark/stage2-20260621/webkit-vulkan-bridge-fixed.png` | Screenshot t=9s (overlay: tick rs=4 t=4.30 1280x720) |
| `benchmark/stage2-20260621/webkit-vulkan-bridge-fixed-5s.png` | Screenshot t=5s (still loading — decode starts t=6s) |
| `benchmark/stage2-20260621/webkit-vulkan-bridge-step3.out` | Prior run (with range error, still PASS on HW-decode criterion) |
| `benchmark/stage2-20260621/webkit-vulkan-bridge-step3-screenshot.png` | Prior screenshot |

## Files changed (this fix)

- `scripts/vvtest/range_server.py` — NEW: 206-capable stdlib HTTP server (localhost only)
- `scripts/vvtest/s2-webkit-decode-test.sh` — replace `http.server` with `range_server.py`; add per-second fuser timeline; add t=5s screenshot
- `scripts/vvtest/gstvkh264bridge.c` — TODO note in `plugin_init` re child-factory guard
- `scripts/vvtest/s2-bridge-result.md` — updated to clean PASS with sustained-playback evidence
