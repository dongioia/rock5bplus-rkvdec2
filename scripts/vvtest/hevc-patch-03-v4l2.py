#!/usr/bin/env python3
"""hevc-patch-03-v4l2: V4L2 layer generalization for HEVC.

Four additive changes to v4l2vk_v4l2.c and v4l2vk_v4l2.h:
  1. OUTPUT format: add set_output_format_codec() branching on codec enum.
  2. Control-ID probe: add HEVC CIDs (SPS/PPS/SCALING/DECODE_PARAMS/SLICE_PARAMS +
     EXT_SPS_ST_RPS/LT_RPS from ext_compat) stored in context struct.
  3. set_init_paramset: generalization of set_init_sps for H265 (STEP0 finding:
     golden v4l2slh265dec sets exactly one non-request control,
     V4L2_CID_STATELESS_HEVC_SPS, between S_FMT(OUTPUT) and CAPTURE setup).
  4. set_codec_controls: generalization of set_h264_controls with H265 arm that
     builds the request-based v4l2_ext_control array from v4l2vk_hevc_frame_params:
     SPS, PPS, SCALING (if has_scaling), DECODE_PARAMS, SLICE_PARAMS (slice_count),
     EXT_SPS_ST_RPS (st_rps_count), EXT_SPS_LT_RPS (lt_rps_count).
     H264 arm delegates unchanged to set_h264_controls.

Idempotent: guarded by a unique sentinel per file.
Asserts every anchor is uniquely present before writing.

Run in the build container:
  python3 /vv/hevc-patch-03-v4l2.py /work/mesa-sree/mesa/src/vulkan-v4l2 [/deploy]
"""
import os
import sys

DIR = sys.argv[1]
pending = []  # (path, new_content)


def patch_file(fname, guard, edits):
    """Apply all edits to a file in sequence (later edits see earlier ones).
    All edits must anchor on unique text present in the original or already-edited string.
    If guard is already present, the file is skipped (idempotent)."""
    p = os.path.join(DIR, fname)
    s = open(p, encoding="utf-8").read()
    if guard in s:
        print(f"{fname}: already patched [{guard[:50]}], skipping")
        return
    for idx, (a, b) in enumerate(edits):
        assert a in s, (
            f"{fname}: edit[{idx}] ANCHOR NOT FOUND:\n  {a[:100]!r}"
        )
        assert s.count(a) == 1, (
            f"{fname}: edit[{idx}] ANCHOR NOT UNIQUE ({s.count(a)}x):\n  {a[:100]!r}"
        )
        s = s.replace(a, b, 1)
    pending.append((p, fname, s))
    print(f"{fname}: staged ({len(edits)} edit(s))")


