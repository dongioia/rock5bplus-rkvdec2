# STEP-0: HEVC golden init control sequence

**Date**: 2026-06-22
**Golden decoder**: `v4l2slh265dec` (GStreamer `gst-plugins-bad`, kernel `rkvdec`, `/dev/video0`)
**Corpus**: `hevc_case1.h265` — Main 8-bit 1280x720, weightp=0 weightb=0 bframes=2
**Method**: `strace -f -e trace=ioctl -s 256` on the full gst-launch pipeline

---

## Corpus verification

```
codec_name=hevc
profile=Main
width=1280
height=720
has_b_frames=2
```

---

## Finding: one non-request control before CAPTURE setup

The golden `v4l2slh265dec` sets exactly **one** `S_EXT_CTRLS` with `which=CUR_VAL`
(non-request, `ctrl_class=0`) between `S_FMT(OUTPUT)` and CAPTURE buffer allocation.

### Init window (strace lines 164-214)

```
[164] VIDIOC_S_FMT OUTPUT  HEVC_SLICE 1280x720              ← session start
[165] VIDIOC_S_EXT_CTRLS  ctrl_class=0  count=1
        id=0xa40a90  (V4L2_CID_STATELESS_HEVC_SPS)  size=40   ← NON-REQUEST init
[166] VIDIOC_G_FMT CAPTURE → NV12 1280x720 sizeimage=1843200
      ... (ENUM_FMT, ENUM_FRAMESIZES) ...
      VIDIOC_CREATE_BUFS OUTPUT  x2
      VIDIOC_CREATE_BUFS CAPTURE x8
[213] VIDIOC_STREAMON OUTPUT
[214] VIDIOC_STREAMON CAPTURE
```

After `STREAMON`, the first per-request `S_EXT_CTRLS` carries 4 controls
(`ctrl_class=0xf010000`, request-fd based):

```
Frame 0 (IDR): count=4  ids=[SPS, PPS, SCALING_MATRIX, DECODE_PARAMS]
Frame 1 (P):   count=3  ids=[PPS, SCALING_MATRIX, DECODE_PARAMS]
```

---

## Ordered init sequence (non-request, before CAPTURE REQBUFS/CREATE_BUFS)

| Order | Control ID | Name | `which` | strace line |
|-------|-----------|------|---------|------------|
| 1 | `0xa40a90` | `V4L2_CID_STATELESS_HEVC_SPS` | `CUR_VAL` (ctrl_class=0) | 165 |

**That is the complete list.** Only SPS is set non-request at init.

---

## Control ID map (from QUERY_EXT_CTRL probing at fd open, strace lines 83-89)

```
V4L2_CTRL_CLASS_CODEC_STATELESS = 0x00a40000

0xa40a90  V4L2_CID_STATELESS_HEVC_SPS              type=HEVC_SPS             size=40
0xa40a91  V4L2_CID_STATELESS_HEVC_PPS              type=HEVC_PPS             size=64
0xa40a92  (no VPS control — not queried, not present in V4L2 HEVC stateless API)
0xa40a93  V4L2_CID_STATELESS_HEVC_SCALING_MATRIX   type=HEVC_SCALING_MATRIX  size=1000
0xa40a94  V4L2_CID_STATELESS_HEVC_DECODE_PARAMS    type=HEVC_DECODE_PARAMS   size=328
0xa40a98  V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS   type=HEVC_EXT_SPS_ST_RPS  size=80
0xa40a99  V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS   type=HEVC_EXT_SPS_LT_RPS  size=4
```

Note: VPS (`0xa40a92`) is absent from the V4L2 stateless HEVC API. VPS data is
embedded in `struct v4l2_ctrl_hevc_sps` by the driver — no separate VPS control exists.

---

## Comparison with H.264 B0 finding

In the H.264 case the golden `v4l2slh264dec` also set exactly one control non-request
at init: `V4L2_CID_STATELESS_H264_SPS`.

HEVC mirrors this exactly: one non-request init call, also the SPS.

**ICD implication for Task 8 (`set_init_paramset`)**: call
`VIDIOC_S_EXT_CTRLS` with `which=V4L2_CTRL_WHICH_CUR_VAL` (i.e. `ctrl_class=0`),
`count=1`, `controls[0].id = V4L2_CID_STATELESS_HEVC_SPS`, immediately after
`S_FMT(OUTPUT, HEVC_SLICE)` and before CAPTURE `G_FMT`/`CREATE_BUFS`/`STREAMON`.

---

## Raw strace excerpt (key init window)

```
71676 ioctl(7, VIDIOC_S_FMT, {OUTPUT, HEVC_SLICE, 1280x720}) = 0
71676 ioctl(7, VIDIOC_S_EXT_CTRLS,
  {ctrl_class=0, count=1,
   controls=[{id=0xa40a90, size=40,
              string="\0\0\0\5\320\2\0\0\4\4\2\3\0\3\0\3\0\0\0\0"
                     "\0\0\0\0\1\0\0\0\0\0\0\0\210\1\0\0\0\0\0\0"}]}) = 0
71676 ioctl(7, VIDIOC_G_FMT, {CAPTURE, NV12, 1280x720, sizeimage=1843200}) = 0
   ... CREATE_BUFS OUTPUT x2, CREATE_BUFS CAPTURE x8 ...
71676 ioctl(7, VIDIOC_STREAMON, [V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE]) = 0
71676 ioctl(7, VIDIOC_STREAMON, [V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE]) = 0
71676 ioctl(7, VIDIOC_S_EXT_CTRLS,
  {ctrl_class=0xf010000, count=4,       ← first per-request frame
   controls=[{0xa40a90 SPS}, {0xa40a91 PPS},
             {0xa40a93 SCALING_MATRIX}, {0xa40a94 DECODE_PARAMS}]}) = 0
```
