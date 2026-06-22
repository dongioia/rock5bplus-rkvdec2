/* SPDX-License-Identifier: MIT */
/*
 * Task 7 unit harness: HEVC slice-segment-header parser byte/field check.
 *
 * Feeds the first VCL slice of an Annex-B .h265 clip into
 * v4l2vk_h265_translate_slice_params() and asserts the I-slice invariants.
 *
 * The clip hevc_case1.h265 is a known IDR_N_LP (nal_unit_type=20) at the
 * first VCL NAL: an I slice, first_slice_segment_in_pic_flag=1.
 *
 * compile (in build container):
 *   cc test_hevc_slice_parse.c v4l2vk_v4l2_hevc.c \
 *      -I/work/mesa-sree/mesa/include -I/work/mesa-sree/mesa/src \
 *      -I/work/mesa-sree/mesa/src/vulkan-v4l2 -o /tmp/t_hevc_slice
 *   /tmp/t_hevc_slice hevc_case1.h265
 *
 * Drives Task 7 red->green. With an empty/stub parser the asserts fail
 * (slice_type != I, or 0 fields). With the real parser it prints:
 *   SLICE0 type=2 (I) addr=0 data_byte_offset=NN num_entry=11 OK
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "v4l2vk_v4l2_hevc.h"

/*
 * Minimal Annex-B SPS/PPS parser, INDEPENDENT of the code under test.
 * It exists only so the harness can build the v4l2_ctrl_hevc_sps/pps gate
 * context that the slice parser consumes (same gates the real ICD fills from
 * the Vulkan std structs). Kept deliberately tiny and only covers what
 * hevc_case1.h265 actually uses (8-bit 4:2:0, no scaling list, no PCM).
 */
struct tbr {
   const uint8_t *b;
   size_t n;
   size_t p; /* bit pos */
};
static uint32_t tbr_u(struct tbr *r, uint32_t k)
{
   uint32_t v = 0;
   for (uint32_t i = 0; i < k; i++) {
      size_t byte = r->p >> 3, bit = 7 - (r->p & 7);
      uint32_t b = (byte < r->n) ? ((r->b[byte] >> bit) & 1) : 0;
      v = (v << 1) | b;
      r->p++;
   }
   return v;
}
static uint32_t tbr_ue(struct tbr *r)
{
   uint32_t z = 0;
   while ((r->p >> 3) < r->n && tbr_u(r, 1) == 0)
      z++;
   uint32_t info = 0;
   for (uint32_t i = 0; i < z; i++)
      info = (info << 1) | tbr_u(r, 1);
   return (1u << z) - 1 + info;
}
static int32_t tbr_se(struct tbr *r)
{
   uint32_t v = tbr_ue(r);
   return (v & 1) ? (int32_t)((v + 1) >> 1) : -(int32_t)(v >> 1);
}

/* EBSP -> RBSP (strip 00 00 03 emulation-prevention bytes) */
static size_t ebsp_to_rbsp(const uint8_t *in, size_t n, uint8_t *out)
{
   size_t o = 0, z = 0;
   for (size_t i = 0; i < n; i++) {
      if (z >= 2 && in[i] == 3) {
         z = 0;
         continue;
      }
      out[o++] = in[i];
      z = (in[i] == 0) ? z + 1 : 0;
   }
   return o;
}

/* Find the byte range [hdr, end) of NAL #idx_type (after the start code). */
static int find_nal(const uint8_t *d, size_t n, uint8_t want_type_lo,
                    uint8_t want_type_hi, size_t *hdr_out, size_t *end_out)
{
   size_t i = 0;
   while (i + 3 < n) {
      if (d[i] == 0 && d[i + 1] == 0 &&
          (d[i + 2] == 1 || (d[i + 2] == 0 && i + 3 < n && d[i + 3] == 1))) {
         size_t sc = (d[i + 2] == 1) ? 3 : 4;
         size_t hdr = i + sc;
         if (hdr + 1 >= n)
            break;
         uint8_t nut = (d[hdr] >> 1) & 0x3F;
         /* next start code */
         size_t j = hdr + 2;
         while (j + 3 < n &&
                !(d[j] == 0 && d[j + 1] == 0 &&
                  (d[j + 2] == 1 || (d[j + 2] == 0 && d[j + 3] == 1))))
            j++;
         if (j + 3 >= n)
            j = n;
         if (nut >= want_type_lo && nut <= want_type_hi) {
            *hdr_out = hdr;
            *end_out = j;
            return 1;
         }
         i = j;
      } else {
         i++;
      }
   }
   return 0;
}

