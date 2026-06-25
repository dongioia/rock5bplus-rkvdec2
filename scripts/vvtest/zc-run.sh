#!/usr/bin/env bash
# Phase-C Stage-1 runner (ON the SBC). Generates an INDEPENDENT ffmpeg NV12 ref
# (SW decode, not rkvdec / not the GstVideoMeta the test reads), builds and runs
# zc_import_test against the pinned !42353 PanVK, and asserts the system mesa pin
# is untouched before and after.
set -uo pipefail
cd "$HOME/vvtest" || { echo "no ~/vvtest"; exit 1; }

CLIP="${1:-case1.h264}"
REF="ref_${CLIP%.*}_f0.nv12"
ICD="$HOME/mesa-zc/panfrost_icd.json"
PIN="1:26.0.6-1"

pin() { pacman -Q mesa 2>/dev/null | awk '{print $2}'; }

echo "== mesa pin PRE: $(pin) (want $PIN) =="
[ "$(pin)" = "$PIN" ] || { echo "ABORT: system mesa not at $PIN"; exit 1; }

# Independent canonical-linear NV12 reference: ffmpeg's own SW H.264/H.265
# decode of display-order frame 0, packed NV12. Regenerate with -f to refresh.
if [ "${2:-}" = "-f" ] || [ ! -f "$REF" ]; then
  echo "== ffmpeg independent ref -> $REF =="
  ffmpeg -nostdin -v error -i "$CLIP" -frames:v 1 -pix_fmt nv12 -f rawvideo "$REF" || { echo "ffmpeg ref FAIL"; exit 1; }
fi
echo "ref: $(ls -l "$REF" | awk '{print $5, $9}')"

echo "== compile compute shader (1b) =="
glslangValidator -V zc.comp -o zc_comp.spv || { echo "SHADER_FAIL"; exit 1; }

echo "== build =="
cc zc_import_test.c -o zc_import_test \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0) \
  -lvulkan -lm || { echo "BUILD_FAIL"; exit 1; }

echo "== run (VK_ICD_FILENAMES=$ICD) =="
VK_ICD_FILENAMES="$ICD" ./zc_import_test "$CLIP" "$REF"
RC=$?

echo "== mesa pin POST: $(pin) =="
[ "$(pin)" = "$PIN" ] || { echo "WARN: system mesa CHANGED"; RC=99; }
echo "zc-run exit=$RC"
exit $RC
