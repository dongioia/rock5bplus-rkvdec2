#!/usr/bin/env python3
"""hevc-patch-02-caps: advertise VK_KHR_video_decode_h265 in the V4L2-Vulkan ICD.

Edits three files additively (mirror of the H.264 sites) so that:
  - v4l2vk_device_exts.h       : H265 entry in the static extension table
  - v4l2vk_vk_physical_device.c: exts.KHR_video_decode_h265 flag +
                                  videoCodecOperations |= DECODE_H265_BIT_KHR +
                                  ADD_EXT(H265) in EnumerateDeviceExtensionProperties
  - v4l2vk_vk_video.c          : H265 branch in GetPhysicalDeviceVideoCapabilitiesKHR
                                  + H265 in v4l2vk_match_h264_8bit_420 (renamed to match
                                  both codecs) + H265 in pick_profile_from_info

This is enumeration-only (no decode session).  GStreamer's vulkanh265dec
will register under this ICD once the library is deployed.

Idempotent (each file guarded by a GUARD string).  Asserts anchor is present
and unique before writing any file.  Run in the build container:
  python3 /vvtest/hevc-patch-02-caps.py /work/mesa-sree/mesa/src/vulkan-v4l2
"""

import os
import sys

DIR = sys.argv[1]
pending = {}   # path -> new_content (last staged wins per file; we chain stages)
staged_set = set()  # paths that have been modified (to avoid double-write noise)


def stage(fname, guard, edits):
    p = os.path.join(DIR, fname)
    # Use staged content if this file was already modified in this run,
    # otherwise read from disk.  This lets multiple stages on the same file
    # compose correctly without the second overwriting the first.
    s = pending.get(p) or open(p, encoding="utf-8").read()
    if guard in s:
        print(f"{fname}: already patched, skipping")
        return
    for anchor, replacement in edits:
        assert anchor in s, f"{fname}: ANCHOR NOT FOUND:\n{anchor!r}"
        assert s.count(anchor) == 1, (
            f"{fname}: ANCHOR NOT UNIQUE ({s.count(anchor)}x):\n{anchor!r}"
        )
        s = s.replace(anchor, replacement, 1)
    pending[p] = s
    staged_set.add(p)
    print(f"{fname}: staged")


# ── 1. v4l2vk_device_exts.h ──────────────────────────────────────────────────
# Add H265 entry immediately after the H264 entry in the static extension table.
stage(
    "v4l2vk_device_exts.h",
    "hevc-patch-02-caps: H265 ext entry",
    [(
        "   {VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,\n"
        "    VK_KHR_VIDEO_DECODE_H264_SPEC_VERSION},\n",
        "   {VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,\n"
        "    VK_KHR_VIDEO_DECODE_H264_SPEC_VERSION},\n"
        "   /* hevc-patch-02-caps: H265 ext entry */\n"
        "   {VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,\n"
        "    VK_KHR_VIDEO_DECODE_H265_SPEC_VERSION},\n",
    )],
)

# ── 2. v4l2vk_vk_physical_device.c ───────────────────────────────────────────
# (a) + (b): exts flag + queue-family videoCodecOperations
stage(
    "v4l2vk_vk_physical_device.c",
    "hevc-patch-02-caps: H265 exts flag",
    [
        # (a) exts flag
        (
            "   exts.KHR_video_decode_h264 = true;\n",
            "   exts.KHR_video_decode_h264 = true;\n"
            "   exts.KHR_video_decode_h265 = true; /* hevc-patch-02-caps: H265 exts flag */\n",
        ),
        # (b) queue-family videoCodecOperations: OR-in the H265 codec operation bit
        (
            "            vp->videoCodecOperations =\n"
            "               VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;\n",
            "            vp->videoCodecOperations =\n"
            "               VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;\n"
            "            /* hevc-patch-02-caps: H265 codec operation */\n"
            "            vp->videoCodecOperations |=\n"
            "               VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;\n",
        ),
    ],
)

