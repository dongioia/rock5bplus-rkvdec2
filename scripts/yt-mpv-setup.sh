#!/bin/bash
# yt-mpv VP9 handoff setup — Rock 5B+ / OPi5+ / any RK3588 SBC on Beryllium OS / archlinuxarm
#
# Native V4L2 stateless path via ffmpeg-v4l2-requests, no libva involvement.
# Following Nicolas Dufresne's 2026-05-20 LKML guidance: libva-v4l2-request is
# deprecated; FFmpeg / GStreamer / Chromium drive RKVDEC2 directly.
#
# Run as a regular user (sudo prompts inline).
# Usage:  curl -fsSL https://raw.githubusercontent.com/dongioia/rock5bplus-rkvdec2/main/scripts/yt-mpv-setup.sh | bash
#   or:   bash yt-mpv-setup.sh

set -euo pipefail

say() { printf "\n\033[1;36m▌ %s\033[0m\n" "$*"; }
warn() { printf "\n\033[1;33m▌ %s\033[0m\n" "$*"; }

say "1/5 install mpv + yt-dlp"
sudo pacman -S --needed --noconfirm mpv yt-dlp

say "2/5 verify ffmpeg-v4l2-requests"
if ! ffmpeg -hwaccels 2>&1 | grep -q v4l2request; then
  warn "ffmpeg-v4l2-requests is missing or not active."
  cat <<'EOF'

  The Beryllium repo binary may be pinned against older libplacebo/libvpx
  than your system has; in that case rebuild locally:

      git clone --depth=1 https://github.com/beryllium-org/sbc-pkgbuilds.git
      cd sbc-pkgbuilds/ffmpeg-v4l2-requests
      makepkg -si

  After install, re-run this script. (If pacman complains about
  libplacebo.so / libvpx.so version pins from the prebuilt package, the
  local rebuild against your current libraries fixes it automatically.)
EOF
  exit 1
fi
echo "  v4l2request hwaccel present"

say "3/5 write ~/.config/mpv/mpv.conf"
mkdir -p ~/.config/mpv
cat > ~/.config/mpv/mpv.conf <<'EOF'
hwdec=v4l2request-copy
vo=gpu-next,gpu
gpu-api=vulkan
gpu-context=waylandvk
cache=yes
demuxer-max-bytes=500M
ytdl-format=bestvideo[height<=?2160][fps<=?60]+bestaudio/best
profile=fast
EOF
echo "  mpv.conf written"

say "4/5 register mpv:// protocol handler"
sudo tee /usr/local/bin/open-in-mpv >/dev/null <<'SH'
#!/bin/bash
url="${1#mpv://play/}"; url="${url#mpv://}"
url=$(python3 -c "import sys,urllib.parse;print(urllib.parse.unquote(sys.argv[1]))" "$url" 2>/dev/null || echo "$url")
exec mpv --force-window=immediate "$url" &
SH
sudo chmod +x /usr/local/bin/open-in-mpv

mkdir -p ~/.local/share/applications
cat > ~/.local/share/applications/open-in-mpv.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Open in mpv
Exec=/usr/local/bin/open-in-mpv %u
NoDisplay=true
MimeType=x-scheme-handler/mpv;
EOF
xdg-mime default open-in-mpv.desktop x-scheme-handler/mpv
echo "  mpv:// handler: $(xdg-mime query default x-scheme-handler/mpv)"

say "5/5 verify"
ffmpeg -hwaccels 2>&1 | grep v4l2request && echo "  ffmpeg v4l2request: OK"
xdg-mime query default x-scheme-handler/mpv | grep -q mpv && echo "  mpv:// scheme: OK"
mpv --version | head -1

cat <<'EOF'

▌ Browser side

Install the Tampermonkey extension in your browser, then open this URL
to install the auto-redirect userscript:

  https://raw.githubusercontent.com/dongioia/rock5bplus-rkvdec2/main/scripts/yt-mpv-vp9.user.js

Tampermonkey will detect the .user.js extension and prompt to install.

Behaviour:
  - Visits any YouTube watch page
  - Inspects the active codec via getStatsForNerds().codecs
  - If VP9 (vp09.*) → redirects to mpv:// (mpv plays via V4L2 stateless HW decode)
  - If AV1 / H.264 → stays in the browser (renders cleanly on Mali Valhall)
  - Uses sessionStorage to avoid back-button redirect loops

Bookmarklet alternative (one-shot, no auto-redirect):
  javascript:location.href='mpv://'+encodeURIComponent(location.href)

▌ Test

  mpv 'https://www.youtube.com/watch?v=aqz-KE-bpKQ'   # Big Buck Bunny 4K VP9
  # Expect: V4L2 stateless HW decode, gpu-next/Vulkan render
  # ~17–21 % CPU @ 1440p on Rock 5B+

▌ Future (June 2026)

Chromium 150 stable (target 2026-06-17) ships the equivalent V4L2 VP9
fix natively via gerrit CL 7794420 (d20dfa664). Once your distro bumps
to 150, the in-tree LibYUV bypass and this mpv handoff become optional —
stock chromium 150 will decode VP9 through the same RKVDEC2 driver path
directly. The userscript still works either way.
EOF
