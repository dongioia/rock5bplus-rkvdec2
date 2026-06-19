# Phase B0 — H264 blank-decode root cause (2026-06-19)

**Status: ROOT CAUSE FOUND (high confidence, evidenced). Fix = bounded init-sequence rework, NOT yet implemented.**

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

## Fix plan (bounded init-sequence rework — next task)
1. Un-`static` + header-declare `v4l2vk_h264_translate_sps`/`_pps`
   (`v4l2vk_v4l2_h264.c:390/455`).
2. New `v4l2vk_v4l2_set_init_params(ctx, sps_std, pps_std)`: translate + a
   **non-request** `S_EXT_CTRLS` (which=0) of SPS+PPS on `video_fd`.
3. `session_init_v4l2`: take `sps`/`pps`; order = `S_FMT(OUTPUT)` → `set_init_params`
   → CAPTURE via **`G_FMT`** (not S_FMT) → buffers (prefer `CREATE_BUFS`).
4. Plumb SPS/PPS from the 2 callers (`vk_video.c:261` real; `vk_device.c:827` lazy from job).
5. Verify: (a) raw CAPTURE non-blank; then byte-exact visible-normalized vs `ref_f0.nv12`
   (`nv12_tool.py compare`), tracer/control re-check, criteria §1.

## Verification harness ready (Part A)
`scripts/vvtest/`: `icd-rebuild.sh`, `icd-deploy.sh` (isolated), `nv12_tool.py`
(byte-exact/PSNR), `strace_ctrl_diff.py`, `icd-instrument.py` (readback hooks),
`sbc-precond.sh`. Corpus + ref in `artifacts/phase-b0/`.
