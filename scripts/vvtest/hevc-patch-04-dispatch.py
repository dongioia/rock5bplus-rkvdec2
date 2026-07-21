#!/usr/bin/env python3
"""hevc-patch-04-dispatch.py: HEVC session/params/decode dispatch wiring.

Additive changes across four volume-only files:

1. v4l2vk_dpb.h
   - Extend v4l2vk_dpb_entry with int32_t hevc_pic_order_cnt_val.

2. v4l2vk_vk_device.h
   - Extend v4l2vk_recorded_job with HEVC fields:
     h265_sps, h265_pps, h265_scaling, h265_lt_rps_sps, h265_dec_pic_buf_mgr,
     h265_st_ref_pic_set[64], h265_picture, h265_valid.
   - Add HEVC includes.

3. v4l2vk_vk_video.c
   A. Add H265 decode + codec includes.
   B. CreateVideoSessionParametersKHR: HEVC branch reads H265 SPS and calls
      new video_session_init_v4l2_hevc() helper.
   C. CmdDecodeVideoKHR: HEVC branch finds VkVideoDecodeH265PictureInfoKHR,
      calls vk_video_get_h265_parameters, deep-copies std structs,
      populates slice offsets, sets hevc_pic_order_cnt_val on DPB entries.
   D. New helper video_session_init_v4l2_hevc() that uses
      set_output_format_codec(H265) + set_init_paramset(H265, &init_params).

4. v4l2vk_vk_device.c
   A. Add HEVC includes.
   B. QueueSubmit job loop: HEVC arm alongside H264 arm.

Idempotent: guarded by unique sentinel per file.
Anchor assertions: every replaced string is asserted unique before writing.

Run inside the build container (hevc-build.sh handles invocation):
  python3 /vv/hevc-patch-04-dispatch.py /work/mesa-sree/mesa/src/vulkan-v4l2
"""
import os
import sys

DIR = sys.argv[1]
pending = []  # (abs_path, fname, new_content)


def patch_file(fname, guard, edits):
    """Apply edits sequentially; skip if guard already present (idempotent)."""
    p = os.path.join(DIR, fname)
    s = open(p, encoding="utf-8").read()
    if guard in s:
        print(f"{fname}: already patched [{guard[:60]}], skipping")
        return
    for idx, (a, b) in enumerate(edits):
        assert a in s, (
            f"{fname}: edit[{idx}] ANCHOR NOT FOUND:\n  {repr(a[:140])}"
        )
        assert s.count(a) == 1, (
            f"{fname}: edit[{idx}] ANCHOR NOT UNIQUE ({s.count(a)}x):\n  {repr(a[:140])}"
        )
        s = s.replace(a, b, 1)
    pending.append((p, fname, s))
    print(f"{fname}: staged ({len(edits)} edit(s))")


# ===========================================================================
# 1. v4l2vk_dpb.h — extend DPB entry with HEVC POC field
# ===========================================================================
patch_file("v4l2vk_dpb.h", "hevc_pic_order_cnt_val", [
    (
        "struct v4l2vk_dpb_entry {\n"
        "   VkVideoPictureResourceInfoKHR resource;\n"
        "   StdVideoDecodeH264ReferenceInfo ref;\n"
        "   uint8_t slot_index;\n"
        "   uint8_t valid;\n"
        "   uint64_t reference_ts; /* V4L2 timestamp: slot_index * 1000 */\n"
        "};",
        "struct v4l2vk_dpb_entry {\n"
        "   VkVideoPictureResourceInfoKHR resource;\n"
        "   StdVideoDecodeH264ReferenceInfo ref;    /* H264 reference info (field PicOrderCnt[]) */\n"
        "   int32_t hevc_pic_order_cnt_val;         /* hevc-patch-04-dispatch: HEVC PicOrderCntVal */\n"
        "   uint8_t slot_index;\n"
        "   uint8_t valid;\n"
        "   uint64_t reference_ts; /* V4L2 timestamp: slot_index * 1000 */\n"
        "};",
    ),
])

