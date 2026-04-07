# Chromium V4L2 Stateless HW Video Decode on RK3588 — Full Analysis

**Date**: 2026-04-07 (updated)
**Original date**: 2026-04-05
**Author**: Sav (dongioia)
**Platform**: Rock 5B+ (Radxa RS129-D24E0), RK3588, 24GB LPDDR5
**Kernel**: Linux 7.0-rc3+ (mainline + rkvdec2 patches)
**Mesa**: 26.0.4 (Panfrost GLES 3.1 + PanVK Vulkan 1.4)
**Chromium**: Ungoogled Chromium 148.0.7774.0 (cross-compiled on x86_64, no Google services/telemetry)

---

## 1. Objective

Enable hardware-accelerated video decoding in Chromium on RK3588 using the mainline V4L2 stateless decoder API. The RK3588 has full V4L2 stateless decoders for H.264, HEVC, VP9, VP8, and AV1.

## 2. V4L2 Hardware — Confirmed Working

All decoders are present and functional on kernel 7.0-rc3+:

```
/dev/video2 (rkvdec2): H.264 + HEVC + VP9 stateless
/dev/video3 (hantro):  H.264 + MPEG-2 + VP8 stateless
/dev/video5 (hantro):  AV1 stateless
```

Driver capabilities (rkvdec2, /dev/video2):
- SUPPORTS_MMAP: Yes
- SUPPORTS_DMABUF: Yes (import only)
- SUPPORTS_REQUESTS: Yes
- **VIDIOC_EXPBUF: NOT SUPPORTED** (errno 25 ENOTTY)

This last point is critical — see Section 5.

## 3. Build Process

### 3.1 Environment

- **Build host**: NucBox K8 Plus (AMD Ryzen 7 8845HS, 64GB DDR5, CachyOS)
- **Method**: Cross-compilation (x86_64 → arm64) using Chromium's official toolchain
- **Time**: ~3 hours for full build, ~5 minutes for incremental rebuilds

### 3.2 Source Fetch

```bash
# depot_tools
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PWD/depot_tools:$PATH"

# Chromium source (shallow, ~28GB)
mkdir chromium && cd chromium
fetch --nohooks --no-history chromium
cd src
gclient runhooks

# ARM64 sysroot
python3 build/linux/sysroot_scripts/install-sysroot.py --arch=arm64
```

### 3.3 GN Configuration

File: `out/Release/args.gn`

```gn
# Cross-compile
target_cpu = "arm64"
target_os = "linux"

# V4L2 stateless decode — the core change
use_v4l2_codec = true
use_vaapi = false

# Proprietary codecs (H.264, HEVC)
proprietary_codecs = true
ffmpeg_branding = "Chrome"

# Ungoogled settings
is_official_build = true
google_default_client_id = ""
google_default_client_secret = ""
google_api_key = ""
enable_hangout_services_extension = false
enable_widevine = false

# Performance (no debug)
is_debug = false
symbol_level = 0
blink_symbol_level = 0
v8_symbol_level = 0
enable_iterator_debugging = false

# Clang + LLD
is_clang = true
use_lld = true
use_sysroot = true

# Disable PGO (not available for cross-compile)
chrome_pgo_phase = 0

# Disable VR (not needed)
enable_vr = false
use_gnome_keyring = false
```

### 3.4 Build Fixes Required

#### Fix 1: MT21 Destructor (chromium-style error)

**File**: `media/gpu/v4l2/mt21/mt21_decompressor.h` and `.cc`

**Error**:
```
mt21_decompressor.h:73: error: [chromium-style] Complex destructor has an inline body.
```

**Fix**: Move destructor from header to .cc inside `namespace media`:

```cpp
// In mt21_decompressor.h, line 73:
// CHANGE: ~MT21DecompressionJob() = default;
// TO:
~MT21DecompressionJob();

// In mt21_decompressor.cc, before closing namespace:
MT21DecompressionJob::~MT21DecompressionJob() = default;
}  // namespace media
```

#### Fix 2: Unsafe Buffer Warnings (fatal with -Werror)

