#!/usr/bin/env bash
# Phase-C Step-3 Increment A: in-browser zero-copy via the GStreamer v4l2codecs
# dmabuf path — NO Vulkan ICD, NO vkh264bridge, NO forced ranks.
#
# Inverts s3-realh264-test.sh: that one forces vulkanh264dec+bridge and zeroes
# v4l2sl* to drive the Vulkan path. Here we leave DEFAULT ranks so WebKit's
# decodebin naturally picks v4l2slh264dec (rank primary+1, 257) over avdec, and
# its frames (memory:DMABuf NV12) reach WebKit's DMABuf renderer with no copy.
#
# Arg1 = mp4 already in ~/vvtest (default c1080.mp4). Run ON the SBC.
#   ssh rock5b '~/vvtest/s3a-webkit-default-rank.sh c1080.mp4'
set +e
VID="${1:-c1080.mp4}"
cd "$HOME/vvtest" || exit 2
[ -f "$VID" ] || { echo "FATAL: $VID not in ~/vvtest"; exit 2; }

PIN="1:26.0.6-1"
[ "$(pacman -Q mesa 2>/dev/null | awk '{print $2}')" = "$PIN" ] || { echo "ABORT: mesa not $PIN"; exit 1; }

export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
# A = non-Vulkan: do NOT set VK_ICD_FILENAMES, do NOT add bridge to GST_PLUGIN_PATH,
# do NOT set GST_PLUGIN_FEATURE_RANK. Clear registry so default ranks apply clean.
unset VK_ICD_FILENAMES GST_PLUGIN_FEATURE_RANK
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin

pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.5
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/tmp/s3a_httpd.log 2>&1 & HTTPD=$!
sleep 1

cat > "$HOME/vvtest/s3a.html" <<HTML
<!doctype html><body style="margin:0;background:#111">
<video id=v src="$VID" muted loop playsinline autoplay
 style="width:100vw;height:88vh;object-fit:contain"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:22px monospace;background:#000">init</div>
<script>const v=document.getElementById('v'),s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' t='+v.currentTime.toFixed(2)+' '+v.videoWidth+'x'+v.videoHeight;}
v.addEventListener('canplay',function(){v.play();u('canplay');});
v.addEventListener('playing',function(){u('playing');});
v.addEventListener('error',function(){u('VERR_'+(v.error&&v.error.code));});
setInterval(function(){u('tick');},700);</script></body>
HTML

rm -f /tmp/s3a_gst.log /tmp/s3a_eph.log /tmp/s3a-*.png
# decodebin element selection + v4l2 + webkit media path
export GST_DEBUG="GST_ELEMENT_FACTORY:4,decodebin:5,v4l2codecs:4,v4l2videodec:4"
export GST_DEBUG_FILE=/tmp/s3a_gst.log
export WEBKIT_DEBUG="Media"
epiphany --incognito-mode "http://localhost:8889/s3a.html" >/tmp/s3a_eph.log 2>&1 & EPIPID=$!

HW=none; TL=()
for i in $(seq 1 16); do
    sleep 1
    F=$(fuser /dev/video0 2>/dev/null)
    if [ -n "$F" ]; then TL+=("${i}:busy"); [ "$HW" = none ] && HW="t=${i}s"; else TL+=("${i}:idle"); fi
    [ "$i" = 6 ]  && grim -o HDMI-A-1 /tmp/s3a-6s.png 2>/dev/null
    [ "$i" = 12 ] && grim -o HDMI-A-1 /tmp/s3a-12s.png 2>/dev/null
done

swaymsg -t get_tree 2>/dev/null | grep -iE '"app_id"|"visible"' | head
pkill -f epiphany 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null; pkill -f WebKitGPUProcess 2>/dev/null
kill $HTTPD 2>/dev/null

echo "== fuser /dev/video0 timeline: ${TL[*]}"
echo "== HW first busy: $HW"
echo "== mesa pin POST: $(pacman -Q mesa | awk '{print $2}') (want $PIN)"
echo "s3a done"
