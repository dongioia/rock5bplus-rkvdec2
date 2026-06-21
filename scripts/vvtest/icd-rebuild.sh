#!/usr/bin/env bash
# Rebuild the V4L2 Vulkan ICD incrementally in a throwaway container and
# repackage the .so into deploy/. SBC-independent.
# NOTE: Stage-2 verified the ICD via INCREMENTAL ninja + fix-presence grep (fast). A clean from-scratch meson reconfigure was not exercised; b0-fix.patch (deploy/vulkan-v4l2-icd/) is the recovery path for a cold rebuild.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="$REPO/deploy/vulkan-v4l2-icd"
IMG="${ICD_BUILD_IMAGE:-rock5b-dev-serena}"
VOL="${ICD_MESA_VOLUME:-mesa-sree-tree}"

# Mount the volume at the SAME path meson was configured with (/work/mesa-sree):
# build.ninja hardcodes the absolute source dir /work/mesa-sree/mesa, so a
# different mount point makes ninja fail to regenerate ("no meson.build").
docker run --rm \
  -v "$VOL:/work/mesa-sree" \
  -v "$DEPLOY:/deploy" \
  "$IMG" sh -lc '
    set -e
    cd /work/mesa-sree/mesa
    ninja -C build src/vulkan-v4l2/libvulkan_v4l2_video.so.1
    cp -v build/src/vulkan-v4l2/libvulkan_v4l2_video.so.1 /deploy/libvulkan_v4l2_video.so
  '
echo "[icd-rebuild] repackaged -> $DEPLOY/libvulkan_v4l2_video.so"
