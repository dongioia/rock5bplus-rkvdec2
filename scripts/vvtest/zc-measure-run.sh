#!/usr/bin/env bash
# Phase-C Step-2 finish: A/B measurement zero-copy vs CPU-copy (vulkandownload).
# Runs both modes over the same clip; CPU is the headline metric (the readback
# burns CPU/bandwidth; zero-copy doesn't). Off-screen (no FIFO) = unclamped.
set -uo pipefail
cd "$HOME/vvtest" || { echo "no ~/vvtest"; exit 1; }
CLIP="${1:-demo.h264}"; N="${2:-1000}"
ICD="$HOME/mesa-zc/panfrost_icd.json"; PIN="1:26.0.6-1"
pin() { pacman -Q mesa 2>/dev/null | awk '{print $2}'; }
echo "== mesa pin PRE: $(pin) =="; [ "$(pin)" = "$PIN" ] || { echo "ABORT"; exit 1; }
glslangValidator -V zc_present.vert -o zc_present_vert.spv >/dev/null || { echo VERT_FAIL; exit 1; }
glslangValidator -V zc_present.frag -o zc_present_frag.spv >/dev/null || { echo FRAG_FAIL; exit 1; }
cc zc_measure.c -o zc_measure \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0) \
  -lvulkan -lm || { echo "BUILD_FAIL"; exit 1; }
export VK_ICD_FILENAMES="$ICD"
echo "== A/B over $CLIP (<= $N frames each) =="
./zc_measure "$CLIP" copy "$N"
./zc_measure "$CLIP" zerocopy "$N"
echo "== mesa pin POST: $(pin) =="
