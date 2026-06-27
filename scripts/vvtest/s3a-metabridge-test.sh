#!/usr/bin/env bash
# Step-3 A: test the meta-bridge — WebKit should now HW-decode zero-copy.
# Bridge (rank 258) shadows v4l2slh264dec (257); injects GstVideoMeta into the
# ALLOCATION query so WebKit's sink can negotiate the rkvdec dmabuf.
set +e
VID="${1:-demo.mp4}"
cd "$HOME/vvtest" || exit 2
PIN="1:26.0.6-1"
[ "$(pacman -Q mesa 2>/dev/null | awk '{print $2}')" = "$PIN" ] || { echo "ABORT mesa pin"; exit 1; }
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
unset VK_ICD_FILENAMES GST_PLUGIN_FEATURE_RANK
export WEBKIT_FORCE_SANDBOX=0 GST_DEBUG_NO_COLOR=1
export GST_PLUGIN_PATH="$HOME/vvtest"     # <-- bring in libgstv4l2metabridge.so
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin /tmp/s3m_gst.log /tmp/s3m.png
pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.4
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/dev/null 2>&1 & H=$!
sleep 1
cat > "$HOME/vvtest/s3a.html" <<HTML
<!doctype html><body style="margin:0;background:#111">
<video id=v src="$VID" muted loop playsinline autoplay style="width:100vw;height:86vh"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:24px monospace;background:#000;z-index:9">init</div>
<script>var v=document.getElementById('v'),s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' '+v.videoWidth+'x'+v.videoHeight+(v.error?(' ERR'+v.error.code):'');}
v.addEventListener('canplay',function(){v.play();u('canplay');});
v.addEventListener('error',function(){u('VERR');});
setInterval(function(){u('tick');},500);</script></body>
HTML
export GST_DEBUG="2,GST_ELEMENT_FACTORY:4,v4l2codecs:6,videodecoder:4"
export GST_DEBUG_FILE=/tmp/s3m_gst.log
epiphany --incognito-mode "http://localhost:8889/s3a.html" >/tmp/s3m_eph.log 2>&1 & E=$!
BUSY=""
for i in $(seq 1 16); do sleep 1; f=$(fuser /dev/video0 2>/dev/null); [ -n "$f" ] && BUSY="$BUSY $i"; done
grim -o HDMI-A-1 /tmp/s3m.png 2>/dev/null
pkill -f epiphany 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null; pkill -f WebKitGPUProcess 2>/dev/null; kill $H 2>/dev/null
echo "== video0 busy seconds:${BUSY:- NONE}"
echo "== element picked =="
grep -iE "creating element .(v4l2h264metabridge|v4l2slh264dec|avdec_h264)" /tmp/s3m_gst.log | sed -E 's/.*creating element .//' | sort | uniq -c
echo "== VideoMeta allocation error still present? (want: NONE) =="
grep -iE "mandatory support of VideoMeta|Failed to negotiate with downstream|not-negotiated" /tmp/s3m_gst.log | wc -l
echo "== mesa pin POST: $(pacman -Q mesa | awk '{print $2}')"
