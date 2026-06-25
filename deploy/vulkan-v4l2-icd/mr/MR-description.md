# Mesa MR — sree/mesa (V4L2-Vulkan wrapper ICD)

**Target:** `sree/mesa`, on top of `5955e6e`. Invited by @sree on issue #14987.
**Patches:** `0001`/`0002`/`0003` in this directory (`git am` them onto a fresh `5955e6e` checkout of your fork, push, open the MR).

---

## MR title

v4l2-video: init-SPS fix, NV12 stride fix, HEVC Main 8-bit decode

## MR description (paste into the GitLab MR body)

Following up on #14987 — the changes I've been running on an RK3588 (Radxa Rock 5B+), on top of `5955e6e`.

- **H.264 SPS set non-request at session init** — the blank-decode fix I reported earlier (note 3528237). rkvdec wants the SPS once as a plain control before CAPTURE setup; without it the per-request controls match v4l2slh264dec but the hardware writes a blank frame.
- **NV12 readback stride taken from the image layout** — instead of rounding the width up to 256, so non-256-multiple widths (640, etc.) stop tearing.
- **HEVC Main 8-bit 4:2:0 decode** — advertise `VK_KHR_video_decode_h265`, translate the std structures to the V4L2 HEVC controls (including the EXT_SPS ST/LT RPS that VDPU381 needs), and parse the slice segment header. The bit-reader and start-code helpers are factored out of the H.264 parser into a shared header.

Validation on RK3588: byte-exact against ffmpeg on H.264 and HEVC across I/P/B, more than one short-term RPS, and 360p/720p/1080p including cropped sizes; both also play in WebKitGTK (Epiphany) through a small GStreamer bridge. Not conformance-tested.

Scope is the working changes, as you suggested: Main profile, 8-bit, 4:2:0. Weighted prediction isn't parsed, and the long-term-reference DPB flag isn't wired yet (no test stream used long-term references). Happy to iterate from here.

The GStreamer bridge and the byte-exact test harness live outside this MR; I can share them if they're useful.

---

## Notes (not for the MR body)

- Each commit carries `Signed-off-by: Saverio Pavone <pavone.lawyer@gmail.com>` (DCO) + `Assisted-by: Claude:claude-opus-4-8` (AI-disclosure per Mesa policy; @sree confirmed AI is a non-issue). No `Co-authored-by`.
- Keep the body plain-text on GitLab; don't claim conformance.
- Patches assembled + reviewed (kernel-patch-reviewer pre-flight) from `5955e6e`; tree compile-proven + byte-exact this session.
