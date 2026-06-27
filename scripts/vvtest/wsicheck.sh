#!/usr/bin/env bash
VID="${1:-c1080.mp4}"
cd "$HOME/vvtest"
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 GST_PLUGIN_PATH="$HOME/vvtest" WEBKIT_FORCE_SANDBOX=0
pkill -9 -f epiphany 2>/dev/null; pkill -9 -f WebKit 2>/dev/null; sleep 1
pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.4
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/dev/null 2>&1 & H=$!
sleep 1
printf "%s" "<!doctype html><body style=margin:0;background:#111><video src=$VID muted loop autoplay playsinline style=width:100vw></video></body>" > wsicheck.html
epiphany --incognito-mode "http://localhost:8889/wsicheck.html" >/tmp/wsi.log 2>&1 &
sleep 10
pkill -9 -f epiphany 2>/dev/null; pkill -9 -f WebKit 2>/dev/null; kill $H 2>/dev/null
echo "clip=$VID  WSI-pitch-errors=$(grep -c "WSI pitch not properly aligned" /tmp/wsi.log)  total-lines=$(wc -l </tmp/wsi.log)"
grep -iE "MESA|WSI|EBADF" /tmp/wsi.log | grep -ivE "spell|portal|enchant" | sort | uniq -c | head -5