# ---------------------------------------------------------------------------
# v4l2vk_v4l2.h — all edits in one pass so each sees the previous result
# ---------------------------------------------------------------------------
patch_file("v4l2vk_v4l2.h", "has_ctrl_hevc_sps", [
    # Edit A: extend context struct with HEVC ctrl flags
    (
        "   /* Which H.264 controls the kernel driver actually supports */\n"
        "   bool has_ctrl_sps;\n"
        "   bool has_ctrl_pps;\n"
        "   bool has_ctrl_scaling;\n"
        "   bool has_ctrl_decode_params;\n"
        "   bool has_ctrl_slice_params;\n"
        "   bool has_ctrl_pred_weights;\n"
        "};",
        "   /* Which H.264 controls the kernel driver actually supports */\n"
        "   bool has_ctrl_sps;\n"
        "   bool has_ctrl_pps;\n"
        "   bool has_ctrl_scaling;\n"
        "   bool has_ctrl_decode_params;\n"
        "   bool has_ctrl_slice_params;\n"
        "   bool has_ctrl_pred_weights;\n"
        "\n"
        "   /* Which HEVC stateless controls the kernel driver supports */\n"
        "   bool has_ctrl_hevc_sps;\n"
        "   bool has_ctrl_hevc_pps;\n"
        "   bool has_ctrl_hevc_scaling;\n"
        "   bool has_ctrl_hevc_decode_params;\n"
        "   bool has_ctrl_hevc_slice_params;\n"
        "   bool has_ctrl_hevc_ext_sps_st_rps;\n"
        "   bool has_ctrl_hevc_ext_sps_lt_rps;\n"
        "};",
    ),
    # Edit B: forward-declare hevc_frame_params alongside h264
    (
        "struct v4l2vk_h264_frame_params;",
        "struct v4l2vk_h264_frame_params;\n"
        "struct v4l2vk_hevc_frame_params;",
    ),
    # Edit C: declare set_output_format_codec
    (
        "int v4l2vk_v4l2_set_output_format(struct v4l2vk_v4l2_context *ctx, uint32_t w,\n"
        "                                  uint32_t h);",
        "int v4l2vk_v4l2_set_output_format(struct v4l2vk_v4l2_context *ctx, uint32_t w,\n"
        "                                  uint32_t h);\n"
        "\n"
        "/* set_output_format_codec: selects pixelformat from codec.\n"
        " * codec=0 (H264) -> H264_SLICE; codec=1 (H265) -> HEVC_SLICE. */\n"
        "int v4l2vk_v4l2_set_output_format_codec(struct v4l2vk_v4l2_context *ctx,\n"
        "                                        uint32_t w, uint32_t h, int codec);",
    ),
    # Edit D: declare generalized init + codec-controls functions
    (
        "int v4l2vk_v4l2_set_init_sps(struct v4l2vk_v4l2_context *ctx,\n"
        "                            const struct v4l2vk_h264_frame_params *params);",
        "int v4l2vk_v4l2_set_init_sps(struct v4l2vk_v4l2_context *ctx,\n"
        "                            const struct v4l2vk_h264_frame_params *params);\n"
        "\n"
        "/* Generalized non-request SPS init for either codec.\n"
        " * H264: sets V4L2_CID_STATELESS_H264_SPS (same as set_init_sps).\n"
        " * H265: sets V4L2_CID_STATELESS_HEVC_SPS (STEP0 finding).\n"
        " * codec: 0=H264, 1=H265. params: matching frame_params pointer. */\n"
        "int v4l2vk_v4l2_set_init_paramset(struct v4l2vk_v4l2_context *ctx,\n"
        "                                  int codec, const void *params);\n"
        "\n"
        "/* Generalized per-request codec control setter.\n"
        " * H264: delegates to set_h264_controls (identical behavior).\n"
        " * H265: builds control array from v4l2vk_hevc_frame_params.\n"
        " * codec: 0=H264, 1=H265. params: matching frame_params pointer. */\n"
        "int v4l2vk_v4l2_set_codec_controls(struct v4l2vk_v4l2_context *ctx,\n"
        "                                   int request_fd, int codec,\n"
        "                                   const void *params);",
    ),
])

