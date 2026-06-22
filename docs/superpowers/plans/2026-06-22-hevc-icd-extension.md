# HEVC ICD Extension Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decode HEVC Main 8-bit 4:2:0 through the V4L2-backed Vulkan Video ICD on RK3588's rkvdec, byte-exact vs ffmpeg, isolated, playable in Epiphany via a bridge.

**Architecture:** Hybrid — a new self-contained `v4l2vk_v4l2_hevc.c` holds the HEVC translate + slice parser; existing files get additive codec branches keyed off a `codec` enum. The byte-exact H.264 path is guarded by a no-regression gate. Validation uses the B0 method: strace structural-diff of the ICD's `S_EXT_CTRLS` payload against the in-tree `v4l2slh265dec` golden drives each control to correctness, then a byte-exact NV12 gate proves the pipeline.

**Tech Stack:** C (Mesa Vulkan runtime + V4L2 stateless uAPI), GStreamer 1.28 (`vulkanh265dec`, custom GstBin bridge), Docker build (volume `mesa-sree-tree`, image `rock5b-dev-serena`), Python harness, ffmpeg/v4l2-ctl/strace on the SBC.

## Global Constraints

- **Source of truth for HEVC code is the repo, not the volume.** New HEVC files live git-tracked under `deploy/vulkan-v4l2-icd/hevc/`; the build copies them into the volume's `src/vulkan-v4l2/` and applies modify-patches before `ninja`. Modify-edits to existing volume files are applied by idempotent Python patchers under `scripts/vvtest/` that emit a `.patch` for provenance (the `b0-fix-init-sps.py` / `s3-fix-readback-stride.py` pattern).
- **Isolation is mandatory.** No system install on the SBC; deploy to `~/vvtest/`; env-var ICD (`VK_ICD_FILENAMES`) + `GST_PLUGIN_PATH`; `mesa` pacman pin `1:26.0.6-1` must stay intact (verify with `s2-icd-verify.sh`).
- **Scope:** HEVC Main profile, 8-bit 4:2:0 only. Weighted prediction excluded (`pred_weight_table` not parsed; clips set `weighted_pred_flag=0`/`weighted_bipred_flag=0`). Main10/RExt/4:2:2/4:4:4/zero-copy/upstreaming/codec-ops-abstraction all out of scope.
- **Build container UAPI predates EXT_SPS.** The container's `/usr/include/linux/v4l2-controls.h` stops at CID `+407`; the EXT_SPS controls (`+408`/`+409`) and their structs must be vendored (Task 0) or the ICD will not compile.
- **No-regression:** every task that touches a shared file ends by re-running the H.264 byte-exact gate (`scripts/vvtest/s3-multires-gate.sh`, must stay all-PASS).
- **Reviews:** each code task gets an independent review (cavecrew-reviewer or `/code-review`) before its commit; ICD edits are public-PoC-adjacent — keep commits Saverio-only, no Claude trailer.
- **Bridge ranking:** MSE/`decodebin3` requires the bridge to outrank the raw decoder (Stage-3 finding): rank `vkh265bridge` high, `vulkanh265dec`/`vulkandownload` to 0.
- **Effort weighting (not equal):** Tasks **5, 6, 7, 10** are ~60% of the real work — the SPS/PPS/RPS translate, the full slice-segment-header parser (the contested frontier), and the iterate-until-byte-exact loop. Tasks 0–4, 11–13 are comparatively mechanical. Do not read checkbox count as progress; expect 7 and 10 to dominate wall-clock.

---

### Task 0: Vendor the EXT_SPS UAPI definitions (build unblock)

**Files:**
- Create: `deploy/vulkan-v4l2-icd/hevc/v4l2vk_hevc_ext_compat.h`
- Test: compile probe (below)

**Interfaces:**
- Produces: macros `V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS` (`V4L2_CID_CODEC_STATELESS_BASE + 408`), `V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS` (`+409`); structs `struct v4l2_ctrl_hevc_ext_sps_st_rps`, `struct v4l2_ctrl_hevc_ext_sps_lt_rps`; flag macros. All `#ifndef`-guarded so a newer header is harmless.

- [ ] **Step 1: Confirm the gap, copying the exact struct bytes from the SBC kernel header (ground truth)**

Run:
```bash
ssh rock5b 'grep -nE "EXT_SPS_ST_RPS|EXT_SPS_LT_RPS|v4l2_ctrl_hevc_ext_sps_(st|lt)_rps|V4L2_HEVC_EXT_SPS" /usr/include/linux/v4l2-controls.h'
```
Expected: the CID macros at `+408/+409`, the two structs, and the flag defines. Copy their exact field layout for Step 2.

- [ ] **Step 2: Write the compat header** (`deploy/vulkan-v4l2-icd/hevc/v4l2vk_hevc_ext_compat.h`)

