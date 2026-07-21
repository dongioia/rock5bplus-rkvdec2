#!/usr/bin/env bash
# scp the ICD + a SBC-local manifest to the SBC, isolated. No system icd.d, no sudo.
# Requires the SBC reachable (Precondition 0).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="$REPO/deploy/vulkan-v4l2-icd"
SBC="${SBC_HOST:-rock5b}"
DST="${SBC_DIR:-/home/sav/vvtest}"

# shellcheck disable=SC2029  # $DST is our local var; client-side expansion is intended
ssh "$SBC" "mkdir -p '$DST'"
scp "$DEPLOY/libvulkan_v4l2_video.so" "$SBC:$DST/"
# Generate a SBC-local manifest whose library_path is the absolute deployed path.
cat > /tmp/v4l2vk_icd.sbc.json <<JSON
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$DST/libvulkan_v4l2_video.so",
        "api_version": "1.3.0"
    }
}
JSON
scp /tmp/v4l2vk_icd.sbc.json "$SBC:$DST/v4l2vk_icd.aarch64.json"
echo "[icd-deploy] deployed to $SBC:$DST"
echo "[icd-deploy] smoke: ssh $SBC \"VK_ICD_FILENAMES=$DST/v4l2vk_icd.aarch64.json vulkaninfo --summary | grep -A2 deviceName\""