static void parse_sps(const uint8_t *d, size_t n, struct v4l2_ctrl_hevc_sps *sps)
{
   memset(sps, 0, sizeof(*sps));
   size_t hdr, end;
   if (!find_nal(d, n, 33, 33, &hdr, &end)) {
      fprintf(stderr, "harness: SPS not found\n");
      exit(2);
   }
   uint8_t rb[4096];
   size_t rn = ebsp_to_rbsp(d + hdr + 2, end - (hdr + 2), rb);
   struct tbr r = {rb, rn, 0};
   tbr_u(&r, 4); /* sps_video_parameter_set_id */
   uint32_t max_sub = tbr_u(&r, 3); /* sps_max_sub_layers_minus1 */
   tbr_u(&r, 1); /* sps_temporal_id_nesting_flag */
   /* profile_tier_level(1, max_sub): general 88 bits */
   tbr_u(&r, 2);
   tbr_u(&r, 1);
   tbr_u(&r, 5);
   tbr_u(&r, 32);
   tbr_u(&r, 4); /* progressive/interlaced/non_packed/frame_only */
   tbr_u(&r, 44); /* general_reserved_zero_43bits + ... */
   tbr_u(&r, 8); /* general_level_idc */
   /* (max_sub==0 -> no sub-layer ptl) */
   tbr_ue(&r); /* sps_seq_parameter_set_id */
   uint32_t chroma = tbr_ue(&r);
   sps->chroma_format_idc = (uint8_t)chroma;
   if (chroma == 3)
      tbr_u(&r, 1);
   tbr_ue(&r); /* pic_width */
   tbr_ue(&r); /* pic_height */
   uint32_t conf = tbr_u(&r, 1);
   if (conf) {
      tbr_ue(&r);
      tbr_ue(&r);
      tbr_ue(&r);
      tbr_ue(&r);
   }
   tbr_ue(&r); /* bit_depth_luma_minus8 */
   tbr_ue(&r); /* bit_depth_chroma_minus8 */
   uint32_t log2_poc = tbr_ue(&r);
   sps->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)log2_poc;
   uint32_t sub_layer_ordering = tbr_u(&r, 1);
   uint32_t start = sub_layer_ordering ? 0 : max_sub;
   for (uint32_t i = start; i <= max_sub; i++) {
      tbr_ue(&r);
      tbr_ue(&r);
      tbr_ue(&r);
   }
   tbr_ue(&r); /* log2_min_luma_coding_block_size_minus3 */
   tbr_ue(&r); /* log2_diff_max_min_luma_coding_block_size */
   tbr_ue(&r); /* log2_min_luma_transform_block_size_minus2 */
   tbr_ue(&r); /* log2_diff_max_min_luma_transform_block_size */
   tbr_ue(&r); /* max_transform_hierarchy_depth_inter */
   tbr_ue(&r); /* max_transform_hierarchy_depth_intra */
   uint32_t scaling = tbr_u(&r, 1);
   if (scaling) {
      fprintf(stderr, "harness: clip has scaling_list (unsupported by harness)\n");
      exit(2);
   }
   tbr_u(&r, 1); /* amp_enabled_flag */
   uint32_t sao = tbr_u(&r, 1);
   if (sao)
      sps->flags |= V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET;
   uint32_t pcm = tbr_u(&r, 1);
   if (pcm) {
      tbr_u(&r, 4);
      tbr_u(&r, 4);
      tbr_ue(&r);
      tbr_ue(&r);
      tbr_u(&r, 1);
   }
   sps->num_short_term_ref_pic_sets = (uint8_t)tbr_ue(&r);
   /* (separate_colour_plane_flag not used by hevc_case1 -> leave flag clear) */
}

