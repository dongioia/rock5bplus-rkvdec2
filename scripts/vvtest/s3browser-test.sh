#!/usr/bin/env bash
# Generic in-browser HW-decode test via the meta-bridge. Per-clip logs/shots.
# Usage: s3browser-test.sh <clip-in-~/vvtest> <tag>
set +e
VID="${1:?clip}"; TAG="${2:-out}"
cd "$HOME/vvtest" || exit 2
[ -f "$VID" ] || { echo "FATAL: $VID missing"; exit 2; }
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 WEBKIT_FORCE_SANDBOX=0 GST_DEBUG_NO_COLOR=1
export GST_PLUGIN_PATH="$HOME/vvtest"
LOG="/tmp/b_${TAG}.log"; PNG="/tmp/b_${TAG}.png"
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin "$LOG" "$PNG"
pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.4
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/dev/null 2>&1 & H=$!
sleep 1
cat > "$HOME/vvtest/b_${TAG}.html" <<HTML
<!doctype html><body style="margin:0;background:#111">
<video id=v src="$VID" muted loop playsinline autoplay style="width:100vw;height:100vh"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:26px monospace;background:#000;z-index:9">init</div>
<script>var v=document.getElementById('v'),s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' '+v.videoWidth+'x'+v.videoHeight+(v.error?(' ERR'+v.error.code):'');}
v.addEventListener('error',function(){u('VERR');});
setInterval(function(){u('tick');},400);</script></body>
HTML
export GST_DEBUG="2,GST_ELEMENT_FACTORY:4,v4l2codecs:5,videodecoder:4" GST_DEBUG_FILE="$LOG"
epiphany --incognito-mode "http://localhost:8889/b_${TAG}.html" >/dev/null 2>&1 &
B=""
for i in $(seq 1 24); do sleep 0.25
  for n in 0 1 2 3 4; do [ -n "$(fuser /dev/video$n 2>/dev/null)" ] && B="$B v$n"; done
done
grim -o HDMI-A-1 "$PNG" 2>/dev/null
pkill -f epiphany 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null; pkill -f WebKitGPUProcess 2>/dev/null; kill $H 2>/dev/null
echo "== [$TAG] video busy: $(echo $B | tr ' ' '\n' | grep . | sort | uniq -c | tr '\n' ' ')"
echo "== decoders created =="
grep -iE "creating element .(metabridge|v4l2sl|vp9dec|av1dec|dav1d|avdec|vp8dec)" "$LOG" | sed -E 's/.*creating element .//' | sort | uniq -c
echo "== DMABuf occ: $(grep -c 'memory:DMABuf' "$LOG")  VideoMeta/not-neg fails: $(grep -ic 'VideoMeta\|not-negotiated' "$LOG")"
echo "== errors =="
grep -iE "ERROR|not.support|no decoder|missing.plugin|GstVideoMeta support required|streamon|VIDIOC.*fail" "$LOG" | grep -ivE "spell|portal|Gdk|enchant" | cut -c1-140 | head -5
echo "== shot: $(ls -la "$PNG" 2>/dev/null | awk '{print $5}')B"