# (c) EnumerateDeviceExtensionProperties: add H265 to the ADD_EXT list.
# Separate stage/guard so it applies even if (a)+(b) were already done.
stage(
    "v4l2vk_vk_physical_device.c",
    "hevc-patch-02-caps: H265 enumerate",
    [(
        "   ADD_EXT(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,\n"
        "           VK_KHR_VIDEO_DECODE_H264_SPEC_VERSION);\n",
        "   ADD_EXT(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,\n"
        "           VK_KHR_VIDEO_DECODE_H264_SPEC_VERSION);\n"
        "   /* hevc-patch-02-caps: H265 enumerate */\n"
        "   ADD_EXT(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,\n"
        "           VK_KHR_VIDEO_DECODE_H265_SPEC_VERSION);\n",
    )],
)

# ── 3. v4l2vk_vk_video.c ─────────────────────────────────────────────────────
# Replace the single H264-only codec-operation check at the top of
# GetPhysicalDeviceVideoCapabilitiesKHR with a dispatcher that handles both
# H264 and H265.  The H264 path is unchanged; the H265 path mirrors it.
stage(
    "v4l2vk_vk_video.c",
    "hevc-patch-02-caps: H265 capabilities branch",
    [(
        # Anchor: the codec-operation guard + profile format check + entire caps block
        "   if (pProfile->videoCodecOperation !=\n"
        "       VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)\n"
        "      return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;\n"
        "\n"
        "   if (pProfile->lumaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ||\n"
        "       pProfile->chromaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ||\n"
        "       pProfile->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)\n"
        "      return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;\n"
        "\n"
        "   pCaps->sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;\n"
        "   pCaps->minBitstreamBufferOffsetAlignment = 16;\n"
        "   pCaps->minBitstreamBufferSizeAlignment = 16;\n"
        "   pCaps->pictureAccessGranularity = (VkExtent2D){16, 16};\n"
        "   pCaps->minCodedExtent = (VkExtent2D){16, 16};\n"
        "   pCaps->maxCodedExtent = (VkExtent2D){4096, 4096};\n"
        "   pCaps->maxDpbSlots = 17;\n"
        "   pCaps->maxActiveReferencePictures = 16;\n"
        "   pCaps->flags = VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR;\n"
        "\n"
        "   strcpy(pCaps->stdHeaderVersion.extensionName,\n"
        "          VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);\n"
        "   pCaps->stdHeaderVersion.specVersion =\n"
        "      VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;\n"
        "\n"
        "   for (VkBaseOutStructure *b = (VkBaseOutStructure *)pCaps->pNext; b;\n"
        "        b = b->pNext) {\n"
        "      if (b->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR) {\n"
        "         VkVideoDecodeCapabilitiesKHR *dec = (VkVideoDecodeCapabilitiesKHR *)b;\n"
        "         dec->flags =\n"
        "            VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;\n"
        "      } else if (b->sType ==\n"
        "                 VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR) {\n"
        "         VkVideoDecodeH264CapabilitiesKHR *h264 =\n"
        "            (VkVideoDecodeH264CapabilitiesKHR *)b;\n"
        "         h264->fieldOffsetGranularity.x = 0;\n"
        "         h264->fieldOffsetGranularity.y = 0;\n"
        "         h264->maxLevelIdc = STD_VIDEO_H264_LEVEL_IDC_5_1;\n"
        "      }\n"
        "   }\n"
        "\n"
        "   struct VkVideoDecodeCapabilitiesKHR *dec_caps =\n"
        "      (struct VkVideoDecodeCapabilitiesKHR *)vk_find_struct(\n"
        "         pCaps, VIDEO_DECODE_CAPABILITIES_KHR);\n"
        "   if (dec_caps)\n"
        "      dec_caps->flags =\n"
        "         VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;\n"
        "\n"
        "   return VK_SUCCESS;\n"
        "}",
        # Replacement: dispatcher for H264 + H265
        "   /* hevc-patch-02-caps: H265 capabilities branch */\n"
        "   const VkVideoCodecOperationFlagBitsKHR op = pProfile->videoCodecOperation;\n"
        "\n"
        "   if (op != VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR &&\n"
        "       op != VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR)\n"
        "      return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;\n"
        "\n"
        "   if (pProfile->lumaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ||\n"
        "       pProfile->chromaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ||\n"
        "       pProfile->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)\n"
        "      return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;\n"
        "\n"
        "   pCaps->sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;\n"
        "   pCaps->minBitstreamBufferOffsetAlignment = 16;\n"
        "   pCaps->minBitstreamBufferSizeAlignment = 16;\n"
        "   pCaps->pictureAccessGranularity = (VkExtent2D){16, 16};\n"
        "   pCaps->minCodedExtent = (VkExtent2D){16, 16};\n"
        "   pCaps->maxCodedExtent = (VkExtent2D){4096, 4096};\n"
        "   pCaps->maxDpbSlots = 17;\n"
        "   pCaps->maxActiveReferencePictures = 16;\n"
        "   pCaps->flags = VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR;\n"
        "\n"
        "   if (op == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {\n"
        "      strcpy(pCaps->stdHeaderVersion.extensionName,\n"
        "             VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);\n"
        "      pCaps->stdHeaderVersion.specVersion =\n"
        "         VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;\n"
        "   } else {\n"
        "      strcpy(pCaps->stdHeaderVersion.extensionName,\n"
        "             VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME);\n"
        "      pCaps->stdHeaderVersion.specVersion =\n"
        "         VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION;\n"
        "   }\n"
        "\n"
        "   for (VkBaseOutStructure *b = (VkBaseOutStructure *)pCaps->pNext; b;\n"
        "        b = b->pNext) {\n"
        "      if (b->sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR) {\n"
        "         VkVideoDecodeCapabilitiesKHR *dec = (VkVideoDecodeCapabilitiesKHR *)b;\n"
        "         dec->flags =\n"
        "            VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;\n"
        "      } else if (b->sType ==\n"
        "                 VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR) {\n"
        "         VkVideoDecodeH264CapabilitiesKHR *h264 =\n"
        "            (VkVideoDecodeH264CapabilitiesKHR *)b;\n"
        "         h264->fieldOffsetGranularity.x = 0;\n"
        "         h264->fieldOffsetGranularity.y = 0;\n"
        "         h264->maxLevelIdc = STD_VIDEO_H264_LEVEL_IDC_5_1;\n"
        "      } else if (b->sType ==\n"
        "                 VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR) {\n"
        "         VkVideoDecodeH265CapabilitiesKHR *h265 =\n"
        "            (VkVideoDecodeH265CapabilitiesKHR *)b;\n"
        "         h265->maxLevelIdc = STD_VIDEO_H265_LEVEL_IDC_5_1;\n"
        "      }\n"
        "   }\n"
        "\n"
        "   struct VkVideoDecodeCapabilitiesKHR *dec_caps =\n"
        "      (struct VkVideoDecodeCapabilitiesKHR *)vk_find_struct(\n"
        "         pCaps, VIDEO_DECODE_CAPABILITIES_KHR);\n"
        "   if (dec_caps)\n"
        "      dec_caps->flags =\n"
        "         VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;\n"
        "\n"
        "   return VK_SUCCESS;\n"
        "}",
    )],
)

