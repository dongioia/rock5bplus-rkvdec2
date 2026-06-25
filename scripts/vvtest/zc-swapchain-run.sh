#!/usr/bin/env bash
# Phase-C Step-2 Increment-2 runner (ON the SBC, under sway): on-screen zero-copy
# present. Compiles shaders, builds with SDL2, runs against the pinned !42353
# PanVK with the Wayland env, asserts the system mesa pin is untouched.
set -uo pipefail
cd "$HOME/vvtest" || { echo "no ~/vvtest"; exit 1; }
CLIP="${1:-case1.h264}"
ICD="$HOME/mesa-zc/panfrost_icd.json"
PIN="1:26.0.6-1"
pin() { pacman -Q mesa 2>/dev/null | awk '{print $2}'; }

echo "== mesa pin PRE: $(pin) (want $PIN) =="
[ "$(pin)" = "$PIN" ] || { echo "ABORT: system mesa not $PIN"; exit 1; }

echo "== compile shaders =="
glslangValidator -V zc_sc.vert -o zc_sc_vert.spv || { echo "VERT_FAIL"; exit 1; }
glslangValidator -V zc_present.frag -o zc_present_frag.spv || { echo "FRAG_FAIL"; exit 1; }

echo "== build =="
cc zc_swapchain_test.c -o zc_swapchain_test \
  $(pkg-config --cflags --libs sdl2 gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0) \
  -lvulkan -lm || { echo "BUILD_FAIL"; exit 1; }

echo "== run on sway (window should appear; clip plays once then holds) =="
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 VK_ICD_FILENAMES="$ICD" SDL_VIDEODRIVER=wayland
./zc_swapchain_test "$CLIP"
RC=$?
echo "== mesa pin POST: $(pin) =="
[ "$(pin)" = "$PIN" ] || { echo "WARN: system mesa CHANGED"; RC=99; }
echo "swapchain-run exit=$RC"
exit $RC
