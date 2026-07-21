#!/usr/bin/env python3
"""S3 fix: readback row stride mismatch in ConvertImageToBuffer.

Root cause (systematic-debugging 2026-06-22): the NV12 image->buffer readback
(v4l2vk_vk_device.c, the CmdCopyImageToBuffer path that vulkandownload drives)
computed the row stride as align256(width). Every other site in the ICD lays
the frame out at align16(width) = the V4L2 CAPTURE bytesperline:
  - CreateImage NV12 plane offsets (align16 width)
  - decode-time bulk memcpy of the V4L2 buffer (V4L2 stride)
  - GetImageSubresourceLayout rowPitch (align16 width)
align256 only coincides with align16 at 256-multiple widths (e.g. 1280 -> both
1280, byte-exact). At width 640 it gives 768, so the readback reads each row at
src+768 while the data is packed at src+640 -> 128 B/row drift -> 5-tile garbage.
case1 (1280x720) was byte-exact precisely because 1280 is 256-aligned.

Fix: derive the stride from the image's own stored plane-1 offset
(= align16(width) the frame was created/copied with), recovering the exact
layout regardless of the codec's width-alignment rule. Falls back to
align16(width) for the (unreached here) single-plane case.

Idempotent; asserts a unique anchor. Run in the build container:
  python3 /vv/s3-fix-readback-stride.py /work/mesa-sree/mesa/src/vulkan-v4l2 [/deploy]
"""
import difflib
import os
import sys

DIR = sys.argv[1]
PATCH_OUT = sys.argv[2] if len(sys.argv) > 2 else None
FNAME = "v4l2vk_vk_device.c"
GUARD = "S3 fix: readback row stride"

ANCHOR = (
    "   const uint32_t img_width = image->ci.extent.width;\n"
    "   const uint32_t img_height = image->ci.extent.height;\n"
    "   const uint32_t stride = v4l2vk_align_u32(img_width, 256u);\n"
    "   const uint32_t uv_height = (img_height + 1u) / 2u;\n"
)

REPLACEMENT = (
    "   const uint32_t img_width = image->ci.extent.width;\n"
    "   const uint32_t img_height = image->ci.extent.height;\n"
    "   /* S3 fix: readback row stride MUST match the layout the frame was\n"
    "    * created and memcpy'd with (CreateImage NV12 + decode bulk copy use\n"
    "    * align16(width) = the V4L2 CAPTURE bytesperline).  Recover it from the\n"
    "    * stored plane-1 offset so it is correct for any width-alignment rule;\n"
    "    * the old align256(width) only matched at 256-multiple widths (1280) and\n"
    "    * drifted by (align256(w)-w) bytes/row otherwise (e.g. 768-640=128). */\n"
    "   const uint32_t h_aligned = v4l2vk_align_u32(img_height, 16u);\n"
    "   const uint32_t stride =\n"
    "      (image->plane_count > 1 && h_aligned)\n"
    "         ? (uint32_t)(image->planes[1].offset / h_aligned)\n"
    "         : v4l2vk_align_u32(img_width, 16u);\n"
    "   const uint32_t uv_height = (img_height + 1u) / 2u;\n"
)

p = os.path.join(DIR, FNAME)
src = open(p).read()

if GUARD in src:
    print(f"{FNAME}: already patched, skipping")
    sys.exit(0)

assert ANCHOR in src, f"{FNAME}: ANCHOR NOT FOUND"
assert src.count(ANCHOR) == 1, f"{FNAME}: ANCHOR NOT UNIQUE ({src.count(ANCHOR)}x)"

new = src.replace(ANCHOR, REPLACEMENT, 1)
open(p, "w").write(new)
print(f"{FNAME}: patched (readback stride align256 -> stored-layout-derived)")

if PATCH_OUT:
    diff = "".join(
        difflib.unified_diff(
            src.splitlines(keepends=True), new.splitlines(keepends=True),
            fromfile=f"a/src/vulkan-v4l2/{FNAME}", tofile=f"b/src/vulkan-v4l2/{FNAME}",
        )
    )
    op = os.path.join(PATCH_OUT, "s3-readback-stride.patch")
    open(op, "w").write(diff)
    print(f"patch written: {op}")