**File**: `media/gpu/v4l2/mt21/mt21_decompressor.cc`

**Error**:
```
error: unsafe buffer access [-Werror,-Wunsafe-buffer-usage]
error: function '__builtin_memset' is unsafe [-Werror,-Wunsafe-buffer-usage-in-libc-call]
```

**Fix**: Add pragma at top of file:
```cpp
// SAFETY: Legacy V4L2 MT21 decompressor
#pragma allow_unsafe_buffers
```

#### Build Performance Note

Chromium's `-Werror` + extensive warnings slow the build significantly. For development builds, consider adding to args.gn:
```gn
treat_warnings_as_errors = false
```

### 3.5 Build Command

```bash
gn gen out/Release
ninja -C out/Release chrome chrome_sandbox -j14
```

Output: `out/Release/chrome` (416MB ARM64 ELF binary)

### 3.6 Required Files for Deployment

```
chrome                    # Main binary (416MB)
chrome_sandbox            # SUID sandbox helper
chrome_crashpad_handler   # Crash reporter
libEGL.so                 # ANGLE EGL
libGLESv2.so              # ANGLE GLES
libvk_swiftshader.so      # Software Vulkan fallback
libqt5_shim.so            # Qt5 integration
libqt6_shim.so            # Qt6 integration
vk_swiftshader_icd.json   # Vulkan ICD
*.pak                     # Resources
*.bin                     # V8 snapshots
*.dat                     # ICU data
locales/                  # Locale data
MEIPreload/               # Media engagement
```

## 4. Results

### 4.1 What Works

- **chrome://gpu** correctly reports V4L2 HW decode capabilities:
  - HEVC main / main 10: up to 65472x65472
  - H.264 baseline/main/high: up to 65520x65520
  - VP9 profile0: up to 65472x65472
  - VP8: up to 3840x2160
