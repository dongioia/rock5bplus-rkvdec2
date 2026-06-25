#!/usr/bin/env python3
"""T10 diagnostic: dump the S_EXT_CTRLS structure of a strace log.
For each call: request vs cur_val context, control ids, and decode_params POC."""
import ast
import re
import sys


def dump(path, label):
    txt = open(path, errors="replace").read()
    print(f"=== {label} ({path}) ===")
    n = 0
    for m in re.finditer(r"VIDIOC_S_EXT_CTRLS, (\{.*?\})\s*(?:=>|\)\s*=)", txt):
        block = m.group(1)
        if "0xf010000" in block:
            cc = "REQ"
        elif re.search(r"ctrl_class=0[,}]", block) or "WHICH_CUR_VAL" in block:
            cc = "CUR"
        else:
            cc = "?"
        ids = re.findall(r"id=(0xa40a9[0-9])", block)
        poc = ""
        dm = re.search(r'id=0xa40a94[^}]*?string="((?:[^"\\]|\\.)*)"', block)
        if dm:
            try:
                b = ast.literal_eval('b"' + dm.group(1) + '"')
                if len(b) >= 9:
                    pv = int.from_bytes(b[0:4], "little", signed=True)
                    poc = f" POC={pv} ndpb={b[8]}"
            except Exception:
                poc = " POC=?"
        n += 1
        if n <= 10:
            print(f"  [{n:2}] {cc} ids={ids}{poc}")
    print(f"  total S_EXT_CTRLS matched: {n}")


for p, lbl in ((sys.argv[1], "OURS"), (sys.argv[2], "GOLDEN")):
    dump(p, lbl)