# ===========================================================================
# 2. v4l2vk_vk_device.h — extend recorded_job with HEVC fields
# ===========================================================================
patch_file("v4l2vk_vk_device.h", "hevc-patch-04-dispatch", [
    # 2A: Add HEVC includes after the existing H264 decode include in vk_video.h
    # The H264 include is in vk_video.c; here we just add H265 std include to device.h
    # which includes dpb.h already. Add the H265 std header next to dpb.h.
    (
        '#include "v4l2vk_dpb.h"\n'
        '#include "vk_device.h"',
        '#include "v4l2vk_dpb.h"\n'
        '#include <vk_video/vulkan_video_codec_h265std.h>        /* hevc-patch-04-dispatch */\n'
        '#include <vk_video/vulkan_video_codec_h265std_decode.h> /* hevc-patch-04-dispatch */\n'
        '#include "vk_device.h"',
    ),
    # 2B: Extend v4l2vk_recorded_job with HEVC fields after the H264 fields
    (
        "   StdVideoH264SequenceParameterSet sps;\n"
        "   StdVideoH264PictureParameterSet pps;\n"
        "   StdVideoH264ScalingLists scaling;\n"
        "   StdVideoDecodeH264PictureInfo picture;\n"
        "   bool h264_valid;",
        "   StdVideoH264SequenceParameterSet sps;\n"
        "   StdVideoH264PictureParameterSet pps;\n"
        "   StdVideoH264ScalingLists scaling;\n"
        "   StdVideoDecodeH264PictureInfo picture;\n"
        "   bool h264_valid;\n"
        "\n"
        "   /* hevc-patch-04-dispatch: HEVC decode fields */\n"
        "   StdVideoH265SequenceParameterSet  h265_sps;\n"
        "   StdVideoH265PictureParameterSet   h265_pps;\n"
        "   StdVideoH265ScalingLists          h265_scaling;\n"
        "   StdVideoH265LongTermRefPicsSps    h265_lt_rps_sps;\n"
        "   StdVideoH265DecPicBufMgr          h265_dec_pic_buf_mgr;\n"
        "   StdVideoH265ShortTermRefPicSet    h265_st_ref_pic_set[64];\n"
        "   StdVideoDecodeH265PictureInfo     h265_picture;\n"
        "   bool h265_valid;",
    ),
])