static void parse_pps(const uint8_t *d, size_t n, struct v4l2_ctrl_hevc_pps *pps)
{
   memset(pps, 0, sizeof(*pps));
   size_t hdr, end;
   if (!find_nal(d, n, 34, 34, &hdr, &end)) {
      fprintf(stderr, "harness: PPS not found\n");
      exit(2);
   }
   uint8_t rb[4096];
   size_t rn = ebsp_to_rbsp(d + hdr + 2, end - (hdr + 2), rb);
   struct tbr r = {rb, rn, 0};
   tbr_ue(&r); /* pps_pic_parameter_set_id */
   tbr_ue(&r); /* pps_seq_parameter_set_id */
   uint32_t dep = tbr_u(&r, 1);
   if (dep)
      pps->flags |= V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED;
   uint32_t out_flag = tbr_u(&r, 1);
   if (out_flag)
      pps->flags |= V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT;
   pps->num_extra_slice_header_bits = (uint8_t)tbr_u(&r, 3);
   tbr_u(&r, 1); /* sign_data_hiding_enabled_flag */
   uint32_t cabac_init = tbr_u(&r, 1);
   if (cabac_init)
      pps->flags |= V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT;
   pps->num_ref_idx_l0_default_active_minus1 = (uint8_t)tbr_ue(&r);
   pps->num_ref_idx_l1_default_active_minus1 = (uint8_t)tbr_ue(&r);
   pps->init_qp_minus26 = (int8_t)tbr_se(&r);
   tbr_u(&r, 1); /* constrained_intra_pred_flag */
   tbr_u(&r, 1); /* transform_skip_enabled_flag */
   uint32_t cu_qp = tbr_u(&r, 1);
   if (cu_qp)
      tbr_ue(&r); /* diff_cu_qp_delta_depth */
   tbr_se(&r); /* pps_cb_qp_offset */
   tbr_se(&r); /* pps_cr_qp_offset */
   uint32_t slice_chroma = tbr_u(&r, 1);
   if (slice_chroma)
      pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT;
   uint32_t wp = tbr_u(&r, 1);
   if (wp)
      pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED;
   uint32_t wbp = tbr_u(&r, 1);
   if (wbp)
      pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
   tbr_u(&r, 1); /* transquant_bypass_enabled_flag */
   uint32_t tiles = tbr_u(&r, 1);
   if (tiles)
      pps->flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
   uint32_t esync = tbr_u(&r, 1);
   if (esync)
      pps->flags |= V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
   if (tiles) {
      uint32_t nc = tbr_ue(&r); /* num_tile_columns_minus1 */
      uint32_t nr = tbr_ue(&r); /* num_tile_rows_minus1 */
      uint32_t uniform = tbr_u(&r, 1);
      if (!uniform) {
         for (uint32_t i = 0; i < nc; i++)
            tbr_ue(&r);
         for (uint32_t i = 0; i < nr; i++)
            tbr_ue(&r);
      }
      tbr_u(&r, 1); /* loop_filter_across_tiles_enabled_flag */
   }
   uint32_t lf_across = tbr_u(&r, 1); /* pps_loop_filter_across_slices_enabled_flag */
   if (lf_across)
      pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
   uint32_t dbf_ctrl = tbr_u(&r, 1);
   if (dbf_ctrl)
      pps->flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
   if (dbf_ctrl) {
      uint32_t ovr = tbr_u(&r, 1);
      if (ovr)
         pps->flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED;
      uint32_t disabled = tbr_u(&r, 1);
      if (disabled)
         pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
      if (!disabled) {
         pps->pps_beta_offset_div2 = (int8_t)tbr_se(&r);
         pps->pps_tc_offset_div2 = (int8_t)tbr_se(&r);
      }
   }
}

