#!/usr/bin/env bash
# Task 10 — drive HEVC control payloads to byte-exact vs the in-tree golden.
#
# Run ON the SBC. Captures the per-request VIDIOC_S_EXT_CTRLS payloads from:
#   golden = v4l2slh265dec  (gst-plugins-bad, kernel rkvdec)
#   ours   = vulkanh265dec  (our V4L2-Vulkan ICD)
# for the same clip, then byte-diffs each control id (B0 method).
#
# strace -s MUST exceed the largest control payload or the byte-diff is bogus:
#   HEVC SCALING_MATRIX = 1000 B, DECODE_PARAMS = 328 B  ->  -s 2048.
# (Step-0 used -s 256 because it only needed presence/order, not full bytes.)
#
# Needs in ~/vvtest: the clip, v4l2vk_icd.aarch64.json, the freshly-scp'd
# libvulkan_v4l2_video.so, and strace_ctrl_diff.py.
set +e
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
CLIP="${1:-hevc_case1.h265}"
SVAL="${STRACE_S:-2048}"
G=/tmp/hevc-golden.strace
O=/tmp/hevc-ours.strace

[ -f "$CLIP" ] || { echo "FATAL: clip $CLIP not in $PWD"; exit 2; }
[ -f strace_ctrl_diff.py ] || { echo "FATAL: strace_ctrl_diff.py not in $PWD"; exit 2; }
command -v strace >/dev/null || { echo "FATAL: strace not installed on SBC"; exit 2; }

echo "== [1/3] golden v4l2slh265dec trace (clip=$CLIP, -s $SVAL) =="
rm -f "$G"
strace -f -e trace=ioctl -s "$SVAL" -o "$G" -- \
  gst-launch-1.0 filesrc location="$CLIP" ! h265parse ! v4l2slh265dec ! fakesink \
  >/dev/null 2>&1
gn=$(grep -c 'VIDIOC_S_EXT_CTRLS' "$G" 2>/dev/null)
echo "   golden S_EXT_CTRLS calls: ${gn:-0}"

echo "== [2/3] ours vulkanh265dec trace (env-var ICD) =="
rm -f "$O" ~/.cache/gstreamer-1.0/registry.aarch64.bin
VK_ICD_FILENAMES="$PWD/v4l2vk_icd.aarch64.json" GST_PLUGIN_PATH="$PWD" \
strace -f -e trace=ioctl -s "$SVAL" -o "$O" -- \
  gst-launch-1.0 filesrc location="$CLIP" ! h265parse ! vulkanh265dec ! fakesink \
  >/dev/null 2>&1
on=$(grep -c 'VIDIOC_S_EXT_CTRLS' "$O" 2>/dev/null)
echo "   ours   S_EXT_CTRLS calls: ${on:-0}"

# Fail LOUD on any decode that produced no controls, errored mid-stream, or stopped
# short — each makes the diff compare an incomplete trace against a complete one
# (a false signal that the 0-call guard alone would miss).
fail=""
[ "${gn:-0}" -eq 0 ] && fail="golden captured 0 S_EXT_CTRLS"
[ "${on:-0}" -eq 0 ] && fail="ours captured 0 S_EXT_CTRLS"
oerr=$(grep -cE 'VIDIOC_(S_EXT_CTRLS|QBUF|STREAMON|DQBUF)[^=]*= -1' "$O" 2>/dev/null)
[ "${oerr:-0}" -gt 0 ] && fail="ours hit ${oerr} failing decode-path ioctl(s) (EINVAL/etc) — partial decode"
delta=$(( ${on:-0} - ${gn:-0} )); adelta=${delta#-}
[ "${adelta:-0}" -gt 5 ] && fail="S_EXT_CTRLS count gap ours=$on golden=$gn (>5) — likely partial decode"
if [ -n "$fail" ]; then
  echo "FATAL: $fail"
  echo "  golden tail:"; grep -nE 'VIDIOC_S_FMT|= -1|EINVAL|ENOENT' "$G" | tail -6
  echo "  ours   tail:"; grep -nE 'VIDIOC_S_FMT|= -1|EINVAL|ENOENT' "$O" | tail -6
  exit 1
fi

echo "== [3/3] per-control byte-diff (POC-aligned: same frame ours vs golden) =="
# POC alignment is required: golden decodes the IDR from init/CUR controls and
# emits NO per-request S_EXT_CTRLS for it, so a naive first-frame compare pits
# ours-IDR against golden-P. --poc keys frames by decode_params pic_order_cnt_val.
python3 strace_ctrl_diff.py "$O" "$G" --poc
rc=$?

echo "--- presence/order sanity (init + first request) ---"
echo "golden:"; grep -aoE 'id=0xa40a9[0-9]' "$G" | sort | uniq -c
echo "ours:";   grep -aoE 'id=0xa40a9[0-9]' "$O" | sort | uniq -c
echo
echo "Green target: every shared control 'IDENTICAL', no PRESENCE diff."
echo "Known watch (STEP0): golden frame-0 = {SPS,PPS,SCALING,DECODE_PARAMS};"
echo "  if ours adds SLICE_PARAMS (0xa40a92) or drops SCALING (0xa40a93) -> fix the control SET first."
exit $rc