# ===========================================================================
# 3. v4l2vk_vk_video.c — HEVC branches + new init helper
# ===========================================================================
patch_file("v4l2vk_vk_video.c", "hevc-patch-04-dispatch", [
    # --- 3A: Add H265 decode + v4l2vk_codec includes ---
    (
        "#include <vk_video/vulkan_video_codec_h264std_decode.h>\n"
        "#include <vk_video/vulkan_video_codecs_common.h>",
        "#include <vk_video/vulkan_video_codec_h264std_decode.h>\n"
        "#include <vk_video/vulkan_video_codec_h265std_decode.h> /* hevc-patch-04-dispatch */\n"
        "#include <vk_video/vulkan_video_codecs_common.h>\n"
        "#include \"v4l2vk_codec.h\"              /* hevc-patch-04-dispatch: V4L2VK_CODEC_* */\n"
        "#include \"v4l2vk_v4l2_hevc.h\"          /* hevc-patch-04-dispatch: translators + frame_params */",
    ),
    # --- 3B: CreateVideoSessionParametersKHR — HEVC branch after H264 block ---
    (
        "      if (v4l2vk_video_session_init_v4l2(sess, dev, w, h, sps, pps) < 0) {\n"
        "         fprintf(stderr,\n"
        "                 \"[V4L2VK][ERR] V4L2 init failed for %ux%u\\n\", w, h);\n"
        "         /* Non-fatal — QueueSubmit2 will try lazy init from job SPS */\n"
        "      }\n"
        "   }\n"
        "\n"
        "   return VK_SUCCESS;\n"
        "}\n"
        "\n"
        "VKAPI_ATTR void VKAPI_CALL\n"
        "v4l2vk_DestroyVideoSessionParametersKHR(",
        "      if (v4l2vk_video_session_init_v4l2(sess, dev, w, h, sps, pps) < 0) {\n"
        "         fprintf(stderr,\n"
        "                 \"[V4L2VK][ERR] V4L2 init failed for %ux%u\\n\", w, h);\n"
        "         /* Non-fatal — QueueSubmit2 will try lazy init from job SPS */\n"
        "      }\n"
        "   }\n"
        "\n"
        "   /* hevc-patch-04-dispatch: HEVC branch — read SPS from H265 session params */\n"
        "   const VkVideoDecodeH265SessionParametersCreateInfoKHR *h265_ci =\n"
        "      (const VkVideoDecodeH265SessionParametersCreateInfoKHR *)\n"
        "         v4l2vk_find_in_pnext(\n"
        "            pCreateInfo->pNext,\n"
        "            VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR);\n"
        "\n"
        "   if (h265_ci && h265_ci->pParametersAddInfo &&\n"
        "       h265_ci->pParametersAddInfo->stdSPSCount > 0 &&\n"
        "       h265_ci->pParametersAddInfo->pStdSPSs) {\n"
        "      const StdVideoH265SequenceParameterSet *h265sps =\n"
        "         &h265_ci->pParametersAddInfo->pStdSPSs[0];\n"
        "\n"
        "      uint32_t w = (uint32_t)h265sps->pic_width_in_luma_samples;\n"
        "      uint32_t h = (uint32_t)h265sps->pic_height_in_luma_samples;\n"
        "\n"
        "      fprintf(stderr,\n"
        "              \"[V4L2VK] HEVC SPS resolution: %ux%u (session maxCodedExtent: %ux%u)\\n\",\n"
        "              w, h, sess->coded_extent.width, sess->coded_extent.height);\n"
        "\n"
        "      if (v4l2vk_video_session_init_v4l2_hevc(sess, dev, w, h, h265sps) < 0) {\n"
        "         fprintf(stderr,\n"
        "                 \"[V4L2VK][ERR] HEVC V4L2 init failed for %ux%u\\n\", w, h);\n"
        "         /* Non-fatal — QueueSubmit2 will try lazy HEVC init from job SPS */\n"
        "      }\n"
        "   }\n"
        "\n"
        "   return VK_SUCCESS;\n"
        "}\n"
        "\n"
        "VKAPI_ATTR void VKAPI_CALL\n"
        "v4l2vk_DestroyVideoSessionParametersKHR(",
    ),
    # --- 3C-1: Add H265 find helper after H264 find helper ---
    (
        "static const VkVideoDecodeH264PictureInfoKHR *\n"
        "v4l2vk_find_h264_picture_info(const VkVideoDecodeInfoKHR *info)\n"
        "{\n"
        "   const VkBaseInStructure *p = (const VkBaseInStructure *)info->pNext;\n"
        "   while (p) {\n"
        "      if (p->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR)\n"
        "         return (const VkVideoDecodeH264PictureInfoKHR *)p;\n"
        "      p = p->pNext;\n"
        "   }\n"
        "   return NULL;\n"
        "}",
        "static const VkVideoDecodeH264PictureInfoKHR *\n"
        "v4l2vk_find_h264_picture_info(const VkVideoDecodeInfoKHR *info)\n"
        "{\n"
        "   const VkBaseInStructure *p = (const VkBaseInStructure *)info->pNext;\n"
        "   while (p) {\n"
        "      if (p->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR)\n"
        "         return (const VkVideoDecodeH264PictureInfoKHR *)p;\n"
        "      p = p->pNext;\n"
        "   }\n"
        "   return NULL;\n"
        "}\n"
        "\n"
        "/* hevc-patch-04-dispatch: H265 picture info finder */\n"
        "static const VkVideoDecodeH265PictureInfoKHR *\n"
        "v4l2vk_find_h265_picture_info(const VkVideoDecodeInfoKHR *info)\n"
        "{\n"
        "   const VkBaseInStructure *p = (const VkBaseInStructure *)info->pNext;\n"
        "   while (p) {\n"
        "      if (p->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR)\n"
        "         return (const VkVideoDecodeH265PictureInfoKHR *)p;\n"
        "      p = p->pNext;\n"
        "   }\n"
        "   return NULL;\n"
        "}",
    ),
    # --- 3C-2: CmdDecodeVideoKHR — add HEVC decode block after H264 block ---
    # Anchor: end of H264 else block, right before setup_slot_index
    (
        "   } else {\n"
        "      job.h264_valid = false;\n"
        "   }\n"
        "\n"
        "   /* Setup reference slot — tells us which DPB slot this decoded frame\n"
        "    * occupies.  We use this as the capture buffer index AND V4L2 timestamp\n"
        "    * so that future frames referencing this slot (via reference_ts =\n"
        "    * slot_index * 1000) can find the right capture buffer. */\n"
        "   job.setup_slot_index = -1;",
        "   } else {\n"
        "      job.h264_valid = false;\n"
        "   }\n"
        "\n"
        "   /* hevc-patch-04-dispatch: HEVC decode info extraction */\n"
        "   {\n"
        "      const VkVideoDecodeH265PictureInfoKHR *h265 =\n"
        "         v4l2vk_find_h265_picture_info(info);\n"
        "\n"
        "      const StdVideoH265SequenceParameterSet *h265sps = NULL;\n"
        "      const StdVideoH265PictureParameterSet  *h265pps = NULL;\n"
        "\n"
        "      if (h265 && h265->pStdPictureInfo) {\n"
        "         vk_video_get_h265_parameters(&sess->vk, sess->bound_params, info, h265,\n"
        "                                      &h265sps, &h265pps);\n"
        "      }\n"
        "\n"
        "      if (h265 && h265->pStdPictureInfo && h265sps && h265pps) {\n"
        "         job.h265_sps     = *h265sps;\n"
        "         job.h265_pps     = *h265pps;\n"
        "         job.h265_picture = *h265->pStdPictureInfo;\n"
        "         job.h265_valid   = true;\n"
        "\n"
        "         /* Deep-copy scaling lists */\n"
        "         if (h265sps->pScalingLists) {\n"
        "            job.h265_scaling = *h265sps->pScalingLists;\n"
        "            job.h265_sps.pScalingLists = &job.h265_scaling;\n"
        "         } else {\n"
        "            job.h265_sps.pScalingLists = NULL;\n"
        "         }\n"
        "\n"
        "         /* Deep-copy LT-RPS SPS if present */\n"
        "         if (h265sps->pLongTermRefPicsSps) {\n"
        "            job.h265_lt_rps_sps = *h265sps->pLongTermRefPicsSps;\n"
        "            job.h265_sps.pLongTermRefPicsSps = &job.h265_lt_rps_sps;\n"
        "         } else {\n"
        "            job.h265_sps.pLongTermRefPicsSps = NULL;\n"
        "         }\n"
        "\n"
        "         /* Deep-copy DecPicBufMgr if present */\n"
        "         if (h265sps->pDecPicBufMgr) {\n"
        "            job.h265_dec_pic_buf_mgr = *h265sps->pDecPicBufMgr;\n"
        "            job.h265_sps.pDecPicBufMgr = &job.h265_dec_pic_buf_mgr;\n"
        "         } else {\n"
        "            job.h265_sps.pDecPicBufMgr = NULL;\n"
        "         }\n"
        "\n"
        "         /* Deep-copy short-term RPS table */\n"
        "         if (h265sps->pShortTermRefPicSet &&\n"
        "             h265sps->num_short_term_ref_pic_sets > 0) {\n"
        "            uint32_t nst = h265sps->num_short_term_ref_pic_sets;\n"
        "            if (nst > 64) nst = 64;\n"
        "            memcpy(job.h265_st_ref_pic_set, h265sps->pShortTermRefPicSet,\n"
        "                   nst * sizeof(job.h265_st_ref_pic_set[0]));\n"
        "            job.h265_sps.pShortTermRefPicSet = job.h265_st_ref_pic_set;\n"
        "         } else {\n"
        "            job.h265_sps.pShortTermRefPicSet = NULL;\n"
        "         }\n"
        "\n"
        "         /* Slice-segment offsets (reuse job.slice_count/slice_offsets) */\n"
        "         job.slice_count = 0;\n"
        "         if (h265->sliceSegmentCount > 0 && h265->pSliceSegmentOffsets) {\n"
        "            job.slice_count = h265->sliceSegmentCount;\n"
        "            if (job.slice_count > 16)\n"
        "               job.slice_count = 16;\n"
        "            memcpy(job.slice_offsets, h265->pSliceSegmentOffsets,\n"
        "                   job.slice_count * sizeof(uint32_t));\n"
        "         }\n"
        "      } else {\n"
        "         job.h265_valid = false;\n"
        "      }\n"
        "   }\n"
        "\n"
        "   /* Setup reference slot — tells us which DPB slot this decoded frame\n"
        "    * occupies.  We use this as the capture buffer index AND V4L2 timestamp\n"
        "    * so that future frames referencing this slot (via reference_ts =\n"
        "    * slot_index * 1000) can find the right capture buffer. */\n"
        "   job.setup_slot_index = -1;",
    ),
    # --- 3C-3: DPB loop — add HEVC POC population after H264 DPB block ---
    (
        "         const VkVideoDecodeH264DpbSlotInfoKHR *h264_slot =\n"
        "            (const VkVideoDecodeH264DpbSlotInfoKHR *)v4l2vk_find_in_pnext(\n"
        "               slot->pNext,\n"
        "               VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);\n"
        "         if (h264_slot && h264_slot->pStdReferenceInfo) {\n"
        "            dst->ref = *h264_slot->pStdReferenceInfo;\n"
        "            dst->valid = 1;\n"
        "            dst->reference_ts = (uint64_t)slot->slotIndex * 1000;\n"
        "         } else {\n"
        "            dst->valid = 0;\n"
        "         }",
        "         const VkVideoDecodeH264DpbSlotInfoKHR *h264_slot =\n"
        "            (const VkVideoDecodeH264DpbSlotInfoKHR *)v4l2vk_find_in_pnext(\n"
        "               slot->pNext,\n"
        "               VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);\n"
        "         if (h264_slot && h264_slot->pStdReferenceInfo) {\n"
        "            dst->ref = *h264_slot->pStdReferenceInfo;\n"
        "            dst->valid = 1;\n"
        "            dst->reference_ts = (uint64_t)slot->slotIndex * 1000;\n"
        "         } else {\n"
        "            dst->valid = 0;\n"
        "         }\n"
        "\n"
        "         /* hevc-patch-04-dispatch: HEVC DPB POC from H265 slot info */\n"
        "         const VkVideoDecodeH265DpbSlotInfoKHR *h265_slot =\n"
        "            (const VkVideoDecodeH265DpbSlotInfoKHR *)v4l2vk_find_in_pnext(\n"
        "               slot->pNext,\n"
        "               VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR);\n"
        "         if (h265_slot && h265_slot->pStdReferenceInfo) {\n"
        "            dst->hevc_pic_order_cnt_val =\n"
        "               h265_slot->pStdReferenceInfo->PicOrderCntVal;\n"
        "            if (!dst->valid) {\n"
        "               /* HEVC-only DPB entry: set valid + timestamp */\n"
        "               dst->valid = 1;\n"
        "               dst->reference_ts = (uint64_t)slot->slotIndex * 1000;\n"
        "            }\n"
        "         }",
    ),
    # --- 3D: Add video_session_init_v4l2_hevc() before Session Create/Destroy ---
    (
        "   fprintf(stderr,\n"
        "           \"[V4L2VK] V4L2 session initialized: %ux%u, \"\n"
        "           \"%u output bufs, CAPTURE MMAP %u bufs (EXPBUF'd)\\n\",\n"
        "           w, h, sess->v4l2_ctx->output_buf_count,\n"
        "           sess->v4l2_ctx->capture_buf_count);\n"
        "\n"
        "   return 0;\n"
        "}\n"
        "\n"
        "/* ---- Session Create/Destroy ---- */",
        "   fprintf(stderr,\n"
        "           \"[V4L2VK] V4L2 session initialized: %ux%u, \"\n"
        "           \"%u output bufs, CAPTURE MMAP %u bufs (EXPBUF'd)\\n\",\n"
        "           w, h, sess->v4l2_ctx->output_buf_count,\n"
        "           sess->v4l2_ctx->capture_buf_count);\n"
        "\n"
        "   return 0;\n"
        "}\n"
        "\n"
        "/* hevc-patch-04-dispatch: HEVC session V4L2 init.\n"
        " * Mirrors video_session_init_v4l2 but uses HEVC pixel format via\n"
        " * set_output_format_codec(H265) and initialises the non-request\n"
        " * SPS control via set_init_paramset — matching the B0 discovery that\n"
        " * the SPS must be submitted non-request before CAPTURE setup.\n"
        " */\n"
        "int\n"
        "v4l2vk_video_session_init_v4l2_hevc(\n"
        "   v4l2vk_video_session *sess,\n"
        "   struct v4l2vk_vk_device *dev,\n"
        "   uint32_t w, uint32_t h,\n"
        "   const StdVideoH265SequenceParameterSet *sps)\n"
        "{\n"
        "   if (sess->v4l2_ctx)\n"
        "      return 0; /* Already initialized */\n"
        "\n"
        "   sess->v4l2_ctx =\n"
        "      v4l2vk_v4l2_context_create(dev->video_fd, dev->media_fd, w, h);\n"
        "   if (!sess->v4l2_ctx) {\n"
        "      fprintf(stderr, \"[V4L2VK][ERR] HEVC: Failed to create V4L2 context\\n\");\n"
        "      return -1;\n"
        "   }\n"
        "\n"
        "   /* Set OUTPUT pixelformat to V4L2_PIX_FMT_HEVC_SLICE */\n"
        "   if (v4l2vk_v4l2_set_output_format_codec(sess->v4l2_ctx, w, h,\n"
        "                                            V4L2VK_CODEC_H265) < 0) {\n"
        "      fprintf(stderr, \"[V4L2VK][ERR] HEVC: OUTPUT format setup failed\\n\");\n"
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }\n"
        "\n"
        "   /* Non-request SPS init — must precede CAPTURE setup (B0 lesson) */\n"
        "   if (sps) {\n"
        "      struct v4l2vk_hevc_frame_params init_params;\n"
        "      memset(&init_params, 0, sizeof(init_params));\n"
        "      v4l2vk_h265_translate_sps(sps, &init_params.sps);\n"
        "      if (v4l2vk_v4l2_set_init_paramset(sess->v4l2_ctx,\n"
        "                                         V4L2VK_CODEC_H265,\n"
        "                                         &init_params) < 0) {\n"
        "         fprintf(stderr,\n"
        "                 \"[V4L2VK][ERR] HEVC: V4L2 non-request SPS set failed\\n\");\n"
        "         v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "         sess->v4l2_ctx = NULL;\n"
        "         return -1;\n"
        "      }\n"
        "   }\n"
        "\n"
        "   if (v4l2vk_v4l2_set_capture_format(sess->v4l2_ctx, w, h, NULL, NULL) < 0) {\n"
        "      fprintf(stderr, \"[V4L2VK][ERR] HEVC: V4L2 CAPTURE format setup failed\\n\");\n"
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }\n"
        "\n"
        "   if (v4l2vk_v4l2_create_output_buffers(sess->v4l2_ctx, 4) < 0) {\n"
        "      fprintf(stderr, \"[V4L2VK][ERR] HEVC: V4L2 OUTPUT buffer setup failed\\n\");\n"
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }\n"
        "\n"
        "   if (v4l2vk_v4l2_create_capture_buffers(sess->v4l2_ctx, 17) < 0) {\n"
        "      fprintf(stderr,\n"
        "              \"[V4L2VK][ERR] HEVC: V4L2 CAPTURE buffer setup failed\\n\");\n"
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }\n"
        "\n"
        "   if (v4l2vk_v4l2_streamon(sess->v4l2_ctx) < 0) {\n"
        "      fprintf(stderr, \"[V4L2VK][ERR] HEVC: V4L2 STREAMON failed\\n\");\n"
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }\n"
        "\n"
        "   sess->coded_extent.width  = w;\n"
        "   sess->coded_extent.height = h;\n"
        "\n"
        "   fprintf(stderr,\n"
        "           \"[V4L2VK] HEVC V4L2 session initialized: %ux%u, \"\n"
        "           \"%u output bufs, CAPTURE MMAP %u bufs\\n\",\n"
        "           w, h, sess->v4l2_ctx->output_buf_count,\n"
        "           sess->v4l2_ctx->capture_buf_count);\n"
        "\n"
        "   return 0;\n"
        "}\n"
        "\n"
        "/* ---- Session Create/Destroy ---- */",
    ),
])