int main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s <clip.h265>\n", argv[0]);
      return 2;
   }
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      fprintf(stderr, "cannot open %s\n", argv[1]);
      return 2;
   }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint8_t *data = malloc(sz);
   if (fread(data, 1, sz, f) != (size_t)sz) {
      fprintf(stderr, "short read\n");
      return 2;
   }
   fclose(f);

   struct v4l2_ctrl_hevc_sps sps;
   struct v4l2_ctrl_hevc_pps pps;
   parse_sps(data, sz, &sps);
   parse_pps(data, sz, &pps);
   fprintf(stderr,
           "harness: SPS sao=%d num_st_rps=%u chroma=%u log2poc=%u | "
           "PPS dep=%d extra=%u tiles=%d esync=%d wp=%d wbp=%d\n",
           !!(sps.flags & V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET),
           sps.num_short_term_ref_pic_sets, sps.chroma_format_idc,
           sps.log2_max_pic_order_cnt_lsb_minus4,
           !!(pps.flags & V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED),
           pps.num_extra_slice_header_bits,
           !!(pps.flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED),
           !!(pps.flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED),
           !!(pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED),
           !!(pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED));

   /* Locate the first VCL slice NAL (nal_unit_type 0..31), build a
    * 1-entry slice_offsets pointing at its start code. */
   size_t i = 0, slice_sc_off = 0;
   int found = 0;
   while (i + 3 < (size_t)sz) {
      if (data[i] == 0 && data[i + 1] == 0 &&
          (data[i + 2] == 1 ||
           (data[i + 2] == 0 && i + 3 < (size_t)sz && data[i + 3] == 1))) {
         size_t sc = (data[i + 2] == 1) ? 3 : 4;
         size_t hdr = i + sc;
         uint8_t nut = (data[hdr] >> 1) & 0x3F;
         if (nut <= 31) {
            slice_sc_off = i; /* offset of the start code */
            found = 1;
            break;
         }
         i = hdr + 2;
      } else {
         i++;
      }
   }
   if (!found) {
      fprintf(stderr, "harness: no VCL slice found\n");
      return 2;
   }
   fprintf(stderr, "harness: first VCL slice start-code at offset=%zu\n",
           slice_sc_off);

   uint32_t slice_offsets[1] = {(uint32_t)slice_sc_off};
   struct v4l2_ctrl_hevc_slice_params sp[1];
   memset(sp, 0, sizeof(sp));

   uint32_t nslices = v4l2vk_h265_translate_slice_params(
      data, (size_t)sz, &sps, &pps, slice_offsets, 1, sp);

   printf("SLICE0 type=%u (%s) addr=%u data_byte_offset=%u num_entry=%u "
          "nal_type=%u tid1=%u qp_delta=%d bit_size=%u\n",
          sp[0].slice_type,
          sp[0].slice_type == V4L2_HEVC_SLICE_TYPE_I   ? "I"
          : sp[0].slice_type == V4L2_HEVC_SLICE_TYPE_P ? "P"
          : sp[0].slice_type == V4L2_HEVC_SLICE_TYPE_B ? "B"
                                                       : "?",
          sp[0].slice_segment_addr, sp[0].data_byte_offset,
          sp[0].num_entry_point_offsets, sp[0].nal_unit_type,
          sp[0].nuh_temporal_id_plus1, sp[0].slice_qp_delta, sp[0].bit_size);

   /* --- Assertions (the red->green gate) --- */
   int ok = 1;
   if (nslices != 1) {
      fprintf(stderr, "FAIL: expected 1 slice parsed, got %u\n", nslices);
      ok = 0;
   }
   if (sp[0].slice_type != V4L2_HEVC_SLICE_TYPE_I) {
      fprintf(stderr, "FAIL: slice_type expected I(2), got %u\n",
              sp[0].slice_type);
      ok = 0;
   }
   if (sp[0].slice_segment_addr != 0) {
      fprintf(stderr, "FAIL: slice_segment_addr expected 0, got %u\n",
              sp[0].slice_segment_addr);
      ok = 0;
   }
   if (sp[0].data_byte_offset == 0) {
      fprintf(stderr, "FAIL: data_byte_offset expected >0, got 0\n");
      ok = 0;
   }
   if (sp[0].nal_unit_type != 20) {
      fprintf(stderr, "FAIL: nal_unit_type expected 20 (IDR_N_LP), got %u\n",
              sp[0].nal_unit_type);
      ok = 0;
   }

   if (!ok) {
      printf("RESULT: FAIL\n");
      return 1;
   }
   printf("RESULT: PASS\n");
   return 0;
}
