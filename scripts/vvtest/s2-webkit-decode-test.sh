#!/usr/bin/env bash
# Run ON the SBC.  $1 = feed: "v4l2direct" (kernel) | "vulkan" (our ICD).
# Fixup #1: s2test.html is written into $HOME/vvtest BEFORE launching Epiphany,
#            served via http.server so WebKit can load it (file:// is sandboxed).
# Fixup #2: vulkan feed clears the GST registry cache so vulkanh264dec is not
#            blacklisted from a previous session without VK_ICD_FILENAMES set.
set +e
FEED="${1:-v4l2direct}"; cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000

[ "$FEED" = "vulkan" ] && export VK_ICD_FILENAMES="$HOME/vvtest/v4l2vk_icd.aarch64.json"

# Fixup #2: clear stale registry for vulkan feed so vulkanh264dec re-registers
[ "$FEED" = "vulkan" ] && rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin

# Kill any leftover http.server from previous run
pkill -f "http.server 8889" 2>/dev/null
sleep 0.5

# Start HTTP server serving $HOME/vvtest/
python3 -m http.server 8889 -d "$HOME/vvtest" >/tmp/httpd2.log 2>&1 & HTTPD=$!
sleep 1

# Fixup #1: write HTML into $HOME/vvtest BEFORE launching Epiphany
cat > "$HOME/vvtest/s2test.html" <<'HTML'
<!doctype html><body style="margin:0;background:#111">
<video id=v src="case1.mp4" muted loop playsinline autoplay
 style="width:100vw;height:88vh;object-fit:contain"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:22px monospace;background:#000">init</div>
<script>const v=document.getElementById('v'),s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' t='+v.currentTime.toFixed(2)+' '+v.videoWidth+'x'+v.videoHeight;}
v.addEventListener('canplay',function(){v.play();u('canplay');});
v.addEventListener('playing',function(){u('playing');});
v.addEventListener('error',function(){u('VERR_'+(v.error&&v.error.code));});
setInterval(function(){u('tick');},700);</script></body>
HTML

# Clean up prior run artifacts
rm -f /tmp/s2.out /tmp/s2.png

export GST_DEBUG=2,GST_ELEMENT_FACTORY:4

# Fixup #1 (cont.): open clean URL — NOT file://, NOT /../ path
epiphany --incognito-mode "http://localhost:8889/s2test.html" >/tmp/s2.out 2>&1 &
EPIPID=$!

HW_FUSER=none
for i in $(seq 1 12); do
    sleep 1
    F=$(fuser /dev/video0 2>/dev/null)
    [ -n "$F" ] && [ "$HW_FUSER" = none ] && HW_FUSER="t=${i}s"
    [ "$i" = 9 ] && grim -o HDMI-A-1 /tmp/s2.png 2>/dev/null
done

pkill -f epiphany 2>/dev/null
pkill -f WebKitWebProcess 2>/dev/null
pkill -f WebKitGPUProcess 2>/dev/null
kill $HTTPD 2>/dev/null
sleep 1

echo "FEED=$FEED  FUSER_VIDEO0=$HW_FUSER"
# Strip ANSI color codes and show decoder/VideoMeta/negotiation markers
sed -E 's/\x1b\[[0-9;]*m//g' /tmp/s2.out \
  | grep -iE 'creating element|VideoMeta|not-negotiat|Failed to negotiate|webkitmediaplayer' \
  | tail -25
