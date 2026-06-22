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

#endif /* V4L2VK_V4L2_HEVC_H */
