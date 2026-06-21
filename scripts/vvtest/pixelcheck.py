"""Decide PASS/FAIL of a captured decoded region vs an ffmpeg reference.
Inputs are raw byte files of EQUAL length (the caller extracts identical
geometry for both). Blank (<=2 distinct byte values) is always FAIL."""
import math

def _read(p):
    with open(p, "rb") as f: return f.read()

def compare_region(cap_path, ref_path, x=0, y=0, w=0, h=0):
    a, b = _read(cap_path), _read(ref_path)
    if len(a) != len(b) or not a:
        return {"verdict": "FAIL", "metric": "geometry", "psnr": None,
                "reason": f"length mismatch {len(a)} vs {len(b)}"}
    if len(set(a)) <= 2:
        return {"verdict": "FAIL", "metric": "blank", "psnr": None,
                "reason": f"capture blank ({len(set(a))} distinct values)"}
    if a == b:
        return {"verdict": "PASS", "metric": "byte-exact", "psnr": None, "reason": "byte-exact"}
    mse = sum((a[i] - b[i]) ** 2 for i in range(len(a))) / len(a)
    psnr = float("inf") if mse == 0 else 10 * math.log10((255 ** 2) / mse)
    if psnr >= 50:
        return {"verdict": "PASS", "metric": "psnr", "psnr": psnr, "reason": f"PSNR {psnr:.1f}dB >= 50"}
    return {"verdict": "FAIL", "metric": "psnr", "psnr": psnr, "reason": f"PSNR {psnr:.1f}dB < 50"}
