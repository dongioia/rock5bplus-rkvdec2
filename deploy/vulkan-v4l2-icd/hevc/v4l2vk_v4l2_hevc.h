/* SPDX-License-Identifier: MIT */
/*
 * HEVC V4L2 stateless translate — frame-params struct + SPS/PPS/scaling
 * translators from Vulkan Video std structs to V4L2 control structs.
 *
 * Task 5: struct definitions and translate function declarations.
 * Per-field byte correctness is validated in Task 10 via strace-diff.
 */
#ifndef V4L2VK_V4L2_HEVC_H
#define V4L2VK_V4L2_HEVC_H

#include <stdbool.h>
#include <stdint.h>

/* Bring in ext_compat first (vendored EXT_SPS RPS structs for older kernels) */
#include "v4l2vk_hevc_ext_compat.h"
/* v4l2vk_hevc_ext_compat.h already includes <linux/v4l2-controls.h> */

#include <vk_video/vulkan_video_codec_h265std.h>
#include <vk_video/vulkan_video_codec_h265std_decode.h>

/*
 * v4l2vk_hevc_frame_params — all V4L2 controls needed for one HEVC frame.
 *
 * Populated by a chain of translate_* functions:
 *   sps/pps/scaling:     v4l2vk_h265_translate_sps/pps/scaling
 *   decode_params:       v4l2vk_h265_translate_decode_params  (Task 7)
 *   slice_params:        v4l2vk_h265_translate_slice_params   (Task 8)
 *   st_rps/lt_rps:       v4l2vk_h265_translate_rps            (Task 6)
 */
struct v4l2vk_hevc_frame_params {
   struct v4l2_ctrl_hevc_sps               sps;
   struct v4l2_ctrl_hevc_pps               pps;
   struct v4l2_ctrl_hevc_scaling_matrix    scaling;
   struct v4l2_ctrl_hevc_decode_params     decode_params;
   struct v4l2_ctrl_hevc_slice_params      slice_params[16];

   /* EXT_SPS RPS arrays — populated by Task 6 */
   struct v4l2_ctrl_hevc_ext_sps_st_rps   st_rps[64];
   struct v4l2_ctrl_hevc_ext_sps_lt_rps   lt_rps[32];

   uint32_t   slice_count;
   uint32_t   st_rps_count;
   uint32_t   lt_rps_count;
   bool       has_scaling;
};

/* --- Translate function declarations --- */

/**
 * v4l2vk_h265_level_idc_to_raw - convert Vulkan H265LevelIdc enum to
 * the raw level*30 byte used by V4L2 (e.g. LEVEL_IDC_4_1 -> 123).
 * HEVC spec Annex A: general_level_idc = level_idc * 30.
 */
uint8_t v4l2vk_h265_level_idc_to_raw(StdVideoH265LevelIdc lvl);

/**
 * v4l2vk_h265_translate_sps - map StdVideoH265SequenceParameterSet ->
 * struct v4l2_ctrl_hevc_sps.
 */
void v4l2vk_h265_translate_sps(const StdVideoH265SequenceParameterSet *vk,
                                struct v4l2_ctrl_hevc_sps *out);

/**
 * v4l2vk_h265_translate_pps - map StdVideoH265PictureParameterSet ->
 * struct v4l2_ctrl_hevc_pps.
 */
void v4l2vk_h265_translate_pps(const StdVideoH265PictureParameterSet *vk,
                                struct v4l2_ctrl_hevc_pps *out);

/**
 * v4l2vk_h265_translate_scaling - map StdVideoH265ScalingLists ->
 * struct v4l2_ctrl_hevc_scaling_matrix.
 * May be called with vk=NULL (scaling list not present).
 */
void v4l2vk_h265_translate_scaling(const StdVideoH265ScalingLists *vk,
                                   struct v4l2_ctrl_hevc_scaling_matrix *out);