```c
/* SPDX-License-Identifier: MIT */
/* Vendored mainline V4L2 HEVC EXT_SPS RPS uAPI for build envs whose
 * linux/v4l2-controls.h predates it (container header stops at +407).
 * Field layout copied verbatim from kernel 7.1 linux/v4l2-controls.h.
 * All guarded — a newer system header wins. */
#ifndef V4L2VK_HEVC_EXT_COMPAT_H
#define V4L2VK_HEVC_EXT_COMPAT_H
#include <linux/v4l2-controls.h>
#include <linux/types.h>

#ifndef V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS
#define V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS (V4L2_CID_CODEC_STATELESS_BASE + 408)
#define V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS (V4L2_CID_CODEC_STATELESS_BASE + 409)

/* paste the exact flag #defines from Step 1 here */

struct v4l2_ctrl_hevc_ext_sps_st_rps {
	__u8  delta_idx_minus1;
	__u8  delta_rps_sign;
	__u8  num_negative_pics;
	__u8  num_positive_pics;
	__u32 used_by_curr_pic;
	__u32 use_delta_flag;
	__u16 abs_delta_rps_minus1;
	__u16 delta_poc_s0_minus1[16];
	__u16 delta_poc_s1_minus1[16];
	__u16 flags;
};

struct v4l2_ctrl_hevc_ext_sps_lt_rps {
	__u16 lt_ref_pic_poc_lsb_sps;
	__u16 flags;
};
#endif /* guard */
#endif
```
(Replace the flag comment with the exact `V4L2_HEVC_EXT_SPS_*_FLAG_*` defines from Step 1. Re-verify the struct fields against Step 1 output — do not trust this from memory.)

- [ ] **Step 3: Compile-probe the header in the build container**

Run:
```bash
docker run --rm -v "$PWD/deploy/vulkan-v4l2-icd/hevc:/h" rock5b-dev-serena \
  sh -lc 'echo "#include \"/h/v4l2vk_hevc_ext_compat.h\"
int main(){return V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS + (int)sizeof(struct v4l2_ctrl_hevc_ext_sps_st_rps);}" > /tmp/t.c && cc -c /tmp/t.c -o /tmp/t.o && echo COMPILE_OK'
```
Expected: `COMPILE_OK`.

- [ ] **Step 4: Commit**

```bash
git add deploy/vulkan-v4l2-icd/hevc/v4l2vk_hevc_ext_compat.h
git commit -m "hevc: vendor EXT_SPS RPS uAPI for pre-+408 build headers"
```

---

### Task 1: Step-0 — discover the HEVC init control sequence from the golden

**Files:**
- Create: `scripts/vvtest/hevc-step0-strace.sh`
- Create (corpus): `artifacts/phase-hevc/hevc_case1.h265` + `.mp4` (Main 8-bit, no weighted pred)
- Create (output): `artifacts/phase-hevc/STEP0-init-sequence.md`

**Interfaces:**
- Produces: a documented ordered list of which `V4L2_CID_STATELESS_HEVC_*` controls `v4l2slh265dec` sets **non-request** (`which=CUR_VAL`) at init, between `S_FMT(OUTPUT)` and CAPTURE setup. Consumed by Task 8 (`set_init_paramset`).

- [ ] **Step 1: Build the HEVC test corpus on the SBC (Main 8-bit, weighted-pred off)**

Run:
```bash
ssh rock5b 'cd ~/vvtest && \
 ffmpeg -y -loglevel error -i bbb1080.mp4 -t 3 -an -c:v libx265 \
   -x265-params "profile=main:weightp=0:weightb=0:bframes=2" -pix_fmt yuv420p \
   -vf scale=1280:720 hevc_case1.mp4 && \
 ffmpeg -y -loglevel error -i hevc_case1.mp4 -c:v copy -bsf:v hevc_mp4toannexb hevc_case1.h265 && \
 ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,has_b_frames -of default=noprint_wrappers=1 hevc_case1.mp4'
```
Expected: `codec_name=hevc profile=Main width=1280 height=720 has_b_frames=2`.

- [ ] **Step 2: Write the strace probe** (`scripts/vvtest/hevc-step0-strace.sh`)

It runs the golden `v4l2slh265dec` under strace and dumps the ordered `VIDIOC_S_EXT_CTRLS` calls with their `which` field, around CAPTURE setup.
```bash
#!/usr/bin/env bash
# Run ON the SBC. Trace the in-tree golden v4l2slh265dec init S_EXT_CTRLS order.
set +e
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
rm -f /tmp/hevc-golden.strace
strace -f -e trace=ioctl -s 256 -o /tmp/hevc-golden.strace -- \
  gst-launch-1.0 filesrc location=hevc_case1.h265 ! h265parse ! v4l2slh265dec ! fakesink 2>/dev/null
# Show S_EXT_CTRLS ordering + the S_FMT/REQBUFS markers that bracket init
grep -nE 'VIDIOC_S_FMT|VIDIOC_S_EXT_CTRLS|VIDIOC_REQBUFS|VIDIOC_STREAMON|HEVC' /tmp/hevc-golden.strace | head -60
```

- [ ] **Step 3: Run it and record the init sequence**

Run: `ssh rock5b 'bash ~/vvtest/hevc-step0-strace.sh' < scripts/vvtest/hevc-step0-strace.sh`
Then write `artifacts/phase-hevc/STEP0-init-sequence.md` documenting: which control IDs appear with a non-request `S_EXT_CTRLS` before the first CAPTURE `REQBUFS`, and the order. (Expected, to verify not assume: at least HEVC_SPS; possibly PPS/VPS and the EXT_SPS RPS controls.)
Expected: a concrete ordered list; if no non-request control precedes CAPTURE setup, that itself is the finding (HEVC may differ from H.264 — document whatever the trace shows).

- [ ] **Step 4: Commit**

```bash
git add scripts/vvtest/hevc-step0-strace.sh artifacts/phase-hevc/STEP0-init-sequence.md
git commit -m "hevc: Step-0 — golden v4l2slh265dec init control sequence"
```

---

### Task 2: Build plumbing — copy-in HEVC sources + meson, rebuild green

**Files:**
- Create: `deploy/vulkan-v4l2-icd/hevc/v4l2vk_codec.h` (the shared codec enum), `deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.c` (stub), `deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.h` (stub)
- Create: `scripts/vvtest/hevc-build.sh`
- Modify (via patcher): the volume `meson.build` to add `v4l2vk_v4l2_hevc.c`

