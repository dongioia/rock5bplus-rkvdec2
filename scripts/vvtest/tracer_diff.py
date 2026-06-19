#!/usr/bin/env python3
"""Diff control VALUES between two v4l2-tracer JSON traces (Phase B0, §6 Step 1).

Finds the first VIDIOC_S_EXT_CTRLS occurrence in each trace and recursively
diffs them. Generic: tolerant to where v4l2-tracer nests the ioctl name and
the controls. If the real trace.json shape differs, the only thing to adjust
is the ioctl-name string (default "VIDIOC_S_EXT_CTRLS").

Convention: a = ours, b = golden.
"""
import argparse
import json
import sys


def first_ioctl(obj, name):
    """Deep-walk; return the first dict node that holds `name` as a value."""
    found = [None]

    def walk(o):
        if found[0] is not None:
            return
        if isinstance(o, dict):
            if any(v == name for v in o.values()):
                found[0] = o
                return
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(obj)
    return found[0]


def jdiff(a, b, path=""):
    diffs = []
    if type(a) is not type(b):
        return [(path or ".", "type", type(a).__name__, type(b).__name__)]
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b), key=str):
            cp = f"{path}.{k}"
            if k not in a:
                diffs.append((cp, "only_golden", None, b[k]))
            elif k not in b:
                diffs.append((cp, "only_ours", a[k], None))
            else:
                diffs += jdiff(a[k], b[k], cp)
    elif isinstance(a, list):
        if len(a) != len(b):
            diffs.append((path or ".", "len", len(a), len(b)))
        for i in range(min(len(a), len(b))):
            diffs += jdiff(a[i], b[i], f"{path}[{i}]")
    else:
        if a != b:
            diffs.append((path or ".", "value", a, b))
    return diffs


def diff_first_controls(ours, golden, ioctl_name="VIDIOC_S_EXT_CTRLS"):
    na = first_ioctl(ours, ioctl_name)
    nb = first_ioctl(golden, ioctl_name)
    if na is None or nb is None:
        return [(".", "missing_ioctl", na is not None, nb is not None)]
    return jdiff(na, nb)


def main(argv=None):
    p = argparse.ArgumentParser(description="v4l2-tracer control diff (B0)")
    p.add_argument("--ours", required=True, help="our ICD trace.json")
    p.add_argument("--golden", required=True, help="v4l2slh264dec trace.json")
    p.add_argument("--ioctl", default="VIDIOC_S_EXT_CTRLS")
    a = p.parse_args(argv)
    with open(a.ours) as f:
        ours = json.load(f)
    with open(a.golden) as f:
        golden = json.load(f)
    diffs = diff_first_controls(ours, golden, a.ioctl)
    if not diffs:
        print("RESULT: MATCH (first S_EXT_CTRLS identical)")
        return 0
    print(f"RESULT: {len(diffs)} difference(s) [a=ours b=golden]")
    for path, kind, av, bv in diffs:
        print(f"  {path}: {kind}: ours={av!r} golden={bv!r}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
