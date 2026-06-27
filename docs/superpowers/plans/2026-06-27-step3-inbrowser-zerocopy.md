# Step-3 In-Browser Zero-Copy (WebKit) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hardware-decoded video playing inside WebKitGTK with the decoded frame reaching WebKit's compositor as a dmabuf — no `vulkandownload`, no CPU readback.

**Architecture:** Increment A proves the in-browser zero-copy plumbing via the GStreamer `v4l2codecs` dmabuf path (no Vulkan), isolating WebKit's dmabuf-import variables. The C spike then determines whether the standalone Vulkan ICD can be made zero-copy (output VkImage backed by the V4L2 CAPTURE dmabuf). C implementation is authored in a separate plan only after a GO spike verdict.

**Tech Stack:** WebKitGTK-6.0 2.52.4 (Epiphany), GStreamer `v4l2codecs` (`v4l2slh264dec`), kernel rkvdec V4L2 stateless, sway/Wayland, PanVK `~/mesa-zc/` (!42353), the sree/mesa Vulkan-V4L2 ICD (`mesa-sree-tree` volume).

## Global Constraints

- System mesa pin `1:26.0.6-1` MUST be unchanged before/after every run (assert in every runner).
- ICD + bridge stay isolated: `~/mesa-zc/`, per-run `GST_PLUGIN_PATH` / `VK_ICD_FILENAMES`; NO system install.
- Board: Rock 5B+, kernel 7.1.0-rc1+, rkvdec; compositor sway, output HDMI-A-1; `SWAYSOCK=/run/user/1000/sway-ipc.1000.*.sock`, `WAYLAND_DISPLAY=wayland-1`, `XDG_RUNTIME_DIR=/run/user/1000`.
- Clip corpus already on board in `~/vvtest/`: `case1.h264`, `demo.h264` (720p), `c1080.h264` (1080p), plus `.mp4` re-muxes.
- Independent-agent review on every code edit (custom GStreamer element, ICD change, harness) before build/deploy.
- mem-search before each increment; systematic-debugging if a gate fails; vault `VulkanVideo/` log + gap-tracker updated after; humanizer on any public outreach.
- Zero-copy is only "confirmed" with a POSITIVE dmabuf-import check — a picture on screen is NOT sufficient (WebKit silently uploads to GBM if the frame isn't a dmabuf; bug 260654).

---

## Task A1: Increment A — progressive H264, default-rank, dmabuf import gate

**Files:**
- Create: `scripts/vvtest/s3a-webkit-default-rank.sh` (board harness; mirrors `s3-realh264-test.sh` but NO forced ranks, NO bridge, NO Vulkan ICD)
- Reference (do not modify): `~/vvtest/s3-realh264-test.sh`, `~/vvtest/s2-webkit-decode-test.sh` on board
- Evidence out: `/tmp/s3a_gst.log`, `/tmp/s3a_present.png` on board

**Interfaces:**
- Consumes: existing board WebKit launch pattern (how Stage-2 served a local `<video>` page + ran Epiphany/MiniBrowser under sway).
- Produces: a board script `s3a-webkit-default-rank.sh <clip.mp4>` that launches WebKit on a local H264 page with default plugin ranks and captures the GST decodebin log + a grim screenshot.

- [ ] **Step 1: Read the existing board harness to mirror its launch shape**

Run: `ssh rock5b 'cat ~/vvtest/s3-realh264-test.sh'`
Expected: see how it serves the page (python http.server / file://) and launches the browser under sway. Note the browser binary + env it uses. Do NOT rely on its `GST_PLUGIN_FEATURE_RANK` lines — A drops them.

- [ ] **Step 2: Write the default-rank A harness (host copy, then scp)**

Write `scripts/vvtest/s3a-webkit-default-rank.sh`. Key differences from Stage-2: assert mesa pin; clear `~/.cache/gstreamer-1.0/registry.aarch64.bin`; env has NO `GST_PLUGIN_FEATURE_RANK`, NO `vkh264bridge` on `GST_PLUGIN_PATH`, NO `VK_ICD_FILENAMES`; set `GST_DEBUG=GST_ELEMENT_FACTORY:4,decodebin:5,v4l2*:4` to `/tmp/s3a_gst.log` and `WEBKIT_DEBUG=Media` (or the 2.52 equivalent) to confirm the DMABuf renderer path. Serve `<video autoplay muted loop src=CLIP>` page. Run the browser ~6 s, `grim /tmp/s3a_present.png`, kill, assert mesa pin POST.

```bash
#!/usr/bin/env bash
# Increment A: WebKit + v4l2codecs, DEFAULT ranks (no bridge, no Vulkan).
set -uo pipefail
export SWAYSOCK=$(ls /run/user/1000/sway-ipc.1000.*.sock | head -1)
export WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000
PIN="1:26.0.6-1"; [ "$(pacman -Q mesa|awk '{print $2}')" = "$PIN" ] || { echo "ABORT mesa pin"; exit 1; }
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
CLIP="${1:-c1080.mp4}"; cd ~/vvtest
# minimal page
cat > /tmp/s3a.html <<EOF
<!doctype html><html><body style="margin:0;background:#000">
<video id=v autoplay muted loop playsinline src="file://$HOME/vvtest/$CLIP" style="width:100%"></video>
</body></html>
EOF
export GST_DEBUG="GST_ELEMENT_FACTORY:4,decodebin:5,v4l2videodec:4,v4l2codecs:4"
export GST_DEBUG_FILE=/tmp/s3a_gst.log
# IMPORTANT: no GST_PLUGIN_FEATURE_RANK, no vkh264bridge, no VK_ICD_FILENAMES
( epiphany "file:///tmp/s3a.html" >/tmp/s3a_eph.log 2>&1 & echo $! > /tmp/s3a.pid )
sleep 6
swaymsg -t get_tree | grep -iE '"app_id"|"visible"' | head
grim /tmp/s3a_present.png && echo "shot $(stat -c%s /tmp/s3a_present.png)B"
kill "$(cat /tmp/s3a.pid)" 2>/dev/null; pkill -f WebKitWebProcess 2>/dev/null
[ "$(pacman -Q mesa|awk '{print $2}')" = "$PIN" ] || { echo "WARN mesa CHANGED"; exit 99; }
echo "A-progressive done"
```

- [ ] **Step 3: Deploy + run**

Run: `scp scripts/vvtest/s3a-webkit-default-rank.sh rock5b:~/vvtest/ && ssh rock5b 'chmod +x ~/vvtest/s3a-webkit-default-rank.sh && fuser -v /dev/video0 2>&1; ~/vvtest/s3a-webkit-default-rank.sh c1080.mp4 & sleep 3; fuser /dev/video0 && echo VIDEO0_BUSY; wait'`
Expected: window `visible:true`; `VIDEO0_BUSY` printed during play.

- [ ] **Step 4: Gate 1 — HW decoder selected (not SW)**

Run: `ssh rock5b 'grep -iE "v4l2slh264dec|avdec_h264|chose|selected" /tmp/s3a_gst.log | head'`
Expected: `v4l2slh264dec` chosen by decodebin; NO `avdec_h264`. If `avdec_h264` won → natural rank insufficient; record and fall through to Step 7 fallback.

- [ ] **Step 5: Gate 2 — POSITIVE dmabuf import (the load-bearing check)**

Run: `ssh rock5b 'grep -iE "memory:DMABuf|GstVideoMeta|gldownload|videoconvert|glupload" /tmp/s3a_gst.log | head; echo "--- webkit ---"; grep -iE "DMABuf|GBM|EGLImage|upload" /tmp/s3a_eph.log | head'`
Expected: decoder src caps carry `memory:DMABuf`; WebKit log shows dmabuf/EGLImage import. NEGATIVE control: NO `gldownload`/`videoconvert` sysmem copy, and NO WebKit "upload GBM buffer" line (bug 260654 copy trap). Record exactly which path appears.

- [ ] **Step 6: Gate 3 — visual, no corruption**

Run: `scp rock5b:/tmp/s3a_present.png scratchpad/` then view it.
Expected: clean 1080p frame, no green/sheared corruption (rkvdec padded NV12 modifier accepted by WebKit GBM import).

- [ ] **Step 7: (only if Step 4 failed) fallback — forced rank, downgrade verdict**

Run: `ssh rock5b 'GST_PLUGIN_FEATURE_RANK="v4l2slh264dec:512" ~/vvtest/s3a-webkit-default-rank.sh c1080.mp4'` then re-check Gates 1–3.
Expected: with forced rank the HW path is taken. If this is needed, A's verdict becomes "free with a one-line env knob", not "free by default".

- [ ] **Step 8: Commit the harness**

```bash
git add scripts/vvtest/s3a-webkit-default-rank.sh
git commit -m "phaseC step-3 A: WebKit default-rank v4l2codecs harness"
```

---

## Task A2: Increment A — MSE path

**Files:**
- Create: `scripts/vvtest/s3a-webkit-mse.sh` (board; mirrors `~/vvtest/s3-mse-test.sh` but default ranks, no bridge)
- Evidence out: `/tmp/s3a_mse_gst.log`, `/tmp/s3a_mse.png`

**Interfaces:**
- Consumes: A1's confirmed default-rank launch env; the existing `~/vvtest/s3-mse-test.sh` MSE page shape (fragmented mp4, `MediaSource`).
- Produces: a board script proving (or refuting) that the dmabuf zero-copy path survives MSE (fragmented mp4 + a backward seek).

- [ ] **Step 1: Inspect the existing MSE harness**

Run: `ssh rock5b 'cat ~/vvtest/s3-mse-test.sh'`
Expected: understand how it builds the MSE page + fragmented H264. Reuse its page; strip forced ranks.

- [ ] **Step 2: Write `s3a-webkit-mse.sh`** — same env discipline as A1 (mesa pin assert, clear registry, no forced ranks/bridge/ICD, `GST_DEBUG_FILE=/tmp/s3a_mse_gst.log`), serve the MSE page, play + one backward seek, `grim /tmp/s3a_mse.png`.

(Reuse the A1 env preamble verbatim; only the served page differs — point it at the MSE page the existing `s3-mse-test.sh` generates.)

- [ ] **Step 3: Deploy + run**

Run: `scp scripts/vvtest/s3a-webkit-mse.sh rock5b:~/vvtest/ && ssh rock5b 'chmod +x ~/vvtest/s3a-webkit-mse.sh; ~/vvtest/s3a-webkit-mse.sh'`
Expected: playback + seek complete, window visible.

- [ ] **Step 4: Re-check Gates 1–3 on the MSE log** (same commands as A1 Steps 4–6, against `/tmp/s3a_mse_gst.log` + `/tmp/s3a_mse.png`)
Expected: HW decoder selected, `memory:DMABuf` import, clean frame after seek. Record if MSE re-negotiation breaks the dmabuf path even though progressive passed.

- [ ] **Step 5: Commit**

```bash
git add scripts/vvtest/s3a-webkit-mse.sh
git commit -m "phaseC step-3 A: WebKit MSE default-rank harness"
```

---

## Task A3: Increment A report + verdict

**Files:**
- Create: `artifacts/phase-c/STEP3-A-webkit-v4l2-dmabuf.md`

**Interfaces:**
- Consumes: gate evidence from A1 + A2 (`/tmp/s3a_*.log`, screenshots).
- Produces: the A verdict consumed by the user decision before the C spike — "is WebKitGTK in-browser zero-copy via v4l2codecs already free (yes / yes-with-env-knob / no), and if not, exactly what copy is inserted."

- [ ] **Step 1: Write the report** — record per-gate PASS/FAIL with the exact log lines, the screenshot reference, the natural-vs-forced-rank outcome, and the MSE result. State the verdict explicitly. Honest scope: if WebKit inserted a GBM upload, say so and name the line.

- [ ] **Step 2: Update vault** — append a `VulkanVideo/wiki/log.md` entry + gap-tracker (the empirical WebKit zero-copy answer).

- [ ] **Step 3: Independent review of the report's claims** — dispatch a reviewer agent to confirm the verdict matches the logged evidence (no over-claim of "zero-copy" without the positive dmabuf line). Fold.

- [ ] **Step 4: Commit**

```bash
git add artifacts/phase-c/STEP3-A-webkit-v4l2-dmabuf.md
git commit -m "phaseC step-3 A: WebKit v4l2 dmabuf verdict"
```

---

## Task C-SPIKE: gating experiment for the ICD zero-copy rewrite

**This task is a GATE. C implementation is NOT planned here — it gets its own plan only on a GO verdict.** The spike answers the three primary-confirmed obstacles. Work happens in the `mesa-sree-tree` Docker volume (ICD) + a throwaway GStreamer probe element; nothing is deployed to system paths.

**Files:**
- Create: `scripts/vvtest/s3c-spike-vkmem-probe.c` (a throwaway GStreamer element/pad-probe that, given a `vulkanh264dec` src buffer, reports whether the memory is `GstVulkanImageMemory` and whether `vkGetMemoryFdKHR` on its `VkDeviceMemory` succeeds)
- Create: `artifacts/phase-c/STEP3-C-spike.md` (GO / NO-GO / sub-approach decision)
- Touch (experiment only, in `mesa-sree-tree`): the ICD `host_alloc` path in `v4l2vk_vk_device_memory.c`

**Interfaces:**
- Consumes: the ICD `.so` build flow (`scripts/vvtest/icd-rebuild.sh`, volume `mesa-sree-tree`); the byte-exact gate (`~/vvtest/nv12_tool.py` / `pixelcheck.py`).
- Produces: a documented GO/NO-GO with the chosen sub-approach (1 V4L2-imports-Vulkan vs 2 Vulkan-imports-V4L2) and the DPB-output model, for the future C implementation plan.

- [ ] **Step 1: mem-search + locate the ICD memory + DPB code**

Run: `docker ps --format '{{.Names}}'` (confirm dev container), then `docker exec <ctr> sh -c 'sed -n "1,80p" /path/to/v4l2vk_vk_device_memory.c'` and the `cap_idx = setup_slot_index` site in `v4l2vk_vk_device.c`.
Expected: see `v4l2vk_AllocateMemory` type-1 `host_alloc` + the `host_alloc` write-combine comment; see the DPB slot≡cap_idx mapping. Ground the spike in primary source, not the spec summary.

- [ ] **Step 2: Spike Q3 (reach) — does `vulkanh264dec` emit `GstVulkanImageMemory`?**

Write `s3c-spike-vkmem-probe.c`: a pad probe on a `vulkanh264dec` src that prints, per buffer, `gst_is_vulkan_image_memory(mem)` and, if true, attempts `vkGetMemoryFdKHR` using the `GstVulkanDevice` from the buffer's memory's `GstVulkanImageMemory.device`. Build with the GStreamer-vulkan flags; run on a small clip.
Expected: a clear yes/no on (a) memory type is `GstVulkanImageMemory`, (b) fd export succeeds. Record the actual outcome — this decides whether the bridge element rewrite is viable.

- [ ] **Step 3: Spike Q2 (ICD back type-1 with CAPTURE dmabuf, throwaway edit)**

In the `mesa-sree-tree` volume, behind an env flag (e.g. `V4L2VK_ZEROCOPY_OUTPUT=1`), make `v4l2vk_AllocateMemory` for type-1 output set `mem->dma_buf_fd` to the dup'd V4L2 CAPTURE dmabuf and skip the posix_memalign+memcpy. Rebuild the ICD `.so` (isolated). Decode one clip via `vulkanh264dec ! vulkandownload` (CPU consumer) and frame-0 byte-check.
Expected: EITHER byte-exact (write-combine not a problem for this consumer) OR corrupted/stale (the author's documented CMA hazard reproduces). Either result is a valid spike finding; record it.

- [ ] **Step 4: Spike Q4 (DPB) — distinct output vs reuse**

With the Step-3 flag on, run a multi-frame clip with B-frames/references and frame-by-frame byte-check across the GOP (not just frame-0). If references corrupt → confirms the cap_idx≡slot reuse hazard; sketch the distinct-output-buffer cost (extra CAPTURE buffer or a blit) and whether it stays zero-copy.
Expected: a concrete statement on whether a distinct-output path is needed and whether it remains zero-copy or degenerates to a copy.

- [ ] **Step 5: Write `STEP3-C-spike.md` — GO / NO-GO + sub-approach**

Record all three answers with evidence. Verdict: GO (with sub-approach 1 or 2 + DPB model) or NO-GO (C not viable on this ICD without larger rework; fall back to A as the shipped in-browser result). Independent-agent review of the spike conclusions before the verdict is final.

- [ ] **Step 6: Revert the throwaway ICD edit / keep behind the off-by-default flag; commit spike artifacts**

```bash
git add scripts/vvtest/s3c-spike-vkmem-probe.c artifacts/phase-c/STEP3-C-spike.md
git commit -m "phaseC step-3 C-spike: vkmem reach + zerocopy-output + DPB findings"
```

- [ ] **Step 7: GATE — stop and re-plan**

Do NOT implement the full C rewrite here. On GO, author a new plan `docs/superpowers/plans/<date>-step3-C-icd-zerocopy.md` (brainstorming/writing-plans) using the spike's fixed sub-approach. On NO-GO, A is the shipped Step-3 result and C is recorded as blocked with the reason.

---

## Self-Review

**Spec coverage:** A increment (progressive A1 + MSE A2 + report A3) ✓; C spike with all three obstacles — CMA write-combine (C-spike Step 3), DPB/output-coincide (Step 4), GstVulkanImageMemory reach (Step 2) ✓; isolation invariants in Global Constraints ✓; independent review at A3 Step 3 + C-spike Step 5 ✓; positive-dmabuf gate (not just visual) in Global Constraints + A1 Step 5 ✓. C *implementation* intentionally deferred to a post-spike plan (spec sequencing) — not a gap.

**Placeholder scan:** A2 Step 2 says "reuse the A1 env preamble verbatim" with the page difference named — acceptable (the preamble is fully shown in A1 Step 2, same file being read out of order is covered by pointing at the exact prior block). No TBD/TODO. Fallback paths are concrete (forced-rank command given).

**Type consistency:** harness names `s3a-webkit-default-rank.sh` / `s3a-webkit-mse.sh` / `s3c-spike-vkmem-probe.c`, report `STEP3-A-webkit-v4l2-dmabuf.md` / `STEP3-C-spike.md`, env flag `V4L2VK_ZEROCOPY_OUTPUT` used consistently. ICD file `v4l2vk_vk_device_memory.c` / `v4l2vk_vk_device.c` match the review's primary sources.
