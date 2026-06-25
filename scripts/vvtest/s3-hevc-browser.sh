#!/usr/bin/env bash
# Task 13 — HEVC HW-decode in WebKitGTK (Epiphany) through the standalone Vulkan
# ICD + vkh265bridge.  Two paths, fork of s3-realh264-test.sh + s3-mse-test.sh:
#   progressive : <video src=hevc_case1.mp4>  (decodebin auto-plug)
#   mse         : MediaSource + fragmented HEVC mp4 (decodebin3 via webkitmediasrc)
# Run ON the SBC.  Arg1 = progressive | mse | both (default both).
#
# Isolated ICD (VK_ICD_FILENAMES, system mesa pin untouched), clear GST registry,
# rank vkh265bridge TOP and demote raw vulkanh265dec/vulkandownload/v4l2sl* and the
# SW HEVC decoders to 0.  Per the Stage-2/3 finding, decodebin treats the raw
# vulkanh265dec memory:VulkanImage SRC as a dead end; the bridge (NV12 system-mem
# SRC, rank 258 compiled, 512 here) is what decodebin(3) auto-plugs.  The bridge
# instantiates vulkanh265dec+vulkandownload internally by factory NAME, so their
# rank 0 does not stop it.
set +e
MODE="${1:-both}"
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
export VK_ICD_FILENAMES="$HOME/vvtest/v4l2vk_icd.aarch64.json"
export GST_PLUGIN_PATH="$HOME/vvtest"
export WEBKIT_GST_ENABLE_MSE=1
RANKS="vkh265bridge:512,vulkanh265dec:0,vulkandownload:0,v4l2slh265dec:0,v4l2slvideo0h265dec:0,avdec_h265:0,libde265dec:0"
FAILED=0

start_httpd() {
  pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.5
  python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/tmp/httpdh265.log 2>&1 &
  HTTPD=$!; sleep 1
}
stop_all() {
  pkill -f epiphany 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null
  pkill -f WebKitGPUProcess 2>/dev/null; kill $HTTPD 2>/dev/null; sleep 1
  # Wait for rkvdec to be released so the NEXT run's fuser reading is not a stale
  # leftover (a hung decoder could otherwise read as "HW active" for the next path).
  local j
  for j in $(seq 1 6); do [ -z "$(fuser /dev/video0 2>/dev/null)" ] && break; sleep 1; done
}
watch_decode() { # N tag
  local N=$1 tag=$2 i F; HW=none; TL=()
  for i in $(seq 1 "$N"); do
    sleep 1
    F=$(fuser /dev/video0 2>/dev/null)
    if [ -n "$F" ]; then TL+=("${i}:busy"); [ "$HW" = none ] && HW="t=${i}s"; else TL+=("${i}:idle"); fi
    [ "$i" = 8 ]  && grim -o HDMI-A-1 "/tmp/${tag}-8s.png" 2>/dev/null
    [ "$i" = 14 ] && grim -o HDMI-A-1 "/tmp/${tag}-14s.png" 2>/dev/null
  done
}
report() { # tag logfile
  local tag=$1 log=$2 v=PASS r="" clean
  clean=$(sed -E 's/\x1b\[[0-9;]*m//g' "$log")
  # Verdict needs BOTH a positive (our bridge was plugged) and a negative (no other
  # HEVC decoder ran). NOTE v4l2slh265dec is HARDWARE too (kernel rkvdec path) — if
  # rank-demotion failed and decodebin plugged it, fuser would still read busy and a
  # SW-only check would falsely credit OUR Vulkan-ICD path. So reject it explicitly.
  echo "$clean" | grep -qE 'creating element "vkh265bridge"' || { v=FAIL; r="$r no-vkh265bridge"; }
  if echo "$clean" | grep -qiE 'creating element "(v4l2slh265dec|v4l2slvideo0h265dec|avdec_h265|libde265dec|de265dec)"'; then
    v=FAIL; r="$r competing-decoder-plugged"
  fi
  [ "$HW" = none ] && { v=FAIL; r="$r rkvdec-never-busy"; }
  [ "$v" = FAIL ] && FAILED=1
  echo "${tag} VERDICT: ${v}  HW_FUSER_VIDEO0=${HW}${r:+ —${r}}"
  echo "TIMELINE: ${TL[*]}"
  echo "--- decoder factory (want vkh265bridge; reject v4l2sl*/avdec_h265/de265) ---"
  echo "$clean" | grep -iE 'creating element.*(vulkanh265|vkh265bridge|vulkandownload|avdec_h265|libde265|de265|v4l2sl)|MediaSource|webkitmediasrc|not-negotiat' | tail -20
}

run_progressive() {
  echo "================ PROGRESSIVE (<video src>) ================"
  local VID="${HEVC_VID:-hevc_case1.mp4}"
  [ -f "$VID" ] || { echo "FATAL: $VID not in ~/vvtest"; return 2; }
  rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
  export GST_PLUGIN_FEATURE_RANK="$RANKS"
  start_httpd
  cat > "$HOME/vvtest/h265prog.html" <<HTML
<!doctype html><body style="margin:0;background:#111">
<video id=v src="$VID" muted loop playsinline autoplay style="width:100vw;height:88vh;object-fit:contain"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:22px monospace;background:#000">init</div>
<script>const v=document.getElementById('v'),s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' t='+v.currentTime.toFixed(2)+' '+v.videoWidth+'x'+v.videoHeight+(v.error?(' ERR'+v.error.code):'');}
v.addEventListener('canplay',function(){v.play();u('canplay');});
v.addEventListener('playing',function(){u('playing');});
v.addEventListener('error',function(){u('VERR_'+(v.error&&v.error.code));});
setInterval(function(){u('tick');},700);</script></body>
HTML
  rm -f /tmp/h265prog.out /tmp/h265prog-*.png
  export GST_DEBUG=2,GST_ELEMENT_FACTORY:4
  epiphany --incognito-mode "http://localhost:8889/h265prog.html" >/tmp/h265prog.out 2>&1 &
  watch_decode 18 h265prog
  stop_all
  report "PROGRESSIVE" /tmp/h265prog.out
}

