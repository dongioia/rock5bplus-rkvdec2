#!/usr/bin/env bash
# Stage-3 Goal-1b: HW-decode H.264 through WebKit's MSE path (MediaSource +
# webkitmediasrc -> decodebin -> vkh264bridge), not the progressive <video src>
# path.  Self-hosted fragmented mp4 so we control the stream (no YouTube
# VP9-default / EME).  Includes a forced seek to stress decode-session re-init.
# Run ON the SBC.
set +e
cd "$HOME/vvtest" || exit 2
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
export VK_ICD_FILENAMES="$HOME/vvtest/v4l2vk_icd.aarch64.json"
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
# MSE uses playbin3/decodebin3, which does NOT backtrack a dead-end decoder the
# way decodebin does.  So the bridge (NV12 out) must be the TOP-ranked H264
# decoder and the raw vulkanh264dec must be demoted, or decodebin3 plugs the raw
# decoder directly and its memory:VulkanImage output dies with not-negotiated.
# The bridge instantiates vulkanh264dec+vulkandownload internally by factory
# name, so their rank does not matter for that.
export GST_PLUGIN_FEATURE_RANK="vkh264bridge:512,vulkanh264dec:0,vulkandownload:0,v4l2slh264dec:0,v4l2slvideo0h264dec:0"
export GST_PLUGIN_PATH="$HOME/vvtest"
# WebKitGTK: make sure MSE is enabled (default-on in recent builds; harmless if ignored)
export WEBKIT_GST_ENABLE_MSE=1

# --- build a fragmented H.264 mp4 (Main@3.1 = avc1.4D401F), 720p, B-frames ---
if [ ! -f mse720_frag.mp4 ]; then
  ffmpeg -y -loglevel error -i bbb1080.mp4 -t 6 -an -c:v libx264 -profile:v main \
    -level:v 3.1 -pix_fmt yuv420p -g 30 -vf scale=1280:720 mse720.mp4
  ffmpeg -y -loglevel error -i mse720.mp4 -c copy \
    -movflags +frag_keyframe+empty_moov+default_base_moof mse720_frag.mp4
fi
echo "frag mp4: $(stat -c%s mse720_frag.mp4) bytes; codec string avc1.4D401F"

pkill -f "range_server.py 8889" 2>/dev/null; sleep 0.5
python3 "$HOME/vvtest/range_server.py" 8889 "$HOME/vvtest" >/tmp/httpdmse.log 2>&1 & HTTPD=$!
sleep 1

cat > "$HOME/vvtest/mse.html" <<'HTML'
<!doctype html><body style="margin:0;background:#111">
<video id=v muted playsinline style="width:100vw;height:88vh;object-fit:contain"></video>
<div id=s style="position:fixed;top:0;left:0;color:#0f0;font:20px monospace;background:#000">mse-init</div>
<script>
const v=document.getElementById('v'), s=document.getElementById('s');
function u(t){s.textContent=t+' rs='+v.readyState+' t='+v.currentTime.toFixed(2)+' '+v.videoWidth+'x'+v.videoHeight+(v.error?(' ERR'+v.error.code):'');}
const MIME='video/mp4; codecs="avc1.4D401F"';
if(!('MediaSource' in window)){u('NO_MEDIASOURCE');}
else if(!MediaSource.isTypeSupported(MIME)){u('UNSUPPORTED');}
else{
 const ms=new MediaSource();
 v.src=URL.createObjectURL(ms);
 ms.addEventListener('sourceopen', ()=>{
   let sb;
   try{ sb=ms.addSourceBuffer(MIME); }catch(e){ u('addSB_fail'); return; }
   fetch('mse720_frag.mp4').then(r=>r.arrayBuffer()).then(buf=>{
     sb.addEventListener('updateend', ()=>{ try{if(ms.readyState=='open')ms.endOfStream();}catch(e){} v.play(); u('appended'); });
     sb.addEventListener('error', ()=>u('SB_ERR'));
     try{ sb.appendBuffer(new Uint8Array(buf)); u('appending'); }catch(e){ u('append_throw'); }
   }).catch(()=>u('fetch_fail'));
 });
 v.addEventListener('playing', ()=>u('playing'));
 v.addEventListener('error', ()=>u('VERR'));
 v.addEventListener('seeked', ()=>u('seeked'));
 // backward seek once playback is established, to force a decode-session re-init
 setTimeout(()=>{ if(v.readyState>=2){ try{v.currentTime=1.0;}catch(e){} u('seeking'); } }, 8000);
}
setInterval(()=>u('tick'),600);
</script></body>
HTML

rm -f /tmp/mse.out /tmp/mse-*.png
export GST_DEBUG=2,GST_ELEMENT_FACTORY:4
epiphany --incognito-mode "http://localhost:8889/mse.html" >/tmp/mse.out 2>&1 & EPIPID=$!

HW=none; TL=()
for i in $(seq 1 16); do
  sleep 1
  F=$(fuser /dev/video0 2>/dev/null)
  if [ -n "$F" ]; then TL+=("${i}:busy"); [ "$HW" = none ] && HW="t=${i}s"; else TL+=("${i}:idle"); fi
  [ "$i" = 4 ]  && grim -o HDMI-A-1 /tmp/mse-4s.png 2>/dev/null
  [ "$i" = 8 ]  && grim -o HDMI-A-1 /tmp/mse-8s.png 2>/dev/null
  [ "$i" = 12 ] && grim -o HDMI-A-1 /tmp/mse-12s.png 2>/dev/null
done

pkill -f epiphany 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null; pkill -f WebKitGPUProcess 2>/dev/null
kill $HTTPD 2>/dev/null; sleep 1

echo "MSE  HW_FUSER_VIDEO0=$HW"
echo "TIMELINE: ${TL[*]}"
echo "--- decoder factory (want vkh264bridge/vulkanh264dec; NOT avdec_h264) ---"
sed -E 's/\x1b\[[0-9;]*m//g' /tmp/mse.out | grep -iE 'creating element.*(vulkanh264|vkh264bridge|vulkandownload|avdec_h264|v4l2sl)|MediaSource|webkitmediasrc|not-negotiat' | tail -20
echo "--- SW fallback / MSE support check ---"
grep -qi 'avdec_h264' /tmp/mse.out && echo "WARN: avdec_h264 present" || echo "OK: no avdec_h264"