# ===========================================================================
# 4. v4l2vk_vk_video.h — declare the HEVC init helper
# ===========================================================================
patch_file("v4l2vk_vk_video.h", "hevc-patch-04-dispatch", [
    # Add H265 include and forward-declare the HEVC init helper alongside the H264 one
    (
        "int v4l2vk_video_session_init_v4l2(v4l2vk_video_session *sess,\n"
        "                                   struct v4l2vk_vk_device *dev,\n"
        "                                   uint32_t w, uint32_t h,\n"
        "                                   const StdVideoH264SequenceParameterSet *sps,\n"
        "                                   const StdVideoH264PictureParameterSet *pps);",
        "int v4l2vk_video_session_init_v4l2(v4l2vk_video_session *sess,\n"
        "                                   struct v4l2vk_vk_device *dev,\n"
        "                                   uint32_t w, uint32_t h,\n"
        "                                   const StdVideoH264SequenceParameterSet *sps,\n"
        "                                   const StdVideoH264PictureParameterSet *pps);\n"
        "\n"
        "/* hevc-patch-04-dispatch: HEVC session V4L2 init (HEVC codec path) */\n"
        "#include <vk_video/vulkan_video_codec_h265std.h>\n"
        "int v4l2vk_video_session_init_v4l2_hevc(v4l2vk_video_session *sess,\n"
        "                                         struct v4l2vk_vk_device *dev,\n"
        "                                         uint32_t w, uint32_t h,\n"
        "                                         const StdVideoH265SequenceParameterSet *sps);",
    ),
])