**Interfaces:**
- Produces: `enum v4l2vk_codec { V4L2VK_CODEC_H264 = 0, V4L2VK_CODEC_H265 = 1 }` in `v4l2vk_codec.h` (H264=0 so existing zero-init paths default to H.264). Consumed by Tasks 4/8/9 — every site the spec calls "codec-branched". `scripts/vvtest/hevc-build.sh` — copies `deploy/vulkan-v4l2-icd/hevc/*.{c,h}` into the volume `src/vulkan-v4l2/`, applies all `scripts/vvtest/hevc-patch-*.py` patchers, runs `ninja`, repackages the `.so` to `deploy/`. Consumed by every later task.

- [ ] **Step 0: Define the codec enum** (`v4l2vk_codec.h`): `#ifndef V4L2VK_CODEC_H ... enum v4l2vk_codec { V4L2VK_CODEC_H264 = 0, V4L2VK_CODEC_H265 = 1 }; ... #endif`. Later patchers add `#include "v4l2vk_codec.h"` to `v4l2vk_v4l2.{c,h}`, `v4l2vk_vk_video.c`, `v4l2vk_vk_device.c` and store the codec on the session struct (defaulting to `V4L2VK_CODEC_H264`).

- [ ] **Step 1: Write a compiling stub** `v4l2vk_v4l2_hevc.c` + `.h` (one no-op exported fn) so meson has a target.

`v4l2vk_v4l2_hevc.h`:
```c
#ifndef V4L2VK_V4L2_HEVC_H
#define V4L2VK_V4L2_HEVC_H
/* HEVC translate — populated in later tasks */
int v4l2vk_hevc_placeholder(void);
#endif
```
`v4l2vk_v4l2_hevc.c`:
```c
#include "v4l2vk_v4l2_hevc.h"
int v4l2vk_hevc_placeholder(void) { return 0; }
```

- [ ] **Step 2: Write `hevc-build.sh`** (mirrors `icd-rebuild.sh`, adds copy-in + patch-apply)

```bash
#!/usr/bin/env bash
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HEVC="$REPO/deploy/vulkan-v4l2-icd/hevc"
DEPLOY="$REPO/deploy/vulkan-v4l2-icd"
IMG="${ICD_BUILD_IMAGE:-rock5b-dev-serena}"
VOL="${ICD_MESA_VOLUME:-mesa-sree-tree}"
docker run --rm -v "$VOL:/work/mesa-sree" -v "$HEVC:/hevc" -v "$REPO/scripts/vvtest:/vv" -v "$DEPLOY:/deploy" "$IMG" sh -lc '
  set -e
  SRC=/work/mesa-sree/mesa/src/vulkan-v4l2
  cp -v /hevc/*.c /hevc/*.h "$SRC"/
  for p in /vv/hevc-patch-*.py; do [ -e "$p" ] && python3 "$p" "$SRC" /deploy; done
  cd /work/mesa-sree/mesa
  ninja -C build src/vulkan-v4l2/libvulkan_v4l2_video.so.1
  cp -v build/src/vulkan-v4l2/libvulkan_v4l2_video.so.1 /deploy/libvulkan_v4l2_video.so
'
echo "[hevc-build] -> $DEPLOY/libvulkan_v4l2_video.so"
```

- [ ] **Step 3: Write the meson patcher** `scripts/vvtest/hevc-patch-00-meson.py` (idempotent; adds `v4l2vk_v4l2_hevc.c` to the sources list — mirror `s3-fix-readback-stride.py` structure: read meson.build, find the H264 source line, insert the HEVC line after it, guard).

- [ ] **Step 4: Build and verify green**

Run: `bash scripts/vvtest/hevc-build.sh`
Expected: ninja links, `.so` repackaged, no errors.

- [ ] **Step 5: No-regression — H.264 gate still PASS**

Run:
```bash
scp deploy/vulkan-v4l2-icd/libvulkan_v4l2_video.so rock5b:vvtest/ && \
ssh rock5b 'bash ~/vvtest/s3-multires-gate.sh'
```
Expected: all four PASS (the stub changed nothing functional).

- [ ] **Step 6: Commit**

```bash
git add deploy/vulkan-v4l2-icd/hevc/ scripts/vvtest/hevc-build.sh scripts/vvtest/hevc-patch-00-meson.py
git commit -m "hevc: build plumbing (copy-in sources + meson) compiling stub"
```

---

### Task 3: Extract shared codec-agnostic helpers

**Files:**
- Create: `deploy/vulkan-v4l2-icd/hevc/v4l2vk_bitreader.h` (header-only bit-reader + start-code skip)
- Modify (patcher `hevc-patch-01-bitreader.py`): `v4l2vk_v4l2_h264.c` to `#include` and use the shared header instead of its local `br_*` / `v4l2vk_skip_start_code`

**Interfaces:**
- Produces: `static inline` `br_init/br_read_bit/br_read_ue/br_read_se/br_skip_bits` and `v4l2vk_skip_start_code` in `v4l2vk_bitreader.h`. Consumed by the HEVC slice parser (Task 7) and the H.264 parser.

- [ ] **Step 1: Read the H.264 bit-reader source to copy it verbatim**

Run: `docker run --rm -v mesa-sree-tree:/work/mesa-sree rock5b-dev-serena sh -lc 'sed -n "25,96p" /work/mesa-sree/mesa/src/vulkan-v4l2/v4l2vk_v4l2_h264.c'`

- [ ] **Step 2: Move those functions verbatim into `v4l2vk_bitreader.h`** as `static inline` (no behavior change).