run_mse() {
  echo "================ MSE (MediaSource fragmented) ================"
  rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
  export GST_PLUGIN_FEATURE_RANK="$RANKS"
  if [ ! -s msehevc720_frag.mp4 ]; then
    # NB: do NOT pass x265 level-idc — it makes libx265 refuse to open the encoder
    # on this build. Let x265 pick the level; we read it back for the MIME below.
    ffmpeg -y -loglevel error -i bbb1080.mp4 -t 6 -an -c:v libx265 \
      -x265-params "profile=main:weightp=0:weightb=0:bframes=3" \
      -pix_fmt yuv420p -g 30 -vf scale=1280:720 -tag:v hvc1 msehevc720.mp4 2>/dev/null \
      || { echo "FATAL: HEVC encode failed"; return 2; }
    ffmpeg -y -loglevel error -i msehevc720.mp4 -c copy -tag:v hvc1 \
      -movflags +frag_keyframe+empty_moov+default_base_moof msehevc720_frag.mp4 2>/dev/null
  fi
  [ -s msehevc720_frag.mp4 ] || { echo "FATAL: fragmented mp4 missing"; return 2; }
  # Derive the MSE codec string from the actual stream (x265 chooses the level —
  # 720p here lands at L120). A hardcoded level would risk an isTypeSupported miss.
  local LVL=$(ffprobe -v error -select_streams v:0 -show_entries stream=level \
                -of default=nokey=1:noprint_wrappers=1 msehevc720.mp4)
  case "$LVL" in ''|*[!0-9]*) echo "FATAL: ffprobe returned no numeric level ('$LVL') — MIME would be malformed"; return 2;; esac
  local CODEC="hvc1.1.6.L${LVL}.B0"
  echo "frag mp4: $(stat -c%s msehevc720_frag.mp4) bytes; MIME codecs=${CODEC}"
  start_httpd
  cat > "$HOME/vvtest/h265mse.html" <<'HTML'
<!doctype html><body style="margin:0;background:#111">
<video id=v muted playsinline style="width:100vw;height:88vh;object-fit:contain"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:20px monospace;background:#000">mse-init</div>
<script>
const v=document.getElementById('v'), s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' t='+v.currentTime.toFixed(2)+' '+v.videoWidth+'x'+v.videoHeight+(v.error?(' ERR'+v.error.code):'');}
const MIME='video/mp4; codecs="__CODEC__"';
if(!('MediaSource' in window)){u('NO_MEDIASOURCE');}
else if(!MediaSource.isTypeSupported(MIME)){u('UNSUPPORTED');}
else{
 const ms=new MediaSource(); v.src=URL.createObjectURL(ms);
 ms.addEventListener('sourceopen', ()=>{
   let sb; try{ sb=ms.addSourceBuffer(MIME); }catch(e){ u('addSB_fail'); return; }
   fetch('msehevc720_frag.mp4').then(r=>r.arrayBuffer()).then(buf=>{
     sb.addEventListener('updateend', ()=>{ try{if(ms.readyState=='open')ms.endOfStream();}catch(e){} v.play(); u('appended'); });
     sb.addEventListener('error', ()=>u('SB_ERR'));
     try{ sb.appendBuffer(new Uint8Array(buf)); u('appending'); }catch(e){ u('append_throw'); }
   }).catch(()=>u('fetch_fail'));
 });
 v.addEventListener('playing', ()=>u('playing'));
 v.addEventListener('error', ()=>u('VERR'));
 v.addEventListener('seeked', ()=>u('seeked'));
 setTimeout(()=>{ if(v.readyState>=2){ try{v.currentTime=1.0;}catch(e){} u('seeking'); } }, 8000);
}
setInterval(()=>u('tick'),600);
</script></body>
HTML
  sed -i "s/__CODEC__/${CODEC}/" "$HOME/vvtest/h265mse.html"
  rm -f /tmp/h265mse.out /tmp/h265mse-*.png
  export GST_DEBUG=2,GST_ELEMENT_FACTORY:4
  epiphany --incognito-mode "http://localhost:8889/h265mse.html" >/tmp/h265mse.out 2>&1 &
  watch_decode 16 h265mse
  stop_all
  report "MSE" /tmp/h265mse.out
}

{ [ "$MODE" = progressive ] || [ "$MODE" = both ]; } && { run_progressive || FAILED=1; }
{ [ "$MODE" = mse ] || [ "$MODE" = both ]; } && { run_mse || FAILED=1; }
if [ "$FAILED" = 0 ]; then
  echo "=== T13 PASS: HEVC HW-decoded via vkh265bridge (no SW/competing decoder). screenshots: /tmp/h265prog-*.png /tmp/h265mse-*.png ==="
else
  echo "=== T13 FAIL: see VERDICT line(s) above. screenshots: /tmp/h265prog-*.png /tmp/h265mse-*.png ==="
fi
exit $FAILED
