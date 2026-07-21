/* SPDX-License-Identifier: MIT
 *
 * Copyright © 2026 Amazon.com, Inc. or its affiliates
 *     Sreerenj Balachandran <sreerenj@amazon.com>
 *
 * Shared codec-agnostic Annex-B bit-reader and start-code helpers.
 * Used by both the H.264 and HEVC slice parsers.
 */
#ifndef V4L2VK_BITREADER_H
#define V4L2VK_BITREADER_H

#include <stdint.h>
#include <stddef.h>

/* ---- Minimal Annex-B bitstream reader ----
 *
 * We need to parse just enough of each slice header to extract:
 *   - nal_ref_idc, nal_unit_type  (from NAL header)
 *   - first_mb_in_slice           (ue(v))
 *   - slice_type                  (ue(v))
 *
 * This avoids the need for a full parser while giving the V4L2
 * stateless driver the critical fields it validates.
 */

struct v4l2vk_bitreader {
   const uint8_t *data;
   size_t size; /* bytes */
   uint32_t bit_offset;
};

static inline void
br_init(struct v4l2vk_bitreader *br, const uint8_t *data, size_t size)
{
   br->data = data;
   br->size = size;
   br->bit_offset = 0;
}

static inline int
br_eof(const struct v4l2vk_bitreader *br)
{
   return (br->bit_offset >> 3) >= br->size;
}

static inline uint32_t
br_read_bit(struct v4l2vk_bitreader *br)
{
   if (br_eof(br))
      return 0;
   uint32_t byte_idx = br->bit_offset >> 3;
   uint32_t bit_idx = 7 - (br->bit_offset & 7);
   br->bit_offset++;
   return (br->data[byte_idx] >> bit_idx) & 1;
}

/* Read unsigned exp-Golomb coded value.
 * Codeword: M zero-bits, 1-bit, M info-bits.
 * Value = 2^M - 1 + INFO.
 */
static inline uint32_t
br_read_ue(struct v4l2vk_bitreader *br)
{
   uint32_t leading_zeros = 0;
   while (!br_eof(br) && br_read_bit(br) == 0)
      leading_zeros++;
   if (leading_zeros > 31)
      return 0; /* overflow protection */
   uint32_t info = 0;
   for (uint32_t i = 0; i < leading_zeros; i++)
      info = (info << 1) | br_read_bit(br);
   return (1u << leading_zeros) - 1 + info;
}

/* Read signed exp-Golomb coded value */
static inline int32_t
br_read_se(struct v4l2vk_bitreader *br)
{
   uint32_t v = br_read_ue(br);
   if (v & 1)
      return (int32_t)((v + 1) >> 1);
   else
      return -(int32_t)(v >> 1);
}

static inline void
br_skip_bits(struct v4l2vk_bitreader *br, uint32_t n)
{
   br->bit_offset += n;
}

/*
 * Skip past any Annex-B start code (00 00 01 or 00 00 00 01) at the
 * given position, returning the offset of the NAL header byte.
 */
static inline size_t
v4l2vk_skip_start_code(const uint8_t *data, size_t size)
{
   if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
       data[3] == 1)
      return 4;
   if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
      return 3;
   return 0; /* no start code — data starts at NAL header */
}

#endif /* V4L2VK_BITREADER_H */
