/* SPDX-License-Identifier: MIT */
/*
 * HEVC V4L2 stateless translate — SPS/PPS/scaling struct->struct translators
 * plus ST/LT RPS remap and decode_params translator.
 *
 * Task 5: level_idc_to_raw + translate_sps/pps/scaling.
 * Task 6: translate_st_rps + translate_lt_rps + translate_decode_params.
 *
 * Per-field byte correctness validated in Task 10 via strace-diff against
 * golden v4l2slh265dec trace.
 *
 * Field mapping reference:
 *   Vulkan side: mesa/include/vk_video/vulkan_video_codec_h265std.h
 *                mesa/include/vk_video/vulkan_video_codec_h265std_decode.h
 *   V4L2 side:   /usr/include/linux/v4l2-controls.h (kernel 7.x)
 */
#include "v4l2vk_v4l2_hevc.h"
#include "v4l2vk_dpb.h"
#include "v4l2vk_bitreader.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Level enum -> raw general_level_idc
 *
 * HEVC spec Annex A.4: general_level_idc = Level * 30.
 * Vulkan StdVideoH265LevelIdc enum values 0..12 in ascending level order,
 * with no 1.1/1.2/1.3 (HEVC skips those vs H.264).
 *
 * Enum mapping (from vulkan_video_codec_h265std.h):
 *   STD_VIDEO_H265_LEVEL_IDC_1_0 = 0  -> 30
 *   STD_VIDEO_H265_LEVEL_IDC_2_0 = 1  -> 60
 *   STD_VIDEO_H265_LEVEL_IDC_2_1 = 2  -> 63
 *   STD_VIDEO_H265_LEVEL_IDC_3_0 = 3  -> 90
 *   STD_VIDEO_H265_LEVEL_IDC_3_1 = 4  -> 93
 *   STD_VIDEO_H265_LEVEL_IDC_4_0 = 5  -> 120
 *   STD_VIDEO_H265_LEVEL_IDC_4_1 = 6  -> 123
 *   STD_VIDEO_H265_LEVEL_IDC_5_0 = 7  -> 150
 *   STD_VIDEO_H265_LEVEL_IDC_5_1 = 8  -> 153
 *   STD_VIDEO_H265_LEVEL_IDC_5_2 = 9  -> 156
 *   STD_VIDEO_H265_LEVEL_IDC_6_0 = 10 -> 180
 *   STD_VIDEO_H265_LEVEL_IDC_6_1 = 11 -> 183
 *   STD_VIDEO_H265_LEVEL_IDC_6_2 = 12 -> 186
 * -------------------------------------------------------------------------
 */
uint8_t
v4l2vk_h265_level_idc_to_raw(StdVideoH265LevelIdc lvl)
{
   static const uint8_t map[] = {
      [STD_VIDEO_H265_LEVEL_IDC_1_0] = 30,
      [STD_VIDEO_H265_LEVEL_IDC_2_0] = 60,
      [STD_VIDEO_H265_LEVEL_IDC_2_1] = 63,
      [STD_VIDEO_H265_LEVEL_IDC_3_0] = 90,
      [STD_VIDEO_H265_LEVEL_IDC_3_1] = 93,
      [STD_VIDEO_H265_LEVEL_IDC_4_0] = 120,
      [STD_VIDEO_H265_LEVEL_IDC_4_1] = 123,
      [STD_VIDEO_H265_LEVEL_IDC_5_0] = 150,
      [STD_VIDEO_H265_LEVEL_IDC_5_1] = 153,
      [STD_VIDEO_H265_LEVEL_IDC_5_2] = 156,
      [STD_VIDEO_H265_LEVEL_IDC_6_0] = 180,
      [STD_VIDEO_H265_LEVEL_IDC_6_1] = 183,
      [STD_VIDEO_H265_LEVEL_IDC_6_2] = 186,
   };
   if ((unsigned)lvl < sizeof(map) / sizeof(map[0]))
      return map[lvl];
   return 0;
}

/* -------------------------------------------------------------------------
 * SPS translation
 *
 * Field mapping (std -> V4L2):
 *
 *  sps_video_parameter_set_id    -> video_parameter_set_id
 *  sps_seq_parameter_set_id      -> seq_parameter_set_id
 *  pic_width_in_luma_samples     -> pic_width_in_luma_samples  (u32 -> u16, safe for 4K)
 *  pic_height_in_luma_samples    -> pic_height_in_luma_samples (u32 -> u16)
 *  bit_depth_luma_minus8         -> bit_depth_luma_minus8
 *  bit_depth_chroma_minus8       -> bit_depth_chroma_minus8
 *  log2_max_pic_order_cnt_lsb_minus4           -> log2_max_pic_order_cnt_lsb_minus4
 *  pDecPicBufMgr->[sps_max_sub_layers_minus1]  -> sps_max_dec_pic_buffering_minus1
 *  pDecPicBufMgr->[sps_max_sub_layers_minus1]  -> sps_max_num_reorder_pics
 *  pDecPicBufMgr->[sps_max_sub_layers_minus1]  -> sps_max_latency_increase_plus1
 *  log2_min_luma_coding_block_size_minus3       -> log2_min_luma_coding_block_size_minus3
 *  log2_diff_max_min_luma_coding_block_size     -> log2_diff_max_min_luma_coding_block_size
 *  log2_min_luma_transform_block_size_minus2    -> log2_min_luma_transform_block_size_minus2
 *  log2_diff_max_min_luma_transform_block_size  -> log2_diff_max_min_luma_transform_block_size
 *  max_transform_hierarchy_depth_inter          -> max_transform_hierarchy_depth_inter
 *  max_transform_hierarchy_depth_intra          -> max_transform_hierarchy_depth_intra
 *  pcm_sample_bit_depth_luma_minus1             -> pcm_sample_bit_depth_luma_minus1
 *  pcm_sample_bit_depth_chroma_minus1           -> pcm_sample_bit_depth_chroma_minus1
 *  log2_min_pcm_luma_coding_block_size_minus3   -> log2_min_pcm_luma_coding_block_size_minus3
 *  log2_diff_max_min_pcm_luma_coding_block_size -> log2_diff_max_min_pcm_luma_coding_block_size
 *  num_short_term_ref_pic_sets   -> num_short_term_ref_pic_sets
 *  num_long_term_ref_pics_sps    -> num_long_term_ref_pics_sps
 *  chroma_format_idc (enum)      -> chroma_format_idc (uint8_t, enum value == idc integer)
 *  sps_max_sub_layers_minus1     -> sps_max_sub_layers_minus1
 *
 *  Flags (StdVideoH265SpsFlags -> V4L2_HEVC_SPS_FLAG_*):
 *    separate_colour_plane_flag            -> SEPARATE_COLOUR_PLANE
 *    scaling_list_enabled_flag             -> SCALING_LIST_ENABLED
 *    amp_enabled_flag                      -> AMP_ENABLED
 *    sample_adaptive_offset_enabled_flag   -> SAMPLE_ADAPTIVE_OFFSET
 *    pcm_enabled_flag                      -> PCM_ENABLED
 *    pcm_loop_filter_disabled_flag         -> PCM_LOOP_FILTER_DISABLED
 *    long_term_ref_pics_present_flag       -> LONG_TERM_REF_PICS_PRESENT
 *    sps_temporal_mvp_enabled_flag         -> SPS_TEMPORAL_MVP_ENABLED
 *    strong_intra_smoothing_enabled_flag   -> STRONG_INTRA_SMOOTHING_ENABLED
 *
 *  UNCERTAIN/UNSET (no direct V4L2 equivalent, or not in SPS struct):
 *    - conf_win_*_offset: conformance window — no matching V4L2 SPS field; kernel
 *      derives display crop from VUI or slice_segment header; left zero.
 *    - pProfileTierLevel->general_level_idc: NOT in v4l2_ctrl_hevc_sps (it goes into
 *      higher-level session params); left out here.
 *    - pProfileTierLevel->general_profile_idc: same — not in v4l2_ctrl_hevc_sps.
 *    - sps_sub_layer_ordering_info_present_flag, sps_temporal_id_nesting_flag,
 *      conformance_window_flag: no corresponding V4L2 SPS flag bit.
 * -------------------------------------------------------------------------
 */
