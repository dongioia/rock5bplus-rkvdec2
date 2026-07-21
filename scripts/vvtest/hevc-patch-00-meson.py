#!/usr/bin/env python3
"""Patcher 00: add v4l2vk_v4l2_hevc.c to the meson.build sources list.

Inserts the HEVC source file immediately after v4l2vk_v4l2_h264.c in the
v4l2vk_vk_files list. Idempotent; asserts a unique anchor. Emits a .patch
file to /deploy if provided.

Run in the build container:
  python3 /vv/hevc-patch-00-meson.py /work/mesa-sree/mesa/src/vulkan-v4l2 [/deploy]
"""
import difflib
import os
import sys

DIR = sys.argv[1]
PATCH_OUT = sys.argv[2] if len(sys.argv) > 2 else None
FNAME = "meson.build"
GUARD = "v4l2vk_v4l2_hevc.c"

ANCHOR = "  'v4l2vk_v4l2_h264.c',\n"
INSERTION = "  'v4l2vk_v4l2_hevc.c',\n"

p = os.path.join(DIR, FNAME)
src = open(p).read()

if GUARD in src:
    print(f"{FNAME}: already patched, skipping")
    sys.exit(0)

assert ANCHOR in src, f"{FNAME}: ANCHOR NOT FOUND"
assert src.count(ANCHOR) == 1, f"{FNAME}: ANCHOR NOT UNIQUE ({src.count(ANCHOR)}x)"

new = src.replace(ANCHOR, ANCHOR + INSERTION, 1)
open(p, "w").write(new)
print(f"{FNAME}: patched (added v4l2vk_v4l2_hevc.c after v4l2vk_v4l2_h264.c)")

if PATCH_OUT:
    diff = "".join(
        difflib.unified_diff(
            src.splitlines(keepends=True), new.splitlines(keepends=True),
            fromfile=f"a/src/vulkan-v4l2/{FNAME}", tofile=f"b/src/vulkan-v4l2/{FNAME}",
        )
    )
    op = os.path.join(PATCH_OUT, "hevc-patch-00-meson.patch")
    open(op, "w").write(diff)
    print(f"patch written: {op}")
