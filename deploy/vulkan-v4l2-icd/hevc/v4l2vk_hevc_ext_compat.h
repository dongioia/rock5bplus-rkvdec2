/* SPDX-License-Identifier: MIT */
/* Vendored mainline V4L2 HEVC EXT_SPS RPS uAPI for build envs whose
 * linux/v4l2-controls.h predates it (container header stops at +407).
 * Field layout copied verbatim from kernel 7.1 linux/v4l2-controls.h.
 * All guarded — a newer system header wins. */
#ifndef V4L2VK_HEVC_EXT_COMPAT_H
#define V4L2VK_HEVC_EXT_COMPAT_H
#include <linux/v4l2-controls.h>
#include <linux/types.h>

#ifndef V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS
#define V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS  (V4L2_CID_CODEC_STATELESS_BASE + 408)
#define V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS  (V4L2_CID_CODEC_STATELESS_BASE + 409)

#define V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED	0x1

/*
 * struct v4l2_ctrl_hevc_ext_sps_st_rps - HEVC short term RPS parameters
 *
 * Dynamic size 1-dimension array for short term RPS. The number of elements
 * is v4l2_ctrl_hevc_sps::num_short_term_ref_pic_sets. It can contain up to 65 elements.
 *
 * @delta_idx_minus1: Specifies the delta compare to the index. See details in section 7.4.8
 *                    "Short-term reference picture set semantics" of the specification.
 * @delta_rps_sign: Sign of the delta as specified in section 7.4.8 "Short-term reference picture
 *                  set semantics" of the specification.
 * @abs_delta_rps_minus1: Absolute delta RPS as specified in section 7.4.8 "Short-term reference
 *                        picture set semantics" of the specification.
 * @num_negative_pics: Number of short-term RPS entries that have picture order count values less
 *                     than the picture order count value of the current picture.
 * @num_positive_pics: Number of short-term RPS entries that have picture order count values
 *                     greater than the picture order count value of the current picture.
 * @used_by_curr_pic: Bit j specifies if short-term RPS j is used by the current picture.
 * @use_delta_flag: Bit j equals to 1 specifies that the j-th entry in the source candidate
 *                  short-term RPS is included in this candidate short-term RPS.
 * @delta_poc_s0_minus1: Specifies the negative picture order count delta for the i-th entry in
 *                       the short-term RPS. See details in section 7.4.8 "Short-term reference
 *                       picture set semantics" of the specification.
 * @delta_poc_s1_minus1: Specifies the positive picture order count delta for the i-th entry in
 *                       the short-term RPS. See details in section 7.4.8 "Short-term reference
 *                       picture set semantics" of the specification.
 * @flags: See V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_{}
 */
struct v4l2_ctrl_hevc_ext_sps_st_rps {
	__u8	delta_idx_minus1;
	__u8	delta_rps_sign;
	__u8	num_negative_pics;
	__u8	num_positive_pics;
	__u32	used_by_curr_pic;
	__u32	use_delta_flag;
	__u16	abs_delta_rps_minus1;
	__u16	delta_poc_s0_minus1[16];
	__u16	delta_poc_s1_minus1[16];
	__u16	flags;
};

#define V4L2_HEVC_EXT_SPS_LT_RPS_FLAG_USED_LT		0x1

/*
 * struct v4l2_ctrl_hevc_ext_sps_lt_rps - HEVC long term RPS parameters
 *
 * Dynamic size 1-dimension array for long term RPS. The number of elements
 * is v4l2_ctrl_hevc_sps::num_long_term_ref_pics_sps. It can contain up to 65 elements.
 *
 * @lt_ref_pic_poc_lsb_sps: picture order count modulo MaxPicOrderCntLsb of the i-th candidate
 *                          long-term reference picture.
 * @flags: See V4L2_HEVC_EXT_SPS_LT_RPS_FLAG_{}
 */
struct v4l2_ctrl_hevc_ext_sps_lt_rps {
	__u16	lt_ref_pic_poc_lsb_sps;
	__u16	flags;
};

#endif /* V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS */
#endif /* V4L2VK_HEVC_EXT_COMPAT_H */
