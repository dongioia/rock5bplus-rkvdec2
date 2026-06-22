# HEVC decode in the V4L2 Vulkan Video ICD — design

**Date:** 2026-06-22
**Status:** design (pending user review → writing-plans)
**Repo:** `dongioia/rock5bplus-rkvdec2`; ICD source in Docker volume `mesa-sree-tree` at `/work/mesa-sree/mesa/src/vulkan-v4l2/`
**Precedes:** implementation plan (writing-plans)

## Context

The V4L2-backed Vulkan Video ICD (sree/mesa `5955e6e` + our fixes) decodes H.264 on RK3588's
rkvdec, byte-exact vs ffmpeg, isolated (no system install), and plays in WebKitGTK via the
`gstvkh264bridge` (B0 → Stage-2 → Stage-3). The codec inventory (Analysis-2026-06-22) showed the
ICD is the sole bottleneck for more codecs: GStreamer 1.28 ships `vulkanh265dec`, and the kernel
ships `v4l2slh265dec` (VDPU381 HEVC, Collabora/Detlev Casanova, mainline ~Feb 2026). HEVC is the
chosen next codec: slice-based like H.264.

## Goal

Decode HEVC **Main profile, 8-bit 4:2:0** through the ICD on rkvdec, byte-exact vs ffmpeg at
multiple resolutions, isolated, and playable in Epiphany via a bridge. 10-bit (Main10/P010) is a
follow-on once 8-bit works. This is a **proof of concept**, the same bar as B0/H.264 — not a
production parser.

## Vault-derived constraints (these shape the design; do not skip)