- [ ] **Step 3: Write `hevc-patch-01-bitreader.py`** — replace the H.264 file's local definitions with `#include "v4l2vk_bitreader.h"` (idempotent, anchored on the exact function bodies).

- [ ] **Step 4: Build + H.264 gate (no-regression is the whole test here)**

Run: `bash scripts/vvtest/hevc-build.sh && scp deploy/vulkan-v4l2-icd/libvulkan_v4l2_video.so rock5b:vvtest/ && ssh rock5b 'bash ~/vvtest/s3-multires-gate.sh'`
Expected: all four PASS — proves the extraction is behavior-preserving.

- [ ] **Step 5: Independent review + commit**

Review the patcher diff (cavecrew-reviewer), then:
```bash
git add deploy/vulkan-v4l2-icd/hevc/v4l2vk_bitreader.h scripts/vvtest/hevc-patch-01-bitreader.py
git commit -m "hevc: extract shared bit-reader/start-code helpers (H.264 unchanged)"
```

---

### Task 4: Capability advertisement — enumerate H.265 decode

**Files:**
- Create (patcher): `scripts/vvtest/hevc-patch-02-caps.py` editing `v4l2vk_device_exts.h`, `v4l2vk_vk_physical_device.c`, `v4l2vk_vk_video.c`

**Interfaces:**
- Consumes: nothing from earlier tasks (pure advert).
- Produces: the ICD reports `VK_KHR_video_decode_h265`, the queue family adds `VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR`, and `GetPhysicalDeviceVideoCapabilitiesKHR` answers an H265 profile (NV12, maxLevel, std header version). This makes `vulkanh265dec` register under the ICD.

- [ ] **Step 1: Write the failing test** — `vulkanh265dec` must NOT yet register (baseline red)

Run:
```bash
ssh rock5b 'cd ~/vvtest && export VK_ICD_FILENAMES=$PWD/v4l2vk_icd.aarch64.json && rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin && gst-inspect-1.0 2>/dev/null | grep -c vulkanh265dec'
```
Expected (pre-fix): `0`.

- [ ] **Step 2: Write `hevc-patch-02-caps.py`** applying these exact additive edits (mirror the H264 sites mapped in the spec §1):
  - `v4l2vk_device_exts.h`: add `{VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H265_SPEC_VERSION},` after the H264 entry; set `exts.KHR_video_decode_h265 = true;` in `v4l2vk_vk_physical_device.c` next to the H264 flag.
  - `v4l2vk_vk_physical_device.c` (queue family video props): `vp->videoCodecOperations |= VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;`.
  - `v4l2vk_vk_video.c` `GetPhysicalDeviceVideoCapabilitiesKHR`: add an H265 branch — accept `VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR` (8-bit/420 only), fill the same `VkVideoCapabilitiesKHR` block, set `stdHeaderVersion` to `VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME`/`_SPEC_VERSION`, and in the pNext loop fill `VkVideoDecodeH265CapabilitiesKHR.maxLevelIdc = STD_VIDEO_H265_LEVEL_IDC_5_1;`.

- [ ] **Step 3: Build + deploy**

Run: `bash scripts/vvtest/hevc-build.sh && scp deploy/vulkan-v4l2-icd/libvulkan_v4l2_video.so rock5b:vvtest/`

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
ssh rock5b 'cd ~/vvtest && export VK_ICD_FILENAMES=$PWD/v4l2vk_icd.aarch64.json && rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin && vulkaninfo 2>/dev/null | grep -i decode_h265 ; gst-inspect-1.0 2>/dev/null | grep -c vulkanh265dec'
```
Expected: `VK_KHR_video_decode_h265` line present; `vulkanh265dec` count `1`.

- [ ] **Step 5: H.264 no-regression gate**

Run: `ssh rock5b 'bash ~/vvtest/s3-multires-gate.sh'` → all PASS.

- [ ] **Step 6: Review + commit**

```bash
git add scripts/vvtest/hevc-patch-02-caps.py
git commit -m "hevc: advertise VK_KHR_video_decode_h265 (enumeration works)"
```

---

### Task 5: HEVC frame-params struct + SPS/PPS/scaling translate (struct→struct)

**Files:**
- Modify: `deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.h` (the `v4l2vk_hevc_frame_params` struct), `v4l2vk_v4l2_hevc.c` (translators)

**Interfaces:**
- Consumes: `v4l2vk_hevc_ext_compat.h` (Task 0).
- Produces:
  - `struct v4l2vk_hevc_frame_params { struct v4l2_ctrl_hevc_sps sps; struct v4l2_ctrl_hevc_pps pps; struct v4l2_ctrl_hevc_scaling_matrix scaling; struct v4l2_ctrl_hevc_decode_params decode_params; struct v4l2_ctrl_hevc_slice_params slice_params[16]; struct v4l2_ctrl_hevc_ext_sps_st_rps st_rps[64]; struct v4l2_ctrl_hevc_ext_sps_lt_rps lt_rps[32]; uint32_t slice_count, st_rps_count, lt_rps_count; bool has_scaling; };`
  - `uint8_t v4l2vk_h265_level_idc_to_raw(StdVideoH265LevelIdc lvl);`
  - `void v4l2vk_h265_translate_sps(const StdVideoH265SequenceParameterSet*, struct v4l2_ctrl_hevc_sps*);`
  - `void v4l2vk_h265_translate_pps(const StdVideoH265PictureParameterSet*, struct v4l2_ctrl_hevc_pps*);`
  - `void v4l2vk_h265_translate_scaling(const StdVideoH265ScalingLists*, struct v4l2_ctrl_hevc_scaling_matrix*);`

- [ ] **Step 1: Read the two struct definitions side by side (do not map from memory)**

Run:
```bash
docker run --rm -v mesa-sree-tree:/work/mesa-sree rock5b-dev-serena sh -lc '
 echo "=== Vulkan StdVideoH265SequenceParameterSet/PPS/ScalingLists ==="; \
 sed -n "/StdVideoH265SequenceParameterSet {/,/}/p;/StdVideoH265PictureParameterSet {/,/}/p;/StdVideoH265ScalingLists {/,/}/p" /work/mesa-sree/mesa/include/vk_video/vulkan_video_codec_h265std.h'