/*
 * v4l2vk_dpb_entry is defined in v4l2vk_dpb.h (mesa-sree-tree).
 * Forward-declare it here so callers don't need to pull in v4l2vk_dpb.h
 * just to use the translate_decode_params signature.
 * The full definition is required at call sites that pass a dpb pointer.
 */
struct v4l2vk_dpb_entry;

/**
 * v4l2vk_h265_translate_st_rps - map an array of StdVideoH265ShortTermRefPicSet
 * structs (SPS RPS table) into V4L2 EXT_SPS_ST_RPS array.
 *
 * @vk:    pointer to the first element of the SPS short-term RPS table.
 * @count: number of sets (= sps->num_short_term_ref_pic_sets, clamped to 64).
 * @out:   output array of at least @count elements.
 *
 * Returns the number of entries written (= min(count, 64)).
 *
 * Field remap (NOT a memcpy — layouts differ):
 *   std flags.inter_ref_pic_set_prediction_flag -> out->flags bit 0
 *   std flags.delta_rps_sign                    -> out->delta_rps_sign (standalone)
 *   std used_by_curr_pic_s0_flag (bitmask)      -> low  num_negative_pics bits of out->used_by_curr_pic
 *   std used_by_curr_pic_s1_flag (bitmask)      -> next num_positive_pics bits of out->used_by_curr_pic
 *   std delta_poc_s0/s1_minus1[i]               -> out->delta_poc_s0/s1_minus1[i] (clamped to 16)
 */
uint32_t v4l2vk_h265_translate_st_rps(const StdVideoH265ShortTermRefPicSet *vk,
                                      uint32_t count,
                                      struct v4l2_ctrl_hevc_ext_sps_st_rps *out);

/**
 * v4l2vk_h265_translate_lt_rps - map StdVideoH265LongTermRefPicsSps (SoA)
 * into V4L2 EXT_SPS_LT_RPS array (AoS).
 *
 * @vk:  pointer to the LT-RPS SPS struct (struct-of-arrays layout).
 * @out: output array of at least vk->num_long_term_ref_pics_sps elements.
 *       The caller must pass an array sized >= num_long_term_ref_pics_sps.
 *
 * Returns the number of entries written (= min(count, 32)).
 *
 * Field remap:
 *   std lt_ref_pic_poc_lsb_sps[i] (u32 array)       -> out[i].lt_ref_pic_poc_lsb_sps (u16, lower 16b)
 *   std used_by_curr_pic_lt_sps_flag bit i (bitmask) -> out[i].flags bit 0 (USED_LT)
 */
uint32_t v4l2vk_h265_translate_lt_rps(const StdVideoH265LongTermRefPicsSps *vk,
                                      uint32_t count,
                                      struct v4l2_ctrl_hevc_ext_sps_lt_rps *out);

/**
 * v4l2vk_h265_translate_decode_params - fill struct v4l2_ctrl_hevc_decode_params
 * from Vulkan decode picture info + DPB.
 *
 * @vk_pic:    current-picture decode info from CmdDecodeVideoKHR.
 * @dpb:       array of DPB entries (opaque to this layer; uses reference_ts +
 *             ref.PicOrderCnt[0] as HEVC poc proxy).
 * @dpb_count: number of valid DPB entries (clamped to V4L2_HEVC_DPB_ENTRIES_NUM_MAX=16).
 * @out:       output struct to fill.
 *
 * poc_st_curr_before/after[] and poc_lt_curr[] are DPB slot indices
 * (matching the V4L2 kernel convention: timestamp = slot_index * 1000).
 *
 * Flags set:
 *   V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC  <- vk_pic->flags.IrapPicFlag
 *   V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC   <- vk_pic->flags.IdrPicFlag
 *   V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR <- not exposed in std; always 0
 */
void v4l2vk_h265_translate_decode_params(
   const StdVideoDecodeH265PictureInfo *vk_pic,
   const struct v4l2vk_dpb_entry *dpb,
   uint32_t dpb_count,
   struct v4l2_ctrl_hevc_decode_params *out);

#endif /* V4L2VK_V4L2_HEVC_H */
