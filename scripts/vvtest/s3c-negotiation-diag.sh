#!/usr/bin/env bash
# Step-3 A diagnosis: capture the EXACT caps negotiation between v4l2slh264dec
# and WebKit's video sink, to confirm/refute the DMA_DRM negotiation wall.
set +e
VID="${1:-demo.mp4}"
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
unset VK_ICD_FILENAMES GST_PLUGIN_FEATURE_RANK
# sandbox off so GST env reaches the decode subprocess and it can write the log
export WEBKIT_FORCE_SANDBOX=0
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin /tmp/s3c_gst.log
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
# rich negotiation categories; GST_DEBUG to stderr -> epiphany redirect (children inherit fd)
export GST_DEBUG="3,v4l2codecs:7,videodecoder:5,GST_BUS:5,basesink:4,gldmabufbufferpool:6,glupload:5,gleglimage:6"
export GST_DEBUG_NO_COLOR=1
epiphany --incognito-mode "http://localhost:8889/s3a.html" >/tmp/s3c_gst.log 2>&1 & E=$!
sleep 10
pkill -f epiphany; pkill -f WebKitWebProcess; pkill -f WebKitGPUProcess; kill $H 2>/dev/null
echo "== log lines: $(wc -l </tmp/s3c_gst.log 2>/dev/null)"
echo "== v4l2slh264dec negotiation / errors =="
grep -iE "v4l2slh264dec|not-negotiated|not.negotiat|DMA_DRM|memory:DMABuf|reconfigure|failed to|cannot|ERROR|refused" /tmp/s3c_gst.log | grep -ivE "spell|portal|Gdk" | cut -c1-200 | head -30