# ---------------------------------------------------------------------------
# v4l2vk_v4l2.c — all edits in one pass
# ---------------------------------------------------------------------------
patch_file("v4l2vk_v4l2.c", "v4l2vk_v4l2_set_codec_controls", [
    # Edit A: add HEVC includes at the top (after existing includes)
    (
        '#include "v4l2vk_v4l2.h"\n'
        '#include "v4l2vk_log.h"\n'
        '#include "v4l2vk_v4l2_h264.h"',
        '#include "v4l2vk_v4l2.h"\n'
        '#include "v4l2vk_log.h"\n'
        '#include "v4l2vk_v4l2_h264.h"\n'
        '#include "v4l2vk_hevc_ext_compat.h"\n'
        '#include "v4l2vk_v4l2_hevc.h"\n'
        '#include "v4l2vk_codec.h"',
    ),
    # Edit B: add HEVC control probes in context_create (after H264 probes block)
    (
        "   PROBE_CTRL(has_ctrl_pred_weights, V4L2_CID_STATELESS_H264_PRED_WEIGHTS);\n"
        "#undef PROBE_CTRL\n"
        "\n"
        "   fprintf(stderr,\n"
        '           "[V4L2VK][V4L2] supported controls: SPS=%d PPS=%d SCALING=%d "\n'
        '           "DECODE_PARAMS=%d SLICE_PARAMS=%d PRED_WEIGHTS=%d\\n",\n'
        "           ctx->has_ctrl_sps, ctx->has_ctrl_pps, ctx->has_ctrl_scaling,\n"
        "           ctx->has_ctrl_decode_params, ctx->has_ctrl_slice_params,\n"
        "           ctx->has_ctrl_pred_weights);",
        "   PROBE_CTRL(has_ctrl_pred_weights, V4L2_CID_STATELESS_H264_PRED_WEIGHTS);\n"
        "\n"
        "   /* Probe HEVC stateless controls (EXT_SPS CIDs from hevc_ext_compat.h) */\n"
        "   PROBE_CTRL(has_ctrl_hevc_sps, V4L2_CID_STATELESS_HEVC_SPS);\n"
        "   PROBE_CTRL(has_ctrl_hevc_pps, V4L2_CID_STATELESS_HEVC_PPS);\n"
        "   PROBE_CTRL(has_ctrl_hevc_scaling, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);\n"
        "   PROBE_CTRL(has_ctrl_hevc_decode_params, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);\n"
        "   PROBE_CTRL(has_ctrl_hevc_slice_params, V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);\n"
        "   PROBE_CTRL(has_ctrl_hevc_ext_sps_st_rps, V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS);\n"
        "   PROBE_CTRL(has_ctrl_hevc_ext_sps_lt_rps, V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS);\n"
        "#undef PROBE_CTRL\n"
        "\n"
        "   fprintf(stderr,\n"
        '           "[V4L2VK][V4L2] supported controls: SPS=%d PPS=%d SCALING=%d "\n'
        '           "DECODE_PARAMS=%d SLICE_PARAMS=%d PRED_WEIGHTS=%d\\n",\n'
        "           ctx->has_ctrl_sps, ctx->has_ctrl_pps, ctx->has_ctrl_scaling,\n"
        "           ctx->has_ctrl_decode_params, ctx->has_ctrl_slice_params,\n"
        "           ctx->has_ctrl_pred_weights);\n"
        "   fprintf(stderr,\n"
        '           "[V4L2VK][V4L2] HEVC controls: SPS=%d PPS=%d SCALING=%d "\n'
        '           "DECODE_PARAMS=%d SLICE_PARAMS=%d ST_RPS=%d LT_RPS=%d\\n",\n'
        "           ctx->has_ctrl_hevc_sps, ctx->has_ctrl_hevc_pps,\n"
        "           ctx->has_ctrl_hevc_scaling, ctx->has_ctrl_hevc_decode_params,\n"
        "           ctx->has_ctrl_hevc_slice_params, ctx->has_ctrl_hevc_ext_sps_st_rps,\n"
        "           ctx->has_ctrl_hevc_ext_sps_lt_rps);",
    ),
    # Edit C: extend ctrl_name helper with HEVC cases
    (
        "   case V4L2_CID_STATELESS_H264_PRED_WEIGHTS:\n"
        '      return "PRED_WEIGHTS";\n'
        "   default:\n"
        '      return "UNKNOWN";\n'
        "   }",
        "   case V4L2_CID_STATELESS_H264_PRED_WEIGHTS:\n"
        '      return "PRED_WEIGHTS";\n'
        "   case V4L2_CID_STATELESS_HEVC_SPS:\n"
        '      return "HEVC_SPS";\n'
        "   case V4L2_CID_STATELESS_HEVC_PPS:\n"
        '      return "HEVC_PPS";\n'
        "   case V4L2_CID_STATELESS_HEVC_SCALING_MATRIX:\n"
        '      return "HEVC_SCALING_MATRIX";\n'
        "   case V4L2_CID_STATELESS_HEVC_DECODE_PARAMS:\n"
        '      return "HEVC_DECODE_PARAMS";\n'
        "   case V4L2_CID_STATELESS_HEVC_SLICE_PARAMS:\n"
        '      return "HEVC_SLICE_PARAMS";\n'
        "   case V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS:\n"
        '      return "HEVC_EXT_SPS_ST_RPS";\n'
        "   case V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS:\n"
        '      return "HEVC_EXT_SPS_LT_RPS";\n'
        "   default:\n"
        '      return "UNKNOWN";\n'
        "   }",
    ),
    # Edit D: add set_output_format_codec after existing set_output_format
    (
        "int\n"
        "v4l2vk_v4l2_set_capture_format(",
        "/* set_output_format_codec: selects pixelformat from codec enum.\n"
        " * H264 (0) -> H264_SLICE (same as set_output_format).\n"
        " * H265 (1) -> HEVC_SLICE. */\n"
        "int\n"
        "v4l2vk_v4l2_set_output_format_codec(struct v4l2vk_v4l2_context *ctx,\n"
        "                                    uint32_t w, uint32_t h, int codec)\n"
        "{\n"
        "   uint32_t pixfmt = (codec == V4L2VK_CODEC_H265)\n"
        "                     ? V4L2_PIX_FMT_HEVC_SLICE\n"
        "                     : V4L2_PIX_FMT_H264_SLICE;\n"
        "\n"
        "   struct v4l2_format fmt;\n"
        "   memset(&fmt, 0, sizeof(fmt));\n"
        "   fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;\n"
        "   fmt.fmt.pix_mp.width = w;\n"
        "   fmt.fmt.pix_mp.height = h;\n"
        "   fmt.fmt.pix_mp.pixelformat = pixfmt;\n"
        "   fmt.fmt.pix_mp.num_planes = 1;\n"
        "   fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;\n"
        "\n"
        "   if (xioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt) < 0) {\n"
        '      V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] S_FMT OUTPUT (codec=%d) failed: %s\\n",\n'
        "                       codec, strerror(errno));\n"
        "      return -errno;\n"
        "   }\n"
        "   ctx->output_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;\n"
        '   V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] OUTPUT fmt (codec=%d): %ux%u\\n",\n'
        "                    codec, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);\n"
        "   return 0;\n"
        "}\n"
        "\n"
        "int\n"
        "v4l2vk_v4l2_set_capture_format(",
    ),
    # Edit E: add set_init_paramset after set_init_sps (before "/* --- Streaming --- */")
    (
        "/* --- Streaming --- */",
        "/* set_init_paramset: generalized non-request init control.\n"
        " * H264: sets V4L2_CID_STATELESS_H264_SPS (= set_init_sps behavior).\n"
        " * H265: sets V4L2_CID_STATELESS_HEVC_SPS (STEP0 finding: single non-request\n"
        " *   ctrl between S_FMT(OUTPUT,HEVC_SLICE) and CAPTURE setup). */\n"
        "int\n"
        "v4l2vk_v4l2_set_init_paramset(struct v4l2vk_v4l2_context *ctx,\n"
        "                              int codec, const void *params)\n"
        "{\n"
        "   struct v4l2_ext_control ctrl;\n"
        "   memset(&ctrl, 0, sizeof(ctrl));\n"
        "\n"
        "   if (codec == V4L2VK_CODEC_H265) {\n"
        "      const struct v4l2vk_hevc_frame_params *hp =\n"
        "         (const struct v4l2vk_hevc_frame_params *)params;\n"
        "      ctrl.id   = V4L2_CID_STATELESS_HEVC_SPS;\n"
        "      ctrl.size = sizeof(hp->sps);\n"
        "      ctrl.ptr  = (void *)&hp->sps;\n"
        "   } else {\n"
        "      const struct v4l2vk_h264_frame_params *hp =\n"
        "         (const struct v4l2vk_h264_frame_params *)params;\n"
        "      ctrl.id   = V4L2_CID_STATELESS_H264_SPS;\n"
        "      ctrl.size = sizeof(hp->sps);\n"
        "      ctrl.ptr  = (void *)&hp->sps;\n"
        "   }\n"
        "\n"
        "   struct v4l2_ext_controls ext;\n"
        "   memset(&ext, 0, sizeof(ext));\n"
        "   ext.which    = V4L2_CTRL_WHICH_CUR_VAL;\n"
        "   ext.count    = 1;\n"
        "   ext.controls = &ctrl;\n"
        "\n"
        "   if (xioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext) < 0) {\n"
        '      V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] init_paramset (codec=%d) failed: %s\\n",\n'
        "                       codec, strerror(errno));\n"
        "      return -errno;\n"
        "   }\n"
        '   V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] init_paramset (codec=%d) OK\\n", codec);\n'
        "   return 0;\n"
        "}\n"
        "\n"
        "/* --- Streaming --- */",
    ),
    # Edit F: add set_codec_controls after set_h264_controls (at end of file)
    # The last two lines of set_h264_controls are the fprintf + return 0 + closing brace.
    # We anchor on the exact tail to avoid ambiguity with the inner per-ctrl debug loop.
    (
        '   fprintf(stderr, "[V4L2VK][V4L2] S_EXT_CTRLS: OK\\n");\n'
        "   return 0;\n"
        "}",
        '   fprintf(stderr, "[V4L2VK][V4L2] S_EXT_CTRLS: OK\\n");\n'
        "   return 0;\n"
        "}\n"
        "\n"
        "/* set_codec_controls: generalized per-request codec control setter.\n"
        " * H264: delegates unchanged to set_h264_controls.\n"
        " * H265: builds control array from v4l2vk_hevc_frame_params:\n"
        " *   SPS, PPS, SCALING (has_scaling), DECODE_PARAMS, SLICE_PARAMS (slice_count),\n"
        " *   EXT_SPS_ST_RPS (st_rps_count), EXT_SPS_LT_RPS (lt_rps_count). */\n"
        "int\n"
        "v4l2vk_v4l2_set_codec_controls(struct v4l2vk_v4l2_context *ctx,\n"
        "                               int request_fd, int codec,\n"
        "                               const void *params)\n"
        "{\n"
        "   if (codec == V4L2VK_CODEC_H265) {\n"
        "      const struct v4l2vk_hevc_frame_params *hp =\n"
        "         (const struct v4l2vk_hevc_frame_params *)params;\n"
        "\n"
        "      /* 7 possible controls: SPS PPS SCALING DECODE_PARAMS SLICE_PARAMS\n"
        "       * EXT_SPS_ST_RPS EXT_SPS_LT_RPS */\n"
        "      struct v4l2_ext_control ctrls[7];\n"
        "      uint32_t n = 0;\n"
        "      memset(ctrls, 0, sizeof(ctrls));\n"
        "\n"
        "      if (ctx->has_ctrl_hevc_sps) {\n"
        "         ctrls[n].id   = V4L2_CID_STATELESS_HEVC_SPS;\n"
        "         ctrls[n].size = sizeof(hp->sps);\n"
        "         ctrls[n].ptr  = (void *)&hp->sps;\n"
        "         n++;\n"
        "      }\n"
        "      if (ctx->has_ctrl_hevc_pps) {\n"
        "         ctrls[n].id   = V4L2_CID_STATELESS_HEVC_PPS;\n"
        "         ctrls[n].size = sizeof(hp->pps);\n"
        "         ctrls[n].ptr  = (void *)&hp->pps;\n"
        "         n++;\n"
        "      }\n"
        "      if (ctx->has_ctrl_hevc_scaling && hp->has_scaling) {\n"
        "         ctrls[n].id   = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX;\n"
        "         ctrls[n].size = sizeof(hp->scaling);\n"
        "         ctrls[n].ptr  = (void *)&hp->scaling;\n"
        "         n++;\n"
        "      }\n"
        "      if (ctx->has_ctrl_hevc_decode_params) {\n"
        "         ctrls[n].id   = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS;\n"
        "         ctrls[n].size = sizeof(hp->decode_params);\n"
        "         ctrls[n].ptr  = (void *)&hp->decode_params;\n"
        "         n++;\n"
        "      }\n"
        "      if (ctx->has_ctrl_hevc_slice_params && hp->slice_count > 0) {\n"
        "         ctrls[n].id   = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS;\n"
        "         ctrls[n].size = sizeof(struct v4l2_ctrl_hevc_slice_params)\n"
        "                         * hp->slice_count;\n"
        "         ctrls[n].ptr  = (void *)hp->slice_params;\n"
        "         n++;\n"
        "      }\n"
        "      if (ctx->has_ctrl_hevc_ext_sps_st_rps && hp->st_rps_count > 0) {\n"
        "         ctrls[n].id   = V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS;\n"
        "         ctrls[n].size = sizeof(struct v4l2_ctrl_hevc_ext_sps_st_rps)\n"
        "                         * hp->st_rps_count;\n"
        "         ctrls[n].ptr  = (void *)hp->st_rps;\n"
        "         n++;\n"
        "      }\n"
        "      if (ctx->has_ctrl_hevc_ext_sps_lt_rps && hp->lt_rps_count > 0) {\n"
        "         ctrls[n].id   = V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS;\n"
        "         ctrls[n].size = sizeof(struct v4l2_ctrl_hevc_ext_sps_lt_rps)\n"
        "                         * hp->lt_rps_count;\n"
        "         ctrls[n].ptr  = (void *)hp->lt_rps;\n"
        "         n++;\n"
        "      }\n"
        "\n"
        "      fprintf(stderr,\n"
        '              "[V4L2VK][V4L2] HEVC S_EXT_CTRLS: video_fd=%d request_fd=%d count=%u\\n",\n'
        "              ctx->video_fd, request_fd, n);\n"
        "\n"
        "      struct v4l2_ext_controls ext_ctrls;\n"
        "      memset(&ext_ctrls, 0, sizeof(ext_ctrls));\n"
        "      ext_ctrls.which      = V4L2_CTRL_WHICH_REQUEST_VAL;\n"
        "      ext_ctrls.request_fd = request_fd;\n"
        "      ext_ctrls.count      = n;\n"
        "      ext_ctrls.controls   = ctrls;\n"
        "\n"
        "      if (xioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext_ctrls) < 0) {\n"
        "         int err = errno;\n"
        "         fprintf(stderr,\n"
        '                 "[V4L2VK][V4L2] HEVC S_EXT_CTRLS FAILED: %s (errno=%d) error_idx=%u\\n",\n'
        "                 strerror(err), err, ext_ctrls.error_idx);\n"
        "         for (uint32_t i = 0; i < n; i++) {\n"
        "            struct v4l2_ext_controls single;\n"
        "            memset(&single, 0, sizeof(single));\n"
        "            single.which      = V4L2_CTRL_WHICH_REQUEST_VAL;\n"
        "            single.request_fd = request_fd;\n"
        "            single.count      = 1;\n"
        "            single.controls   = &ctrls[i];\n"
        "            int rc = xioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &single);\n"
        "            fprintf(stderr,\n"
        '                    "[V4L2VK][V4L2]   ctrl 0x%08x (%s) size=%u: %s\\n",\n'
        "                    ctrls[i].id, v4l2vk_ctrl_name(ctrls[i].id),\n"
        "                    ctrls[i].size, rc < 0 ? strerror(errno) : \"OK\");\n"
        "         }\n"
        "         return -err;\n"
        "      }\n"
        '      fprintf(stderr, "[V4L2VK][V4L2] HEVC S_EXT_CTRLS: OK\\n");\n'
        "      return 0;\n"
        "   }\n"
        "\n"
        "   /* H264 (default): delegate unchanged to existing function */\n"
        "   return v4l2vk_v4l2_set_h264_controls(\n"
        "      ctx, request_fd,\n"
        "      (const struct v4l2vk_h264_frame_params *)params);\n"
        "}",
    ),
])

# ---------------------------------------------------------------------------
# Write all staged files atomically
# ---------------------------------------------------------------------------
for p, fname, content in pending:
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(content)
    os.replace(tmp, p)
    print(f"WROTE {fname}")

print(f"[hevc-patch-03-v4l2] DONE: wrote {len(pending)} file(s)")
