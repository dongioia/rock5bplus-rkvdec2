#!/usr/bin/env python3
"""B0 fix: set SPS non-request at V4L2 init + read native CAPTURE format (G_FMT).

Root cause: artifacts/phase-b0/compare/ROOT-CAUSE.md. The ICD never set the SPS
control at session init, so rkvdec was not decoder-configured -> blank decode,
even though per-request controls + bitstream were byte-identical to the golden
v4l2slh264dec. Golden sets SPS non-request after S_FMT(OUTPUT), before CAPTURE
format + buffer allocation. This patch replicates that:
  1. h264.c/.h : public v4l2vk_h264_translate_sps_pps() wrapper.
  2. v4l2.c/.h : v4l2vk_v4l2_set_init_sps() (non-request S_EXT_CTRLS SPS);
                 set_capture_format S_FMT -> G_FMT (read driver's native format).
  3. vk_video.c/.h, vk_device.c : plumb SPS/PPS into session_init_v4l2 and set
                 the init SPS between S_FMT(OUTPUT) and CAPTURE setup.

Collects + asserts every anchor first, then writes all files (atomic across the
set). Idempotent (each file guarded). Run in the build container:
  python3 /vvtest/b0-fix-init-sps.py /work/mesa-sree/mesa/src/vulkan-v4l2
"""
import os
import sys

DIR = sys.argv[1]
pending = []  # (path, new_content)


def stage(fname, guard, edits):
    p = os.path.join(DIR, fname)
    s = open(p).read()
    if guard in s:
        print(f"{fname}: already patched, skipping")
        return
    for a, b in edits:
        assert a in s, f"{fname}: ANCHOR NOT FOUND: {a[:70]!r}"
        assert s.count(a) == 1, f"{fname}: ANCHOR NOT UNIQUE ({s.count(a)}x): {a[:70]!r}"
        s = s.replace(a, b, 1)
    pending.append((p, s))
    print(f"{fname}: staged")


# 1. h264.c — public translate_sps_pps wrapper (before scaling translation)
stage("v4l2vk_v4l2_h264.c", "v4l2vk_h264_translate_sps_pps", [(
    "/* --- Scaling matrix translation --- */",
    "/* --- B0 fix: public SPS+PPS translate wrapper (init non-request set) --- */\n"
    "void\n"
    "v4l2vk_h264_translate_sps_pps(const StdVideoH264SequenceParameterSet *vk_sps,\n"
    "                             const StdVideoH264PictureParameterSet *vk_pps,\n"
    "                             struct v4l2vk_h264_frame_params *out)\n"
    "{\n"
    "   v4l2vk_h264_translate_sps(vk_sps, &out->sps);\n"
    "   v4l2vk_h264_translate_pps(vk_pps, &out->pps);\n"
    "}\n\n"
    "/* --- Scaling matrix translation --- */",
)])

# 2. h264.h — declare it (before translate_params)
stage("v4l2vk_v4l2_h264.h", "v4l2vk_h264_translate_sps_pps", [(
    "int v4l2vk_h264_translate_params(const StdVideoH264SequenceParameterSet *vk_sps,",
    "void v4l2vk_h264_translate_sps_pps(\n"
    "   const StdVideoH264SequenceParameterSet *vk_sps,\n"
    "   const StdVideoH264PictureParameterSet *vk_pps,\n"
    "   struct v4l2vk_h264_frame_params *out);\n\n"
    "int v4l2vk_h264_translate_params(const StdVideoH264SequenceParameterSet *vk_sps,",
)])