ssh rock5b 'sed -n "/struct v4l2_ctrl_hevc_sps {/,/};/p;/struct v4l2_ctrl_hevc_pps {/,/};/p;/struct v4l2_ctrl_hevc_scaling_matrix {/,/};/p" /usr/include/linux/v4l2-controls.h'
```

- [ ] **Step 2: Write the translators** mirroring `v4l2vk_h264_translate_sps`/`_pps`/`_scaling` (read those first: `sed -n '369,522p' v4l2vk_v4l2_h264.c` — this window covers `v4l2vk_h264_level_idc_to_raw` at **h264.c:369**, `translate_sps` from **390**, plus `translate_pps`/`_scaling`). Map every field per Step 1. `general_level_idc` uses `v4l2vk_h265_level_idc_to_raw` (mirror `v4l2vk_h264_level_idc_to_raw` at **h264.c:369**; HEVC enum `STD_VIDEO_H265_LEVEL_IDC_1_0=0 … `→ raw `level×30`). Scaling fills 4x4/8x8/16x16/32x32 + dc_coef_16x16/32x32.

- [ ] **Step 3: Build (compile is the gate for this struct-only task)**

Run: `bash scripts/vvtest/hevc-build.sh`
Expected: links clean.

- [ ] **Step 4: Commit**

```bash
git add deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.c deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.h
git commit -m "hevc: SPS/PPS/scaling translate + level_idc_to_raw"
```

(Per-field byte correctness is validated against the golden in Task 10 via strace-diff; compile-clean is the gate here.)

---

### Task 6: RPS field-remap (ST/LT) + decode_params translate

**Files:**
- Modify: `v4l2vk_v4l2_hevc.c` / `.h`

**Interfaces:**
- Produces:
  - `uint32_t v4l2vk_h265_translate_st_rps(const StdVideoH265ShortTermRefPicSet*, uint32_t count, struct v4l2_ctrl_hevc_ext_sps_st_rps* out);`
  - `uint32_t v4l2vk_h265_translate_lt_rps(const StdVideoH265LongTermRefPicsSps*, struct v4l2_ctrl_hevc_ext_sps_lt_rps* out);`
  - `void v4l2vk_h265_translate_decode_params(const StdVideoDecodeH265PictureInfo*, const struct v4l2vk_dpb_entry*, uint32_t dpb_count, struct v4l2_ctrl_hevc_decode_params*);`

- [ ] **Step 1: Read both RPS struct layouts (the remap is per-field, not memcpy — spec §constraint #2)**

Run:
```bash
docker run --rm -v mesa-sree-tree:/work/mesa-sree rock5b-dev-serena sh -lc 'sed -n "/StdVideoH265ShortTermRefPicSet {/,/}/p;/StdVideoH265LongTermRefPicsSps {/,/}/p" /work/mesa-sree/mesa/include/vk_video/vulkan_video_codec_h265std.h'
ssh rock5b 'sed -n "/v4l2_ctrl_hevc_ext_sps_st_rps {/,/};/p;/v4l2_ctrl_hevc_ext_sps_lt_rps {/,/};/p;/v4l2_ctrl_hevc_decode_params {/,/};/p" /usr/include/linux/v4l2-controls.h'
```

- [ ] **Step 2: Implement the remap.** ST: std `used_by_curr_pic_flag`/`_s0`/`_s1` → V4L2 `used_by_curr_pic` bitmask; std `flags.delta_rps_sign` → V4L2 standalone `delta_rps_sign`; copy `num_negative/positive_pics`, `delta_poc_s0/s1_minus1[]`, `abs_delta_rps_minus1`, `delta_idx_minus1`, `use_delta_flag`. LT: std SoA (`lt_ref_pic_poc_lsb_sps[]` + `used_by_curr_pic_lt_sps_flag` bitmask) → V4L2 AoS (one entry each). decode_params: POC, `num_active_dpb_entries`, `poc_st_curr_before/after[]`, `poc_lt_curr[]`, dpb[] (mirror `v4l2vk_h264_translate_decode_params` at **h264.c:523** for the DPB pattern), flags `IRAP/IDR/NO_OUTPUT`.

- [ ] **Step 3: Build clean**

Run: `bash scripts/vvtest/hevc-build.sh` → links.

- [ ] **Step 4: Commit**

```bash
git add deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.c deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.h
git commit -m "hevc: RPS (ST/LT) field-remap + decode_params translate"
```

---

### Task 7: HEVC slice-segment-header parser (the hard part)

**Files:**
- Modify: `v4l2vk_v4l2_hevc.c` / `.h`
- Test: `deploy/vulkan-v4l2-icd/hevc/test_hevc_slice_parse.c` (standalone unit harness) + `scripts/vvtest/hevc-slice-parse-check.sh`

**Interfaces:**
- Consumes: `v4l2vk_bitreader.h` (Task 3), the SPS RPS context (Task 6).
- Produces: `uint32_t v4l2vk_h265_translate_slice_params(const uint8_t* bitstream, size_t size, const struct v4l2_ctrl_hevc_sps* sps, const struct v4l2_ctrl_hevc_pps* pps, const uint32_t* slice_offsets, uint32_t slice_count, struct v4l2_ctrl_hevc_slice_params* out);` — parses each slice segment header for the ~20 fields (spec §4): `nal_unit_type`, `slice_segment_addr`, `slice_type`, `colour_plane_id`, `slice_pic_order_cnt`, `num_ref_idx_l0/l1_active_minus1`, `collocated_ref_idx`, `five_minus_max_num_merge_cand`, QP/offset deltas, `slice_beta/tc_offset_div2`, `data_byte_offset`, `bit_size`, `num_entry_point_offsets`, the inline `short_term_ref_pic_set()` size. (Weighted prediction excluded — assert `pred_weight_table()` is not reached for in-scope clips.)

- [ ] **Step 1: Read the H.264 slice parser as the structural template**

Run: `docker run --rm -v mesa-sree-tree:/work/mesa-sree rock5b-dev-serena sh -lc 'sed -n "140,384p;583,683p" /work/mesa-sree/mesa/src/vulkan-v4l2/v4l2vk_v4l2_h264.c'` (the `583,683` window includes the `translate_slice_params` signature at 583) and the H.265 spec §7.3.6.1 slice_segment_header syntax (Source `vk-khr-h265-appendix` / kernel docs).

- [ ] **Step 2: Write the failing unit test** `test_hevc_slice_parse.c` — feed the first slice of `hevc_case1.h265` (a known IDR) and assert `slice_type == V4L2_HEVC_SLICE_TYPE_I` (2) and `slice_segment_addr == 0` and `data_byte_offset > 0`.

```c
/* compile: cc test_hevc_slice_parse.c v4l2vk_v4l2_hevc.c -I<vulkan/v4l2 includes> -o t */
#include <assert.h>
/* read hevc_case1.h265 from argv[1]; scan Annex-B start codes (00 00 01);
   HEVC NAL header is 2 bytes, nal_unit_type = (nal[0] >> 1) & 0x3F;
   first VCL slice = first NAL with type 0..31 (an IDR_W_RADL=19/IDR_N_LP=20
   for this clip). Call v4l2vk_h265_translate_slice_params on it. */
