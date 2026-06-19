#!/usr/bin/env python3
"""NV12 stride-normalization + byte-exact/PSNR comparison for Phase B0.

normalize_nv12: collapse a strided, coded-aligned NV12 capture buffer into a
packed NV12 of out_w x out_h (Y plane out_w*out_h, then interleaved UV plane
out_w*(out_h/2)). Read row strides from real V4L2 metadata; do NOT assume
stride_uv == stride_y. Chroma row = out_w bytes (out_w/2 CbCr pairs), NOT out_w/2.

Use out=(coded Wc,Hc) for the diagnostic coded-normalized artifact, and
out=(visible Wv,Hv) for the GATE artifact (ffmpeg already applies the crop).
"""
import argparse
import math
import sys


def normalize_nv12(raw, stride_y, stride_uv, buf_h, out_w, out_h, uv_offset=None):
    if uv_offset is None:
        uv_offset = stride_y * buf_h  # UV follows the coded-aligned Y plane
    out = bytearray()
    for y in range(out_h):  # Y plane
        start = y * stride_y
        row = raw[start:start + out_w]
        if len(row) != out_w:
            raise ValueError(f"Y row {y}: have {len(row)} need {out_w}")
        out += row
    for y in range(out_h // 2):  # UV interleaved: out_w bytes per row
        start = uv_offset + y * stride_uv
        row = raw[start:start + out_w]
        if len(row) != out_w:
            raise ValueError(f"UV row {y}: have {len(row)} need {out_w}")
        out += row
    return bytes(out)


def psnr(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return -math.inf
    se = 0
    for i in range(n):
        d = a[i] - b[i]
        se += d * d
    mse = se / n
    return math.inf if mse == 0 else 10.0 * math.log10((255.0 * 255.0) / mse)


def distinct_values(data):
    return len(set(data))


def compare(a, b):
    res = {"len_a": len(a), "len_b": len(b), "byte_exact": a == b}
    if not res["byte_exact"]:
        n = min(len(a), len(b))
        res["first_diff"] = next((i for i in range(n) if a[i] != b[i]), n)
        res["psnr_db"] = psnr(a, b)
        res["distinct_a"] = distinct_values(a)
        res["distinct_b"] = distinct_values(b)
    return res


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def main(argv=None):
    p = argparse.ArgumentParser(description="NV12 normalize/compare (Phase B0)")
    sub = p.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("normalize")
    n.add_argument("--in", dest="inp", required=True)
    n.add_argument("--out", required=True)
    for k in ("stride-y", "stride-uv", "buf-h", "out-w", "out-h"):
        n.add_argument(f"--{k}", type=int, required=True)
    n.add_argument("--uv-offset", type=int, default=None)
    c = sub.add_parser("compare")
    c.add_argument("--a", required=True)
    c.add_argument("--b", required=True)
    a = p.parse_args(argv)

    if a.cmd == "normalize":
        out = normalize_nv12(_read(a.inp), a.stride_y, a.stride_uv, a.buf_h,
                             a.out_w, a.out_h, a.uv_offset)
        with open(a.out, "wb") as f:
            f.write(out)
        print(f"normalized -> {a.out} ({len(out)} bytes)")
    elif a.cmd == "compare":
        res = compare(_read(a.a), _read(a.b))
        for k, v in res.items():
            print(f"{k}: {v}")
        if res["byte_exact"]:
            print("RESULT: PASS (byte-exact)")
            return 0
        if res.get("distinct_b", 99) > 2 and res.get("distinct_a", 99) <= 2:
            print("RESULT: FAIL (ours is blank)")
            return 2
        if res["psnr_db"] >= 50.0:
            print("RESULT: PASS-with-fallback (PSNR>=50 — residual MUST be explained)")
            return 0
        print("RESULT: FAIL (not byte-exact, PSNR<50)")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
