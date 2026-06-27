#!/usr/bin/env bash
# Verify meta-bridge: HW device busy + zero-copy (DMABuf) negotiated caps.
set +e
VID="${1:-c1080.mp4}"
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 WEBKIT_FORCE_SANDBOX=0 GST_DEBUG_NO_COLOR=1
export GST_PLUGIN_PATH="$HOME/vvtest"
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin /tmp/s3mv.log
pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.4
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/dev/null 2>&1 & H=$!
sleep 1
cat > "$HOME/vvtest/s3a.html" <<HTML
<!doctype html><body style="margin:0;background:#111">
<video id=v src="$VID" muted loop playsinline autoplay style="width:100vw;height:86vh"></video></body>
HTML
export GST_DEBUG="2,v4l2codecs:5,videodecoder:5" GST_DEBUG_FILE=/tmp/s3mv.log
epiphany --incognito-mode "http://localhost:8889/s3a.html" >/dev/null 2>&1 &
V0=0; V1=0; V2=0; V3=0
for i in $(seq 1 40); do
  sleep 0.25
  [ -n "$(fuser /dev/video0 2>/dev/null)" ] && V0=$((V0+1))
  [ -n "$(fuser /dev/video1 2>/dev/null)" ] && V1=$((V1+1))
  [ -n "$(fuser /dev/video2 2>/dev/null)" ] && V2=$((V2+1))
  [ -n "$(fuser /dev/video3 2>/dev/null)" ] && V3=$((V3+1))
done
pkill -f epiphany 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null; pkill -f WebKitGPUProcess 2>/dev/null; kill $H 2>/dev/null
echo "== video device busy samples (of 40, 0.25s): v0=$V0 v1=$V1 v2=$V2 v3=$V3"
echo "== DMABuf occurrences in decoder negotiation (zero-copy indicator): $(grep -c 'memory:DMABuf' /tmp/s3mv.log)"
echo "== negotiated caps lines (DMABuf vs system) =="
grep -iE "memory:DMABuf|format=.string.NV12" /tmp/s3mv.log | grep -iE "raw" | sed -E 's/^.*(video.x-raw[^,;]*)(,[^;]*format=.string.[A-Z0-9_]*)?.*/\1\2/' | sort -u | head -4
echo "== VideoMeta / not-negotiated failures (want 0): $(grep -ic 'VideoMeta\|not-negotiated' /tmp/s3mv.log)"
echo "== avdec_h264 SW created (want 0): $(grep -c 'creating element .avdec_h264' /tmp/s3mv.log)"
