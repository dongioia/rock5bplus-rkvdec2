#!/usr/bin/env python3
"""Insert B0 debug readback hooks into v4l2vk_vk_device.c (idempotent).

Run INSIDE the build container against the volume copy:
  python3 /vvtest/icd-instrument.py /work/mesa-sree/mesa/src/vulkan-v4l2/v4l2vk_vk_device.c
Anchored on unique existing substrings so it survives line drift.
"""
import os
import sys

INCLUDES = '''#include <linux/dma-buf.h>
#include <sys/ioctl.h>
'''

HELPER = r'''
/* --- B0 debug: raw plane dump (env-gated, removable) --- */
static void
v4l2vk_b0_dump_raw(const char *env, const char *tag, unsigned frame_idx,
                   const void *addr, size_t len, int dmabuf_fd)
{
   if (!getenv(env) || !addr || !len)
      return;
#ifdef DMA_BUF_IOCTL_SYNC
   if (dmabuf_fd >= 0) { /* R11: CPU cache coherency before read */
      struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
      ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s);
   }
#endif
   const char *dir = getenv("V4L2VK_DUMP_DIR");
   char path[256];
   snprintf(path, sizeof(path), "%s/v4l2vk_%s_%04u.bin", dir ? dir : "/tmp",
            tag, frame_idx);
   FILE *fp = fopen(path, "wb");
   if (fp) { fwrite(addr, 1, len, fp); fclose(fp); }
#ifdef DMA_BUF_IOCTL_SYNC
   if (dmabuf_fd >= 0) {
      struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
      ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s);
   }
#endif
   fprintf(stderr, "[V4L2VK][B0] %s: %zu bytes -> %s (dmabuf_fd=%d)\n",
           tag, len, path, dmabuf_fd);
}

'''

# (a) raw CAPTURE buffer right after DQBUF, pre-COPY2 + per-plane/format log
DQ_ANCHOR = "v4l2vk_v4l2_dequeue_capture(v4l2_ctx, &dq_cap);"
DUMP_A = '''
            /* B0 (a): raw V4L2 CAPTURE buffer + format log (pre-COPY2) */
            if (dq_cap < v4l2_ctx->capture_buf_count) {
               struct v4l2vk_v4l2_buffer *b0cb =
                  &v4l2_ctx->capture_bufs[dq_cap];
               v4l2vk_b0_dump_raw("V4L2VK_DUMP_CAPTURE", "capture",
                                  dev->frame_counter, b0cb->mmap_addr,
                                  b0cb->mmap_size, b0cb->dma_buf_fd);
               fprintf(stderr,
                       "[V4L2VK][B0] cap idx=%u stride=%u sizeimage=%u "
                       "mmap_size=%zu w=%u h=%u dmabuf_mode=%d dma_buf_fd=%d\\n",
                       dq_cap, v4l2_ctx->capture_stride,
                       v4l2_ctx->capture_sizeimage, b0cb->mmap_size,
                       v4l2_ctx->width, v4l2_ctx->height,
                       v4l2_ctx->capture_dmabuf_mode, b0cb->dma_buf_fd);
            }
'''

# (b) VkImage host memory right after the COPY2 memcpy
COPY2_ANCHOR = ("v4l2_ctx->capture_bufs[dq_cap].mmap_addr,\n"
                "                      copy_size);")
DUMP_B = '''
               /* B0 (b): VkImage host memory (post-COPY2) */
               v4l2vk_b0_dump_raw("V4L2VK_DUMP_VKIMAGE", "vkimage",
                                  dev->frame_counter,
                                  (uint8_t *)dst_mem->map + dst_img->bound_offset,
                                  copy_size, -1);
'''

INC_ANCHOR = '#include "vk_sync.h"'
HELPER_ANCHOR = "static void\nv4l2vk_dump_nv12_image("


def main(path):
    src = open(path).read()
    if "V4L2VK_DUMP_CAPTURE" in src:
        print("already instrumented; nothing to do")
        return 0
    assert INC_ANCHOR in src, "include anchor not found"
    assert HELPER_ANCHOR in src, "helper anchor not found"
    assert DQ_ANCHOR in src, "dequeue_capture anchor not found"
    assert COPY2_ANCHOR in src, "COPY2 memcpy anchor not found"
    src = src.replace(INC_ANCHOR, INC_ANCHOR + "\n" + INCLUDES, 1)
    src = src.replace(HELPER_ANCHOR, HELPER + HELPER_ANCHOR, 1)
    src = src.replace(DQ_ANCHOR, DQ_ANCHOR + DUMP_A, 1)
    src = src.replace(COPY2_ANCHOR, COPY2_ANCHOR + DUMP_B, 1)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(src)
    os.replace(tmp, path)  # atomic; never leave a half-written source
    print("instrumented OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
