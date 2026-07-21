#ifndef V4L2VK_CODEC_H
#define V4L2VK_CODEC_H

/* Codec discriminator shared by all codec-branched sites.
 * H264=0 so existing zero-init paths default to H.264. */
enum v4l2vk_codec {
   V4L2VK_CODEC_H264 = 0,
   V4L2VK_CODEC_H265 = 1,
};

#endif /* V4L2VK_CODEC_H */