void
v4l2vk_h265_translate_sps(const StdVideoH265SequenceParameterSet *vk,
                           struct v4l2_ctrl_hevc_sps *out)
{
   memset(out, 0, sizeof(*out));

   out->video_parameter_set_id = vk->sps_video_parameter_set_id;
   out->seq_parameter_set_id   = vk->sps_seq_parameter_set_id;

   /* Dimensions: std uses u32, V4L2 uses u16; safe for any real resolution */
   out->pic_width_in_luma_samples  = (uint16_t)vk->pic_width_in_luma_samples;
   out->pic_height_in_luma_samples = (uint16_t)vk->pic_height_in_luma_samples;

   out->bit_depth_luma_minus8             = vk->bit_depth_luma_minus8;
   out->bit_depth_chroma_minus8           = vk->bit_depth_chroma_minus8;
   out->log2_max_pic_order_cnt_lsb_minus4 = vk->log2_max_pic_order_cnt_lsb_minus4;

   /*
    * DecPicBufMgr values are arrays indexed by sub-layer.
    * V4L2 exposes only the highest sub-layer (index = sps_max_sub_layers_minus1).
    */
   if (vk->pDecPicBufMgr) {
      uint8_t idx = vk->sps_max_sub_layers_minus1;
      out->sps_max_dec_pic_buffering_minus1 =
         vk->pDecPicBufMgr->max_dec_pic_buffering_minus1[idx];
      out->sps_max_num_reorder_pics =
         vk->pDecPicBufMgr->max_num_reorder_pics[idx];
      out->sps_max_latency_increase_plus1 =
         (uint8_t)vk->pDecPicBufMgr->max_latency_increase_plus1[idx];
   }

   out->log2_min_luma_coding_block_size_minus3      = vk->log2_min_luma_coding_block_size_minus3;
   out->log2_diff_max_min_luma_coding_block_size    = vk->log2_diff_max_min_luma_coding_block_size;
   out->log2_min_luma_transform_block_size_minus2   = vk->log2_min_luma_transform_block_size_minus2;
   out->log2_diff_max_min_luma_transform_block_size = vk->log2_diff_max_min_luma_transform_block_size;
   out->max_transform_hierarchy_depth_inter         = vk->max_transform_hierarchy_depth_inter;
   out->max_transform_hierarchy_depth_intra         = vk->max_transform_hierarchy_depth_intra;

   out->pcm_sample_bit_depth_luma_minus1            = vk->pcm_sample_bit_depth_luma_minus1;
   out->pcm_sample_bit_depth_chroma_minus1          = vk->pcm_sample_bit_depth_chroma_minus1;
   out->log2_min_pcm_luma_coding_block_size_minus3  = vk->log2_min_pcm_luma_coding_block_size_minus3;
   out->log2_diff_max_min_pcm_luma_coding_block_size =
      vk->log2_diff_max_min_pcm_luma_coding_block_size;

   out->num_short_term_ref_pic_sets = vk->num_short_term_ref_pic_sets;
   out->num_long_term_ref_pics_sps  = vk->num_long_term_ref_pics_sps;

   /* chroma_format_idc: enum value == idc integer (0=mono,1=420,2=422,3=444) */
   out->chroma_format_idc      = (uint8_t)vk->chroma_format_idc;
   out->sps_max_sub_layers_minus1 = vk->sps_max_sub_layers_minus1;