# ===========================================================================
# 5. v4l2vk_vk_device.c — HEVC job loop arm
# ===========================================================================
patch_file("v4l2vk_vk_device.c", "hevc-patch-04-dispatch", [
    # 5A: Add HEVC includes after the H264 include
    (
        '#include "v4l2vk_v4l2_h264.h"',
        '#include "v4l2vk_v4l2_h264.h"\n'
        '#include "v4l2vk_v4l2_hevc.h"  /* hevc-patch-04-dispatch */\n'
        '#include "v4l2vk_codec.h"       /* hevc-patch-04-dispatch: V4L2VK_CODEC_* */',
    ),
    # 5B: Replace the H264-only gate with one that accepts both H264 and H265 jobs
    # Current: "if (!job->h264_valid)\n               continue;"
    # New: skip only if NEITHER codec is valid
    (
        "         util_dynarray_foreach (&cb->jobs, struct v4l2vk_recorded_job, job) {\n"
        "            if (!job->h264_valid)\n"
        "               continue;",
        "         util_dynarray_foreach (&cb->jobs, struct v4l2vk_recorded_job, job) {\n"
        "            /* hevc-patch-04-dispatch: skip only if no valid codec */\n"
        "            if (!job->h264_valid && !job->h265_valid)\n"
        "               continue;",
    ),
    # 5C: Replace the H264-only lazy-init block with codec-aware lazy init
    (
        "            /* Lazy V4L2 init: if the SPS wasn't available at\n"
        "             * CreateVideoSessionParametersKHR time, initialize\n"
        "             * now from the job's SPS. */\n"
        "            if (!sess->v4l2_ctx && job->h264_valid) {\n"
        "               uint32_t sps_w =\n"
        "                  ((uint32_t)job->sps.pic_width_in_mbs_minus1 + 1) * 16;\n"
        "               uint32_t sps_h =\n"
        "                  ((uint32_t)job->sps.pic_height_in_map_units_minus1 + 1) *\n"
        "                  16;\n"
        "               if (!job->sps.flags.frame_mbs_only_flag)\n"
        "                  sps_h *= 2;\n"
        "               fprintf(stderr,\n"
        "                       \"[V4L2VK] Late V4L2 init from SPS: %ux%u\\n\",\n"
        "                       sps_w, sps_h);\n"
        "               v4l2vk_video_session_init_v4l2(sess, dev, sps_w, sps_h, &job->sps, &job->pps);\n"
        "            }",
        "            /* hevc-patch-04-dispatch: codec-aware lazy V4L2 init */\n"
        "            if (!sess->v4l2_ctx && job->h264_valid) {\n"
        "               uint32_t sps_w =\n"
        "                  ((uint32_t)job->sps.pic_width_in_mbs_minus1 + 1) * 16;\n"
        "               uint32_t sps_h =\n"
        "                  ((uint32_t)job->sps.pic_height_in_map_units_minus1 + 1) *\n"
        "                  16;\n"
        "               if (!job->sps.flags.frame_mbs_only_flag)\n"
        "                  sps_h *= 2;\n"
        "               fprintf(stderr,\n"
        "                       \"[V4L2VK] Late H264 V4L2 init from SPS: %ux%u\\n\",\n"
        "                       sps_w, sps_h);\n"
        "               v4l2vk_video_session_init_v4l2(sess, dev, sps_w, sps_h,\n"
        "                                              &job->sps, &job->pps);\n"
        "            } else if (!sess->v4l2_ctx && job->h265_valid) {\n"
        "               uint32_t sps_w =\n"
        "                  (uint32_t)job->h265_sps.pic_width_in_luma_samples;\n"
        "               uint32_t sps_h =\n"
        "                  (uint32_t)job->h265_sps.pic_height_in_luma_samples;\n"
        "               fprintf(stderr,\n"
        "                       \"[V4L2VK] Late HEVC V4L2 init from SPS: %ux%u\\n\",\n"
        "                       sps_w, sps_h);\n"
        "               v4l2vk_video_session_init_v4l2_hevc(sess, dev, sps_w, sps_h,\n"
        "                                                   &job->h265_sps);\n"
        "            }",
    ),
    # 5D: After the H264 translate+submit block, add HEVC arm.
    # Anchor: the H264 translate block — after the successful translate call
    # and the set_h264_controls call we insert an else-if for HEVC.
    # Strategy: wrap the existing H264 translate+controls block in if(h264_valid),
    # add else-if(h265_valid) for HEVC.
    # Anchor on the translate call start (unique string).
    (
        "            /* Translate Vulkan H.264 params to V4L2 controls */\n"
        "            struct v4l2vk_h264_frame_params v4l2_params;\n"
        "\n"
        "            if (v4l2vk_h264_translate_params(\n"
        "                   &job->sps, &job->pps, &job->picture, job->dpb,\n"
        "                   job->dpb_count, job->slice_offsets, job->slice_count,\n"
        "                   frame_ts, bs_data, bs_size, &v4l2_params) != 0) {\n"
        "               fprintf(stderr,\n"
        "                       \"[V4L2VK][ERR] H.264 param translation failed\\n\");\n"
        "               continue;\n"
        "            }\n"
        "\n"
        "            /* Use round-robin output buffer */\n"
        "            uint32_t out_idx =\n"
        "               dev->frame_counter % v4l2_ctx->output_buf_count;\n"
        "\n"
        "            /* Allocate media request */\n"
        "            int request_fd = -1;\n"
        "            if (v4l2vk_v4l2_alloc_request(v4l2_ctx, &request_fd) < 0) {\n"
        "               fprintf(stderr, \"[V4L2VK][ERR] alloc_request failed\\n\");\n"
        "               continue;\n"
        "            }\n"
        "\n"
        "            /* Set H.264 controls on the request */\n"
        "            if (v4l2vk_v4l2_set_h264_controls(v4l2_ctx, request_fd,\n"
        "                                              &v4l2_params) < 0) {\n"
        "               fprintf(stderr, \"[V4L2VK][ERR] set_h264_controls failed\\n\");\n"
        "               close(request_fd);\n"
        "               continue;\n"
        "            }\n"
        "\n"
        "            /* Queue OUTPUT (bitstream) buffer */\n"
        "            if (v4l2vk_v4l2_queue_output(v4l2_ctx, out_idx, request_fd,\n"
        "                                         bs_data, bs_size, frame_ts) < 0) {\n"
        "               fprintf(stderr, \"[V4L2VK][ERR] queue_output failed\\n\");\n"
        "               close(request_fd);\n"
        "               continue;\n"
        "            }",
        "            /* Translate codec params to V4L2 controls */\n"
        "            int request_fd = -1;\n"
        "\n"
        "            if (job->h264_valid) {\n"
        "               /* H264 path */\n"
        "               struct v4l2vk_h264_frame_params v4l2_params;\n"
        "\n"
        "               if (v4l2vk_h264_translate_params(\n"
        "                      &job->sps, &job->pps, &job->picture, job->dpb,\n"
        "                      job->dpb_count, job->slice_offsets, job->slice_count,\n"
        "                      frame_ts, bs_data, bs_size, &v4l2_params) != 0) {\n"
        "                  fprintf(stderr,\n"
        "                          \"[V4L2VK][ERR] H.264 param translation failed\\n\");\n"
        "                  continue;\n"
        "               }\n"
        "\n"
        "               /* Use round-robin output buffer */\n"
        "               uint32_t out_idx =\n"
        "                  dev->frame_counter % v4l2_ctx->output_buf_count;\n"
        "\n"
        "               if (v4l2vk_v4l2_alloc_request(v4l2_ctx, &request_fd) < 0) {\n"
        "                  fprintf(stderr, \"[V4L2VK][ERR] alloc_request failed\\n\");\n"
        "                  continue;\n"
        "               }\n"
        "\n"
        "               if (v4l2vk_v4l2_set_h264_controls(v4l2_ctx, request_fd,\n"
        "                                                 &v4l2_params) < 0) {\n"
        "                  fprintf(stderr, \"[V4L2VK][ERR] set_h264_controls failed\\n\");\n"
        "                  close(request_fd);\n"
        "                  continue;\n"
        "               }\n"
        "\n"
        "               if (v4l2vk_v4l2_queue_output(v4l2_ctx, out_idx, request_fd,\n"
        "                                            bs_data, bs_size, frame_ts) < 0) {\n"
        "                  fprintf(stderr, \"[V4L2VK][ERR] queue_output failed\\n\");\n"
        "                  close(request_fd);\n"
        "                  continue;\n"
        "               }\n"
        "            } else if (job->h265_valid) {\n"
        "               /* hevc-patch-04-dispatch: HEVC path */\n"
        "               struct v4l2vk_hevc_frame_params hevc_params;\n"
        "               memset(&hevc_params, 0, sizeof(hevc_params));\n"
        "\n"
        "               v4l2vk_h265_translate_sps(&job->h265_sps, &hevc_params.sps);\n"
        "               v4l2vk_h265_translate_pps(&job->h265_pps, &hevc_params.pps);\n"
        "               v4l2vk_h265_translate_scaling(job->h265_sps.pScalingLists,\n"
        "                                             &hevc_params.scaling);\n"
        "               hevc_params.has_scaling =\n"
        "                  (job->h265_sps.pScalingLists != NULL);\n"
        "\n"
        "               /* ST-RPS: use the SPS short-term ref pic set table */\n"
        "               hevc_params.st_rps_count = 0;\n"
        "               if (job->h265_sps.pShortTermRefPicSet &&\n"
        "                   job->h265_sps.num_short_term_ref_pic_sets > 0) {\n"
        "                  hevc_params.st_rps_count = v4l2vk_h265_translate_st_rps(\n"
        "                     job->h265_sps.pShortTermRefPicSet,\n"
        "                     job->h265_sps.num_short_term_ref_pic_sets,\n"
        "                     hevc_params.st_rps);\n"
        "               }\n"
        "\n"
        "               /* LT-RPS: from SPS long-term ref pics if present */\n"
        "               hevc_params.lt_rps_count = 0;\n"
        "               if (job->h265_sps.pLongTermRefPicsSps &&\n"
        "                   job->h265_sps.num_long_term_ref_pics_sps > 0) {\n"
        "                  hevc_params.lt_rps_count = v4l2vk_h265_translate_lt_rps(\n"
        "                     job->h265_sps.pLongTermRefPicsSps,\n"
        "                     job->h265_sps.num_long_term_ref_pics_sps,\n"
        "                     hevc_params.lt_rps);\n"
        "               }\n"
        "\n"
        "               v4l2vk_h265_translate_decode_params(&job->h265_picture,\n"
        "                                                   job->dpb,\n"
        "                                                   job->dpb_count,\n"
        "                                                   &hevc_params.decode_params);\n"
        "\n"
        "               hevc_params.slice_count =\n"
        "                  v4l2vk_h265_translate_slice_params(\n"
        "                     bs_data, bs_size,\n"
        "                     &hevc_params.sps, &hevc_params.pps,\n"
        "                     job->slice_offsets, job->slice_count,\n"
        "                     hevc_params.slice_params);\n"
        "\n"
        "               uint32_t out_idx =\n"
        "                  dev->frame_counter % v4l2_ctx->output_buf_count;\n"
        "\n"
        "               if (v4l2vk_v4l2_alloc_request(v4l2_ctx, &request_fd) < 0) {\n"
        "                  fprintf(stderr,\n"
        "                          \"[V4L2VK][ERR] HEVC: alloc_request failed\\n\");\n"
        "                  continue;\n"
        "               }\n"
        "\n"
        "               if (v4l2vk_v4l2_set_codec_controls(v4l2_ctx, request_fd,\n"
        "                                                   V4L2VK_CODEC_H265,\n"
        "                                                   &hevc_params) < 0) {\n"
        "                  fprintf(stderr,\n"
        "                          \"[V4L2VK][ERR] HEVC: set_codec_controls failed\\n\");\n"
        "                  close(request_fd);\n"
        "                  continue;\n"
        "               }\n"
        "\n"
        "               if (v4l2vk_v4l2_queue_output(v4l2_ctx, out_idx, request_fd,\n"
        "                                            bs_data, bs_size, frame_ts) < 0) {\n"
        "                  fprintf(stderr,\n"
        "                          \"[V4L2VK][ERR] HEVC: queue_output failed\\n\");\n"
        "                  close(request_fd);\n"
        "                  continue;\n"
        "               }\n"
        "            } else {\n"
        "               /* Should not happen — gated above */\n"
        "               continue;\n"
        "            }\n"
        "\n"
        "            /* --- Shared submit path (H264 + HEVC) --- */",
    ),
])

# ===========================================================================
# Write all staged changes
# ===========================================================================
if not pending:
    print("No files staged — all already patched.")
else:
    for abs_path, fname, content in pending:
        with open(abs_path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"{fname}: written ({len(content)} bytes)")

print("hevc-patch-04-dispatch: done")
