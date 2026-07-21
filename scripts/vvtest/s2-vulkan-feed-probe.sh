#!/usr/bin/env bash
# Run ON the SBC. Probes whether the Vulkan feed (our ICD) negotiates to a sink.
set +e
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
export VK_ICD_FILENAMES="$HOME/vvtest/v4l2vk_icd.aarch64.json"
# Clear the GST plugin cache so libgstvulkan.so is rescanned with our ICD loaded
# (without VK_ICD_FILENAMES the plugin scanner cannot enumerate VK_KHR_video_decode_h264
# and caches the whole plugin as blacklisted; with it set the scanner registers vulkanh264dec)
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
GD='2,GST_ELEMENT_FACTORY:4'
runone() { # $1=label  $2..=pipeline
  local label="$1"; shift
  echo "=== $label ==="
  GST_DEBUG="$GD" timeout 25 gst-launch-1.0 "$@" 2>&1 \
    | sed -E 's/\x1b\[[0-9;]*m//g' \
    | grep -iE 'PLAYING|Got EOS|not-negotiat|VideoMeta|Failed to negotiate|vulkanh264dec|error' | tail -10
}
# A) zero-copy Vulkan present (our ICD decode -> Mali vulkansink = cross-device)
runone "A vulkansink"     filesrc location=case1.h264 ! h264parse ! vulkanh264dec ! vulkansink
# B) CPU-copy download then GL (B0-style)
runone "B vulkandownload" filesrc location=case1.h264 ! h264parse ! vulkanh264dec ! vulkandownload ! videoconvert ! glimagesink
# C) raw fakesink (isolate decode from any sink)
runone "C fakesink"       filesrc location=case1.h264 ! h264parse ! vulkanh264dec ! vulkandownload ! fakesink
