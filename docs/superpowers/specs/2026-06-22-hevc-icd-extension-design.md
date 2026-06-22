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
2. **The RPS fills from Vulkan std structs — but it is field-remapping, not a memcpy (MEDIUM).**
   `StdVideoH265SequenceParameterSet` exposes `num_short_term_ref_pic_sets`, `pShortTermRefPicSet`
   (`StdVideoH265ShortTermRefPicSet`), and `pLongTermRefPicsSps` (verified present). The std→V4L2
   layouts are NOT 1:1: V4L2 `used_by_curr_pic` is one `__u32` bitmask vs the std's split
   `used_by_curr_pic[_s0/_s1]_flag`; `delta_rps_sign` is a standalone field in V4L2 but packed in
   `flags` in the std; the LT set is array-of-structs in V4L2 vs struct-of-arrays in the std. So the
   ST/LT RPS controls are populated from the std structs (no RPS *bitstream* parse) by per-field
   bit-repacking. The "V4L2 mirrors Vulkan RPS" alignment is a Collabora design *claim* (MEDIUM —
   the proving lore patchset was Anubis-blocked); the **golden `v4l2slh265dec` strace is the ground
   truth** for the exact bytes rkvdec expects.
3. **The HEVC slice-segment header must be PARSED in full — this is the hard part, not a stopgap.**
   The Vulkan *decode* std (`vulkan_video_codec_h265std_decode.h`) exposes only
   `StdVideoDecodeH265PictureInfo` (POC + RPS list *indices*) and reference info. It does NOT carry
   the slice-level fields. `v4l2_ctrl_hevc_slice_params` needs ~20 of them with no decode-std
   source: `slice_type`, `colour_plane_id`, `slice_pic_order_cnt`, `num_ref_idx_l0/l1_active_minus1`,
   `collocated_ref_idx`, `five_minus_max_num_merge_cand`, the QP/ACT deltas, `slice_beta/tc_offset`,
   `slice_segment_addr`, `ref_idx_l0/l1[]`, the inline `short_term_ref_pic_set()` size, plus
   `data_byte_offset`/`bit_size`/`num_entry_point_offsets`. The existing H.264 path proves the
   pattern: `v4l2vk_h264_translate_slice_params` parses these from the bitstream. So HEVC needs a
   **full slice-segment-header parser**, materially more work than H.264's (HEVC's
   `short_term_ref_pic_set()` parse needs SPS RPS context). This is exactly the frontier Nicolas
   Dufresne flagged (#14987 note 3528322: "the bitstream parsing in the VK driver is a problem").
   Strategic line (gap-tracker 2026-06-18): do not over-invest in *upstreaming* a polished parser —
   but a *correct* one is required for byte-exact decode. To bound the first round, **weighted
   prediction is scope-excluded** (see §Out of scope), so `pred_weight_table` need not be parsed yet.
4. **init-paramset sequence is unverified for HEVC.** The B0 fix set the H.264 SPS non-request at
   init before CAPTURE setup. rkvdec HEVC likely needs VPS/SPS/PPS at init similarly, but which
   controls and in what order is **unknown** → a Step-0 probe against the `v4l2slh265dec` golden
   (strace structural diff, the B0 method) is the first task, before building the translate.

## Approach: Hybrid (mirror translate + minimal codec dispatch)

A new self-contained `v4l2vk_v4l2_hevc.c` holds the HEVC translate (the bulk). The ~10 dispatch
sites (capability advert, session-params finder, picture-info finder, OUTPUT format, control-ID
probe list, `set_codec_controls`, init-paramset, the `translate_params` call) get **additive
codec branches** keyed off a small `codec` enum threaded through the session. The H.264 decode
*logic* is unchanged — but note some shared functions get generalized signatures
(`set_init_sps`→`set_init_paramset`, `set_h264_controls`→`set_codec_controls`,
`video_session_init_v4l2` takes a codec enum), so the no-regression guarantee rests on the §DoD
"H.264 still byte-exact" gate, not on the H.264 files being untouched.

**Rationale (vs generalize-now):** abstracting a codec-ops table from one example (H.264) risks a
wrong abstraction redone when HEVC reveals the real seams, and refactors the working path.
Hybrid's branches are localized and mechanical to convert to an ops-table later — informed by two
real codecs. Defer the ops-table until VP9/AV1.

**Cleanliness refinement:** extract the already codec-agnostic helpers into a shared unit now so
`hevc.c` does not duplicate them: the bit-reader (`br_*`), `v4l2vk_skip_start_code`, and the V4L2
queue/request/submit plumbing (already generic in `v4l2vk_v4l2.c`). Do NOT extract the
codec-specific translate.

## Prerequisite (build blocker — task 0)

The `mesa-sree-tree` build container's `/usr/include/linux/v4l2-controls.h` (dated Feb 9) predates
the EXT_SPS extension: HEVC control IDs stop at `+407` (`ENTRY_POINT_OFFSETS`); the structs
`v4l2_ctrl_hevc_ext_sps_st_rps`/`_lt_rps` and `V4L2_HEVC_EXT_SPS_*_FLAG_*` are absent. The SBC's
running kernel (7.1.0-rc1+) has them at `+408`/`+409` (advertised live on `/dev/video0` as
`hevc_short_term_ref_sets 0x00a40a98` / `hevc_long_term_ref_sets 0x00a40a99`). Referencing the
EXT_SPS controls or structs will not compile against the stale header. **Before any HEVC code:**
refresh the container's `linux/v4l2-controls.h` (and confirm `videodev2.h` has
`V4L2_PIX_FMT_HEVC_SLICE`=`S265`, which it does) to match the SBC kernel UAPI, and gate the build
on it. (`V4L2_PIX_FMT_HEVC_SLICE` and the +400..+407 controls are already present; only the EXT_SPS
additions are missing.)

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
  `slice_params[]`, `ext_sps_st_rps[]`, `ext_sps_lt_rps[]`, counts/flags. (No `pred_weight_table` —
  weighted prediction is scope-excluded this round; see §Out of scope.)