- **Video Decode**: listed as "Hardware accelerated"
- **V4L2VideoDecoder is selected** for video playback (confirmed in chrome://media-internals)
- **GPU compositing**: Hardware accelerated via Panfrost GLES 3.1 (ANGLE)
- **WebGL/WebGPU**: Hardware accelerated

### 4.2 What Doesn't Work (Yet)

V4L2 HW decode is selected but **frames don't reach the display**. Chromium falls back to software decode (VpxVideoDecoder for VP9, Dav1dVideoDecoder for AV1). Videos play, but in software — which is the same as stock Chromium.

The root cause is a GPU driver limitation — see Section 5.

## 5. Root Cause Analysis

### The Problem Chain

The Chromium V4L2 video decoder pipeline has two paths for delivering decoded frames to the GPU compositor:

**Path A — V4L2 MMAP + EXPBUF**:
1. V4L2 allocates CAPTURE buffers via MMAP
2. Buffers exported as DMABUF fd via `VIDIOC_EXPBUF`
3. DMABUF fd wrapped in `NativePixmapFrameResource`
4. Frame sent to compositor

**Path B — V4L2 DMABUF import**:
1. GBM allocates NV12 buffers
2. DMABUF fds passed to V4L2 CAPTURE queue via `V4L2_MEMORY_DMABUF`
3. V4L2 decodes into GBM-allocated buffers
4. Frame sent to compositor

### Why Both Paths Fail on RK3588 + Panfrost

**Path A fails**: The `rkvdec2` kernel driver does NOT support `VIDIOC_EXPBUF`:
```
EXPBUF: FAILED - [Errno 25] Inappropriate ioctl for device
```
Without EXPBUF, MMAP buffers cannot be exported as DMABUF fds, so no frames reach the compositor.

**Path B fails**: Panfrost (Mesa 26.0.4) cannot allocate NV12 buffers via GBM:
```python
# All NV12 GBM allocations fail:
NV12 SCANOUT: FAILED
NV12 RENDERING: FAILED
NV12 LINEAR: FAILED
NV12 SCANOUT|RENDERING: FAILED
NV12 SCANOUT|LINEAR: FAILED
NV12 RENDERING|LINEAR: FAILED

# Only RGBA formats work:
ARGB8888: OK
XRGB8888: OK
```

### The Missing Pieces

1. **Kernel (rkvdec2)**: Needs `VIDIOC_EXPBUF` support to export MMAP buffers as DMABUF. This is a standard V4L2 feature supported by `videobuf2-dma-contig` but may require explicit enablement in the rkvdec driver.

2. **Mesa (Panfrost)**: Needs NV12 GBM buffer allocation support. Collabora added AFBC YUV texture support in Mesa 25.2, but linear NV12 GBM allocation is not yet implemented for Panfrost.

3. **Chromium ImageProcessor**: A software NV12→ARGB conversion via libyuv could work as a fallback, but the current ImageProcessor path also fails because it tries to allocate NV12 buffers via GBM for the input side.

## 6. Attempted Fixes

### Fix 1: Force V4L2 MMAP path with NV12 fallback
**File**: `media/gpu/chromeos/video_decoder_pipeline.cc`
**Change**: In `PickDecoderOutputFormat()`, force NV12 as viable candidate via MMAP path
**Result**: V4L2 selected, no crash, but video doesn't play (EXPBUF not supported → no frames)

### Fix 2: Keep DmabufVideoFramePool active (ChromeOS approach)
**File**: `media/gpu/chromeos/video_decoder_pipeline.cc`
**Change**: Don't reset `main_frame_pool_`, use DMABUF import
**Result**: GPU process crash (Panfrost GBM can't allocate NV12 → `NativePixmapFrameResource::Create()` fails)

### Fix 3: Skip to ImageProcessor path
**File**: `media/gpu/chromeos/video_decoder_pipeline.cc`
**Change**: Skip the Linux V4L2 `viable_candidate` return, fall through to ImageProcessor
**Result**: Partial success — V4L2 selected initially, fails on resolution change, graceful fallback to software decode. Browser is stable and usable.

**Fix 3 is deployed** as the current solution. It provides the correct infrastructure for when the underlying driver issues are resolved.

## 7. Recommended Upstream Changes

### For Chromium (crbug.com/372630272)

The `#elif BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_V4L2_CODEC)` section in `PickDecoderOutputFormat()` assumes either EXPBUF works (MMAP path) or GBM can allocate YUV buffers (DMABUF path). Neither assumption holds on RK3588 + Panfrost. The code should:

1. Check if EXPBUF is actually supported before choosing the MMAP path
2. Check if GBM can allocate the candidate format before choosing the DMABUF path
3. Fall back gracefully to the ImageProcessor when neither works

### For Mesa/Panfrost

NV12 GBM buffer allocation support for Panfrost (Mali-G610 on RK3588). This would unblock Path B entirely.

### For Linux kernel (rkvdec2)

`VIDIOC_EXPBUF` support in the rkvdec2 driver. This would unblock Path A. The `videobuf2-dma-contig` framework already supports EXPBUF — it may just need to be enabled in the driver's `vb2_ops`.

## 8. Current Status

| Component | Status |
|-----------|--------|
| V4L2 decoder HW | Fully working (H.264, HEVC, VP9, VP8, AV1) |
| Chromium V4L2 selection | Working (V4L2VideoDecoder selected) |
| Frame delivery to compositor | **BLOCKED** (EXPBUF + GBM NV12 both missing) |
| Software fallback | Working (VpxVideoDecoder, Dav1dVideoDecoder) |
| Browser stability | Stable with Fix 3 |
| chrome://gpu HW decode listed | Yes — all codecs |

## 9. User-Facing Summary: What Works and What Doesn't

### WORKS (as of 2026-04-07)
- **H.264 V4L2 HW decode** — V4L2VideoDecoder decodes H.264 in hardware via rkvdec2
- **HEVC V4L2 HW decode** — available via rkvdec2 (same pipeline, not yet tested on YouTube)
- **YouTube H.264** — works with h264ify extension (blocks VP9/AV1, forces H.264)
- **720p playback** — smooth with HW decode (~13ms/frame LibYUV conversion)
- **Resolution changes** — 720p↔1080p work without crash
- Browser fully usable, GPU-accelerated rendering, WebGL, WebGPU
- chrome://gpu lists H.264 + HEVC HW decoders via rkvdec2
- Kernel stable — zero oops after VP9 VDPU381 fix
- Chromium closes normally

### LIMITATIONS
- **1080p stutters** — LibYUV CPU conversion takes 23.7ms/frame (over budget for 30fps)
- **YouTube needs h264ify** — without it, YouTube serves VP9/AV1 which fall to software decode
- **VP9 HW decode disabled** on rkvdec2 (kernel crash fix — out-of-tree code had NULL ptr bug)
- VP9 still available via hantro (lower performance, 1080p max)
- Vulkan not usable in Chromium (PanVK limits too low)
- Video encode: software only
- Direct Rendering Display Compositor: disabled (Android-only feature, not a bug)

### PERFORMANCE PATH (Current → Future)
```
Current:  V4L2 HW decode → LibYUV NV12→ARGB (CPU, 23ms@1080p) → compositor
                            ↑ bottleneck

Future:   V4L2 HW decode → EGL NV12 DMABUF import → GPU YUV→RGB → compositor
          (zero-copy, ~0ms overhead)
          Requires: Mesa single-BO NV12 with correct plane offsets
```

### HOW TO GET ZERO-COPY (next step)
Mesa needs to allocate NV12 as a single GBM BO with Y plane at offset 0 and UV plane at
offset = stride * height. Currently allocates two separate BOs (separate inodes, both offset=0).
EGL can't import two separate BOs as one NV12 surface. Fix in `panfrost_resource_create`.

## 10. Reproduction

### Install on Rock 5B+ (Beryllium OS)

```bash
# Extract to /opt/chromium-v4l2/
sudo mkdir -p /opt/chromium-v4l2
sudo tar xzf chromium-v4l2-148-arm64.tar.gz -C /opt/chromium-v4l2/
sudo chown root:root /opt/chromium-v4l2/chrome_sandbox
sudo chmod 4755 /opt/chromium-v4l2/chrome_sandbox

# Launcher
cat > /usr/local/bin/chromium-v4l2 << 'EOF'
#!/bin/bash
export CHROME_DEVEL_SANDBOX=/opt/chromium-v4l2/chrome_sandbox
exec /opt/chromium-v4l2/chrome \
  --enable-features=AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL \
  --enable-gpu-rasterization \
  --enable-zero-copy \
  --ozone-platform-hint=auto \
  --ignore-gpu-blocklist \
  "$@"
EOF
chmod +x /usr/local/bin/chromium-v4l2
```

### Verify

1. Launch `chromium-v4l2`
2. Open `chrome://gpu` → "Video Decode: Hardware accelerated"
3. Open `chrome://media-internals` → Play YouTube → check decoder selection

## 10. Files

| File | Description |
|------|-------------|
| `out/Release/args.gn` | GN build configuration |
| `media/gpu/v4l2/mt21/mt21_decompressor.{h,cc}` | Build fix (destructor + unsafe buffers) |
| `media/gpu/chromeos/video_decoder_pipeline.cc` | Fix 3 (ImageProcessor fallback path) |

## 11. References

- [Chromium Issue #372630272 — Improve V4L2 HW decoding on Linux](https://issues.chromium.org/issues/372630272)
- [Collabora — Chromium HW codecs on MediaTek](https://www.collabora.com/news-and-blog/news-and-events/chromium-hardware-codecs-on-mediatek-genio-700-and-720-from-test-plans-to-real-world-performance.html)
- [Collabora — Panfrost video decode improvements](https://www.collabora.com/news-and-blog/news-and-events/improvements-to-mesa-video-decoding-for-panfrost.html)
- [V4L2 EXPBUF kernel documentation](https://www.kernel.org/doc/html/latest/media/uapi/v4l/vidioc-expbuf.html)
- [chromium-dev: V4L2 on Rockchip rk3399](https://groups.google.com/a/chromium.org/g/chromium-dev/c/AOPuSQVz9Gs)
