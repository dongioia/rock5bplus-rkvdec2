#!/usr/bin/env bash
# Stage-3 multi-resolution byte-exact gate.  Run ON the SBC.
# Decodes each clip through the vkh264bridge (the browser path) and byte-compares
# the LUMA plane of frame-0 against the ffmpeg reference.  Luma is the most
# sensitive probe for a row-stride bug.
#
# Pre-fix expectation (RED): 1280x720 PASS, 640x352 / 640x360 / 1920x1080 FAIL.
# Post-fix expectation (GREEN): all PASS.
set +e
cd "$HOME/vvtest" || exit 2
export VK_ICD_FILENAMES="$HOME/vvtest/v4l2vk_icd.aarch64.json"
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
export GST_PLUGIN_PATH="$HOME/vvtest"

# Regenerate the synthetic clips from the real 1080p source (deterministic geometry).
gen() { # $1=W $2=H $3=name
  ffmpeg -y -loglevel error -i bbb1080.mp4 -t 2 -an -c:v libx264 -profile:v main \
    -pix_fmt yuv420p -s ${1}x${2} -g 30 ${3}.mp4
  ffmpeg -y -loglevel error -i ${3}.mp4 -c:v copy -bsf:v h264_mp4toannexb ${3}.h264
}
[ -f c352.mp4 ] || gen 640 352 c352
gen 640 360 c360
gen 1920 1080 c1080

gate() { # $1=h264  $2=mp4-for-ref  $3=W  $4=H  $5=label
  local LUMA=$(( $3 * $4 ))
  ffmpeg -y -loglevel error -i "$2" -pix_fmt nv12 -f rawvideo /tmp/gref.nv12
  timeout 30 gst-launch-1.0 filesrc location="$1" ! h264parse ! vkh264bridge \
    ! filesink location=/tmp/gout.nv12 >/dev/null 2>&1
  head -c $LUMA /tmp/gref.nv12 > /tmp/gref_y.bin
  head -c $LUMA /tmp/gout.nv12 > /tmp/gout_y.bin
  if cmp -s /tmp/gref_y.bin /tmp/gout_y.bin; then
    echo "  ${5} (${3}x${4}): PASS (luma byte-exact)"
  else
    local FIRST=$(cmp /tmp/gref_y.bin /tmp/gout_y.bin 2>&1 | sed 's/.*differ: //')
    echo "  ${5} (${3}x${4}): FAIL  [${FIRST}]"
  fi
}

echo "=== Stage-3 multi-resolution luma byte-exact gate ==="
gate case1.h264 case1.mp4 1280 720 "case1-control"
gate c352.h264  c352.mp4   640 352 "no-crop-640"
gate c360.h264  c360.mp4   640 360 "crop8-640"
gate c1080.h264 c1080.mp4 1920 1080 "fullhd-1080"