# 3. v4l2.c — G_FMT CAPTURE + set_init_sps()
stage("v4l2vk_v4l2.c", "v4l2vk_v4l2_set_init_sps", [
    (
        '   if (xioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt) < 0) {\n'
        '      V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] S_FMT CAPTURE failed: %s\\n",',
        '   /* B0 fix: read the driver native CAPTURE format (G_FMT), do not force */\n'
        '   if (xioctl(ctx->video_fd, VIDIOC_G_FMT, &fmt) < 0) {\n'
        '      V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] G_FMT CAPTURE failed: %s\\n",',
    ),
    (
        "/* --- Streaming --- */",
        "/* B0 fix: set SPS NON-request at init so rkvdec configures its decoder and\n"
        " * computes the native CAPTURE layout (golden v4l2slh264dec does this before\n"
        " * CAPTURE setup). Without it the decoder is unconfigured -> blank decode. */\n"
        "int\n"
        "v4l2vk_v4l2_set_init_sps(struct v4l2vk_v4l2_context *ctx,\n"
        "                         const struct v4l2vk_h264_frame_params *params)\n"
        "{\n"
        "   struct v4l2_ext_control ctrl;\n"
        "   memset(&ctrl, 0, sizeof(ctrl));\n"
        "   ctrl.id = V4L2_CID_STATELESS_H264_SPS;\n"
        "   ctrl.size = sizeof(params->sps);\n"
        "   ctrl.ptr = (void *)&params->sps;\n\n"
        "   struct v4l2_ext_controls ext;\n"
        "   memset(&ext, 0, sizeof(ext));\n"
        "   ext.which = V4L2_CTRL_WHICH_CUR_VAL;\n"
        "   ext.count = 1;\n"
        "   ext.controls = &ctrl;\n\n"
        "   if (xioctl(ctx->video_fd, VIDIOC_S_EXT_CTRLS, &ext) < 0) {\n"
        '      V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] init SPS S_EXT_CTRLS failed: %s\\n",\n'
        "                       strerror(errno));\n"
        "      return -errno;\n"
        "   }\n"
        '   V4L2VK_LOGI_V4L2("[V4L2VK][V4L2] init SPS set (non-request) OK\\n");\n'
        "   return 0;\n"
        "}\n\n"
        "/* --- Streaming --- */",
    ),
])

# 4. v4l2.h — declare set_init_sps
stage("v4l2vk_v4l2.h", "v4l2vk_v4l2_set_init_sps", [(
    "v4l2vk_v4l2_set_h264_controls(struct v4l2vk_v4l2_context *ctx, int request_fd,\n"
    "                              const struct v4l2vk_h264_frame_params *params);",
    "v4l2vk_v4l2_set_h264_controls(struct v4l2vk_v4l2_context *ctx, int request_fd,\n"
    "                              const struct v4l2vk_h264_frame_params *params);\n\n"
    "int v4l2vk_v4l2_set_init_sps(struct v4l2vk_v4l2_context *ctx,\n"
    "                            const struct v4l2vk_h264_frame_params *params);",
)])

# 5. vk_video.h — extend session_init_v4l2 signature
stage("v4l2vk_vk_video.h", "const StdVideoH264SequenceParameterSet *sps", [(
    "int v4l2vk_video_session_init_v4l2(v4l2vk_video_session *sess,\n"
    "                                   struct v4l2vk_vk_device *dev,\n"
    "                                   uint32_t w, uint32_t h);",
    "int v4l2vk_video_session_init_v4l2(v4l2vk_video_session *sess,\n"
    "                                   struct v4l2vk_vk_device *dev,\n"
    "                                   uint32_t w, uint32_t h,\n"
    "                                   const StdVideoH264SequenceParameterSet *sps,\n"
    "                                   const StdVideoH264PictureParameterSet *pps);",
)])

