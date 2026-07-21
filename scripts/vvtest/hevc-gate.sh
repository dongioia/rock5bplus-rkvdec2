#!/usr/bin/env bash
# Task 12 — HEVC byte-exact integration gate.  Run ON the SBC.
#
# Decodes each clip through the vkh265bridge (the browser path) and FULL-frame
# (luma+chroma) byte-compares the WHOLE decoded stream against ffmpeg's software
# decode.  Both sides are display order, so the compare aligns frame-for-frame
# even with B-frames.  This is a higher bar than the luma-only frame-0 H.264 gate
# (s3-multires-gate.sh): the full clip exercises every I/P/B frame, the DPB, and
# every short-term RPS in the SPS — not just intra geometry.
#
# Corpus exercises the parser, not only resolution: 3 geometries x {I-only, P, B}
# plus a longer-GOP (>=2 ST-RPS) stress.  All Main 8-bit 4:2:0, weighted-pred off.
#
# Residual (documented): long-term references are not exercised — x265 does not
# emit LT refs for these clips.  Watch #4 (DPB LONG_TERM flag) stays validated only
# by the H.264-proxy lineage + the multi-ST-RPS coverage here.  See TASK10 notes.
set +e
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
export VK_ICD_FILENAMES="$HOME/vvtest/v4l2vk_icd.aarch64.json"
export GST_PLUGIN_PATH="$HOME/vvtest"
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
SRC="${SRC:-bbb1080.mp4}"
[ -f "$SRC" ] || { echo "FATAL: source $SRC missing in $PWD"; exit 2; }

gen() { # W H name x265extra
  local W=$1 H=$2 name=$3 extra=$4
  # idempotent, but regenerate if a prior run left a truncated/corrupt clip
  [ -f "${name}.h265" ] && [ "$(stat -c%s "${name}.h265" 2>/dev/null || echo 0)" -gt 1024 ] \
    && [ -f "${name}.mp4" ] && return 0
  ffmpeg -y -loglevel error -i "$SRC" -t 2 -an -c:v libx265 \
    -x265-params "profile=main:weightp=0:weightb=0:${extra}" \
    -pix_fmt yuv420p -vf scale=${W}:${H} "${name}.mp4" 2>/dev/null \
    || { echo "  gen ${name}: ENCODE FAILED"; rm -f "${name}.h265"; return 1; }
  ffmpeg -y -loglevel error -i "${name}.mp4" -c:v copy -bsf:v hevc_mp4toannexb "${name}.h265" 2>/dev/null
}

ptypes() { # name -> "Nx I My P Kz B" frame-type histogram (grounds the coverage claim)
  ffprobe -v error -select_streams v -show_entries frame=pict_type -of csv=p=0 "$1.mp4" 2>/dev/null \
    | sort | uniq -c | awk '{gsub(/[^A-Za-z]/,"",$2); printf "%s:%s ", $2, $1}' | sed 's/ *$//'
}

pass=0; fail=0
gate() { # name W H
  local name=$1 W=$2 H=$3
  local FS=$(( W * H * 3 / 2 )) LUMA=$(( W * H ))
  [ -f "${name}.h265" ] && [ -f "${name}.mp4" ] || { echo "  ${name} (${W}x${H}): SKIP (no clip)"; fail=$((fail+1)); return; }
  local PT=$(ptypes "$name")
  ffmpeg -y -loglevel error -i "${name}.mp4" -vf format=nv12 -f rawvideo /tmp/href.nv12 2>/dev/null
  # The reference itself must be valid before it can arbitrate: non-trivial and a
  # whole number of full NV12 frames. A bad ref must FAIL, never reach cmp.
  local sref=$(stat -c%s /tmp/href.nv12 2>/dev/null || echo 0)
  if [ "$sref" -lt "$FS" ] || [ $(( sref % FS )) -ne 0 ]; then
    echo "  ${name} (${W}x${H}): FAIL (bad ffmpeg ref ${sref}B, not a multiple of ${FS})"; fail=$((fail+1)); return
  fi
  rm -f /tmp/hout.nv12
  timeout 90 gst-launch-1.0 filesrc location="${name}.h265" ! h265parse ! vkh265bridge \
    ! filesink location=/tmp/hout.nv12 >/dev/null 2>&1
  local sout=$(stat -c%s /tmp/hout.nv12 2>/dev/null || echo 0)
  # Reach cmp only when both sides are full, equal-size, frame-aligned — so a PASS
  # can never come from two truncated/empty files or a partial (timed-out) decode.
  if [ "$sout" -lt "$FS" ]; then
    echo "  ${name} (${W}x${H}): FAIL (decoder output ${sout}B < one frame)"; fail=$((fail+1)); return
  fi
  if [ "$sref" != "$sout" ]; then
    echo "  ${name} (${W}x${H}): FAIL (frame-count mismatch ref=$((sref/FS)) out=$((sout/FS)) — partial decode?)"; fail=$((fail+1)); return
  fi
  if cmp -s /tmp/href.nv12 /tmp/hout.nv12; then
    echo "  ${name} (${W}x${H}) [${PT}]: PASS (byte-exact, $(( sout / FS )) frames)"; pass=$((pass+1))
  else
    local byte=$(cmp /tmp/href.nv12 /tmp/hout.nv12 2>&1 | sed -n 's/.*byte \([0-9]*\).*/\1/p')
    local fr=$(( byte / FS )) off=$(( byte % FS ))
    local plane=$([ "$off" -lt "$LUMA" ] && echo luma || echo chroma)
    echo "  ${name} (${W}x${H}) [${PT}]: FAIL [byte ${byte}, frame ${fr}, ${plane}]"; fail=$((fail+1))
  fi
}

echo "=== HEVC byte-exact gate (full-clip luma+chroma, via vkh265bridge) ==="
for res in "1280 720 720p" "640 360 360p" "1920 1080 1080p"; do
  set -- $res; W=$1; H=$2; tag=$3
  gen $W $H "h_${tag}_ionly" "keyint=1:bframes=0";  gate "h_${tag}_ionly" $W $H
  gen $W $H "h_${tag}_p"     "bframes=0:keyint=30"; gate "h_${tag}_p"     $W $H
  gen $W $H "h_${tag}_b"     "bframes=3:keyint=30"; gate "h_${tag}_b"     $W $H
done
# longer-GOP / >=2 ST-RPS stress (one resolution is enough — RPS is geometry-independent)
gen 1280 720 "h_longgop" "bframes=3:keyint=60:no-open-gop=1"; gate "h_longgop" 1280 720

echo "=== HEVC gate: ${pass} PASS / ${fail} FAIL ==="
[ "$fail" -eq 0 ] && echo "ALL_PASS" || echo "SOME_FAIL"
exit $fail
