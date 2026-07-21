#!/usr/bin/env python3
"""Byte-diff V4L2 H264/H265 control payloads between two `strace -f -e ioctl -s N` logs.

v4l2-tracer's LD_PRELOAD does not intercept GStreamer's v4l2 ioctls on rkvdec,
but strace does. This parses the first per-request (V4L2_CTRL_WHICH_REQUEST_VAL,
ctrl_class=0xf010000) VIDIOC_S_EXT_CTRLS in each log, octal-decodes each
control's payload, and diffs ours vs golden per control id.

Codec-agnostic: the control-id NAMES map below covers both the H.264 stateless
CIDs (base 0xa40900, +0..7) and the HEVC stateless CIDs (base 0xa40900, +400..409
= 0xa40a90..0xa40a99). Capture with a LARGE `-s` (HEVC SCALING_MATRIX is 1000
bytes, DECODE_PARAMS 328) — `-s 2048` so strace does not truncate the payload,
else large controls compare as length-mismatched. See hevc-ctrl-diff.sh.

Usage: strace_ctrl_diff.py <ours.log> <golden.log>
"""
import ast
import re
import sys

NAMES = {
    # H.264 stateless CIDs (V4L2_CID_CODEC_STATELESS_BASE + 0..7)
    "0xa40900": "H264_DECODE_MODE", "0xa40901": "H264_START_CODE",
    "0xa40902": "H264_SPS", "0xa40903": "H264_PPS", "0xa40904": "H264_SCALING",
    "0xa40905": "H264_PRED_WEIGHTS", "0xa40906": "H264_SLICE_PARAMS",
    "0xa40907": "H264_DECODE_PARAMS",
    # HEVC stateless CIDs (V4L2_CID_CODEC_STATELESS_BASE + 400..409)
    "0xa40a90": "HEVC_SPS", "0xa40a91": "HEVC_PPS", "0xa40a92": "HEVC_SLICE_PARAMS",
    "0xa40a93": "HEVC_SCALING", "0xa40a94": "HEVC_DECODE_PARAMS",
    "0xa40a95": "HEVC_DECODE_MODE", "0xa40a96": "HEVC_START_CODE",
    "0xa40a97": "HEVC_ENTRY_POINT_OFFSETS", "0xa40a98": "HEVC_EXT_SPS_ST_RPS",
    "0xa40a99": "HEVC_EXT_SPS_LT_RPS",
}


def first_request_ctrls(path):
    txt = open(path, errors="replace").read()
    for m in re.finditer(r"VIDIOC_S_EXT_CTRLS, (\{.*?\}) =>", txt):
        block = m.group(1)
        if "0xf010000" not in block:  # request-context only
            continue
        ctrls = {}
        for cm in re.finditer(r'\{id=(0x[0-9a-f]+)[^}]*?string="((?:[^"\\]|\\.)*)"', block):
            cid, s = cm.group(1), cm.group(2)
            try:
                ctrls[cid] = ast.literal_eval('b"' + s + '"')
            except Exception:
                ctrls[cid] = None  # truncated / unparseable
        if ctrls:
            return ctrls
    return {}


def all_request_ctrls_by_poc(path):
    """{key: {cid: bytes}} for every per-request frame. key is
    ("poc", pic_order_cnt_val, occurrence) keyed off decode_params (HEVC 0xa40a94 /
    H264 0xa40907, bytes 0-3, s32 LE); ("seq", n) when decode_params is absent or
    undecodable. `occurrence` disambiguates a POC that repeats across multiple
    IDRs/CVSs (POC resets to 0 at each IRAP) so no two frames ever collide on one
    key — a plain `poc->ctrls` dict would silently drop the earlier frame and
    false-green the gate."""
    txt = open(path, errors="replace").read()
    frames, seen, seq = {}, {}, 0
    for m in re.finditer(r"VIDIOC_S_EXT_CTRLS, (\{.*?\}) =>", txt):
        block = m.group(1)
        if "0xf010000" not in block:
            continue
        ctrls = {}
        for cm in re.finditer(r'\{id=(0x[0-9a-f]+)[^}]*?string="((?:[^"\\]|\\.)*)"', block):
            cid, s = cm.group(1), cm.group(2)
            try:
                ctrls[cid] = ast.literal_eval('b"' + s + '"')
            except Exception:
                ctrls[cid] = None
        if not ctrls:
            continue
        dp = ctrls.get("0xa40a94") or ctrls.get("0xa40907")
        if dp and len(dp) >= 4:
            poc = int.from_bytes(dp[0:4], "little", signed=True)
            occ = seen.get(poc, 0)
            seen[poc] = occ + 1
            key = ("poc", poc, occ)
        else:
            key = ("seq", seq)
        frames[key] = ctrls
        seq += 1
    return frames