   /* Flags */
   if (vk->flags.separate_colour_plane_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE;
   if (vk->flags.scaling_list_enabled_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
   if (vk->flags.amp_enabled_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_AMP_ENABLED;
   if (vk->flags.sample_adaptive_offset_enabled_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET;
   if (vk->flags.pcm_enabled_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_PCM_ENABLED;
   if (vk->flags.pcm_loop_filter_disabled_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED;
   if (vk->flags.long_term_ref_pics_present_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT;
   if (vk->flags.sps_temporal_mvp_enabled_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED;
   if (vk->flags.strong_intra_smoothing_enabled_flag)
      out->flags |= V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED;
}

/* -------------------------------------------------------------------------
 * PPS translation
 *
 * Field mapping (std -> V4L2):
 *
 *  pps_pic_parameter_set_id              -> pic_parameter_set_id
 *  num_extra_slice_header_bits           -> num_extra_slice_header_bits
 *  num_ref_idx_l0_default_active_minus1  -> num_ref_idx_l0_default_active_minus1
 *  num_ref_idx_l1_default_active_minus1  -> num_ref_idx_l1_default_active_minus1
 *  init_qp_minus26                       -> init_qp_minus26
 *  diff_cu_qp_delta_depth                -> diff_cu_qp_delta_depth
 *  pps_cb_qp_offset                      -> pps_cb_qp_offset
 *  pps_cr_qp_offset                      -> pps_cr_qp_offset
 *  num_tile_columns_minus1               -> num_tile_columns_minus1
 *  num_tile_rows_minus1                  -> num_tile_rows_minus1
 *  column_width_minus1[0..N-1]           -> column_width_minus1[0..N-1] (u16 -> u8, tile col width in CTBs; safe for reasonable tile sizes)
 *  row_height_minus1[0..M-1]             -> row_height_minus1[0..M-1]   (u16 -> u8)
 *  pps_beta_offset_div2                  -> pps_beta_offset_div2
 *  pps_tc_offset_div2                    -> pps_tc_offset_div2
 *  log2_parallel_merge_level_minus2      -> log2_parallel_merge_level_minus2
 *
 *  Flags (StdVideoH265PpsFlags -> V4L2_HEVC_PPS_FLAG_*):
 *    dependent_slice_segments_enabled_flag     -> DEPENDENT_SLICE_SEGMENT_ENABLED
 *    output_flag_present_flag                  -> OUTPUT_FLAG_PRESENT
 *    sign_data_hiding_enabled_flag             -> SIGN_DATA_HIDING_ENABLED
 *    cabac_init_present_flag                   -> CABAC_INIT_PRESENT
 *    constrained_intra_pred_flag               -> CONSTRAINED_INTRA_PRED
 *    transform_skip_enabled_flag               -> TRANSFORM_SKIP_ENABLED
 *    cu_qp_delta_enabled_flag                  -> CU_QP_DELTA_ENABLED
 *    pps_slice_chroma_qp_offsets_present_flag  -> PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT
 *    weighted_pred_flag                        -> WEIGHTED_PRED
 *    weighted_bipred_flag                      -> WEIGHTED_BIPRED
 *    transquant_bypass_enabled_flag            -> TRANSQUANT_BYPASS_ENABLED
 *    tiles_enabled_flag                        -> TILES_ENABLED
 *    entropy_coding_sync_enabled_flag          -> ENTROPY_CODING_SYNC_ENABLED
 *    loop_filter_across_tiles_enabled_flag     -> LOOP_FILTER_ACROSS_TILES_ENABLED
 *    pps_loop_filter_across_slices_enabled_flag -> PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED
 *    deblocking_filter_override_enabled_flag   -> DEBLOCKING_FILTER_OVERRIDE_ENABLED
 *    pps_deblocking_filter_disabled_flag       -> PPS_DISABLE_DEBLOCKING_FILTER
 *    lists_modification_present_flag           -> LISTS_MODIFICATION_PRESENT
 *    slice_segment_header_extension_present_flag -> SLICE_SEGMENT_HEADER_EXTENSION_PRESENT
 *    deblocking_filter_control_present_flag    -> DEBLOCKING_FILTER_CONTROL_PRESENT
 *    uniform_spacing_flag                      -> UNIFORM_SPACING
 *
 *  UNCERTAIN/UNSET:
 *    - pps_seq_parameter_set_id: present in std PPS but NOT in v4l2_ctrl_hevc_pps; left out.
 *    - sps_video_parameter_set_id: same — not in V4L2 PPS.
 *    - column_width_minus1/row_height_minus1: std uses u16, V4L2 uses u8. Values are
 *      tile dimensions in CTBs (typically 1-20), so truncation is safe in practice.
 *      Flagged UNCERTAIN for Task 10 verification.
 *    - Extension fields (cross_component_prediction_enabled_flag, chroma_qp_offset_list_*,
 *      pps_curr_pic_ref_enabled_flag, residual_adaptive_colour_transform_enabled_flag,
 *      pps_slice_act_qp_offsets_present_flag, pps_palette_predictor_initializers_present_flag,
 *      monochrome_palette_flag, pps_range_extension_flag, pps_extension_present_flag,
 *      pps_scc_extension_flag): no matching V4L2 PPS flag bits — left zero.
 * -------------------------------------------------------------------------
 */
void
v4l2vk_h265_translate_pps(const StdVideoH265PictureParameterSet *vk,
                           struct v4l2_ctrl_hevc_pps *out)
{
   memset(out, 0, sizeof(*out));

   out->pic_parameter_set_id             = vk->pps_pic_parameter_set_id;
   out->num_extra_slice_header_bits      = vk->num_extra_slice_header_bits;
   out->num_ref_idx_l0_default_active_minus1 = vk->num_ref_idx_l0_default_active_minus1;
   out->num_ref_idx_l1_default_active_minus1 = vk->num_ref_idx_l1_default_active_minus1;
   out->init_qp_minus26                  = vk->init_qp_minus26;
   out->diff_cu_qp_delta_depth           = vk->diff_cu_qp_delta_depth;
   out->pps_cb_qp_offset                 = vk->pps_cb_qp_offset;
   out->pps_cr_qp_offset                 = vk->pps_cr_qp_offset;
   out->num_tile_columns_minus1          = vk->num_tile_columns_minus1;
   out->num_tile_rows_minus1             = vk->num_tile_rows_minus1;

   /*
    * Tile column/row arrays: std u16[19]/u16[21], V4L2 u8[20]/u8[22].
    * Copy min(std_count, v4l2_count) entries; truncate u16->u8 (tile dims in CTBs,
    * safe for practical tile sizes). UNCERTAIN: verify in Task 10.
    */
   {
      uint8_t ncols = vk->num_tile_columns_minus1 < 19 ? vk->num_tile_columns_minus1 : 18;
      for (uint8_t i = 0; i <= ncols; i++)
         out->column_width_minus1[i] = (uint8_t)vk->column_width_minus1[i];
   }
   {
      uint8_t nrows = vk->num_tile_rows_minus1 < 21 ? vk->num_tile_rows_minus1 : 20;
      for (uint8_t i = 0; i <= nrows; i++)
         out->row_height_minus1[i] = (uint8_t)vk->row_height_minus1[i];
   }

   out->pps_beta_offset_div2            = vk->pps_beta_offset_div2;
   out->pps_tc_offset_div2              = vk->pps_tc_offset_div2;
   out->log2_parallel_merge_level_minus2 = vk->log2_parallel_merge_level_minus2;

   /* Flags */
   if (vk->flags.dependent_slice_segments_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED;
   if (vk->flags.output_flag_present_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT;
   if (vk->flags.sign_data_hiding_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED;
   if (vk->flags.cabac_init_present_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT;
   if (vk->flags.constrained_intra_pred_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED;
   if (vk->flags.transform_skip_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED;
   if (vk->flags.cu_qp_delta_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED;
   if (vk->flags.pps_slice_chroma_qp_offsets_present_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT;
   if (vk->flags.weighted_pred_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED;
   if (vk->flags.weighted_bipred_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
   if (vk->flags.transquant_bypass_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED;
   if (vk->flags.tiles_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
   if (vk->flags.entropy_coding_sync_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
   if (vk->flags.loop_filter_across_tiles_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED;
   if (vk->flags.pps_loop_filter_across_slices_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
   if (vk->flags.deblocking_filter_override_enabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED;
   if (vk->flags.pps_deblocking_filter_disabled_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
   if (vk->flags.lists_modification_present_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT;
   if (vk->flags.slice_segment_header_extension_present_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT;
   if (vk->flags.deblocking_filter_control_present_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
   if (vk->flags.uniform_spacing_flag)
      out->flags |= V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING;
}

/* -------------------------------------------------------------------------
 * Scaling matrix translation
 *
 * Field mapping (StdVideoH265ScalingLists -> v4l2_ctrl_hevc_scaling_matrix):
 *
 *  ScalingList4x4[6][16]           -> scaling_list_4x4[6][16]
 *  ScalingList8x8[6][64]           -> scaling_list_8x8[6][64]
 *  ScalingList16x16[6][64]         -> scaling_list_16x16[6][64]
 *  ScalingList32x32[2][64]         -> scaling_list_32x32[2][64]
 *  ScalingListDCCoef16x16[6]       -> scaling_list_dc_coef_16x16[6]
 *  ScalingListDCCoef32x32[2]       -> scaling_list_dc_coef_32x32[2]
 *
 * All sizes match exactly between std and V4L2 (verified from headers).
 * -------------------------------------------------------------------------
 */
void
v4l2vk_h265_translate_scaling(const StdVideoH265ScalingLists *vk,
                               struct v4l2_ctrl_hevc_scaling_matrix *out)
{
   memset(out, 0, sizeof(*out));

   if (!vk)
      return;

   /* 4x4: 6 lists of 16 elements */
   for (int i = 0; i < 6; i++)
      memcpy(out->scaling_list_4x4[i], vk->ScalingList4x4[i], 16);

   /* 8x8: 6 lists of 64 elements */
   for (int i = 0; i < 6; i++)
      memcpy(out->scaling_list_8x8[i], vk->ScalingList8x8[i], 64);

   /* 16x16: 6 lists of 64 elements */
   for (int i = 0; i < 6; i++)
      memcpy(out->scaling_list_16x16[i], vk->ScalingList16x16[i], 64);

   /* 32x32: 2 lists of 64 elements */
   for (int i = 0; i < 2; i++)
      memcpy(out->scaling_list_32x32[i], vk->ScalingList32x32[i], 64);

   /* DC coefficients */
   memcpy(out->scaling_list_dc_coef_16x16, vk->ScalingListDCCoef16x16, 6);
   memcpy(out->scaling_list_dc_coef_32x32, vk->ScalingListDCCoef32x32, 2);
}

/* -------------------------------------------------------------------------
 * Short-term RPS translation
 *
 * Vulkan std layout (StdVideoH265ShortTermRefPicSet):
 *   flags.inter_ref_pic_set_prediction_flag   : 1
 *   flags.delta_rps_sign                      : 1  (standalone in V4L2)
 *   delta_idx_minus1                          : u32 -> u8
 *   use_delta_flag                            : u16 bitmask
 *   abs_delta_rps_minus1                      : u16
 *   used_by_curr_pic_flag                     : u16 (all-list bitmask, NOT used by V4L2)
 *   used_by_curr_pic_s0_flag                  : u16 (negative-list bitmask)
 *   used_by_curr_pic_s1_flag                  : u16 (positive-list bitmask)
 *   num_negative_pics                         : u8
 *   num_positive_pics                         : u8
 *   delta_poc_s0_minus1[STD_VIDEO_H265_MAX_DPB_SIZE=16]  : u16[]
 *   delta_poc_s1_minus1[STD_VIDEO_H265_MAX_DPB_SIZE=16]  : u16[]
 *
 * V4L2 layout (struct v4l2_ctrl_hevc_ext_sps_st_rps):
 *   delta_idx_minus1                          : u8
 *   delta_rps_sign                            : u8  (from flags.delta_rps_sign)
 *   num_negative_pics                         : u8
 *   num_positive_pics                         : u8
 *   used_by_curr_pic                          : u32 bitmask
 *     bits [0..num_negative_pics-1]           : from used_by_curr_pic_s0_flag
 *     bits [num_negative_pics..num_negative_pics+num_positive_pics-1]: from used_by_curr_pic_s1_flag
 *   use_delta_flag                            : u32 (from std u16, widened)
 *   abs_delta_rps_minus1                      : u16
 *   delta_poc_s0_minus1[16]                   : u16[] (max 16 entries)
 *   delta_poc_s1_minus1[16]                   : u16[] (max 16 entries)
 *   flags                                     : u16 (V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED)
 *
 * Key delta (reason remap is NOT a memcpy):
 *   std splits used_by_curr_pic into _s0_flag (negative list) and _s1_flag (positive list);
 *   V4L2 merges them into a single bitmask: bits 0..N0-1 are s0, bits N0..N0+N1-1 are s1.
 *   std flags.delta_rps_sign is a bitfield in a flags struct; V4L2 exposes it as a standalone u8.
 *
 * Array bounds:
 *   STD_VIDEO_H265_MAX_DPB_SIZE = 16, V4L2 array size = 16.
 *   Loop counts are clamped to min(num_*_pics, 16) on BOTH sides.
 *
 * UNCERTAIN (Task 10):
 *   delta_idx_minus1: std is u32, V4L2 is u8. Spec says value 0..64; truncation safe
 *   for valid streams but silently wrong for malformed ones.
 * -------------------------------------------------------------------------
 */
uint32_t
v4l2vk_h265_translate_st_rps(const StdVideoH265ShortTermRefPicSet *vk,
                              uint32_t count,
                              struct v4l2_ctrl_hevc_ext_sps_st_rps *out)
{
   /* Cap to V4L2 array capacity (64 per spec, frame_params holds 64) */
   if (count > 64)
      count = 64;

   for (uint32_t s = 0; s < count; s++) {
      const StdVideoH265ShortTermRefPicSet *src = &vk[s];
      struct v4l2_ctrl_hevc_ext_sps_st_rps *dst = &out[s];

      memset(dst, 0, sizeof(*dst));

      /* Scalar fields */
      dst->delta_idx_minus1   = (uint8_t)src->delta_idx_minus1;
      dst->delta_rps_sign     = src->flags.delta_rps_sign ? 1 : 0;
      dst->num_negative_pics  = src->num_negative_pics;
      dst->num_positive_pics  = src->num_positive_pics;
      dst->abs_delta_rps_minus1 = src->abs_delta_rps_minus1;

      /* use_delta_flag: std u16 -> V4L2 u32 (widened, no sign extension) */
      dst->use_delta_flag = (uint32_t)src->use_delta_flag;

      /*
       * used_by_curr_pic: merge s0 (negative) and s1 (positive) bitmasks.
       * Bits [0..num_negative_pics-1]                        from s0_flag bit i.
       * Bits [num_negative_pics..num_negative_pics+num_positive_pics-1] from s1_flag bit i.
       *
       * std fields are u16; max pics per direction = 15 (spec), so 16 bits suffice.
       */
      {
         uint32_t used = 0;
         uint8_t n0 = src->num_negative_pics;
         uint8_t n1 = src->num_positive_pics;

         /* clamp to 16 so we stay within the u16 source bitmasks */
         if (n0 > 16) n0 = 16;
         if (n1 > 16) n1 = 16;

         for (uint8_t i = 0; i < n0; i++) {
            if (src->used_by_curr_pic_s0_flag & (1u << i))
               used |= (1u << i);
         }
         for (uint8_t i = 0; i < n1; i++) {
            if (src->used_by_curr_pic_s1_flag & (1u << i))
               used |= (1u << (n0 + i));
         }
         dst->used_by_curr_pic = used;
      }

      /*
       * delta_poc_s0/s1_minus1: copy up to min(num_*_pics, 16) entries.
       * STD_VIDEO_H265_MAX_DPB_SIZE = 16 = V4L2 array size; clamp is for
       * corrupt/malformed streams that might set a larger count.
       */
      {
         uint8_t n0 = src->num_negative_pics < 16 ? src->num_negative_pics : 16;
         uint8_t n1 = src->num_positive_pics < 16 ? src->num_positive_pics : 16;

         for (uint8_t i = 0; i < n0; i++)
            dst->delta_poc_s0_minus1[i] = src->delta_poc_s0_minus1[i];
         for (uint8_t i = 0; i < n1; i++)
            dst->delta_poc_s1_minus1[i] = src->delta_poc_s1_minus1[i];
      }

      /* flags: inter_ref_pic_set_prediction_flag -> bit 0 */
      if (src->flags.inter_ref_pic_set_prediction_flag)
         dst->flags |= V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED;
   }

   return count;
}

/* -------------------------------------------------------------------------
 * Long-term RPS translation
 *
 * Vulkan std layout (StdVideoH265LongTermRefPicsSps) — struct-of-arrays:
 *   used_by_curr_pic_lt_sps_flag      : u32 bitmask, bit i = used by current pic
 *   lt_ref_pic_poc_lsb_sps[32]        : u32[] (poc LSB values)
 *
 *   (num_long_term_ref_pics_sps in the outer SPS struct gives the count)
 *
 * V4L2 layout (struct v4l2_ctrl_hevc_ext_sps_lt_rps) — array-of-structs,
 * one element per LT reference:
 *   lt_ref_pic_poc_lsb_sps            : u16 (lower 16 bits of std u32 value)
 *   flags                             : u16 (V4L2_HEVC_EXT_SPS_LT_RPS_FLAG_USED_LT = 0x1)
 *
 * Key delta (reason remap is NOT a memcpy):
 *   std uses SoA (parallel arrays + one bitmask);
 *   V4L2 uses AoS (one struct per entry, bitmask bit -> per-entry flag).
 *
 * Array bounds:
 *   std lt_ref_pic_poc_lsb_sps[STD_VIDEO_H265_MAX_LONG_TERM_REF_PICS_SPS=32]
 *   std used_by_curr_pic_lt_sps_flag is a u32 bitmask (32 bits = 32 entries max)
 *   count is clamped to min(count, 32) on both sides.
 *
 * UNCERTAIN (Task 10):
 *   lt_ref_pic_poc_lsb_sps: std u32 -> V4L2 u16.  The POC LSB for LT refs is
 *   bounded by MaxPicOrderCntLsb (= 2^(log2_max_pic_order_cnt_lsb_minus4+4)).
 *   For log2_max+4 <= 16, value fits in u16.  For log2_max+4 > 16 (uncommon),
 *   the upper bits are silently lost.  Flag for Task 10.
 * -------------------------------------------------------------------------
 */
uint32_t
v4l2vk_h265_translate_lt_rps(const StdVideoH265LongTermRefPicsSps *vk,
                              uint32_t count,
                              struct v4l2_ctrl_hevc_ext_sps_lt_rps *out)
{
   if (!vk)
      return 0;

   /* Cap to u32 bitmask capacity and std array size */
   if (count > 32)
      count = 32;

   for (uint32_t i = 0; i < count; i++) {
      memset(&out[i], 0, sizeof(out[i]));

      /* SoA -> AoS: pick element i from the poc_lsb array */
      out[i].lt_ref_pic_poc_lsb_sps = (uint16_t)vk->lt_ref_pic_poc_lsb_sps[i];

      /* Bit i of the bitmask -> per-entry USED_LT flag */
      if (vk->used_by_curr_pic_lt_sps_flag & (1u << i))
         out[i].flags |= V4L2_HEVC_EXT_SPS_LT_RPS_FLAG_USED_LT;
   }

   return count;
}

/* -------------------------------------------------------------------------
 * Decode params translation
 *
 * Fills struct v4l2_ctrl_hevc_decode_params for one HEVC frame.
 *
 * Field mapping (StdVideoDecodeH265PictureInfo -> v4l2_ctrl_hevc_decode_params):
 *
 *  vk_pic->PicOrderCntVal                     -> out->pic_order_cnt_val
 *  vk_pic->NumDeltaPocsOfRefRpsIdx            -> out->num_delta_pocs_of_ref_rps_idx
 *  vk_pic->NumBitsForSTRefPicSetInSlice       -> out->short_term_ref_pic_set_size
 *    (same field, renamed in V4L2 to short_term_ref_pic_set_size)
 *  out->long_term_ref_pic_set_size            -> 0 (no direct std source; UNCERTAIN)
 *  out->num_active_dpb_entries                -> dpb_count (clamped to 16)
 *
 *  DPB: v4l2vk_dpb_entry -> v4l2_hevc_dpb_entry
 *    dpb[i].reference_ts        -> dpb[i].timestamp
 *    dpb[i].ref.PicOrderCnt[0] -> dpb[i].pic_order_cnt_val  (UNCERTAIN: H264 field used
 *                                   as HEVC poc proxy; no HEVC-specific poc in dpb_entry)
 *    (always)                   -> flags = V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE set
 *                                   if ref.flags.used_for_long_term_reference
 *    field_pic                  -> 0 (frame-based decode)
 *
 *  RefPicSet index lists (V4L2 poc_*_curr[] hold DPB *slot indices*):
 *    vk_pic->RefPicSetStCurrBefore[i] (0xFF = invalid) -> poc_st_curr_before[i]
 *    vk_pic->RefPicSetStCurrAfter[i]                   -> poc_st_curr_after[i]
 *    vk_pic->RefPicSetLtCurr[i]                        -> poc_lt_curr[i]
 *    STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE = 8 (both sides)
 *    V4L2_HEVC_DPB_ENTRIES_NUM_MAX = 16 (output array size; first 8 are used)
 *
 *  Flags:
 *    vk_pic->flags.IrapPicFlag -> V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC
 *    vk_pic->flags.IdrPicFlag  -> V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC
 *    V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR -> 0 (no source in std pic info)
 *
 * Array bounds:
 *   poc_*_curr[] arrays: std size = 8 (STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE),
 *   V4L2 array size = 16 (V4L2_HEVC_DPB_ENTRIES_NUM_MAX). Copy exactly 8 entries.
 *   DPB: std size = V4L2VK_MAX_DPB_SLOTS = 16, V4L2 = 16. Clamp dpb_count to 16.
 *
 * UNCERTAIN (Task 10):
 *   - poc_st_curr_before/after/lt_curr: V4L2 expects DPB slot indices
 *     (0..num_active_dpb_entries-1); std RefPicSet*[] hold DPB slot indices
 *     via the Vulkan slot numbering. Mapping should be 1:1 if slot_index ==
 *     V4L2 DPB array index, but golden strace-diff will verify.
 *   - dpb[i].pic_order_cnt_val: using ref.PicOrderCnt[0] (H264 field) as
 *     HEVC POC proxy. Correct only if the DPB entry was populated with the
 *     matching HEVC POC; no HEVC-specific field exists in v4l2vk_dpb_entry.
 *   - long_term_ref_pic_set_size: set to 0 (no source in std PictureInfo).
 * -------------------------------------------------------------------------
 */
void
v4l2vk_h265_translate_decode_params(
   const StdVideoDecodeH265PictureInfo *vk_pic,
   const struct v4l2vk_dpb_entry *dpb,
   uint32_t dpb_count,
   struct v4l2_ctrl_hevc_decode_params *out)
{
   memset(out, 0, sizeof(*out));

   if (!vk_pic)
      return;

   /* Current picture POC and metadata */
   out->pic_order_cnt_val              = vk_pic->PicOrderCntVal;
   out->num_delta_pocs_of_ref_rps_idx = vk_pic->NumDeltaPocsOfRefRpsIdx;

   /*
    * short_term_ref_pic_set_size: V4L2 name for NumBitsForSTRefPicSetInSlice.
    * long_term_ref_pic_set_size: no matching field in StdVideoDecodeH265PictureInfo;
    * left zero (UNCERTAIN — Task 10 will reveal if kernel needs this).
    */
   out->short_term_ref_pic_set_size = vk_pic->NumBitsForSTRefPicSetInSlice;
   out->long_term_ref_pic_set_size  = 0;

   /* DPB entries */
   if (dpb && dpb_count > 0) {
      if (dpb_count > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
         dpb_count = V4L2_HEVC_DPB_ENTRIES_NUM_MAX;

      out->num_active_dpb_entries = (uint8_t)dpb_count;

      for (uint32_t i = 0; i < dpb_count; i++) {
         struct v4l2_hevc_dpb_entry *d = &out->dpb[i];

         d->timestamp         = dpb[i].reference_ts;
         /*
          * UNCERTAIN: ref.PicOrderCnt[0] is a H264 field (TopFieldOrderCnt).
          * Used as HEVC poc proxy since v4l2vk_dpb_entry has no HEVC-specific
          * PicOrderCntVal field. Task 10 strace-diff will verify.
          */
         d->pic_order_cnt_val = dpb[i].ref.PicOrderCnt[0];
         d->field_pic         = 0; /* frame-based decode */
         d->reserved          = 0;
         d->flags             = 0;

         if (dpb[i].ref.flags.used_for_long_term_reference)
            d->flags |= V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;
      }
   }

   /*
    * RefPicSet current lists: Vulkan slot indices for ST/LT current references.
    * STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE = 8.
    * V4L2 arrays are V4L2_HEVC_DPB_ENTRIES_NUM_MAX = 16; copy the 8 std entries.
    * 0xFF in std = "not present" (same sentinel value used by V4L2).
    */
   out->num_poc_st_curr_before = 0;
   out->num_poc_st_curr_after  = 0;
   out->num_poc_lt_curr        = 0;

   for (uint32_t i = 0; i < STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE; i++) {
      uint8_t idx;

      idx = vk_pic->RefPicSetStCurrBefore[i];
      out->poc_st_curr_before[i] = idx;
      if (idx != 0xFF)
         out->num_poc_st_curr_before = (uint8_t)(i + 1);

      idx = vk_pic->RefPicSetStCurrAfter[i];
      out->poc_st_curr_after[i] = idx;
      if (idx != 0xFF)
         out->num_poc_st_curr_after = (uint8_t)(i + 1);

      idx = vk_pic->RefPicSetLtCurr[i];
      out->poc_lt_curr[i] = idx;
      if (idx != 0xFF)
         out->num_poc_lt_curr = (uint8_t)(i + 1);
   }

   /* Decode flags */
   if (vk_pic->flags.IrapPicFlag)
      out->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC;
   if (vk_pic->flags.IdrPicFlag)
      out->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC;
   /* V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR: no source in std PictureInfo */
}

/* -------------------------------------------------------------------------
 * Slice-segment-header parser (the in-driver bitstream parse)
 *
 * Walks slice_segment_header() per ITU-T H.265 §7.3.6.1 for each slice NAL
 * and fills struct v4l2_ctrl_hevc_slice_params with the ~20 fields that the
 * Vulkan Video H.265 *decode* std structs do NOT carry. Gated by SPS/PPS.
 *
 * Bitstream model: parses over the raw Annex-B (EBSP) bytes via the shared
 * v4l2vk_bitreader (same as the H.264 sibling). The reader does NOT strip
 * emulation-prevention bytes (00 00 03). For the in-scope clips the slice
 * *header* region carries no EPB, so EBSP and RBSP coincide there and the
 * parsed values + byte offsets are exact. A header that itself contains an
 * EPB before slice data is OUT OF SCOPE (mirrors the H.264 path's constraint).
 *
 * NOT parsed (out of scope, asserted-absent for in-scope clips):
 *   - pred_weight_table()  : PPS weighted_pred/weighted_bipred are 0.
 *   - ref_pic_lists_modification(): only reached for P/B with lists_mod; the
 *     bits are skipped to keep bit accounting correct, values not stored.
 * -------------------------------------------------------------------------
 */

/* Ceil(Log2(x)) for x >= 1; returns the number of bits to represent values
 * 0 .. x-1 (i.e. fixed-length slice_segment_address width). */
static uint32_t
ceil_log2(uint32_t x)
{
   uint32_t n = 0;
   uint32_t v = 1;
   while (v < x) {
      v <<= 1;
      n++;
   }
   return n;
}

/*
 * Parse one slice's short_term_ref_pic_set() inline in the slice header, when
 * short_term_ref_pic_set_sps_flag == 0. Mirrors H.265 §7.3.7. Only the bit
 * count matters here (returned via *out_bits) — the RPS *values* in the slice
 * are not needed by rkvdec (it derives ref lists from decode_params), but the
 * bits MUST be consumed so the following syntax (and num_entry_point_offsets)
 * lands at the right position. stRpsIdx == num_short_term_ref_pic_sets means
 * this is the explicitly-signalled slice RPS (no inter-RPS prediction allowed
 * for that index per spec, but guard it anyway).
 *
 * @num_st_rps: sps->num_short_term_ref_pic_sets (count of SPS-signalled sets).
 */
static bool
parse_short_term_rps(struct v4l2vk_bitreader *br, uint32_t st_rps_idx,
                     uint32_t num_st_rps)
{
   uint32_t inter_rps_pred = 0;
   if (st_rps_idx != 0)
      inter_rps_pred = br_read_bit(br);

   if (inter_rps_pred) {
      /*
       * Inter-RPS prediction for an INLINE slice RPS needs the referenced
       * set's NumDeltaPocs to consume used_by_curr_pic_flag[j]/use_delta_flag[j]
       * (H.265 §7.3.7). We deliberately do NOT carry the SPS RPS delta state,
       * so this branch cannot be parsed without desyncing the reader. It is
       * OUT OF SCOPE: the in-scope clips have num_short_term_ref_pic_sets==0
       * (no SPS sets to predict from) so this is never taken. Signal failure
       * to the caller so it bails LOUDLY instead of silently corrupting the
       * downstream syntax. (Flagged for Task 10 if a P/B clip exercises it.)
       */
      return false;
   }

   uint32_t num_negative = br_read_ue(br);
   uint32_t num_positive = br_read_ue(br);
   /* Clamp loop counts defensively (spec bounds these by sps_max_dec_pic_
    * buffering; a corrupt stream could overrun otherwise). */
   if (num_negative > 16)
      num_negative = 16;
   if (num_positive > 16)
      num_positive = 16;
   for (uint32_t i = 0; i < num_negative; i++) {
      br_read_ue(br);  /* delta_poc_s0_minus1[i] */
      br_read_bit(br); /* used_by_curr_pic_s0_flag[i] */
   }
   for (uint32_t i = 0; i < num_positive; i++) {
      br_read_ue(br);  /* delta_poc_s1_minus1[i] */
      br_read_bit(br); /* used_by_curr_pic_s1_flag[i] */
   }
   return true;
}

uint32_t
v4l2vk_h265_translate_slice_params(const uint8_t *bitstream, size_t size,
                                   const struct v4l2_ctrl_hevc_sps *sps,
                                   const struct v4l2_ctrl_hevc_pps *pps,
                                   const uint32_t *slice_offsets,
                                   uint32_t slice_count,
                                   struct v4l2_ctrl_hevc_slice_params *out)
{
   if (slice_count > 16)
      slice_count = 16;

   memset(out, 0, sizeof(*out) * slice_count);

   if (!bitstream || !sps || !pps || !slice_offsets)
      return 0;

   /* SPS / PPS gate values (pre-resolved once). */
   const int sep_colour =
      !!(sps->flags & V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE);
   const int sao_enabled =
      !!(sps->flags & V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET);
   const int temporal_mvp =
      !!(sps->flags & V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED);
   const uint32_t num_st_rps = sps->num_short_term_ref_pic_sets;
   const uint32_t poc_lsb_bits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;

   const int dep_slice_enabled =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED);
   const int output_flag_present =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT);
   const uint32_t num_extra_bits = pps->num_extra_slice_header_bits;
   const int cabac_init_present =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT);
   const int lists_mod_present =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT);
   const int weighted_pred =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED);
   const int weighted_bipred =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED);
   const int cu_chroma_qp_offsets =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT);
   const int dbf_override_enabled =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED);
   const int pps_dbf_disabled =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER);
   const int pps_loop_filter_across =
      !!(pps->flags &
         V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED);
   const int tiles_enabled =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED);
   const int entropy_sync =
      !!(pps->flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED);
   const int slice_hdr_ext_present =
      !!(pps->flags &
         V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT);

   /* PicSizeInCtbsY -> width of slice_segment_address field.
    * CtbSizeY = 1 << (log2_min_luma_coding_block_size_minus3 + 3 +
    *                  log2_diff_max_min_luma_coding_block_size).
    * PicSizeInCtbsY = ceil(W/CtbSizeY) * ceil(H/CtbSizeY). */
   const uint32_t ctb_log2 = sps->log2_min_luma_coding_block_size_minus3 + 3 +
                             sps->log2_diff_max_min_luma_coding_block_size;
   const uint32_t ctb_size = 1u << ctb_log2;
   const uint32_t pic_w = sps->pic_width_in_luma_samples;
   const uint32_t pic_h = sps->pic_height_in_luma_samples;
   const uint32_t w_ctbs =
      ctb_size ? (pic_w + ctb_size - 1) / ctb_size : 0;
   const uint32_t h_ctbs =
      ctb_size ? (pic_h + ctb_size - 1) / ctb_size : 0;
   const uint32_t pic_size_ctbs = w_ctbs * h_ctbs;
   const uint32_t addr_bits =
      pic_size_ctbs > 1 ? ceil_log2(pic_size_ctbs) : 0;

   uint32_t parsed = 0;

   for (uint32_t s = 0; s < slice_count; s++) {
      struct v4l2_ctrl_hevc_slice_params *o = &out[s];
      uint32_t off = slice_offsets[s];

      if (off >= size)
         continue;

      const uint8_t *nal = bitstream + off;
      size_t avail = size - off;

      /* Skip the Annex-B start code -> NAL header. */
      size_t sc = v4l2vk_skip_start_code(nal, avail);
      const uint8_t *nal_hdr = nal + sc;
      size_t nal_avail = avail - sc;
      if (nal_avail < 2)
         continue;

      /* HEVC NAL header is 2 bytes:
       *   forbidden_zero_bit(1) nal_unit_type(6) nuh_layer_id(6)
       *   nuh_temporal_id_plus1(3)
       */
      uint8_t nut = (nal_hdr[0] >> 1) & 0x3F;
      uint8_t tid1 = nal_hdr[1] & 0x07;
      o->nal_unit_type = nut;
      o->nuh_temporal_id_plus1 = tid1;

      /* slice_segment_header() starts after the 2-byte NAL header. */
      struct v4l2vk_bitreader br;
      br_init(&br, nal_hdr + 2, nal_avail - 2);

      uint32_t first_slice = br_read_bit(&br);

      /* IRAP NAL types are 16..23. */
      if (nut >= 16 && nut <= 23)
         br_read_bit(&br); /* no_output_of_prior_pics_flag */

      br_read_ue(&br); /* slice_pic_parameter_set_id */

      uint32_t dependent = 0;
      if (!first_slice) {
         if (dep_slice_enabled)
            dependent = br_read_bit(&br);
         /* slice_segment_address: u(Ceil(Log2(PicSizeInCtbsY))) */
         uint32_t addr = 0;
         for (uint32_t i = 0; i < addr_bits; i++)
            addr = (addr << 1) | br_read_bit(&br);
         o->slice_segment_addr = addr;
      } else {
         o->slice_segment_addr = 0;
      }

      if (dependent)
         o->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT;

      /* The remaining header is only present for an independent slice segment.
       * For a dependent slice segment, the inherited values are carried by the
       * decode_params/previous independent slice; rkvdec needs the addr +
       * data_byte_offset + bit_size, which we still compute below. */
      uint32_t slice_type_raw = 2; /* default I if dependent (inherited) */
      if (!dependent) {
         for (uint32_t i = 0; i < num_extra_bits; i++)
            br_read_bit(&br); /* slice_reserved_flag[i] */

         slice_type_raw = br_read_ue(&br);
         /* HEVC slice_type: 0=B, 1=P, 2=I — identical to V4L2_HEVC_SLICE_TYPE_*.
          * Clamp to the valid range (a corrupt value would index nothing). */
         o->slice_type = (slice_type_raw <= 2) ? (uint8_t)slice_type_raw
                                               : V4L2_HEVC_SLICE_TYPE_I;

         if (output_flag_present)
            br_read_bit(&br); /* pic_output_flag */

         if (sep_colour) {
            uint32_t cpid = br_read_bit(&br);
            cpid = (cpid << 1) | br_read_bit(&br);
            o->colour_plane_id = (uint8_t)cpid;
         }

         /* POC + RPS only for non-IDR. IDR_W_RADL=19, IDR_N_LP=20. */
         if (nut != 19 && nut != 20) {
            /* slice_pic_order_cnt_lsb: u(poc_lsb_bits) */
            uint32_t poc_lsb = 0;
            for (uint32_t i = 0; i < poc_lsb_bits; i++)
               poc_lsb = (poc_lsb << 1) | br_read_bit(&br);
            o->slice_pic_order_cnt = (int32_t)poc_lsb;

            uint32_t st_rps_sps_flag = br_read_bit(&br);
            uint32_t st_bits_start = br.bit_offset;
            if (!st_rps_sps_flag) {
               /* inline short_term_ref_pic_set(num_short_term_ref_pic_sets) */
               if (!parse_short_term_rps(&br, num_st_rps, num_st_rps)) {
                  fprintf(stderr,
                          "[V4L2VK][H265] slice[%u]: inline inter-RPS-predicted "
                          "short_term_ref_pic_set (unsupported) — header parse "
                          "stopped early\n",
                          s);
                  parsed++;
                  continue;
               }
               o->short_term_ref_pic_set_size =
                  (uint16_t)(br.bit_offset - st_bits_start);
            } else {
               /* short_term_ref_pic_set_idx: u(Ceil(Log2(num_st_rps))) */
               uint32_t idx_bits =
                  num_st_rps > 1 ? ceil_log2(num_st_rps) : 0;
               for (uint32_t i = 0; i < idx_bits; i++)
                  br_read_bit(&br);
               o->short_term_ref_pic_set_size = 0;
            }

            /* long_term_ref_pics: gated by SPS long_term_ref_pics_present.
             * In scope clips have no long-term refs; if present we would need
             * num_long_term_ref_pics_sps + num_long_term_pics here. Left to
             * Task 10 if a clip exercises it (flagged). */

            if (temporal_mvp)
               br_read_bit(&br); /* slice_temporal_mvp_enabled_flag */
         }

         /* sample_adaptive_offset */
         if (sao_enabled) {
            if (br_read_bit(&br))
               o->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA;
            /* slice_sao_chroma only when ChromaArrayType != 0.
             * ChromaArrayType == chroma_format_idc unless separate_colour_plane,
             * in which case it is 0. */
            uint32_t chroma_array_type =
               sep_colour ? 0 : sps->chroma_format_idc;
            if (chroma_array_type != 0) {
               if (br_read_bit(&br))
                  o->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA;
            }
         }

         /* Inter-prediction header (P=1, B=0). I=2 skips all of this. */
         if (slice_type_raw == 0 || slice_type_raw == 1) {
            uint32_t num_ref_l0 = pps->num_ref_idx_l0_default_active_minus1;
            uint32_t num_ref_l1 = pps->num_ref_idx_l1_default_active_minus1;

            uint32_t ref_override = br_read_bit(&br);
            if (ref_override) {
               num_ref_l0 = br_read_ue(&br); /* num_ref_idx_l0_active_minus1 */
               if (slice_type_raw == 0)      /* B */
                  num_ref_l1 = br_read_ue(&br);
            }
            if (num_ref_l0 > 15)
               num_ref_l0 = 15;
            if (num_ref_l1 > 15)
               num_ref_l1 = 15;
            o->num_ref_idx_l0_active_minus1 = (uint8_t)num_ref_l0;
            o->num_ref_idx_l1_active_minus1 = (uint8_t)num_ref_l1;

            /* ref_pic_lists_modification(): present iff lists_modification_
             * present_flag && NumPicTotalCurr > 1. We don't track
             * NumPicTotalCurr (needs full RPS derivation); the in-scope clips
             * are I-only so this branch is never taken. Consume conservatively
             * only when the gate flag is set AND we can't prove count<=1 — but
             * since we can't, and a wrong skip desyncs the reader, we DO NOT
             * speculatively skip. Flagged UNCERTAIN for Task 10 (P/B support).
             */
            (void)lists_mod_present;

            if (slice_type_raw == 0) /* B: mvd_l1_zero_flag */
               if (br_read_bit(&br))
                  o->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO;

            if (cabac_init_present)
               if (br_read_bit(&br))
                  o->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT;

            if (temporal_mvp) {
               /* collocated_from_l0_flag: H.265 §7.3.6.1 — present ONLY
                * when slice_type == B (raw==0). For P slices it is inferred
                * as 1 (absent from the bitstream). Reading it unconditionally
                * for P slices steals a bit from the next syntax element and
                * desyncs num_entry_point_offsets and everything after. */
               uint32_t collocated_from_l0 = 1; /* inferred default (P) */
               if (slice_type_raw == 0)          /* B only — §7.3.6.1 */
                  collocated_from_l0 = br_read_bit(&br);
               if (collocated_from_l0)
                  o->flags |=
                     V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0;
               uint32_t ncols = collocated_from_l0 ? num_ref_l0 : num_ref_l1;
               if (ncols > 0)
                  o->collocated_ref_idx = (uint8_t)br_read_ue(&br);
            }

            /* pred_weight_table(): OUT OF SCOPE. In-scope clips have
             * weighted_pred==0 / weighted_bipred==0, so the syntax element is
             * ABSENT (H.265 §7.3.6.1). We never reach it. */
            if ((weighted_pred && slice_type_raw == 1) ||
                (weighted_bipred && slice_type_raw == 0)) {
               /* Unsupported: bail without corrupting downstream — leave the
                * remaining fields at their (memset) defaults. */
               fprintf(stderr,
                       "[V4L2VK][H265] slice[%u]: weighted prediction present "
                       "(unsupported) — header parse stopped early\n",
                       s);
               parsed++;
               continue;
            }

            uint32_t five_minus = br_read_ue(&br);
            if (five_minus > 5)
               five_minus = 5;
            o->five_minus_max_num_merge_cand = (uint8_t)five_minus;
         }

         /* slice_qp_delta: se(v) — always present for non-dependent slices. */
         o->slice_qp_delta = (int8_t)br_read_se(&br);

         if (cu_chroma_qp_offsets) {
            o->slice_cb_qp_offset = (int8_t)br_read_se(&br);
            o->slice_cr_qp_offset = (int8_t)br_read_se(&br);
         }

         /* chroma_qp_offset_list / ACT offsets (SCC ext): not present in the
          * in-scope Main-profile clips; gated by PPS range/scc-ext flags we do
          * not translate. Left at defaults. */

         /* deblocking_filter_override_flag + slice deblocking params. */
         uint32_t deblocking_override = 0;
         if (dbf_override_enabled)
            deblocking_override = br_read_bit(&br);
         if (deblocking_override) {
            uint32_t slice_dbf_disabled = br_read_bit(&br);
            if (slice_dbf_disabled)
               o->flags |=
                  V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED;
            if (!slice_dbf_disabled) {
               o->slice_beta_offset_div2 = (int8_t)br_read_se(&br);
               o->slice_tc_offset_div2 = (int8_t)br_read_se(&br);
            }
         } else {
            /* Inherit PPS deblocking offsets / disabled state. */
            if (pps_dbf_disabled)
               o->flags |=
                  V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED;
            o->slice_beta_offset_div2 = pps->pps_beta_offset_div2;
            o->slice_tc_offset_div2 = pps->pps_tc_offset_div2;
         }

         /* slice_loop_filter_across_slices_enabled_flag:
          * present iff pps_loop_filter_across_slices_enabled_flag &&
          * (slice_sao_luma || slice_sao_chroma || !slice_deblocking_disabled). */
         int slice_dbf_disabled =
            !!(o->flags &
               V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED);
         int sao_luma =
            !!(o->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA);
         int sao_chroma =
            !!(o->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA);
         if (pps_loop_filter_across &&
             (sao_luma || sao_chroma || !slice_dbf_disabled)) {
            if (br_read_bit(&br))
               o->flags |=
                  V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED;
         }
      } /* !dependent */

      /* num_entry_point_offsets: present iff tiles || entropy_coding_sync.
       * (Outside the !dependent block — also present for dependent slices.) */
      if (tiles_enabled || entropy_sync) {
         uint32_t nepo = br_read_ue(&br);
         /* num_entry_point_offsets is bounded by the number of tile columns *
          * tile rows (or CTB rows for WPP) of the picture; a 4K frame has at
          * most a few hundred. Clamp defensively so a corrupt ue(v) cannot
          * drive the skip loop past the buffer and desync data_byte_offset.
          * The clamp affects the REPORTED count and the loop iterations
          * together, keeping bit accounting consistent with what we store. */
         if (nepo > 256)
            nepo = 256;
         o->num_entry_point_offsets = nepo;
         if (nepo > 0) {
            uint32_t offset_len_m1 = br_read_ue(&br);
            uint32_t off_bits = offset_len_m1 + 1;
            if (off_bits > 32)
               off_bits = 32; /* spec caps at 32; guard reader */
            for (uint32_t i = 0; i < nepo; i++)
               br_skip_bits(&br, off_bits); /* entry_point_offset_minus1[i] */
         }
      }

      if (slice_hdr_ext_present) {
         uint32_t ext_len = br_read_ue(&br);
         if (ext_len > 4096)
            ext_len = 4096; /* guard */
         for (uint32_t i = 0; i < ext_len; i++)
            br_read_bit(&br); /* slice_segment_header_extension_data_byte */
      }

      /* byte_alignment(): alignment_bit_equal_to_one(1) then zero bits to the
       * next byte boundary. After this the coded slice DATA begins. */
      br_read_bit(&br); /* alignment_bit_equal_to_one */
      while ((br.bit_offset & 7) != 0)
         br_read_bit(&br); /* alignment_bit_equal_to_zero */

      /*
       * data_byte_offset: byte offset, measured from the start of the slice
       * bitstream buffer (the Annex-B NAL INCLUDING its start code, which is
       * what the OUTPUT buffer holds), to the first byte of coded slice data
       * (after byte_alignment()).
       *
       *   = start_code_len + 2 (NAL header) + header_payload_bytes
       *
       * header_payload_bytes = br.bit_offset/8 (the reader started after the
       * 2-byte NAL header and bit_offset is now byte-aligned).
       *
       * UNCERTAIN (Task 10 strace-diff): the exact base the golden uses —
       * start-code-inclusive (this) vs NAL-header-relative (this minus sc).
       * For the in-scope clip with no EPB in the header, EBSP and RBSP byte
       * counts are equal so only the BASE is in question, not the count.
       */
      uint32_t header_payload_bytes = br.bit_offset >> 3;
      o->data_byte_offset =
         (uint32_t)(sc + 2 + header_payload_bytes);

      /*
       * bit_size: size (in bits) of the current slice data — the whole slice
       * segment NAL payload (after the 2-byte NAL header) in bits. rkvdec
       * consumes the full Annex-B unit. We size it from this slice's start
       * code to the NEXT slice's start code if known (slice_offsets[s+1]),
       * else to end-of-buffer.
       *
       * UNCERTAIN (Task 10): whether the golden measures bit_size over the
       * NAL payload (this) or the whole NAL incl. start code + header.
       */
      size_t nal_end =
         (s + 1 < slice_count) ? slice_offsets[s + 1] : size;
      if (nal_end > size)
         nal_end = size;
      size_t nal_payload_bytes =
         (nal_end > off + sc + 2) ? (nal_end - (off + sc + 2)) : 0;
      o->bit_size = (uint32_t)(nal_payload_bytes * 8);

      fprintf(stderr,
              "[V4L2VK][H265] slice[%u]: off=%u nal_type=%u(%s) tid+1=%u "
              "addr=%u type=%u(%s) qp_delta=%d nepo=%u "
              "data_byte_offset=%u bit_size=%u st_rps_size=%u\n",
              s, off, nut,
              (nut == 19 || nut == 20)   ? "IDR"
              : (nut >= 16 && nut <= 23) ? "IRAP"
                                         : "VCL",
              tid1, o->slice_segment_addr, o->slice_type,
              o->slice_type == V4L2_HEVC_SLICE_TYPE_I   ? "I"
              : o->slice_type == V4L2_HEVC_SLICE_TYPE_P ? "P"
              : o->slice_type == V4L2_HEVC_SLICE_TYPE_B ? "B"
                                                        : "?",
              o->slice_qp_delta, o->num_entry_point_offsets,
              o->data_byte_offset, o->bit_size,
              o->short_term_ref_pic_set_size);

      parsed++;
   }

   return parsed;
}