int main(int argc, char** argv){
  /* ... locate first VCL NAL per the rule above; build a 1-entry slice_offsets;
     call v4l2vk_h265_translate_slice_params(...); ... */
  /* assert sp[0].slice_type == 2 && sp[0].slice_segment_addr == 0 && sp[0].data_byte_offset > 0; */
  return 0;
}
```
(VCL-NAL detection rule is concrete above; a competent agent fills the scan loop.)

- [ ] **Step 3: Run it — expect FAIL (parser empty)**

Run: `scripts/vvtest/hevc-slice-parse-check.sh` (compiles the harness in-container, runs against `artifacts/phase-hevc/hevc_case1.h265`). Expected: assertion fails / function returns 0 fields.

- [ ] **Step 4: Implement the parser** (mirror H.264 structure; HEVC `short_term_ref_pic_set()` needs `num_short_term_ref_pic_sets` from the SPS). Keep to the in-scope fields; do not parse `pred_weight_table`.

- [ ] **Step 5: Run the unit test — expect PASS**

Run: `scripts/vvtest/hevc-slice-parse-check.sh`. Expected: `SLICE0 type=2 addr=0 data_byte_offset=NN OK`.

- [ ] **Step 6: Independent review (parser correctness is high-risk) + commit**

```bash
git add deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.c deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.h deploy/vulkan-v4l2-icd/hevc/test_hevc_slice_parse.c scripts/vvtest/hevc-slice-parse-check.sh
git commit -m "hevc: slice-segment-header parser (in-scope fields, no weighted pred)"
```

---

### Task 8: V4L2 layer — OUTPUT format, EXT_SPS probe, init-paramset, codec controls

**Files:**
- Modify (patcher `hevc-patch-03-v4l2.py`): `v4l2vk_v4l2.c` / `.h`

**Interfaces:**
- Consumes: Task 1 (`STEP0-init-sequence.md`), Task 5/6/7 (the frame-params).
- Produces: `set_output_format(ctx, codec)` → `V4L2_PIX_FMT_HEVC_SLICE` for HEVC; HEVC control probe flags (incl. `EXT_SPS_ST_RPS`/`LT_RPS`); `v4l2vk_v4l2_set_init_paramset(ctx, codec, ...)` (generalizes `set_init_sps`, sets the controls Step-0 found non-request at init); `v4l2vk_v4l2_set_codec_controls(ctx, fd, codec, params)` (generalizes `set_h264_controls`, builds the HEVC `v4l2_ext_control` array request-based).

- [ ] **Step 1: Read the H.264 control-setting code to generalize** (`sed -n '40,110p;231,310p;485,581p' v4l2vk_v4l2.c`). NOTE the corrected windows: `v4l2vk_v4l2_set_h264_controls` spans **485–581** — it must be read whole, the tail (542–581) contains the actual `VIDIOC_S_EXT_CTRLS` ioctl + return.
- [ ] **Step 2: Write `hevc-patch-03-v4l2.py`**: add HEVC CID probes (incl. EXT_SPS), branch `set_output_format` on codec, generalize `set_init_sps`→`set_init_paramset` (per Step-0 order), generalize `set_h264_controls`→`set_codec_controls` with an HEVC arm building controls from `v4l2vk_hevc_frame_params` (SPS, PPS, SCALING if has_scaling, DECODE_PARAMS, SLICE_PARAMS, EXT_SPS_ST_RPS, EXT_SPS_LT_RPS).
- [ ] **Step 3: Build + H.264 gate (generalized signatures must not regress H.264)**

Run: `bash scripts/vvtest/hevc-build.sh && scp deploy/vulkan-v4l2-icd/libvulkan_v4l2_video.so rock5b:vvtest/ && ssh rock5b 'bash ~/vvtest/s3-multires-gate.sh'`
Expected: H.264 all PASS. **Plus a full-frame (luma+chroma) check** — this task generalizes the byte-exact control-setting function and the luma-only gate could miss a chroma regression:
```bash
ssh rock5b 'cd ~/vvtest && export VK_ICD_FILENAMES=$PWD/v4l2vk_icd.aarch64.json GST_PLUGIN_PATH=$PWD && rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin && gst-launch-1.0 filesrc location=case1.h264 ! h264parse ! vkh264bridge ! filesink location=/tmp/c1.nv12 >/dev/null 2>&1 && head -c 1382400 /tmp/c1.nv12 > /tmp/c1_f0.nv12 && python3 nv12_tool.py compare --a ref_f0.nv12 --b /tmp/c1_f0.nv12 | grep byte_exact'
```
Expected: `byte_exact: True`.
- [ ] **Step 4: Review + commit**

```bash
git add scripts/vvtest/hevc-patch-03-v4l2.py
git commit -m "hevc: V4L2 layer — HEVC_SLICE fmt, EXT_SPS probe, init-paramset, codec controls"
```

---

### Task 9: Session/params/decode dispatch (vk_video.c, vk_device.c)

**Files:**
- Modify (patcher `hevc-patch-04-dispatch.py`): `v4l2vk_vk_video.c`, `v4l2vk_vk_device.c`

**Interfaces:**
- Consumes: Tasks 5–8.
- Produces: HEVC branches in `CreateVideoSessionParametersKHR` (read `VkVideoDecodeH265SessionParametersCreateInfoKHR` VPS/SPS/PPS), `CmdDecodeVideoKHR` (find `VkVideoDecodeH265PictureInfoKHR`, deep-copy `StdVideoDecodeH265PictureInfo` + `pSliceSegmentOffsets` + DPB), `video_session_init_v4l2` (codec enum), and the QueueSubmit job loop (`if codec==H265 → v4l2vk_h265_translate_* → set_codec_controls`).

- [ ] **Step 1: Read the H.264 dispatch sites** (spec §2/§5: vk_video.c:267-376,445-531; vk_device.c:880-1000).
- [ ] **Step 2: Write `hevc-patch-04-dispatch.py`** adding the H265 branches, threading the `codec` enum from the session profile.
- [ ] **Step 3: Build + deploy + H.264 gate** → all PASS (additive branches).
- [ ] **Step 4: Smoke — does an HEVC frame reach rkvdec?**

Run:
```bash
ssh rock5b 'cd ~/vvtest && export VK_ICD_FILENAMES=$PWD/v4l2vk_icd.aarch64.json GST_PLUGIN_FEATURE_RANK="vulkanh265dec:512" && rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin && (fuser /dev/video0; gst-launch-1.0 -v filesrc location=hevc_case1.h265 ! h265parse ! vulkanh265dec ! fakesink 2>&1 | grep -iE "PLAYING|EOS|error|not-negotiat" | tail) & sleep 4; fuser /dev/video0; wait'
```
Expected: pipeline reaches PLAYING/EOS; `/dev/video0` busy (decode attempted — correctness comes next). Errors here feed systematic-debugging.

- [ ] **Step 5: Review + commit**

```bash
git add scripts/vvtest/hevc-patch-04-dispatch.py
git commit -m "hevc: session/params/decode dispatch (frame reaches rkvdec)"
```

---

### Task 10: Drive to byte-exact via strace structural-diff vs golden

**Files:**
- Create: `scripts/vvtest/hevc-ctrl-diff.sh` (extends `strace_ctrl_diff.py` for HEVC)
- Iterates: `v4l2vk_v4l2_hevc.c` (field fixes)

**Interfaces:**
- Consumes: Tasks 5–9. Produces: byte-identical `S_EXT_CTRLS` payloads vs the golden `v4l2slh265dec` for the same clip → the per-control correctness gate (the B0 method).

- [ ] **Step 1: Capture both traces** — golden `v4l2slh265dec` and our `vulkanh265dec` (via the bridge or raw), same `hevc_case1.h265`, strace `VIDIOC_S_EXT_CTRLS` payloads.
- [ ] **Step 2: Structural-diff** the per-control byte payloads (SPS, PPS, SCALING, DECODE_PARAMS, SLICE_PARAMS, EXT_SPS_ST/LT_RPS). Each mismatching control = a red.
- [ ] **Step 3: Fix the translate field(s)** the diff points to; rebuild; re-diff. Repeat until every control matches the golden byte-for-byte.

Run (loop): `bash scripts/vvtest/hevc-build.sh && scp ... && ssh rock5b 'bash ~/vvtest/hevc-ctrl-diff.sh'`
Expected (green): `ALL CONTROLS MATCH GOLDEN`.

- [ ] **Step 4: Commit** (one commit per control group fixed, or a squashed "match golden" commit)

```bash
git add deploy/vulkan-v4l2-icd/hevc/v4l2vk_v4l2_hevc.c scripts/vvtest/hevc-ctrl-diff.sh
git commit -m "hevc: control payloads byte-match v4l2slh265dec golden"
```

---

### Task 11: gstvkh265bridge

**Files:**
- Create: `deploy/vulkan-v4l2-icd/hevc/gstvkh265bridge.c` (the tracked source; the repo copy IS this file)

**Interfaces:**
- Produces: a GstBin `vkh265bridge` wrapping `vulkanh265dec ! vulkandownload`, sink caps `video/x-h265`, src `video/x-raw` NV12 system-mem, rank 258. Mirror `scripts/vvtest/gstvkh264bridge.c` (swap element name + sink caps + the `plugin_init` guard on `libgstvulkan.so`).

- [ ] **Step 1: Read `scripts/vvtest/gstvkh264bridge.c`** and copy it to `gstvkh265bridge.c`, changing: element factory `vulkanh264dec`→`vulkanh265dec`; sink caps `video/x-h264, stream-format={byte-stream,avc,avc3}` → `video/x-h265, stream-format={hvc1,hev1,byte-stream}`; names `vkh264bridge`→`vkh265bridge`.
- [ ] **Step 2: Build the plugin** using the gcc one-liner documented in `gstvkh264bridge.c` (lines 8–11): `gcc -shared -fPIC -O2 -o libgstvkh265bridge.so gstvkh265bridge.c $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0)`; deploy `libgstvkh265bridge.so` to `~/vvtest`.
- [ ] **Step 3: Test — `gst-inspect` shows the bridge at rank 258**

Run: `ssh rock5b 'cd ~/vvtest && export GST_PLUGIN_PATH=$PWD && gst-inspect-1.0 vkh265bridge | grep -i rank'`
Expected: `Rank ... 258`.
- [ ] **Step 4: Commit**

```bash
git add deploy/vulkan-v4l2-icd/hevc/gstvkh265bridge.c
git commit -m "hevc: gstvkh265bridge (vulkanh265dec+vulkandownload, NV12, rank 258)"
```

---

### Task 12: Byte-exact gate (integration green) — parser-coverage corpus

**Files:**
- Create: `scripts/vvtest/hevc-gate.sh` (mirror `s3-multires-gate.sh`, via the bridge, H265)

**Interfaces:**
- Consumes: all prior tasks. Produces: the integration proof — HEVC byte-exact vs ffmpeg across the DoD corpus.

- [ ] **Step 1: Build the DoD corpus on the SBC** — clips that exercise the parser, not just geometry: 1280×720 (control), 640×360 (cropped, exercises the Stage-3 stride path for HEVC too), 1920×1080; each in an **I-only**, a **P** (`bframes=0`), and a **B** (`bframes=3`) variant; and a **≥2-ST-RPS** variant (longer GOP). All `weightp=0:weightb=0`.
- [ ] **Step 2: Write `hevc-gate.sh`** — for each clip: `filesrc ! h265parse ! vkh265bridge ! filesink` NV12 vs `ffmpeg -i clip -pix_fmt nv12`. `nv12_tool.py compare` takes `--a --b` and compares whole files byte-for-byte, so **cut exactly one full NV12 frame from each side first** (`head -c $((W*H*3/2))`) then `python3 nv12_tool.py compare --a ref_f0 --b out_f0`. This is full-frame (luma+chroma) — a higher bar than the luma-only H.264 `s3-multires-gate.sh`. Report PASS/FAIL per clip.
- [ ] **Step 3: Run — expect all PASS**

Run: `scp scripts/vvtest/hevc-gate.sh rock5b:vvtest/ && ssh rock5b 'bash ~/vvtest/hevc-gate.sh'`
Expected: every clip `PASS (byte-exact)`. Any FAIL → systematic-debugging via the Task-10 strace-diff on that clip.
- [ ] **Step 4: H.264 no-regression final check**

Run: `ssh rock5b 'bash ~/vvtest/s3-multires-gate.sh'` → all PASS.
- [ ] **Step 5: Commit**

```bash
git add scripts/vvtest/hevc-gate.sh
git commit -m "hevc: byte-exact gate — Main 8-bit correct across I/P/B + RPS + resolutions"
```

---

### Task 13: In-browser (progressive + MSE) via gstvkh265bridge

**Files:**
- Create: `scripts/vvtest/s3-hevc-browser.sh` (fork of `s3-realh264-test.sh` + `s3-mse-test.sh`, H265 element/caps/ranks)

**Interfaces:**
- Consumes: Task 11/12. Produces: clean HEVC playback in Epiphany, HW path confirmed.

- [ ] **Step 1: Fork the H264 browser harnesses** swapping: `vkh265bridge`, `vulkanh265dec`, `video/x-h265`, ranks (`vkh265bridge:512,vulkanh265dec:0,vulkandownload:0,v4l2slh265dec:0`), and an HEVC mp4 source.
- [ ] **Step 2: Progressive test** — fuser busy, `vkh265bridge`/`vulkanh265dec` in the GST log, no `avdec_h265`/`de265`, clean screenshot.
- [ ] **Step 3: MSE test** — fragmented HEVC mp4 (`avc`→`hev1`/`hvc1` codecs string `hvc1.1.6.L93.B0`), `decodebin3` plugs the bridge (Stage-3 rank finding), playback advances, backward seek clean.
- [ ] **Step 4: Commit**

```bash
git add scripts/vvtest/s3-hevc-browser.sh
git commit -m "hevc: in-browser progressive + MSE via gstvkh265bridge"
```

---

## Done criteria (whole plan)

Task-0 header sync compiles; `vulkaninfo`/`gst-inspect` show H265; control payloads byte-match the `v4l2slh265dec` golden; `hevc-gate.sh` byte-exact across I/P/B + ≥2 ST RPS + ≥3 resolutions incl. cropped; clean Epiphany playback (progressive + MSE); H.264 gate still all-PASS; isolated deploy, mesa pin intact (`s2-icd-verify.sh`).