def diff_ctrls(o, g, header):
    """Print per-control diff for one aligned frame; return 0 ok / 1 presence / 2 byte-diff."""
    print(header)
    worst = 0
    for cid in sorted(set(o) | set(g)):
        nm = NAMES.get(cid, cid)
        if cid not in o or cid not in g:
            print(f"  {nm}: PRESENCE diff — ours={'y' if cid in o else 'N'} golden={'y' if cid in g else 'N'}")
            worst = max(worst, 1)
            continue
        ob, gb = o[cid], g[cid]
        if ob is None or gb is None:
            print(f"  {nm}: (truncated payload — raise strace -s)")
            worst = max(worst, 1)
            continue
        if ob == gb:
            print(f"  {nm}: IDENTICAL ({len(ob)} bytes)")
            continue
        n = min(len(ob), len(gb))
        fd = next((i for i in range(n) if ob[i] != gb[i]), n)
        lo = max(0, fd - 2)
        print(f"  {nm}: DIFFER len ours={len(ob)} golden={len(gb)} first_diff@{fd}")
        print(f"     ours  [{lo}:{fd+10}] = {ob[lo:fd+10].hex(' ')}")
        print(f"     golden[{lo}:{fd+10}] = {gb[lo:fd+10].hex(' ')}")
        worst = max(worst, 2)
    return worst


def main_poc(ours_path, golden_path):
    """Frame-aligned diff: match request frames by POC. Golden decodes the IDR
    from init/CUR controls (no per-request S_EXT_CTRLS), so its POC set lacks the
    IDR — those show as ours-only and are expected, not a regression."""
    o = all_request_ctrls_by_poc(ours_path)
    g = all_request_ctrls_by_poc(golden_path)
    pkeys = lambda d: {k for k in d if k[0] == "poc"}
    shared = sorted(pkeys(o) & pkeys(g))
    ours_only = sorted(pkeys(o) - pkeys(g))
    golden_only = sorted(pkeys(g) - pkeys(o))
    print(f"POC-aligned: ours frames={len(o)} golden frames={len(g)} shared POCs={len(shared)}")
    if ours_only:
        print(f"POCs only in ours (golden decodes via init/CUR — expected for IDR): "
              f"{[k[1] for k in ours_only]}")
    if golden_only:
        # golden frames absent from ours = ours decoded fewer frames -> partial decode
        print(f"WARNING: POCs only in golden (ours did NOT emit them — possible partial "
              f"decode / crash): {[k[1] for k in golden_only]}")
    worst = 1 if golden_only else 0
    for key in shared:
        lbl = f"--- POC {key[1]}" + (f" #{key[2]}" if key[2] else "") + " ---"
        worst = max(worst, diff_ctrls(o[key], g[key], lbl))
    print("ALL ALIGNED CONTROLS MATCH GOLDEN" if worst == 0
          else "ALIGNED DIFFS PRESENT (see above; presence-only on SPS is benign — see TASK10 notes)")
    return worst


def main(ours_path, golden_path):
    o = first_request_ctrls(ours_path)
    g = first_request_ctrls(golden_path)
    print("ours controls  :", [NAMES.get(k, k) for k in o])
    print("golden controls:", [NAMES.get(k, k) for k in g])
    for cid in sorted(set(o) | set(g)):
        nm = NAMES.get(cid, cid)
        if cid not in o or cid not in g:
            print(f"{nm}: PRESENCE diff — ours={'y' if cid in o else 'N'} golden={'y' if cid in g else 'N'}")
            continue
        ob, gb = o[cid], g[cid]
        if ob is None or gb is None:
            print(f"{nm}: (truncated payload, cannot decode)")
            continue
        if ob == gb:
            print(f"{nm}: IDENTICAL ({len(ob)} bytes)")
            continue
        n = min(len(ob), len(gb))
        fd = next((i for i in range(n) if ob[i] != gb[i]), n)
        lo = max(0, fd - 2)
        print(f"{nm}: DIFFER ours_len={len(ob)} golden_len={len(gb)} first_diff@{fd}")
        print(f"   ours  [{lo}:{fd+10}] = {ob[lo:fd+10].hex(' ')}")
        print(f"   golden[{lo}:{fd+10}] = {gb[lo:fd+10].hex(' ')}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[3] == "--poc":
        sys.exit(main_poc(sys.argv[1], sys.argv[2]))
    sys.exit(main(sys.argv[1], sys.argv[2]))