- Translators (struct→struct, no parse):
  `StdVideoH265SequenceParameterSet→v4l2_ctrl_hevc_sps` — including a
  **`v4l2vk_h265_level_idc_to_raw`** (StdVideoH265LevelIdc is an enum 0..N, `general_level_idc` is
  raw e.g. 120 for L4.0; the H.264 path has the analogous `v4l2vk_h264_level_idc_to_raw`);
  `…PictureParameterSet→_pps`; scaling (4x4/8x8/16x16/32x32 + DC, vs H.264's 4x4/8x8);
  `StdVideoDecodeH265PictureInfo + DPB→_decode_params` (POC, RPS curr-before/after/lt index lists);
  **RPS field-remap: `StdVideoH265ShortTermRefPicSet[]→_ext_sps_st_rps[]`,
  `…LongTermRefPicsSps→_ext_sps_lt_rps`** (per-field bit-repacking per constraint #2, golden-validated).
- `translate_paramset` public wrapper (VPS/SPS/PPS) for the init-paramset, mirroring
  `v4l2vk_h264_translate_sps_pps`.
- **Slice-segment-header parser (the hard part, parse from bitstream):** produce the ~20
  `v4l2_ctrl_hevc_slice_params` fields with no decode-std source — `slice_type`, `colour_plane_id`,
  `slice_pic_order_cnt`, `num_ref_idx_l0/l1_active_minus1`, `collocated_ref_idx`,
  `five_minus_max_num_merge_cand`, QP/offset deltas, `slice_segment_addr`, `ref_idx_l0/l1[]`,
  the inline `short_term_ref_pic_set()` size, and `data_byte_offset`/`bit_size`/
  `num_entry_point_offsets`. Reuse the shared bit-reader. HEVC `short_term_ref_pic_set()` parsing
  needs SPS RPS context (thread the SPS in). pred_weight_table parsing is omitted (scope-excluded).

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

- **init-paramset *sequencing* (R1):** which VPS/SPS/PPS controls go non-request at init, in what
  order, is unknown → Step-0 strace-diff resolves this (it shows which ioctls the golden fires).
  This de-risks *sequencing only*.
- **Per-frame parse *correctness* (R2) — the main risk:** the slice_params values (R3) are NOT
  de-risked by the strace. The strace shows the golden's *output* payload, but matching it requires
  the ICD to already parse slice_type/ref-idx/QP/segment-addr/ST-RPS correctly. This is validated
  only by the full parser + the byte-exact gate, not by Step-0.
- **Slice-segment-header parser (R3) — the hard part:** ~20 fields parsed from the bitstream (see
  §4), HEVC `short_term_ref_pic_set()` needs SPS context, materially more than H.264's parser. The
  contested frontier; keep it correct but do not gold-plate for upstreaming.
- **EXT_SPS RPS field-remap (R4):** std↔V4L2 RPS layouts differ (bitmask vs split flags, AoS vs
  SoA); per-field repack (MEDIUM, Collabora claim, lore unread); golden strace is ground truth.
- **Scaling matrix (R5):** 4 list sizes + DC coefficients (vs H.264's 2) — more fields, mechanical.
- **10-bit (out of scope now):** Main10 = P010 CAPTURE format + negotiation; revisit after 8-bit.

## Out of scope

Main10/RExt/SCC; 4:2:2/4:4:4; **weighted prediction** (`pred_weight_table` not parsed this round —
test clips must set `weighted_pred_flag=0`/`weighted_bipred_flag=0`); zero-copy DMA-BUF;
upstreaming the parser; the codec-ops abstraction (deferred to VP9/AV1); real youtube.com
(browser-config blocked, separate).

## Definition of done (this round)

Task-0 header sync done (container UAPI has EXT_SPS); Step-0 init sequence documented;
`vulkaninfo`+`gst-inspect` show H265. **Byte-exact vs ffmpeg on a corpus that exercises the parser,
not just geometry:** ≥3 resolutions incl. a cropped/non-MB-aligned size, AND P-slice and B-slice
streams, AND ≥2 short-term RPS variants — so "byte-exact" means "HEVC-correct for the in-scope
feature set", not "passes on one trivial clip". Weighted-pred streams are excluded by construction.
Clean playback in Epiphany (progressive + MSE) via `gstvkh265bridge`; H.264 path still byte-exact
(no regression, the gate that guards the generalized signatures); isolated deploy, mesa pin intact.
