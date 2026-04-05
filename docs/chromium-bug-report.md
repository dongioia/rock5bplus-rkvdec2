# Bug Report for crbug.com/372630272

## Title: V4L2 stateless HW decode on RK3588 (Panfrost): frames don't reach compositor

## Environment
- **SoC**: Rockchip RK3588 (Rock 5B+)
- **GPU**: Mali-G610 MC4 (Panfrost driver, Mesa 26.0.4)
- **Kernel**: Linux 7.0-rc3+ (mainline + rkvdec2 patches for H.264/HEVC/VP9)
- **Chromium**: 148.0.7774.0 (cross-compiled with `use_v4l2_codec=true`)
- **Display**: Wayland (labwc compositor)

## Summary

V4L2 stateless video decoding is correctly detected and V4L2VideoDecoder is selected for playback (visible in chrome://media-internals). However, decoded frames never reach the display. Chromium gracefully falls back to software decode.

## Root Cause

The Linux V4L2 code path in `PickDecoderOutputFormat()` (video_decoder_pipeline.cc) assumes one of two things works:

1. **V4L2 MMAP + EXPBUF**: The driver allocates CAPTURE buffers via MMAP, then exports them as DMABUF via `VIDIOC_EXPBUF`. **This fails** because `rkvdec2` does not support `VIDIOC_EXPBUF` (returns ENOTTY).

2. **V4L2 DMABUF import**: GBM allocates NV12 buffers, passed to V4L2 via `V4L2_MEMORY_DMABUF`. **This fails** because Panfrost's GBM implementation does not support NV12 buffer allocation (all YUV formats fail, only RGBA works).

Neither path delivers frames to the compositor.

## Evidence

### EXPBUF test (Python, on device):
```
REQBUFS MMAP: allocated 1 buffer, caps=0x0000001d
  SUPPORTS_MMAP: True
  SUPPORTS_DMABUF: True (import only)
  SUPPORTS_REQUESTS: True
EXPBUF: FAILED - [Errno 25] Inappropriate ioctl for device
```

### GBM NV12 allocation test (Python, on device):
```
NV12 SCANOUT: FAILED
NV12 RENDERING: FAILED
NV12 LINEAR: FAILED
ARGB8888: OK  (only RGBA formats work)
```

## Proposed Fix

Skip the V4L2 MMAP `viable_candidate` return on Linux, letting the code fall through to the ImageProcessor path. This is a safe change — when driver support for EXPBUF or GBM NV12 is added upstream, the viable_candidate path will automatically work again.

```diff
#elif BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_V4L2_CODEC)
  CHECK(!allocator.has_value());
-  if (viable_candidate) {
-    frame_converter_->set_get_original_frame_cb(base::NullCallback());
-    main_frame_pool_.reset();
-    return *viable_candidate;
-  }
+  // On ARM64 with Panfrost, GBM cannot allocate NV12 buffers
+  // and EXPBUF is not supported by rkvdec2. Skip the MMAP path
+  // and let the ImageProcessor handle NV12->ARGB conversion.
+  // When driver support improves, the viable_candidate path
+  // above (after #endif) will handle it correctly.
```

Note: this alone doesn't complete HW decode — the ImageProcessor also needs NV12 input buffers, which hits the same GBM limitation. The full fix requires Mesa/Panfrost to support NV12 GBM allocation OR the kernel rkvdec2 driver to support VIDIOC_EXPBUF.

## Additional build fixes needed

Two build fixes are required for Chromium 148 with `use_v4l2_codec=true`:

1. **MT21 destructor**: `mt21_decompressor.h:73` — move `~MT21DecompressionJob() = default;` to .cc file (chromium-style violation)
2. **Unsafe buffers**: `mt21_decompressor.cc` — add `#pragma allow_unsafe_buffers` or add path to `unsafe_buffers_paths.txt`

Patch attached.
