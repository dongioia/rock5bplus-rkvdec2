#!/usr/bin/env bash
# Task 7 TDD gate: compile the HEVC slice-parser unit harness in the build
# container against the live mesa-sree tree and run it on hevc_case1.h265.
#
# Mounts:
#   mesa-sree-tree  -> /work/mesa-sree   (Vulkan/V4L2 include paths + std headers)
#   deploy/.../hevc -> /hevc             (v4l2vk_v4l2_hevc.{c,h} + test harness)
#   artifacts       -> /art              (hevc_case1.h265)
#
# The harness compiles v4l2vk_v4l2_hevc.c directly (standalone, no .so) so it
# exercises the real parser. RED: empty/stub parser -> RESULT: FAIL (exit 1).
# GREEN: implemented parser -> "SLICE0 type=2 (I) ... RESULT: PASS" (exit 0).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HEVC="$REPO/deploy/vulkan-v4l2-icd/hevc"
ART="$REPO/artifacts/phase-hevc"
IMG="${ICD_BUILD_IMAGE:-rock5b-dev-serena}"
VOL="${ICD_MESA_VOLUME:-mesa-sree-tree}"
CLIP="${1:-hevc_case1.h265}"

docker run --rm \
  -v "$VOL:/work/mesa-sree" \
  -v "$HEVC:/hevc" \
  -v "$ART:/art" \
  "$IMG" sh -lc '
    set -e
    MESA=/work/mesa-sree/mesa
    SRC=$MESA/src/vulkan-v4l2
    # Use the in-tree HEVC sources (the ones under test) but compile from /hevc
    # so an in-progress edit is picked up even before hevc-build.sh injects it.
    cp -v /hevc/v4l2vk_v4l2_hevc.c /hevc/v4l2vk_v4l2_hevc.h "$SRC"/ >/dev/null
    cc -O0 -g -Wall \
       -I"$MESA"/include -I"$MESA"/src -I"$SRC" \
       /hevc/test_hevc_slice_parse.c "$SRC"/v4l2vk_v4l2_hevc.c \
       -o /tmp/t_hevc_slice
    echo "[hevc-slice-check] compiled OK; running on '"$CLIP"'"
    /tmp/t_hevc_slice /art/'"$CLIP"'
  '
