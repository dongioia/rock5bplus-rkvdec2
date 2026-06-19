#!/usr/bin/env bash
# Precondition 0 (SBC availability gate) + Precondition 1 (driver identity).
# Saves metadata under artifacts/phase-b0/metadata/. Exits non-zero if any
# gating tool/device is missing. Run when the SBC is up (Task 6).
# NOTE: intentionally NO `-e`. This is a gather-and-gate script: it captures all
# metadata best-effort and accumulates failures (fail=1), then reports every
# missing tool/device at the end. `-e` would abort on the first hiccup and defeat
# that. The critical checks set fail=1 and the final gate enforces them.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SBC="${SBC_HOST:-rock5b}"
OUT="$REPO/artifacts/phase-b0/metadata"
mkdir -p "$OUT"
fail=0

echo "== Precondition 0: SBC availability =="
ssh "$SBC" 'true' || { echo "FAIL: SBC unreachable"; exit 1; }
ssh "$SBC" 'uname -a'              | tee "$OUT/uname.txt"
ssh "$SBC" 'gst-launch-1.0 --version' | tee "$OUT/gst-version.txt"
ssh "$SBC" 'ls -l /dev/video0 /dev/media* 2>&1' | tee "$OUT/devnodes.txt" \
  | grep -q '/dev/video0' || { echo "FAIL: /dev/video0 missing"; fail=1; }
for tool in v4l2-tracer v4l2-ctl ffmpeg media-ctl; do
  # shellcheck disable=SC2029  # $tool is our loop var; client-side expansion intended
  ssh "$SBC" "command -v $tool >/dev/null" \
    || { echo "FAIL: $tool missing (install v4l-utils)"; fail=1; }
done
if ssh "$SBC" 'gst-inspect-1.0 v4l2slh264dec >/dev/null 2>&1'; then
  echo "v4l2slh264dec OK"
else
  echo "FAIL: v4l2slh264dec missing"; fail=1
fi

echo "== Precondition 1: driver identity =="
ssh "$SBC" 'v4l2-ctl -d /dev/video0 --info'            | tee "$OUT/v4l2-info.txt"
ssh "$SBC" 'media-ctl -p'                              | tee "$OUT/media-topology.txt"
ssh "$SBC" 'cat /sys/class/video4linux/video0/name'    | tee "$OUT/video0-name.txt"
ssh "$SBC" 'tr -d "\0" < /sys/class/video4linux/video0/device/of_node/compatible 2>/dev/null || echo "(of_node compatible not exposed)"' | tee "$OUT/dt-compatible.txt"
ssh "$SBC" 'v4l2-ctl -d /dev/video0 --list-formats-ext' | tee "$OUT/formats-ext.txt"

if [ "$fail" -eq 0 ]; then
  echo "PRECONDITIONS: PASS"
else
  echo "PRECONDITIONS: FAIL"; exit 1
fi
