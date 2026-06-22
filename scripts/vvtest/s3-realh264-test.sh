#!/usr/bin/env bash
# Stage-3 Goal-1: HW-decode a REAL-world H264 clip (not the synthetic testsrc-
# derived case1) in WebKitGTK (Epiphany) through the standalone Vulkan ICD +
# vkh264bridge.  Proves the bridge handles real GOP / B-frames / entropy / res.
#
# Arg1 = mp4 filename already present inside ~/vvtest.  Run ON the SBC.
#   ssh rock5b 'bash -s yt_h264.mp4' < scripts/vvtest/s3-realh264-test.sh
#
# Env mirrors the proven Stage-2 s2-webkit-decode-test.sh combo:
#   - VK_ICD_FILENAMES -> isolated ICD (mesa system pin untouched)
#   - clear GST registry so vulkanh264dec re-registers under the ICD
#   - rank: bump vulkanh264dec+vulkandownload, ZERO v4l2sl* so decodebin
#     auto-plugs our vkh264bridge (rank 258, NV12 system-mem src) instead.
set +e
VID="${1:-yt_h264.mp4}"
cd "$HOME/vvtest" || exit 2
[ -f "$VID" ] || { echo "FATAL: $VID not in ~/vvtest"; exit 2; }

export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
export VK_ICD_FILENAMES="$HOME/vvtest/v4l2vk_icd.aarch64.json"
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
export GST_PLUGIN_FEATURE_RANK="vulkanh264dec:512,vulkandownload:512,v4l2slh264dec:0,v4l2slvideo0h264dec:0"
export GST_PLUGIN_PATH="$HOME/vvtest${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"

pkill -f "range_server.py 8889" 2>/dev/null
pkill -f "http.server 8889" 2>/dev/null
sleep 0.5
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/tmp/httpd3.log 2>&1 & HTTPD=$!
sleep 1

cat > "$HOME/vvtest/s3test.html" <<HTML
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

rm -f /tmp/s3.out /tmp/s3-*.png
export GST_DEBUG=2,GST_ELEMENT_FACTORY:4
epiphany --incognito-mode "http://localhost:8889/s3test.html" >/tmp/s3.out 2>&1 & EPIPID=$!

HW=none; TL=()
for i in $(seq 1 18); do
    sleep 1
    F=$(fuser /dev/video0 2>/dev/null)
    if [ -n "$F" ]; then TL+=("${i}:busy"); [ "$HW" = none ] && HW="t=${i}s"; else TL+=("${i}:idle"); fi
    [ "$i" = 8 ]  && grim -o HDMI-A-1 /tmp/s3-8s.png 2>/dev/null
    [ "$i" = 14 ] && grim -o HDMI-A-1 /tmp/s3-14s.png 2>/dev/null
done

pkill -f epiphany 2>/dev/null
pkill -f WebKitWebProcess 2>/dev/null
pkill -f WebKitGPUProcess 2>/dev/null
kill $HTTPD 2>/dev/null
sleep 1

echo "VID=$VID  HW_FUSER_VIDEO0=$HW"
echo "TIMELINE: ${TL[*]}"
echo "--- decoder factory selection (want vkh264bridge / vulkanh264dec; NOT avdec_h264) ---"
sed -E 's/\x1b\[[0-9;]*m//g' /tmp/s3.out \
  | grep -iE 'creating element.*(vulkanh264|vkh264bridge|vulkandownload|avdec_h264|v4l2sl)|not-negotiat|Failed to negotiate' \
  | tail -20
echo "--- SW fallback check ---"
if grep -qi 'avdec_h264' /tmp/s3.out; then echo "WARN: avdec_h264 present (SW fallback)"; else echo "OK: no avdec_h264 in log"; fi
