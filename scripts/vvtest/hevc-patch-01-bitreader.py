#!/usr/bin/env python3
"""hevc-patch-01-bitreader: replace local bit-reader/start-code helpers in
v4l2vk_v4l2_h264.c with #include "v4l2vk_bitreader.h".

The helpers (struct v4l2vk_bitreader + br_* functions + v4l2vk_skip_start_code)
are now shared with the HEVC slice parser via the new header.  This is a pure
refactor — zero behavior change.  The H.264 byte-exact gate is the regression
proof.

Idempotent; asserts a unique anchor on the exact function-body text so the
assert fails safely if the source differs from what was extracted.

Run in the build container:
  python3 /vv/hevc-patch-01-bitreader.py /work/mesa-sree/mesa/src/vulkan-v4l2 [/deploy]
"""
import difflib
import os
import sys

DIR = sys.argv[1]
PATCH_OUT = sys.argv[2] if len(sys.argv) > 2 else None
FNAME = "v4l2vk_v4l2_h264.c"
GUARD = "v4l2vk_bitreader.h"

# The exact block to remove — verbatim from the original source (lines 12-103).
# Anchored on the full text so the assert fires if the source ever diverges.
ANCHOR = (
    "/* ---- Minimal H.264 bitstream reader for slice header parsing ----\n"
    " *\n"
    " * We need to parse just enough of each slice header to extract:\n"
    " *   - nal_ref_idc, nal_unit_type  (from NAL header)\n"
    " *   - first_mb_in_slice           (ue(v))\n"
    " *   - slice_type                  (ue(v))\n"
    " *\n"
    " * This avoids the need for a full H.264 parser while giving the V4L2\n"
    " * stateless driver the critical fields it validates.\n"
    " */\n"
    "\n"
    "struct v4l2vk_bitreader {\n"
    "   const uint8_t *data;\n"
    "   size_t size; /* bytes */\n"
    "   uint32_t bit_offset;\n"
    "};\n"
    "\n"
    "static inline void\n"
    "br_init(struct v4l2vk_bitreader *br, const uint8_t *data, size_t size)\n"
    "{\n"
    "   br->data = data;\n"
    "   br->size = size;\n"
    "   br->bit_offset = 0;\n"
    "}\n"
    "\n"
    "static inline int\n"
    "br_eof(const struct v4l2vk_bitreader *br)\n"
    "{\n"
    "   return (br->bit_offset >> 3) >= br->size;\n"
    "}\n"
    "\n"
    "static inline uint32_t\n"
    "br_read_bit(struct v4l2vk_bitreader *br)\n"
    "{\n"
    "   if (br_eof(br))\n"
    "      return 0;\n"
    "   uint32_t byte_idx = br->bit_offset >> 3;\n"
    "   uint32_t bit_idx = 7 - (br->bit_offset & 7);\n"
    "   br->bit_offset++;\n"
    "   return (br->data[byte_idx] >> bit_idx) & 1;\n"
    "}\n"
    "\n"
    "/* Read unsigned exp-Golomb coded value.\n"
    " * Codeword: M zero-bits, 1-bit, M info-bits.\n"
    " * Value = 2^M - 1 + INFO.\n"
    " */\n"
    "static uint32_t\n"
    "br_read_ue(struct v4l2vk_bitreader *br)\n"
    "{\n"
    "   uint32_t leading_zeros = 0;\n"
    "   while (!br_eof(br) && br_read_bit(br) == 0)\n"
    "      leading_zeros++;\n"
    "   if (leading_zeros > 31)\n"
    "      return 0; /* overflow protection */\n"
    "   uint32_t info = 0;\n"
    "   for (uint32_t i = 0; i < leading_zeros; i++)\n"
    "      info = (info << 1) | br_read_bit(br);\n"
    "   return (1u << leading_zeros) - 1 + info;\n"
    "}\n"
    "\n"
    "/* Read signed exp-Golomb coded value */\n"
    "static int32_t\n"
    "br_read_se(struct v4l2vk_bitreader *br)\n"
    "{\n"
    "   uint32_t v = br_read_ue(br);\n"
    "   if (v & 1)\n"
    "      return (int32_t)((v + 1) >> 1);\n"
    "   else\n"
    "      return -(int32_t)(v >> 1);\n"
    "}\n"
    "\n"
    "static inline void\n"
    "br_skip_bits(struct v4l2vk_bitreader *br, uint32_t n)\n"
    "{\n"
    "   br->bit_offset += n;\n"
    "}\n"
    "\n"
    "/*\n"
    " * Skip past any Annex-B start code (00 00 01 or 00 00 00 01) at the\n"
    " * given position, returning the offset of the NAL header byte.\n"
    " */\n"
    "static size_t\n"
    "v4l2vk_skip_start_code(const uint8_t *data, size_t size)\n"
    "{\n"
    "   if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&\n"
    "       data[3] == 1)\n"
    "      return 4;\n"
    "   if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)\n"
    "      return 3;\n"
    "   return 0; /* no start code — data starts at NAL header */\n"
    "}\n"
    "\n"
)

# Replace the entire block with a single include line.
REPLACEMENT = '#include "v4l2vk_bitreader.h"\n\n'

p = os.path.join(DIR, FNAME)
src = open(p, encoding="utf-8").read()

if GUARD in src:
    print(f"{FNAME}: already patched, skipping")
    sys.exit(0)

assert ANCHOR in src, f"{FNAME}: ANCHOR NOT FOUND — source may differ from expected"
assert src.count(ANCHOR) == 1, (
    f"{FNAME}: ANCHOR NOT UNIQUE ({src.count(ANCHOR)}x)"
)

new = src.replace(ANCHOR, REPLACEMENT, 1)
open(p, "w", encoding="utf-8").write(new)
print(f"{FNAME}: patched (local bit-reader/start-code helpers -> #include v4l2vk_bitreader.h)")

if PATCH_OUT:
    diff = "".join(
        difflib.unified_diff(
            src.splitlines(keepends=True),
            new.splitlines(keepends=True),
            fromfile=f"a/src/vulkan-v4l2/{FNAME}",
            tofile=f"b/src/vulkan-v4l2/{FNAME}",
        )
    )
    op = os.path.join(PATCH_OUT, "hevc-patch-01-bitreader.patch")
    open(op, "w", encoding="utf-8").write(diff)
    print(f"patch written: {op}")
