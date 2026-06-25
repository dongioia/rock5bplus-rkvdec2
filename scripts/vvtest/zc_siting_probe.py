#!/usr/bin/env python3
"""Identify PanVK's actual NV12 chroma reconstruction empirically.
Given the real HW RGBA dump + the source NV12, compute candidate CPU references
(BT.709 limited) under each chroma-siting hypothesis and PSNR each vs HW. The
candidate that matches (~50-90 dB) is what the HW actually does; the rest are
the wrong-siting signatures (~26-33 dB).

Usage: zc_siting_probe.py <ref_nv12> <hw_rgba.bin> <W> <H>
"""
import sys
import numpy as np

ref_nv12, hw_path, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
nv12 = np.fromfile(ref_nv12, dtype=np.uint8)
hw = np.fromfile(hw_path, dtype=np.uint8).astype(np.int64)
cw, ch = W // 2, H // 2
Y = nv12[:W * H].reshape(H, W).astype(np.float64)
UV = nv12[W * H:W * H + W * ch].reshape(ch, W)
Cb = UV[:, 0::2].astype(np.float64)   # (ch, cw)
Cr = UV[:, 1::2].astype(np.float64)


def axis_idx(n, half, shift):
    i = np.arange(n) // 2
    if shift:
        i = i + (np.arange(n) & 1)   # COSITED_EVEN reconstruction: odd -> next
    return np.minimum(i, half - 1)


def candidate(xs, ys):
    xi, yi = axis_idx(W, cw, xs), axis_idx(H, ch, ys)
    cb = Cb[np.ix_(yi, xi)]
    cr = Cr[np.ix_(yi, xi)]
    yv = (Y - 16.0) / 219.0
    cbn = (cb - 128.0) / 224.0
    crn = (cr - 128.0) / 224.0
    r = yv + 1.5748 * crn
    g = yv - 0.1873 * cbn - 0.4681 * crn
    b = yv + 1.8556 * cbn
    c8 = lambda t: np.clip(np.round(t * 255.0), 0, 255)
    out = np.stack([c8(r), c8(g), c8(b), np.full((H, W), 255.0)], axis=-1)
    return out.astype(np.int64).reshape(-1)


def psnr(a, b):
    n = min(len(a), len(b))
    d = a[:n] - b[:n]
    mse = float((d * d).mean())
    return (999.0, 0) if mse == 0 else (10.0 * np.log10(255.0 * 255.0 / mse), int(np.abs(d).max()))


print(f"{W}x{H}  hw bytes={hw.size}")
for xs in (0, 1):
    for ys in (0, 1):
        p, m = psnr(candidate(xs, ys), hw)
        lbl = {(0, 0): "midpoint/plain", (1, 0): "cosited-X only", (0, 1): "cosited-Y only", (1, 1): "cosited-both"}[(xs, ys)]
        print(f"  xshift={xs} yshift={ys} ({lbl:15s}): PSNR={p:6.2f} dB  max|d|={m}")
