// ==UserScript==
// @name         YouTube → mpv for VP9 streams
// @namespace    https://github.com/dongioia/rock5bplus-rkvdec2
// @version      1.0.0
// @description  Hand VP9 YouTube streams off to mpv via the mpv:// protocol handler. AV1/H.264 stay in the browser. Gates on the active codec via getStatsForNerds() — not resolution — because the Skia GrYUVtoRGB miscompile on Mali Valhall hits any VP9 frame.
// @author       dongioia
// @match        https://www.youtube.com/watch*
// @run-at       document-idle
// @grant        none
// @license      GPL-2.0-or-later
// @homepageURL  https://github.com/dongioia/rock5bplus-rkvdec2
// @updateURL    https://raw.githubusercontent.com/dongioia/rock5bplus-rkvdec2/main/scripts/yt-mpv-vp9.user.js
// @downloadURL  https://raw.githubusercontent.com/dongioia/rock5bplus-rkvdec2/main/scripts/yt-mpv-vp9.user.js
// ==/UserScript==

(function () {
  'use strict';

  const v = new URL(location.href).searchParams.get('v');
  if (!v) return;

  // Back-button safety: if we already redirected this video this session,
  // don't loop the next time the user comes back.
  if (sessionStorage.getItem('ytmpv-skip-' + v) === '1') return;

  let tries = 0;
  const probe = setInterval(() => {
    const player = document.querySelector('#movie_player');
    if (!player || typeof player.getStatsForNerds !== 'function') {
      if (++tries > 40) clearInterval(probe);   // give up after ~20 s
      return;
    }

    const stats = player.getStatsForNerds() || {};
    // codecs string looks like: "vp09.00.40.08 (244) / mp4a.40.2 (140)"
    //                     or:    "av01.0.05M.08 (399) / opus (251)"
    //                     or:    "avc1.4d401f (134) / mp4a.40.2 (140)"
    const codecs = stats.codecs || '';
    if (!codecs) return;

    clearInterval(probe);

    const isVP9 = /\bvp09\b/i.test(codecs);
    if (!isVP9) return;

    // Optional: keep small VP9 clips in the browser. Uncomment to gate on resolution too.
    // const q = (typeof player.getPlaybackQuality === 'function') ? player.getPlaybackQuality() : '';
    // if (!/^hd(1080|1440|2160|2880|4320)$/.test(q)) return;

    sessionStorage.setItem('ytmpv-skip-' + v, '1');
    location.href = 'mpv://' + location.href;
  }, 500);
})();
