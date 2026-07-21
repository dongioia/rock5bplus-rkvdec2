# Task 10 — drive HEVC to byte-exact (result)

**Date**: 2026-06-25
**Method**: B0 strace structural-diff vs golden `v4l2slh265dec`, with the
ground-truth byte-exact NV12 gate vs ffmpeg as the arbiter.
**Outcome**: **byte-exact achieved with no ICD field-fixes** — Task 9's translate
was already correct. T10 work = build the reusable POC-aligned control gate and
prove the watches benign.

## Ground truth: byte-exact NV12 vs ffmpeg (the real gate)

| Clip | Geometry | Frames | Profile | cmp vs ffmpeg |
|------|----------|--------|---------|---------------|
| `hevc_case1` | 1280×720 | 180 (I/P/B, bframes=2) | Main 8-bit | **FULL_CLIP_BYTE_EXACT** |
| `hevc_640`   | 640×360  | 120 (I/P/B, bframes=2) | Main 8-bit | **FULL_CLIP_BYTE_EXACT** |

The proof is an **independent** byte compare of the decoded NV12 against ffmpeg's
software decode — it does **not** depend on `strace_ctrl_diff.py` (the gate under
review). Exact commands run on the SBC:

```
ffmpeg -i hevc_case1.mp4 -vf format=nv12 -f rawvideo ref_hevc_full.nv12   # ground truth
gst-launch-1.0 filesrc location=hevc_case1.h265 ! h265parse ! vulkanh265dec \
  ! vulkandownload ! filesink location=/tmp/hevc_ours.nv12                 # our ICD
cmp ref_hevc_full.nv12 /tmp/hevc_ours.nv12   ->  (silent) = every byte identical
```

`cmp` over the whole decoded stream (display order): every byte identical, all
P and B frames included (248832000 B = 180×1382400 for case1; 41472000 B = 120×
345600 for 640×360). 640 is not a 256-multiple width, so this also clears the
Stage-3 readback-stride class (which was masked at 1280) for the HEVC path — the
stride fix in `v4l2vk_vk_device.c` is codec-agnostic and generalizes.

## Task-10 watches — all resolved

| # | Watch (from Tasks 7/8/9 ledger) | Resolution |
|---|--------------------------------|------------|
| 1 | ICD sends SLICE_PARAMS vs golden sends SCALING+no-SLICE_PARAMS | Ours sends `{SPS,PPS,SCALING,DECODE_PARAMS}` — **no SLICE_PARAMS** (runtime gate drops it). Matches golden's frame-based set. |
| 2 | P/B parse + collocated-gate on real P/B | All P/B frames byte-exact → parser + the Task-7 collocated fix correct. |
| 3 | `data_byte_offset` base sc-incl(30) vs NAL-rel(26) | Byte-exact → correct (a wrong base corrupts every slice). |
| 4 | DPB LONG_TERM flag (H264-proxy) | **Not exercised** — short-GOP corpus has no long-term refs. See residual below. |
| 5 | `bit_size` span | Byte-exact → correct. |

## POC-aligned strace-diff: 3 divergences, all benign

The golden decodes the IDR from init/CUR controls and emits **no** per-request
`S_EXT_CTRLS` for it, so frames must be aligned by `pic_order_cnt_val`
(`strace_ctrl_diff.py --poc`) — a naive first-frame compare pits ours-IDR against
golden-P. After alignment, on every shared POC:

- `HEVC_PPS`: **IDENTICAL**.
- `HEVC_SPS`: presence-only diff — ours re-sends SPS each request, golden sets it
  once non-request. rkvdec re-reads the same SPS; no effect (proven byte-exact).
- `HEVC_SCALING`: differs only at bytes 992–999 (`scaling_list_dc_coef_*`): ours
  `0x08`, golden `0x00`. The corpus has `scaling_list_enabled_flag=0`, so rkvdec
  ignores the matrix; the 0–991 list bytes are already identical.
- `HEVC_DECODE_PARAMS`: differs in `poc_st_curr_before/after[]` — these are **DPB
  indices**, not POC values. Ours orders the DPB differently than golden, so the
  valid indices differ (e.g. `[1,2]` vs `[0,1]`) but resolve to the **same physical
  reference frames**; unused entries differ in fill convention (ours `0x00`, golden
  `0xff`). Only `num_active_dpb_entries` / `num_poc_*` valid entries are read. The
  independent ffmpeg `cmp` above (byte-exact across all 180/120 frames incl.
  multi-ref B-frames) is the proof these resolve correctly — not the strace gate.

**Decision**: the byte-exact NV12 gate is ground truth and passes. The three
divergences are benign and explained, not unverified. Per systematic-debugging
discipline, byte-exact-passing code is not risk-edited to match a proxy. The
control SET (which controls, in what order) matches golden; the residual byte
diffs are in fields rkvdec either ignores (scaling) or resolves equivalently
(DPB index order), or in a redundant-but-harmless extra (per-request SPS).

## Residual for Task 12 (byte-exact gate corpus)

- **Long-term references** (watch #4) are not exercised by the short-GOP corpus.
  T12's corpus must include a longer-GOP / ≥2-ST-RPS clip (and ideally LT refs) to
  exercise `poc_lt_curr[]` and the LONG_TERM DPB flag. If a divergence appears, the
  POC-aligned `hevc-ctrl-diff.sh` localizes the control.
- T12 adds 1920×1080 and the I-only / P-only variants per the plan DoD.

## Reusable gate

`scripts/vvtest/hevc-ctrl-diff.sh <clip>` (runs on the SBC): captures golden +
ours with `strace -s 2048` (must exceed SCALING=1000 B or the diff is bogus),
then `strace_ctrl_diff.py --poc` for the frame-aligned per-control diff.
`_t10_dump.py` dumps per-call ctrl-class / ids / POC for structure inspection.
