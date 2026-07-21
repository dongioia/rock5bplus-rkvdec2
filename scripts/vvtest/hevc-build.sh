#!/usr/bin/env bash
# Rebuild the V4L2 Vulkan ICD with HEVC sources injected.
# Mirrors icd-rebuild.sh but first copies hevc/*.{c,h} into the volume
# src/vulkan-v4l2/ and applies all hevc-patch-*.py patchers.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HEVC="$REPO/deploy/vulkan-v4l2-icd/hevc"
DEPLOY="$REPO/deploy/vulkan-v4l2-icd"
IMG="${ICD_BUILD_IMAGE:-rock5b-dev-serena}"
VOL="${ICD_MESA_VOLUME:-mesa-sree-tree}"

docker run --rm \
  -v "$VOL:/work/mesa-sree" \
  -v "$HEVC:/hevc" \
  -v "$REPO/scripts/vvtest:/vv" \
  -v "$DEPLOY:/deploy" \
  "$IMG" sh -lc '
    set -e
    SRC=/work/mesa-sree/mesa/src/vulkan-v4l2
    cp -v /hevc/*.c /hevc/*.h "$SRC"/
    for p in /vv/hevc-patch-*.py; do [ -e "$p" ] && python3 "$p" "$SRC" /deploy; done
    cd /work/mesa-sree/mesa
    ninja -C build src/vulkan-v4l2/libvulkan_v4l2_video.so.1
    cp -v build/src/vulkan-v4l2/libvulkan_v4l2_video.so.1 /deploy/libvulkan_v4l2_video.so
  '
echo "[hevc-build] -> $DEPLOY/libvulkan_v4l2_video.so"