# ── 4. v4l2vk_vk_video.c (format properties) ────────────────────────────────
# Extend v4l2vk_match_h264_8bit_420 to also accept H265, and rename the picker
# to v4l2vk_pick_video_profile_from_info so it works for both codecs.
# Separate stage/guard from the capabilities stage.
stage(
    "v4l2vk_vk_video.c",
    "hevc-patch-02-caps: H265 format properties",
    [(
        # Anchor: the H264-only match function + the picker that calls it.
        # Must include "static bool\n" prefix and exact 12-space indent for "return vp;".
        "static bool\n"
        "v4l2vk_match_h264_8bit_420(const VkVideoProfileInfoKHR *vp)\n"
        "{\n"
        "   if (!vp)\n"
        "      return false;\n"
        "   if (vp->videoCodecOperation != VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)\n"
        "      return false;\n"
        "   if (vp->lumaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)\n"
        "      return false;\n"
        "   if (vp->chromaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)\n"
        "      return false;\n"
        "   if (vp->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)\n"
        "      return false;\n"
        "   return true;\n"
        "}\n"
        "\n"
        "static const VkVideoProfileInfoKHR *\n"
        "v4l2vk_pick_h264_profile_from_info(\n"
        "   const VkPhysicalDeviceVideoFormatInfoKHR *info)\n"
        "{\n"
        "   for (const VkBaseInStructure *b = (const VkBaseInStructure *)info->pNext; b;\n"
        "        b = b->pNext) {\n"
        "      if (b->sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR) {\n"
        "         const VkVideoProfileListInfoKHR *list =\n"
        "            (const VkVideoProfileListInfoKHR *)b;\n"
        "         for (uint32_t i = 0; i < list->profileCount; i++)\n"
        "            if (v4l2vk_match_h264_8bit_420(&list->pProfiles[i]))\n"
        "               return &list->pProfiles[i];\n"
        "         return NULL;\n"
        "      }\n"
        "   }\n"
        "   for (const VkBaseInStructure *b = (const VkBaseInStructure *)info->pNext; b;\n"
        "        b = b->pNext) {\n"
        "      if (b->sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR) {\n"
        "         const VkVideoProfileInfoKHR *vp = (const VkVideoProfileInfoKHR *)b;\n"
        "         if (v4l2vk_match_h264_8bit_420(vp))\n"
        "            return vp;\n"
        "      }\n"
        "   }\n"
        "   return NULL;\n"
        "}",
        # Replacement: accept H264 or H265 8-bit/420
        "/* hevc-patch-02-caps: H265 format properties — accept H264 or H265 8bit/420 */\n"
        "static bool\n"
        "v4l2vk_match_video_8bit_420(const VkVideoProfileInfoKHR *vp)\n"
        "{\n"
        "   if (!vp)\n"
        "      return false;\n"
        "   if (vp->videoCodecOperation != VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR &&\n"
        "       vp->videoCodecOperation != VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR)\n"
        "      return false;\n"
        "   if (vp->lumaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)\n"
        "      return false;\n"
        "   if (vp->chromaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)\n"
        "      return false;\n"
        "   if (vp->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)\n"
        "      return false;\n"
        "   return true;\n"
        "}\n"
        "\n"
        "static const VkVideoProfileInfoKHR *\n"
        "v4l2vk_pick_video_profile_from_info(\n"
        "   const VkPhysicalDeviceVideoFormatInfoKHR *info)\n"
        "{\n"
        "   for (const VkBaseInStructure *b = (const VkBaseInStructure *)info->pNext; b;\n"
        "        b = b->pNext) {\n"
        "      if (b->sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR) {\n"
        "         const VkVideoProfileListInfoKHR *list =\n"
        "            (const VkVideoProfileListInfoKHR *)b;\n"
        "         for (uint32_t i = 0; i < list->profileCount; i++)\n"
        "            if (v4l2vk_match_video_8bit_420(&list->pProfiles[i]))\n"
        "               return &list->pProfiles[i];\n"
        "         return NULL;\n"
        "      }\n"
        "   }\n"
        "   for (const VkBaseInStructure *b = (const VkBaseInStructure *)info->pNext; b;\n"
        "        b = b->pNext) {\n"
        "      if (b->sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR) {\n"
        "         const VkVideoProfileInfoKHR *vp = (const VkVideoProfileInfoKHR *)b;\n"
        "         if (v4l2vk_match_video_8bit_420(vp))\n"
        "            return vp;\n"
        "      }\n"
        "   }\n"
        "   return NULL;\n"
        "}",
    )],
)

# Also fix the call site: v4l2vk_GetPhysicalDeviceVideoFormatPropertiesKHR
# calls v4l2vk_pick_h264_profile_from_info — rename to the new generic picker.
# Use a separate guard so it's independent.
stage(
    "v4l2vk_vk_video.c",
    "hevc-patch-02-caps: pick_video_profile call site",
    [(
        "   const VkVideoProfileInfoKHR *vp = v4l2vk_pick_h264_profile_from_info(pInfo);\n",
        "   /* hevc-patch-02-caps: pick_video_profile call site */\n"
        "   const VkVideoProfileInfoKHR *vp = v4l2vk_pick_video_profile_from_info(pInfo);\n",
    )],
)

# ── commit ────────────────────────────────────────────────────────────────────
for path in staged_set:
    open(path, "w", encoding="utf-8").write(pending[path])
    print(f"written: {path}")

if not staged_set:
    print("all files already patched — nothing to write")
