#!/usr/bin/env bash
# Run ON the SBC. Trace the in-tree golden v4l2slh265dec init S_EXT_CTRLS order.
set +e
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
rm -f /tmp/hevc-golden.strace
strace -f -e trace=ioctl -s 256 -o /tmp/hevc-golden.strace -- \
  gst-launch-1.0 filesrc location=hevc_case1.h265 ! h265parse ! v4l2slh265dec ! fakesink 2>/dev/null
# Show S_EXT_CTRLS ordering + the S_FMT/REQBUFS markers that bracket init
grep -nE 'VIDIOC_S_FMT|VIDIOC_S_EXT_CTRLS|VIDIOC_REQBUFS|VIDIOC_STREAMON|HEVC' /tmp/hevc-golden.strace | head -60