# 6. vk_video.c — include, def signature, body split + init SPS, caller
stage("v4l2vk_vk_video.c", "v4l2vk_v4l2_set_init_sps", [
    (
        '#include "v4l2vk_vk_video.h"',
        '#include "v4l2vk_vk_video.h"\n#include "v4l2vk_v4l2.h"\n#include "v4l2vk_v4l2_h264.h"',
    ),
    (
        "v4l2vk_video_session_init_v4l2(v4l2vk_video_session *sess,\n"
        "                               struct v4l2vk_vk_device *dev,\n"
        "                               uint32_t w, uint32_t h)\n"
        "{",
        "v4l2vk_video_session_init_v4l2(v4l2vk_video_session *sess,\n"
        "                               struct v4l2vk_vk_device *dev,\n"
        "                               uint32_t w, uint32_t h,\n"
        "                               const StdVideoH264SequenceParameterSet *sps,\n"
        "                               const StdVideoH264PictureParameterSet *pps)\n"
        "{",
    ),
    (
        "   if (v4l2vk_v4l2_set_output_format(sess->v4l2_ctx, w, h) < 0 ||\n"
        "       v4l2vk_v4l2_set_capture_format(sess->v4l2_ctx, w, h, NULL, NULL) < 0) {\n"
        '      fprintf(stderr, "[V4L2VK][ERR] V4L2 format setup failed\\n");\n'
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }",
        "   if (v4l2vk_v4l2_set_output_format(sess->v4l2_ctx, w, h) < 0) {\n"
        '      fprintf(stderr, "[V4L2VK][ERR] V4L2 OUTPUT format setup failed\\n");\n'
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }\n\n"
        "   /* B0 fix: set SPS non-request BEFORE CAPTURE format/buffers. */\n"
        "   if (sps && pps) {\n"
        "      struct v4l2vk_h264_frame_params init_params;\n"
        "      memset(&init_params, 0, sizeof(init_params));\n"
        "      v4l2vk_h264_translate_sps_pps(sps, pps, &init_params);\n"
        "      if (v4l2vk_v4l2_set_init_sps(sess->v4l2_ctx, &init_params) < 0) {\n"
        '         fprintf(stderr, "[V4L2VK][ERR] V4L2 init SPS set failed\\n");\n'
        "         v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "         sess->v4l2_ctx = NULL;\n"
        "         return -1;\n"
        "      }\n"
        "   }\n\n"
        "   if (v4l2vk_v4l2_set_capture_format(sess->v4l2_ctx, w, h, NULL, NULL) < 0) {\n"
        '      fprintf(stderr, "[V4L2VK][ERR] V4L2 CAPTURE format setup failed\\n");\n'
        "      v4l2vk_v4l2_context_destroy(sess->v4l2_ctx);\n"
        "      sess->v4l2_ctx = NULL;\n"
        "      return -1;\n"
        "   }",
    ),
    (
        "      const StdVideoH264SequenceParameterSet *sps =\n"
        "         &h264_ci->pParametersAddInfo->pStdSPSs[0];",
        "      const StdVideoH264SequenceParameterSet *sps =\n"
        "         &h264_ci->pParametersAddInfo->pStdSPSs[0];\n"
        "      const StdVideoH264PictureParameterSet *pps =\n"
        "         (h264_ci->pParametersAddInfo->stdPPSCount > 0 &&\n"
        "          h264_ci->pParametersAddInfo->pStdPPSs)\n"
        "            ? &h264_ci->pParametersAddInfo->pStdPPSs[0]\n"
        "            : NULL;",
    ),
    (
        "if (v4l2vk_video_session_init_v4l2(sess, dev, w, h) < 0) {",
        "if (v4l2vk_video_session_init_v4l2(sess, dev, w, h, sps, pps) < 0) {",
    ),
])

# 7. vk_device.c — lazy caller passes job SPS/PPS
stage("v4l2vk_vk_device.c", "&job->sps, &job->pps);", [(
    "v4l2vk_video_session_init_v4l2(sess, dev, sps_w, sps_h);",
    "v4l2vk_video_session_init_v4l2(sess, dev, sps_w, sps_h, &job->sps, &job->pps);",
)])

for p, s in pending:
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        f.write(s)
    os.replace(tmp, p)
print(f"WROTE {len(pending)} file(s)")
