#!/usr/bin/env bash
# Stage-2 ICD verification: enumerate V4L2 Vulkan Video Decoder via the isolated ICD,
# check no missing libs, confirm mesa system pin is intact.
# Run: ssh rock5b 'bash -s' < scripts/vvtest/s2-icd-verify.sh
# Or locally (will ssh in): bash scripts/vvtest/s2-icd-verify.sh --remote
set -euo pipefail

VVTEST_DIR="${VVTEST_DIR:-$HOME/vvtest}"
ICD_JSON="$VVTEST_DIR/v4l2vk_icd.aarch64.json"
ICD_SO="$VVTEST_DIR/libvulkan_v4l2_video.so"

echo "=== s2-icd-verify: isolated ICD check ==="
echo "ICD_JSON: $ICD_JSON"
echo "ICD_SO:   $ICD_SO"
echo ""

# 1. Enumeration: must find V4L2 Vulkan Video Decoder + VK_KHR_video_decode_h264
echo "--- vulkaninfo enumeration ---"
export VK_ICD_FILENAMES="$ICD_JSON"
vulkaninfo 2>/dev/null | grep -iE "V4L2 Vulkan Video Decoder|VK_KHR_video_decode_h264|deviceName" | head -10
echo ""

# 2. ldd: no missing libraries
echo "--- ldd check ---"
if ldd "$ICD_SO" 2>&1 | grep -i "not found"; then
    echo "RESULT: LDD FAIL - missing libs"
    exit 1
else
    echo "RESULT: LDD OK"
fi
echo ""

# 3. Mesa system pin: confirm unchanged
echo "--- mesa system pin ---"
pacman -Q mesa 2>/dev/null || true
echo ""

echo "=== s2-icd-verify: DONE ==="
