#!/usr/bin/env bash
# Step-3 A diagnostic: longer window + busy-seconds + overlay, default ranks.
set +e
VID="${1:-demo.mp4}"; SECS="${2:-24}"
cd "$HOME/vvtest" || exit 2
[ -f "$VID" ] || { echo "FATAL: $VID missing"; exit 2; }
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
unset VK_ICD_FILENAMES GST_PLUGIN_FEATURE_RANK
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.5
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/tmp/s3a_httpd.log 2>&1 & H=$!
sleep 1
cat > "$HOME/vvtest/s3a.html" <<HTML
<!doctype html><body style="margin:0;background:#111">
<video id=v src="$VID" muted loop playsinline autoplay style="width:100vw;height:86vh;object-fit:contain"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:26px monospace;background:#000;z-index:9">init</div>
<script>var v=document.getElementById('v'),s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' t='+v.currentTime.toFixed(2)+' '+v.videoWidth+'x'+v.videoHeight+(v.error?(' ERR'+v.error.code):'');}
v.addEventListener('canplay',function(){v.play();u('canplay');});
v.addEventListener('playing',function(){u('playing');});
v.addEventListener('error',function(){u('VERR');});
setInterval(function(){u('tick');},500);</script></body>
HTML
rm -f /tmp/s3a2_gst.log /tmp/s3a2.png
export GST_DEBUG="GST_ELEMENT_FACTORY:4" GST_DEBUG_FILE=/tmp/s3a2_gst.log
epiphany --incognito-mode "http://localhost:8889/s3a.html" >/tmp/s3a2_eph.log 2>&1 & E=$!
BUSY=""
for i in $(seq 1 "$SECS"); do sleep 1; F=$(fuser /dev/video0 2>/dev/null); [ -n "$F" ] && BUSY="$BUSY $i"; done
grim -o HDMI-A-1 /tmp/s3a2.png 2>/dev/null
pkill -f epiphany 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null; pkill -f WebKitGPUProcess 2>/dev/null; kill $H 2>/dev/null
echo "== video0 busy seconds:${BUSY:- NONE}"
echo "== decoders created =="
grep -iE "creating element .(v4l2slh264dec|avdec_h264|openh264dec)" /tmp/s3a2_gst.log | sed -E 's/.*creating element .//' | sort | uniq -c
