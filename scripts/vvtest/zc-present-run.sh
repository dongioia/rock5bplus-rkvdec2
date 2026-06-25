#!/usr/bin/env bash
# Phase-C Step-2 Increment-1 runner (ON the SBC): independent ffmpeg NV12 ref +
# compile shaders + build + run zc_present_test against pinned !42353 PanVK,
# asserting the system mesa pin is untouched. sub-gate 2a (graphics ycbcr).
set -uo pipefail
cd "$HOME/vvtest" || { echo "no ~/vvtest"; exit 1; }
CLIP="${1:-case1.h264}"
REF="ref_${CLIP%.*}_f0.nv12"
ICD="$HOME/mesa-zc/panfrost_icd.json"
PIN="1:26.0.6-1"
pin() { pacman -Q mesa 2>/dev/null | awk '{print $2}'; }

echo "== mesa pin PRE: $(pin) (want $PIN) =="
[ "$(pin)" = "$PIN" ] || { echo "ABORT: system mesa not $PIN"; exit 1; }

if [ "${2:-}" = "-f" ] || [ ! -f "$REF" ]; then
  echo "== ffmpeg independent ref -> $REF =="
  ffmpeg -nostdin -v error -i "$CLIP" -frames:v 1 -pix_fmt nv12 -f rawvideo "$REF" || { echo "ffmpeg ref FAIL"; exit 1; }
fi
echo "ref: $(ls -l "$REF" | awk '{print $5, $9}')"

echo "== compile shaders =="
glslangValidator -V zc_present.vert -o zc_present_vert.spv || { echo "VERT_FAIL"; exit 1; }
glslangValidator -V zc_present.frag -o zc_present_frag.spv || { echo "FRAG_FAIL"; exit 1; }

echo "== build =="
cc zc_present_test.c -o zc_present_test \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0) \
  -lvulkan -lm || { echo "BUILD_FAIL"; exit 1; }

echo "== run (VK_ICD_FILENAMES=$ICD) =="
VK_ICD_FILENAMES="$ICD" ./zc_present_test "$CLIP" "$REF"
RC=$?
echo "== mesa pin POST: $(pin) =="
[ "$(pin)" = "$PIN" ] || { echo "WARN: system mesa CHANGED"; RC=99; }
echo "present-run exit=$RC"
exit $RC
