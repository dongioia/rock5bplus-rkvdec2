#!/usr/bin/env python3
"""Byte-diff V4L2 H264 control payloads between two `strace -f -e ioctl -s N` logs.

v4l2-tracer's LD_PRELOAD does not intercept GStreamer's v4l2 ioctls on rkvdec,
but strace does. This parses the first per-request (V4L2_CTRL_WHICH_REQUEST_VAL,
ctrl_class=0xf010000) VIDIOC_S_EXT_CTRLS in each log, octal-decodes each
control's payload, and diffs ours vs golden per control id.

Usage: strace_ctrl_diff.py <ours.log> <golden.log>
"""
import ast
import re
import sys

NAMES = {"0xa40900": "DECODE_MODE", "0xa40901": "START_CODE", "0xa40902": "SPS",
         "0xa40903": "PPS", "0xa40904": "SCALING", "0xa40905": "PRED_WEIGHTS",
         "0xa40906": "SLICE_PARAMS", "0xa40907": "DECODE_PARAMS"}


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
    sys.exit(main(sys.argv[1], sys.argv[2]))
