/* SPDX-License-Identifier: MIT */
/*
 * HEVC V4L2 stateless translate — SPS/PPS/scaling struct->struct translators.
 *
 * Task 5: level_idc_to_raw + translate_sps/pps/scaling.
 * Per-field byte correctness validated in Task 10 via strace-diff against
 * golden v4l2slh265dec trace.
 *
 * Field mapping reference:
 *   Vulkan side: mesa/include/vk_video/vulkan_video_codec_h265std.h
 *   V4L2 side:   /usr/include/linux/v4l2-controls.h (kernel 7.x)
 */
#include "v4l2vk_v4l2_hevc.h"

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
