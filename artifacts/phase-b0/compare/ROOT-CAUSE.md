# Phase B0 — H264 blank-decode root cause + FIX (2026-06-19)

**Status: FIXED + VERIFIED.** Init-sequence fix implemented; decode is now
**byte-exact vs ffmpeg across 8/8 frames** (IDR + P), nv12_tool gate PASS.
Patch: `deploy/vulkan-v4l2-icd/b0-fix.patch` (applied by `scripts/vvtest/b0-fix-init-sps.py`).

## Symptom
ICD (`sree/mesa` V4L2 Vulkan-Video, `5955e6e`) on RK3588 (`rkvdec`, DT
`rockchip,rk3588-vdec`, kernel 7.1): `vulkanh264dec` pipeline runs to EOS but the
decoded CAPTURE buffer is blank (frame-0 IDR luma distinct=6 vs ffmpeg ref=206).

## Method (systematic-debugging, GStreamer-validated)
Corpus `case1.h264` = baseline IDR+P, single-slice, 1280×720 (coded==visible).
Golden = `v4l2slh264dec` on the SAME driver → frame-0 **byte-exact == ffmpeg ref**.
So driver + stream are correct; the bug is entirely in the ICD.

Instruments: in-ICD readback dump (a) (`V4L2VK_DUMP_CAPTURE`); `strace -f -e ioctl`
(v4l2-tracer's LD_PRELOAD does NOT intercept gst's rkvdec ioctls — strace does);
`scripts/vvtest/strace_ctrl_diff.py` (byte-diff control payloads).

## Evidence chain (hypotheses falsified in order)
1. **(a) raw CAPTURE is blank** → decode-side, not the COPY2/COPY3 readback. IDR is
   intra → **H-DPB ruled out**.
2. **Control SET matches golden**: both send SPS+PPS+DECODE_PARAMS per request, NO
   SLICE_PARAMS (rkvdec is frame-based). Adding SLICE_PARAMS → S_EXT_CTRLS EINVAL.
   → **"missing SLICE_PARAMS" hypothesis REJECTED.**
3. **Control VALUES are byte-IDENTICAL** (SPS 1048B, PPS 12B, DECODE_PARAMS 560B all
   match golden exactly) → **H-CONTROL/H-PARSER value bugs ruled out.**
4. **Bitstream is byte-identical** (IDR slice `00 00 01 65 88 84 37 c4…` == case1.h264)
   and delivered with `bytesused=size` → **bitstream-content ruled out.**
5. **DECODE_MODE/START_CODE**: golden only G_FMT-reads them (=1 FRAME_BASED, =1 ANNEX_B,
   driver defaults); ICD already emits Annex-B start codes → ruled out.
6. **CAPTURE frame layout identical** (both NV12, bytesperline=1280; only trailing
   hardware-scratch differs: ours 1843200 vs golden 1612832) → CAPTURE format not the
   blank cause (G_FMT-vs-S_FMT made no difference).

## ROOT CAUSE — incomplete V4L2 init sequence
Structural ioctl-sequence diff of the 1280×720 session:

| step | golden (`v4l2slh264dec`, works) | ours (ICD, blank) |
|---|---|---|
| 1 | `S_FMT(OUTPUT)` H264 | `S_FMT(OUTPUT)` H264 |
| 2 | **`S_EXT_CTRLS` SPS — non-request, on video fd** | *(absent)* |
| 3 | `G_FMT(CAPTURE)` → native 1612832 | `S_FMT(CAPTURE)` forced → 1843200 |
| 4 | `CREATE_BUFS` (dynamic) | `REQBUFS` (legacy) |

The ICD **never sets the SPS control at session init** (only per-request). Golden sets
SPS non-request *after* `S_FMT(OUTPUT)` and *before* CAPTURE format/buffer allocation,
which is what makes rkvdec configure its decoder + compute the native decoded-picture
layout. Without it the decoder is not session-configured → it writes nothing → blank,
even though every per-request control + the bitstream are byte-identical to golden.

Source site: `v4l2vk_video_session_init_v4l2()` (`v4l2vk_vk_video.c:481`) does
`set_output_format` → `set_capture_format`(S_FMT) → `create_*_buffers`, with no SPS set.
The SPS **is** available at the caller (`v4l2vk_vk_video.c:261`, from
`pParametersAddInfo->pStdSPSs[0]`) but is not passed in.

## Fix (IMPLEMENTED — `b0-fix.patch`, 7 files)
1. `v4l2vk_h264_translate_sps_pps()` — public wrapper over the static translators (h264.c/.h).
2. `v4l2vk_v4l2_set_init_sps()` — **non-request** `S_EXT_CTRLS` of SPS (`which=CUR_VAL`)
   on the video fd (v4l2.c/.h).
3. `set_capture_format`: `S_FMT` → `G_FMT` (read the driver's native format).
4. `session_init_v4l2`: `S_FMT(OUTPUT)` → **`set_init_sps`** (fail-hard on error) → CAPTURE
   `G_FMT` → buffers; SPS/PPS plumbed from `vk_video.c` param-create + `vk_device.c` lazy
   caller. (`CREATE_BUFS` NOT needed — REQBUFS works once the decoder is configured.)

## Verification (byte-exact)
After fix: raw CAPTURE (a) Y-distinct 6→**206**; output **byte-exact == ffmpeg ref on
8/8 frames** (IDR + P), `nv12_tool.py compare` → PASS. cavecrew-reviewed (1 finding
applied: fail-hard on init-SPS error). Note: CAPTURE `sizeimage` stays 1843200 (vs golden
1612832) — benign: the frame is the first 1382400 bytes at stride 1280 either way.

## Outcome
**B0 gate PASSED.** Standalone V4L2-backed Vulkan-Video H264 decode is **feasible and
pixel-correct** on RK3588 — the prototype's failure was a fixable init omission, not an
architectural wall.

## Corpus hardening (2026-06-19 — all PASS, fix generalizes)
- **case-1** baseline IDR+P, single-slice, no crop: 120/120 byte-exact.
- **case-2** High profile + 2 B-frames (DPB reordering, CABAC): 60/60 byte-exact.
- **case-3** crop (1278×718, coded 1280×720) + 4 slices/frame: 30/30 byte-exact after
  visible-normalize (§6.B).

Confirms the fix works across profiles, B-frame reference reordering, multi-slice, and
cropping — not just the baseline.

**Integration note (not a decode bug):** for cropped streams `vulkandownload` emits the
coded WIDTH (1280, = stride) with the visible HEIGHT (718); the width crop (1278) is not
applied in the gst output caps. Pixels are all correct (byte-exact after the §6.B
visible-crop). A consumer (Chromium / display sink) must apply the width crop, or the
decoder should report the visible width in its caps. Flag for the Stage-2 / display path.

## Verification harness ready (Part A)
`scripts/vvtest/`: `icd-rebuild.sh`, `icd-deploy.sh` (isolated), `nv12_tool.py`
(byte-exact/PSNR), `strace_ctrl_diff.py`, `icd-instrument.py` (readback hooks),
`sbc-precond.sh`. Corpus + ref in `artifacts/phase-b0/`.