1. **VDPU381 HEVC mandates RPS controls.** `V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS` and
   `EXT_SPS_LT_RPS` are required for VDPU381 HEVC decode — most other HEVC decoders do not need
   them (Open-Q #8; Source-2026-06-17-v4l2-hevc-ext-rps, HIGH). The ICD MUST populate them
   per-frame or rkvdec will not decode correctly.
2. **The RPS fills from Vulkan std structs — no bitstream parsing.**
   `StdVideoH265SequenceParameterSet` exposes `num_short_term_ref_pic_sets`, `pShortTermRefPicSet`
   (`StdVideoH265ShortTermRefPicSet`), and `pLongTermRefPicsSps`. These map structurally onto
   `v4l2_ctrl_hevc_ext_sps_st_rps` / `_lt_rps` (the "V4L2 mirrors Vulkan RPS" design claim). So the
   mandatory RPS controls are a struct translation, not a parse.
3. **Only the slice-header byte offset needs parsing (stopgap).** `v4l2_ctrl_hevc_slice_params`
   needs `data_byte_offset`, `bit_size`, `num_entry_point_offsets` — the byte where slice data
   begins, like H.264's slice offset. This minimal parse is unavoidable for rkvdec (Hantro), and is
   exactly the contested frontier Nicolas Dufresne flagged (#14987 note 3528322: "bitstream parsing
   in the VK driver is a problem… TODOs on the VK spec side"). Strategic line (gap-tracker
   2026-06-18): **do not over-invest in the parsing path.** Keep the slice parser minimal; document
   it as a stopgap; do not build HEVC RPS/SPS parsing (the std structs supply that).
4. **init-paramset sequence is unverified for HEVC.** The B0 fix set the H.264 SPS non-request at
   init before CAPTURE setup. rkvdec HEVC likely needs VPS/SPS/PPS at init similarly, but which
   controls and in what order is **unknown** → a Step-0 probe against the `v4l2slh265dec` golden
   (strace structural diff, the B0 method) is the first task, before building the translate.

## Approach: Hybrid (mirror translate + minimal codec dispatch)

A new self-contained `v4l2vk_v4l2_hevc.c` holds the HEVC translate (the bulk). The ~10 dispatch
sites (capability advert, session-params finder, picture-info finder, OUTPUT format, control-ID
probe list, `set_codec_controls`, init-paramset, the `translate_params` call) get **additive
codec branches** keyed off a small `codec` enum threaded through the session. The byte-exact H.264
path is only branched, never refactored.

**Rationale (vs generalize-now):** abstracting a codec-ops table from one example (H.264) risks a
wrong abstraction redone when HEVC reveals the real seams, and refactors the working path.
Hybrid's branches are localized and mechanical to convert to an ops-table later — informed by two
real codecs. Defer the ops-table until VP9/AV1.

**Cleanliness refinement:** extract the already codec-agnostic helpers into a shared unit now so
`hevc.c` does not duplicate them: the bit-reader (`br_*`), `v4l2vk_skip_start_code`, and the V4L2
queue/request/submit plumbing (already generic in `v4l2vk_v4l2.c`). Do NOT extract the
codec-specific translate.

## Components

### 1. Capability advertisement (`v4l2vk_vk_physical_device.c`, `v4l2vk_vk_video.c`, `device_exts.h`)
- Queue family: `videoCodecOperations |= VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR`.
- Device ext: add `VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME` to `device_exts.h` + set the flag.
- `GetPhysicalDeviceVideoCapabilitiesKHR`: H265 branch — profile match, `VkVideoDecodeH265CapabilitiesKHR.maxLevelIdc`, `stdHeaderVersion` = H265 decode std name/version, NV12 picture format.
- **Exit test:** `vulkaninfo` lists `VK_KHR_video_decode_h265` and `gst-inspect-1.0` registers `vulkanh265dec` under the ICD.

### 2. Session + params + decode (`v4l2vk_vk_video.c`)
- `CreateVideoSessionParametersKHR`: H265 branch reading `VkVideoDecodeH265SessionParametersCreateInfoKHR` (VPS/SPS/PPS arrays). Derive coded dimensions from `StdVideoH265SequenceParameterSet.pic_width/height_in_luma_samples`.
- `CmdDecodeVideoKHR`: H265 branch — find `VkVideoDecodeH265PictureInfoKHR`, deep-copy `StdVideoDecodeH265PictureInfo` + slice segment offsets + DPB (`VkVideoDecodeH265DpbSlotInfoKHR`) into the recorded job.
- `video_session_init_v4l2`: generalize to take a codec enum + the param set; HEVC path sets OUTPUT `V4L2_PIX_FMT_HEVC_SLICE` and the init-paramset (per Step-0 finding).

### 3. V4L2 layer (`v4l2vk_v4l2.c/.h`)
- `set_output_format`: codec param → `V4L2_PIX_FMT_HEVC_SLICE`.
- Control probe: add the HEVC control IDs — `SPS`, `PPS`, `SCALING_MATRIX`, `DECODE_PARAMS`,
  `SLICE_PARAMS`, **`EXT_SPS_ST_RPS`**, **`EXT_SPS_LT_RPS`**, plus `DECODE_MODE`/`START_CODE` as the
  H.264 path does; index support flags by codec.
- `set_init_paramset` (generalize `set_init_sps`): set the HEVC init controls non-request per Step-0.
- `set_codec_controls` (generalize `set_h264_controls`): build the `v4l2_ext_control` array from the
  HEVC frame-params struct, request-based.

### 4. Translate (`v4l2vk_v4l2_hevc.c`, new) + `v4l2vk_v4l2_hevc.h`
- `struct v4l2vk_hevc_frame_params`: `v4l2_ctrl_hevc_{sps,pps,scaling_matrix,decode_params}`,
  `slice_params[]`, `ext_sps_st_rps[]`, `ext_sps_lt_rps[]`, pred-weight tables, counts/flags.
- Translators: `StdVideoH265SequenceParameterSet→v4l2_ctrl_hevc_sps`;
  `…PictureParameterSet→_pps`; scaling (4x4/8x8/16x16/32x32 + DC, vs H.264's 4x4/8x8);
  `StdVideoDecodeH265PictureInfo + DPB→_decode_params` (POC, RPS curr-before/after/lt lists);
  **RPS: `StdVideoH265ShortTermRefPicSet[]→_ext_sps_st_rps[]`, `…LongTermRefPicsSps→_ext_sps_lt_rps`**;
  `slice_params` incl. the parsed `data_byte_offset`/`bit_size`/`num_entry_point_offsets`.
- `translate_paramset` public wrapper (VPS/SPS/PPS) for the init-paramset, mirroring
  `v4l2vk_h264_translate_sps_pps`.
- Slice parser: minimal HEVC slice-header reader for byte offset + entry points ONLY (stopgap).

### 5. Decode dispatch (`v4l2vk_vk_device.c`)
- Job loop: `if codec==H265 → v4l2vk_hevc_translate_params` then `set_codec_controls`. The NV12
  readback (the Stage-3 stride fix) is codec-agnostic and unchanged.

### 6. Bridge (`gstvkh265bridge`)
- Add a **parallel** `gstvkh265bridge` sibling (do not modify the working `gstvkh264bridge`): a
  GstBin wrapping `vulkanh265dec ! vulkandownload`, sink caps `video/x-h265`, NV12 system-mem src,
  ranked to outrank raw `vulkanh265dec` (Stage-3 decodebin3 finding applies). Factoring the two
  bridges into one parameterized element is deferred with the codec-ops abstraction.

## Data flow (unchanged shape from H.264)

`webkitmediasrc/filesrc → h265parse → gstvkh265bridge[ vulkanh265dec → vulkandownload ] → NV12`,
where `vulkanh265dec` drives the ICD: CreateVideoSession(Params) stores VPS/SPS/PPS → init-paramset
set non-request → per-frame CmdDecodeVideo records StdVideoH265 → QueueSubmit translates to V4L2
HEVC controls (incl. EXT_SPS RPS) + request-based S_EXT_CTRLS → rkvdec → CAPTURE NV12 → stride-fixed
readback.

## Test strategy (TDD, mirrors B0/Stage-3)

1. **Step-0 probe** (de-risk first): strace the golden `v4l2slh265dec` decoding a Main-8bit clip;
   structural-diff the init `S_EXT_CTRLS` sequence to learn which HEVC controls are set non-request
   at init, in what order, before CAPTURE setup. Output = the init-paramset spec for §3.
2. **Capability gate:** `vulkaninfo` shows H265 decode; `vulkanh265dec` registers.
3. **Byte-exact gate:** HEVC Main-8bit clips (libx265) at several resolutions →
   `filesrc ! h265parse ! gstvkh265bridge ! filesink` NV12 vs `ffmpeg -i clip -pix_fmt nv12` →
   luma+chroma byte-exact (extend `s3-multires-gate.sh`). Use `v4l2slh265dec` as the golden for
   strace diffs when blank.
4. **In-browser:** Epiphany progressive + self-hosted MSE (the Stage-3 harnesses, swapped to H265).

## Risks / open questions

- **init-paramset (R1):** rkvdec HEVC init sequence unknown → Step-0 resolves before building.
- **EXT_SPS RPS field mapping (R2):** std↔V4L2 RPS structs are *claimed* aligned (MEDIUM); verify
  field-by-field; the golden strace shows the exact bytes rkvdec expects.
- **Slice parser (R3):** HEVC slice headers are more involved than H.264; keep to byte-offset +
  entry points; lean on the std structs for everything else.
- **Scaling matrix (R4):** 4 list sizes + DC coefficients (vs H.264's 2) — more fields, mechanical.
- **10-bit (out of scope now):** Main10 = P010 CAPTURE format + negotiation; revisit after 8-bit.

## Out of scope

Main10/RExt/SCC; 4:2:2/4:4:4; zero-copy DMA-BUF; upstreaming the parser; the codec-ops abstraction
(deferred to VP9/AV1); real youtube.com (browser-config blocked, separate).

## Definition of done (this round)

Step-0 documented; `vulkaninfo`+`gst-inspect` show H265; HEVC Main-8bit byte-exact vs ffmpeg at
≥3 resolutions incl. a cropped/non-MB-aligned size; clean playback in Epiphany (progressive + MSE)
via `gstvkh265bridge`; H.264 path still byte-exact (no regression); isolated deploy, mesa pin intact.
