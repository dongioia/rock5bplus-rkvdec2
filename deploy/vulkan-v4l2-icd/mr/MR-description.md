# Mesa MR — sree/mesa (V4L2-Vulkan wrapper ICD)

**Target:** `sree/mesa`, on top of `5955e6e`. Invited by @sree on issue #14987.
**Patches:** `0001`/`0002`/`0003` in this directory (`git am` them onto a fresh `5955e6e` checkout of your fork, push, open the MR).

---

## MR title

v4l2-video: HEVC Main 8-bit decode, plus H.264 init-SPS and NV12 stride fixes

## MR description (paste into the GitLab MR body)

Matches the channel's house style (terse; `### What does this MR do and why?` template, as in e.g. !42210 and !42339; commits carry the detail).

```
### What does this MR do and why?

Follow-up to #14987 — three changes I've been running on an RK3588 (Rock 5B+),
on top of 5955e6e:

- v4l2-video: set the H.264 SPS non-request at session init (the blank-decode
  fix from note 3528237). rkvdec needs the SPS once as a plain control before
  CAPTURE setup, otherwise it writes a blank frame.
- v4l2-video: take the NV12 readback stride from the image layout rather than
  rounding the width up to 256, so non-256-multiple widths (640 etc.) stop
  tearing.
- v4l2-video: add HEVC Main 8-bit 4:2:0 decode — advertise
  VK_KHR_video_decode_h265, translate the std structs to the V4L2 HEVC controls
  (incl. the EXT_SPS ST/LT RPS VDPU381 needs), and parse the slice header.

Byte-exact against ffmpeg on RK3588 for H.264 and HEVC across I/P/B, more than
one short-term RPS, and 360p/720p/1080p including cropped sizes; both also play
in WebKitGTK through a small GStreamer bridge. Not conformance-tested.

Scope is the working changes, as you suggested: Main 8-bit 4:2:0. Weighted
prediction isn't parsed and the long-term-reference DPB flag isn't wired yet
(no test stream used long-term references). Happy to iterate.
```

(The GStreamer bridge and the byte-exact harness live outside this MR; available on request.)

---

## Notes (not for the MR body)

- Each commit carries `Signed-off-by: Saverio Pavone <pavone.lawyer@gmail.com>` (DCO) + `Assisted-by: Claude:claude-opus-4-8` (AI-disclosure per Mesa policy; @sree confirmed AI is a non-issue). No `Co-authored-by`.
- Keep the body plain-text on GitLab; don't claim conformance.
- Patches assembled + reviewed (kernel-patch-reviewer pre-flight) from `5955e6e`; tree compile-proven + byte-exact this session.
