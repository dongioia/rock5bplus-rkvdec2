#!/usr/bin/env bash
# wsicheck but video WINDOWED (50vw, centered) not fullscreen
VID="${1:-w854.mp4}"; cd "$HOME/vvtest"
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 GST_PLUGIN_PATH="$HOME/vvtest" WEBKIT_FORCE_SANDBOX=0
pkill -9 -f epiphany 2>/dev/null; pkill -9 -f WebKit 2>/dev/null; sleep 1
pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.4
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/dev/null 2>&1 & H=$!
sleep 1
printf "%s" "<!doctype html><body style=\"margin:0;background:#3030a0;display:flex;justify-content:center;align-items:center;height:100vh\"><video src=$VID muted loop autoplay playsinline style=\"width:50vw\"></video></body>" > wsiwin.html
epiphany --incognito-mode "http://localhost:8889/wsiwin.html" >/tmp/wsiwin.log 2>&1 &
sleep 9
grim -o HDMI-A-1 /tmp/wsiwin.png 2>/dev/null
pkill -9 -f epiphany 2>/dev/null; pkill -9 -f WebKit 2>/dev/null; kill $H 2>/dev/null
echo "clip=$VID WINDOWED  WSI-pitch=$(grep -c "WSI pitch not properly aligned" /tmp/wsiwin.log)"
