# Phase B0 — RK3588 Vulkan Video H264 Decode Correctness — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the BLANK H264 decode in the standalone Vulkan-Video V4L2-backed ICD on RK3588 (rkvdec/VDPU381, kernel 7.1rc1), validate byte-exact vs ffmpeg via GStreamer, and document the root cause + fix.

**Architecture:** Build the deterministic, SBC-independent foundation first (reproducible rebuild/deploy harness, a byte-exact NV12 compare tool, a v4l2-tracer control-diff tool, in-ICD readback instrumentation) — all of this compiles/tests on the Mac with the SBC down. Then run the spec's systematic-debugging decision tree on the SBC as a precise runbook once it is reachable. The ICD source lives only in Docker volume `mesa-sree-tree`; we never edit it on the host.

**Tech Stack:** Mesa `sree/mesa` ICD (C, meson/ninja, commit `5955e6e`), Docker `rock5b-dev-serena` image + `mesa-sree-tree` volume (aarch64 native), Python 3 stdlib (compare/diff tooling + `unittest`), GStreamer 1.28 (`vulkanh264dec`/`vulkandownload`/`v4l2slh264dec`), `v4l2-tracer`/`v4l2-ctl`/`media-ctl`/`ffmpeg`, SSH to SBC `rock5b`.

**Companion spec:** `docs/superpowers/specs/2026-06-18-vulkanvideo-rk3588-phaseB0-h264-correctness-design.md` (rev.4). This plan implements it. Section references like "§6 Step 1" point at the spec.

---

## Conventions (read once)

- **Repo root**: `/Volumes/Tonio/Rock5bPlus`. All host paths below are relative to it.
- **ICD source** (read-only investigation, edits via container only): Docker volume `mesa-sree-tree`, tree at `/mesa/mesa/src/vulkan-v4l2/`. Build dir `/mesa/mesa/build` (meson already configured; `ninja -C build` re-links incrementally — verified).
- **Build image**: `rock5b-dev-serena`. Throwaway containers: `docker run --rm -v mesa-sree-tree:/mesa <image> …`.
- **Deploy artifact** (host): `deploy/vulkan-v4l2-icd/` — `libvulkan_v4l2_video.so`, `v4l2vk_icd.aarch64.json`, `compat-mesa26.patch`, `BUILD-INFO.txt`.
- **SBC**: host alias `rock5b` (192.168.50.157), test dir `~/vvtest/`. **Isolated** deploy only: `VK_ICD_FILENAMES` → our JSON, **never** touch system `/usr/share/vulkan/icd.d`, **never** `sudo`, **mesa pin 26.0.6 stays**.
- **Contribution gate CLOSED**: nothing in B0 goes outbound. A kernel bug or upstreamable fix is a separate gated decision (humanizer + AI-disclosure + reviewer).
- **Commits**: prefix with `VERIFIED=1` (commit guard). Private repo → `Co-Authored-By` trailer OK.
- **Grounded leads** (from the prior session's decode log + source read — to look for, not to assume): COPY2 (`v4l2vk_vk_device.c:947`) is a **flat `memcpy(mmap_size)`**; COPY3 readback (`v4l2vk_vk_device.c:282`) hardcodes `stride = ALIGN(width, 256)`; the decode log showed `SLICE_PARAMS`/`PRED_WEIGHTS` `QUERY_EXT_CTRL failed (EINVAL)` (sizeof 152/772) and `decode activity count = 0` → H-CONTROL / R6 ABI-drift is the leading hypothesis. The decision tree confirms or refutes.

---

## PART A — Foundation (SBC-independent; build + test NOW)

### Task 1: Reproducible ICD rebuild + isolated deploy scripts

**Files:**
- Create: `scripts/vvtest/icd-rebuild.sh`
- Create: `scripts/vvtest/icd-deploy.sh`

- [ ] **Step 1: Write the rebuild script**

`scripts/vvtest/icd-rebuild.sh`:
```bash
#!/usr/bin/env bash
# Rebuild the V4L2 Vulkan ICD incrementally in a throwaway container and
# repackage the .so into deploy/. SBC-independent.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="$REPO/deploy/vulkan-v4l2-icd"
IMG="${ICD_BUILD_IMAGE:-rock5b-dev-serena}"
VOL="${ICD_MESA_VOLUME:-mesa-sree-tree}"

docker run --rm \
  -v "$VOL:/mesa" \
  -v "$DEPLOY:/deploy" \
  "$IMG" sh -lc '
    set -e
    cd /mesa/mesa
    ninja -C build src/vulkan-v4l2/libvulkan_v4l2_video.so.1
    cp -v build/src/vulkan-v4l2/libvulkan_v4l2_video.so.1 /deploy/libvulkan_v4l2_video.so
  '
echo "[icd-rebuild] repackaged -> $DEPLOY/libvulkan_v4l2_video.so"
```

- [ ] **Step 2: Make executable and shellcheck**

Run: `chmod +x scripts/vvtest/icd-rebuild.sh && shellcheck scripts/vvtest/icd-rebuild.sh`
Expected: exit 0 (no warnings). If `shellcheck` absent, skip — not gating.

- [ ] **Step 3: Run a real incremental rebuild (proves the harness)**

Run: `scripts/vvtest/icd-rebuild.sh`
Expected: ninja prints `Linking target src/vulkan-v4l2/libvulkan_v4l2_video.so.1` (or "no work to do"), then `repackaged -> …`. `deploy/vulkan-v4l2-icd/libvulkan_v4l2_video.so` mtime updates.

- [ ] **Step 4: Write the isolated deploy script**

`scripts/vvtest/icd-deploy.sh`:
```bash
#!/usr/bin/env bash
# scp the ICD + a SBC-local manifest to the SBC, isolated. No system icd.d, no sudo.
# Requires the SBC reachable (Precondition 0).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="$REPO/deploy/vulkan-v4l2-icd"
SBC="${SBC_HOST:-rock5b}"
DST="${SBC_DIR:-/home/sav/vvtest}"

ssh "$SBC" "mkdir -p '$DST'"
scp "$DEPLOY/libvulkan_v4l2_video.so" "$SBC:$DST/"
# Generate a SBC-local manifest whose library_path is the absolute deployed path.
cat > /tmp/v4l2vk_icd.sbc.json <<JSON
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$DST/libvulkan_v4l2_video.so",
        "api_version": "1.3.0"
    }
}
JSON
scp /tmp/v4l2vk_icd.sbc.json "$SBC:$DST/v4l2vk_icd.aarch64.json"
echo "[icd-deploy] deployed to $SBC:$DST"
echo "[icd-deploy] smoke: ssh $SBC \"VK_ICD_FILENAMES=$DST/v4l2vk_icd.aarch64.json vulkaninfo --summary | grep -A2 deviceName\""
```

- [ ] **Step 5: Shellcheck + dry validate (deploy runs later, SBC down now)**

Run: `chmod +x scripts/vvtest/icd-deploy.sh && bash -n scripts/vvtest/icd-deploy.sh && shellcheck scripts/vvtest/icd-deploy.sh`
Expected: exit 0. (Real run is gated on Precondition 0 — Task 6.)

- [ ] **Step 6: Commit**

```bash
git add scripts/vvtest/icd-rebuild.sh scripts/vvtest/icd-deploy.sh
VERIFIED=1 git commit -m "vvtest: reproducible ICD rebuild + isolated SBC deploy scripts"
```

---

### Task 2: NV12 normalize + byte-exact/PSNR compare tool (TDD)

Implements §6.B (stride-normalization: coded vs visible, UV row = W bytes, strides from metadata) and §1.3 (byte-exact primary, PSNR≥50 fallback, blank=fail).

**Files:**
- Create: `scripts/vvtest/nv12_tool.py`
- Test: `scripts/vvtest/test_nv12_tool.py`

- [ ] **Step 1: Write the failing tests**

`scripts/vvtest/test_nv12_tool.py`:
```python
import math
import unittest

import nv12_tool as t


class TestNormalize(unittest.TestCase):
    def test_strips_y_and_uv_padding(self):
        # stride_y=8, visible 4x2, UV plane starts at stride_y*buf_h=16.
        # Y rows: bytes 0..3 then pad to 8; UV rows: bytes 0..3 then pad.
        raw = bytearray(64)
        for y in range(2):
            for x in range(8):
                raw[y * 8 + x] = (y * 10 + x) if x < 4 else 0xAA  # pad = 0xAA
        uv_off = 16
        for y in range(1):  # out_h//2 = 1 UV row
            for x in range(8):
                raw[uv_off + y * 8 + x] = (100 + x) if x < 4 else 0xBB
        out = t.normalize_nv12(bytes(raw), stride_y=8, stride_uv=8,
                               buf_h=2, out_w=4, out_h=2)
        # Y: 2 rows * 4 bytes, then UV: 1 row * 4 bytes  -> 12 bytes, no 0xAA/0xBB
        self.assertEqual(out, bytes([0, 1, 2, 3, 10, 11, 12, 13, 100, 101, 102, 103]))

    def test_uv_row_is_w_bytes_not_half(self):
        # NV12 chroma row = out_w bytes (out_w/2 interleaved CbCr pairs), NOT out_w/2.
        raw = bytes(4096)
        out = t.normalize_nv12(raw, stride_y=64, stride_uv=64,
                               buf_h=16, out_w=16, out_h=8)
        self.assertEqual(len(out), 16 * 8 + 16 * (8 // 2))  # Y + UV(W bytes * H/2)

    def test_raises_when_raw_too_small(self):
        with self.assertRaises(ValueError):
            t.normalize_nv12(b"\x00" * 4, stride_y=8, stride_uv=8,
                             buf_h=2, out_w=4, out_h=2)


class TestCompare(unittest.TestCase):
    def test_byte_exact(self):
        a = bytes(range(256))
        r = t.compare(a, bytes(range(256)))
        self.assertTrue(r["byte_exact"])

    def test_psnr_inf_when_equal(self):
        self.assertEqual(t.psnr(bytes([5, 5, 5]), bytes([5, 5, 5])), math.inf)

    def test_psnr_finite_and_first_diff(self):
        r = t.compare(bytes([10, 10, 10, 10]), bytes([10, 12, 10, 10]))
        self.assertFalse(r["byte_exact"])
        self.assertEqual(r["first_diff"], 1)
        self.assertTrue(0 < r["psnr_db"] < math.inf)

    def test_blank_detection(self):
        self.assertEqual(t.distinct_values(bytes([7] * 1000)), 1)
        self.assertGreater(t.distinct_values(bytes(range(256)) * 4), 200)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd scripts/vvtest && python3 -m unittest test_nv12_tool -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'nv12_tool'`.

- [ ] **Step 3: Implement the tool**

`scripts/vvtest/nv12_tool.py`:
```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd scripts/vvtest && python3 -m unittest test_nv12_tool -v`
Expected: PASS (7 tests OK).

- [ ] **Step 5: Commit**

```bash
git add scripts/vvtest/nv12_tool.py scripts/vvtest/test_nv12_tool.py
VERIFIED=1 git commit -m "vvtest: NV12 visible-normalize + byte-exact/PSNR compare tool (TDD)"
```

---

### Task 3: v4l2-tracer control-value diff tool (TDD)

Implements §6 Step 1 (compare control VALUES of `ours` vs golden on frame 1, criterion §1.1). Generic recursive JSON diff of the first `VIDIOC_S_EXT_CTRLS` group in each trace — robust to v4l2-tracer schema nesting.

**Files:**
- Create: `scripts/vvtest/tracer_diff.py`
- Test: `scripts/vvtest/test_tracer_diff.py`

- [ ] **Step 1: Write the failing tests**

`scripts/vvtest/test_tracer_diff.py`:
```python
import unittest

import tracer_diff as td

OURS = {"trace": [
    {"ioctl": "VIDIOC_S_EXT_CTRLS", "controls": [
        {"id": "SPS", "level_idc": 31, "num_ref_frames": 1},
        {"id": "DECODE_PARAMS", "flags": 0},
    ]},
    {"ioctl": "VIDIOC_S_EXT_CTRLS", "controls": [{"id": "SPS", "level_idc": 31}]},
]}
GOLDEN = {"trace": [
    {"ioctl": "VIDIOC_S_EXT_CTRLS", "controls": [
        {"id": "SPS", "level_idc": 30, "num_ref_frames": 1},
        {"id": "DECODE_PARAMS", "flags": 0},
        {"id": "SCALING_MATRIX", "present": True},
    ]},
]}


class TestTracerDiff(unittest.TestCase):
    def test_finds_first_s_ext_ctrls(self):
        node = td.first_ioctl(OURS, "VIDIOC_S_EXT_CTRLS")
        self.assertIsNotNone(node)
        self.assertEqual(node["controls"][0]["level_idc"], 31)

    def test_value_diff_detected(self):
        diffs = td.diff_first_controls(OURS, GOLDEN)
        paths = {d[0] for d in diffs}
        kinds = {d[1] for d in diffs}
        self.assertTrue(any("level_idc" in p for p in paths))  # 31 vs 30
        self.assertIn("value", kinds)

    def test_missing_control_detected(self):
        diffs = td.diff_first_controls(OURS, GOLDEN)
        # golden has SCALING_MATRIX (controls[2]); ours' first group has only 2
        self.assertTrue(any(d[1] in ("len", "only_golden") for d in diffs))

    def test_identical_traces_no_diff(self):
        self.assertEqual(td.diff_first_controls(OURS, OURS), [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd scripts/vvtest && python3 -m unittest test_tracer_diff -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'tracer_diff'`.

- [ ] **Step 3: Implement the tool**

`scripts/vvtest/tracer_diff.py`:
```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd scripts/vvtest && python3 -m unittest test_tracer_diff -v`
Expected: PASS (4 tests OK).

- [ ] **Step 5: Commit**

```bash
git add scripts/vvtest/tracer_diff.py scripts/vvtest/test_tracer_diff.py
VERIFIED=1 git commit -m "vvtest: v4l2-tracer control-value diff tool (TDD)"
```

---

### Task 4: Artifacts scaffold + gitignore + SBC precondition/metadata capture

Implements §4 (Precond 0 SBC gate + Precond 1 driver identity) and §8 (reproducible `artifacts/phase-b0/`).

**Files:**
- Create: `scripts/vvtest/sbc-precond.sh`
- Create: `artifacts/phase-b0/README.md`
- Modify: `.gitignore`

- [ ] **Step 1: Write the precondition + metadata capture script**

`scripts/vvtest/sbc-precond.sh`:
```bash
#!/usr/bin/env bash
# Precondition 0 (SBC availability gate) + Precondition 1 (driver identity).
# Saves metadata under artifacts/phase-b0/metadata/. Exits non-zero if any
# gating tool/device is missing. Run when the SBC is up (Task 6).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SBC="${SBC_HOST:-rock5b}"
OUT="$REPO/artifacts/phase-b0/metadata"
mkdir -p "$OUT"
fail=0

echo "== Precondition 0: SBC availability =="
ssh "$SBC" 'true' || { echo "FAIL: SBC unreachable"; exit 1; }
ssh "$SBC" 'uname -a'              | tee "$OUT/uname.txt"
ssh "$SBC" 'gst-launch-1.0 --version' | tee "$OUT/gst-version.txt"
ssh "$SBC" 'ls -l /dev/video0 /dev/media* 2>&1' | tee "$OUT/devnodes.txt" \
  | grep -q '/dev/video0' || { echo "FAIL: /dev/video0 missing"; fail=1; }
for tool in v4l2-tracer v4l2-ctl ffmpeg media-ctl; do
  ssh "$SBC" "command -v $tool >/dev/null" \
    || { echo "FAIL: $tool missing (install v4l-utils)"; fail=1; }
done
ssh "$SBC" 'gst-inspect-1.0 v4l2slh264dec >/dev/null 2>&1' \
  && echo "v4l2slh264dec OK" || { echo "FAIL: v4l2slh264dec missing"; fail=1; }

echo "== Precondition 1: driver identity =="
ssh "$SBC" 'v4l2-ctl -d /dev/video0 --info'            | tee "$OUT/v4l2-info.txt"
ssh "$SBC" 'media-ctl -p'                              | tee "$OUT/media-topology.txt"
ssh "$SBC" 'cat /sys/class/video4linux/video0/name'    | tee "$OUT/video0-name.txt"
ssh "$SBC" 'tr -d "\0" < /sys/class/video4linux/video0/device/of_node/compatible 2>/dev/null || echo "(of_node compatible not exposed)"' | tee "$OUT/dt-compatible.txt"
ssh "$SBC" 'v4l2-ctl -d /dev/video0 --list-formats-ext' | tee "$OUT/formats-ext.txt"

[ "$fail" -eq 0 ] && echo "PRECONDITIONS: PASS" || { echo "PRECONDITIONS: FAIL"; exit 1; }
```

- [ ] **Step 2: Make executable + syntax check + shellcheck**

Run: `chmod +x scripts/vvtest/sbc-precond.sh && bash -n scripts/vvtest/sbc-precond.sh && shellcheck scripts/vvtest/sbc-precond.sh`
Expected: exit 0. (Real run gated on SBC — Task 6.)

- [ ] **Step 3: Create the artifacts scaffold**

`artifacts/phase-b0/README.md`:
```markdown
# Phase B0 artifacts (reproducible)

- `input/`     stream(s) + SHA256 (gitignored — binaries)
- `traces/`    v4l2-tracer ours + golden (gitignored)
- `dumps/`     readback (a)/(b)/(c) (gitignored)
- `metadata/`  driver identity, G_FMT per-plane, strides (committed — text)
- `compare/`   normalized + diff/PSNR summaries (committed — text)

Per dump record: SHA256 + frame-index + PTS + buffer-index + pixelformat +
w/h + visible-crop + strides(y,uv) + num_planes + bytesused.
Findings live in the vault: OBSIDIAN_Kernel/VulkanVideo/wiki/analyses/phase-b0-h264-correctness-findings.md
```

- [ ] **Step 4: Gitignore the heavy subdirs**

Append to `.gitignore`:
```
# Phase B0 binary artifacts (keep metadata/ + compare/ text only)
artifacts/phase-b0/input/
artifacts/phase-b0/traces/
artifacts/phase-b0/dumps/
```

- [ ] **Step 5: Commit**

```bash
git add scripts/vvtest/sbc-precond.sh artifacts/phase-b0/README.md .gitignore
VERIFIED=1 git commit -m "vvtest: SBC precondition+driver-identity capture; phase-b0 artifacts scaffold"
```

---

## PART B — In-ICD readback instrumentation (build NOW; observe on SBC)

### Task 5: Add raw readback dump hooks (a)/(b) + per-plane log + DMA sync

Implements §6 Step 4 hooks (a)=raw V4L2 CAPTURE pre-COPY2, (b)=VkImage post-COPY2; §6.A multi-planar/format logging; R11 cache/DMA sync. Debug-only, env-gated, captured as a removable patch.

**Files (in the volume, via container):**
- Modify: `/mesa/mesa/src/vulkan-v4l2/v4l2vk_vk_device.c` (anchored inserts)
- Create (host): `scripts/vvtest/icd-instrument.py` (idempotent anchored editor)
- Create (host): `deploy/vulkan-v4l2-icd/debug-instrumentation.patch` (captured diff)

- [ ] **Step 1: Write the idempotent instrumentation editor**

`scripts/vvtest/icd-instrument.py`:
```python
#!/usr/bin/env python3
"""Insert B0 debug readback hooks into v4l2vk_vk_device.c (idempotent).

Run INSIDE the build container against the volume copy:
  python3 /vvtest/icd-instrument.py /mesa/mesa/src/vulkan-v4l2/v4l2vk_vk_device.c
Anchored on unique existing substrings so it survives line drift.
"""
import sys

INCLUDES = '''#include <linux/dma-buf.h>
#include <sys/ioctl.h>
'''

HELPER = r'''
/* --- B0 debug: raw plane dump (env-gated, removable) --- */
static void
v4l2vk_b0_dump_raw(const char *env, const char *tag, unsigned frame_idx,
                   const void *addr, size_t len, int dmabuf_fd)
{
   if (!getenv(env) || !addr || !len)
      return;
#ifdef DMA_BUF_IOCTL_SYNC
   if (dmabuf_fd >= 0) { /* R11: CPU cache coherency before read */
      struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
      ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s);
   }
#endif
   const char *dir = getenv("V4L2VK_DUMP_DIR");
   char path[256];
   snprintf(path, sizeof(path), "%s/v4l2vk_%s_%04u.bin", dir ? dir : "/tmp",
            tag, frame_idx);
   FILE *fp = fopen(path, "wb");
   if (fp) { fwrite(addr, 1, len, fp); fclose(fp); }
#ifdef DMA_BUF_IOCTL_SYNC
   if (dmabuf_fd >= 0) {
      struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
      ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s);
   }
#endif
   fprintf(stderr, "[V4L2VK][B0] %s: %zu bytes -> %s (dmabuf_fd=%d)\n",
           tag, len, path, dmabuf_fd);
}

'''

# (a) raw CAPTURE buffer right after DQBUF, pre-COPY2 + per-plane/format log
DQ_ANCHOR = "v4l2vk_v4l2_dequeue_capture(v4l2_ctx, &dq_cap);"
DUMP_A = '''
            /* B0 (a): raw V4L2 CAPTURE buffer + format log (pre-COPY2) */
            if (dq_cap < v4l2_ctx->capture_buf_count) {
               struct v4l2vk_v4l2_buffer *b0cb =
                  &v4l2_ctx->capture_bufs[dq_cap];
               v4l2vk_b0_dump_raw("V4L2VK_DUMP_CAPTURE", "capture",
                                  dev->frame_counter, b0cb->mmap_addr,
                                  b0cb->mmap_size, b0cb->dma_buf_fd);
               fprintf(stderr,
                       "[V4L2VK][B0] cap idx=%u stride=%u sizeimage=%u "
                       "mmap_size=%zu w=%u h=%u dmabuf_mode=%d dma_buf_fd=%d\\n",
                       dq_cap, v4l2_ctx->capture_stride,
                       v4l2_ctx->capture_sizeimage, b0cb->mmap_size,
                       v4l2_ctx->width, v4l2_ctx->height,
                       v4l2_ctx->capture_dmabuf_mode, b0cb->dma_buf_fd);
            }
'''

# (b) VkImage host memory right after the COPY2 memcpy
COPY2_ANCHOR = ("v4l2_ctx->capture_bufs[dq_cap].mmap_addr,\n"
                "                      copy_size);")
DUMP_B = '''
               /* B0 (b): VkImage host memory (post-COPY2) */
               v4l2vk_b0_dump_raw("V4L2VK_DUMP_VKIMAGE", "vkimage",
                                  dev->frame_counter,
                                  (uint8_t *)dst_mem->map + dst_img->bound_offset,
                                  copy_size, -1);
'''

INC_ANCHOR = '#include "vk_sync.h"'
HELPER_ANCHOR = "static void\nv4l2vk_dump_nv12_image("


def main(path):
    src = open(path).read()
    if "V4L2VK_DUMP_CAPTURE" in src:
        print("already instrumented; nothing to do")
        return 0
    assert INC_ANCHOR in src, "include anchor not found"
    assert HELPER_ANCHOR in src, "helper anchor not found"
    assert DQ_ANCHOR in src, "dequeue_capture anchor not found"
    assert COPY2_ANCHOR in src, "COPY2 memcpy anchor not found"
    src = src.replace(INC_ANCHOR, INC_ANCHOR + "\n" + INCLUDES, 1)
    src = src.replace(HELPER_ANCHOR, HELPER + HELPER_ANCHOR, 1)
    src = src.replace(DQ_ANCHOR, DQ_ANCHOR + DUMP_A, 1)
    src = src.replace(COPY2_ANCHOR, COPY2_ANCHOR + DUMP_B, 1)
    open(path, "w").write(src)
    print("instrumented OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
```

- [ ] **Step 2: Apply instrumentation into the volume + capture the patch**

Run:
```bash
docker run --rm \
  -v mesa-sree-tree:/mesa \
  -v "$(pwd)/scripts/vvtest:/vvtest:ro" \
  -v "$(pwd)/deploy/vulkan-v4l2-icd:/deploy" \
  rock5b-dev-serena sh -lc '
    set -e
    cd /mesa/mesa
    python3 /vvtest/icd-instrument.py src/vulkan-v4l2/v4l2vk_vk_device.c
    git diff -- src/vulkan-v4l2/v4l2vk_vk_device.c > /deploy/debug-instrumentation.patch
    wc -l /deploy/debug-instrumentation.patch
  '
```
Expected: `instrumented OK`, and `deploy/vulkan-v4l2-icd/debug-instrumentation.patch` non-empty (~50 lines). Re-running prints `already instrumented`.

- [ ] **Step 3: Rebuild and confirm the hooks linked**

Run:
```bash
scripts/vvtest/icd-rebuild.sh
docker run --rm -v mesa-sree-tree:/mesa:ro rock5b-dev-serena \
  sh -lc 'strings /mesa/mesa/build/src/vulkan-v4l2/libvulkan_v4l2_video.so.1 | grep -c "\[V4L2VK\]\[B0\]"'
```
Expected: rebuild succeeds (compile + link clean); `grep -c` ≥ 1 (the `[V4L2VK][B0]` strings are present → hooks compiled in).

- [ ] **Step 4: Commit the host-side patch + editor (volume source is not in git)**

```bash
git add scripts/vvtest/icd-instrument.py deploy/vulkan-v4l2-icd/debug-instrumentation.patch
VERIFIED=1 git commit -m "vvtest: env-gated readback dump hooks (a)/(b) + DMA sync; captured as removable patch"
```

> **Revert note:** to remove instrumentation later — `docker run --rm -v mesa-sree-tree:/mesa rock5b-dev-serena sh -lc 'cd /mesa/mesa && git checkout -- src/vulkan-v4l2/v4l2vk_vk_device.c'` then rebuild.

---

## PART C — On-SBC diagnosis (GATED on Precondition 0)

> These tasks require the SBC up. They are a runbook over the spec §6 decision tree: each step is a command + the observation that decides the branch. Do NOT fabricate a fix before the diagnosis points to one.

### Task 6: Preconditions + reference stream + corpus case-1

**Files:** writes to `artifacts/phase-b0/{metadata,input}/`

- [ ] **Step 1: Run the precondition gate**

Run: `scripts/vvtest/sbc-precond.sh`
Expected: `PRECONDITIONS: PASS`. If FAIL → stop, install the missing tool (`v4l-utils` etc.), re-run. Record `metadata/video0-name.txt` + `dt-compatible.txt` (the real driver identity — "rkvdec" varies by BSP).

- [ ] **Step 2: Generate the minimal corpus case-1 stream (IDR+P, single-slice, no B)**

Run (on SBC, or on Mac then scp):
```bash
ssh rock5b 'cd ~/vvtest && ffmpeg -y -f lavfi -i testsrc2=size=1280x720:rate=30 \
  -frames:v 30 -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
  -g 15 -bf 0 -x264-params slices=1 -f h264 case1.h264 && sha256sum case1.h264'
```
Expected: `case1.h264` created; SHA256 recorded. Baseline = no B-frames, CAVLC, single ref, single slice → minimal decode path. (1280×720 is MB-aligned → coded == visible, no crop; crop is exercised in case-3 later.)

- [ ] **Step 3: Generate the ffmpeg reference (visible NV12)**

Run:
```bash
ssh rock5b 'cd ~/vvtest && ffmpeg -y -i case1.h264 -f rawvideo -pix_fmt nv12 ref.nv12 && ls -l ref.nv12'
```
Expected: `ref.nv12` size = 1280*720*3/2 * 30 frames? No — single frame for compare: extract frame 0 with `-frames:v 1` for the gate, or split. Run instead:
```bash
ssh rock5b 'cd ~/vvtest && ffmpeg -y -i case1.h264 -frames:v 1 -f rawvideo -pix_fmt nv12 ref_f0.nv12 && wc -c ref_f0.nv12'
```
Expected: `ref_f0.nv12` = 1280*720*3/2 = 1382400 bytes (packed visible NV12, stride=1280, no padding).

- [ ] **Step 4: Pull metadata + SHA into the repo**

Run: `scp rock5b:~/vvtest/case1.h264 artifacts/phase-b0/input/ && shasum -a256 artifacts/phase-b0/input/case1.h264 | tee artifacts/phase-b0/metadata/case1.sha256`
Expected: file pulled + SHA recorded (R10 reproducibility).

- [ ] **Step 5: Commit metadata (binaries are gitignored)**

```bash
git add artifacts/phase-b0/metadata/
VERIFIED=1 git commit -m "phase-b0: capture driver identity + case-1 stream metadata"
```

---

### Task 7: Step 0 fast-boundary-probe + Step 1 tracer control-diff

- [ ] **Step 1: Deploy the instrumented ICD (isolated)**

Run: `scripts/vvtest/icd-deploy.sh && ssh rock5b 'VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json vulkaninfo --summary | grep -i "V4L2 Vulkan"'`
Expected: deploy OK; vulkaninfo enumerates "V4L2 Vulkan Video Decoder" (ICD still loads after instrumentation).

- [ ] **Step 2: Step 0 — fast boundary probe: dump raw CAPTURE (a)**

Run:
```bash
ssh rock5b 'cd ~/vvtest && mkdir -p dumps && \
  VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json \
  V4L2VK_DUMP_CAPTURE=1 V4L2VK_DUMP_DIR=$PWD/dumps V4L2VK_LOG_V4L2=1 \
  gst-launch-1.0 filesrc location=case1.h264 ! h264parse config-interval=1 ! \
    video/x-h264,alignment=au,stream-format=byte-stream ! identity eos-after=1 ! \
    vulkanh264dec ! fakesink sync=false 2>&1 | grep -E "\[B0\]|ERR"'
```
Expected: `[V4L2VK][B0] capture: <N> bytes -> .../v4l2vk_capture_0000.bin` plus the `cap idx=… stride=… sizeimage=… w=1280 h=720 dmabuf_mode=…` line. **Record stride/sizeimage/dmabuf_mode** — these feed §6.A and the normalize step.

- [ ] **Step 3: Decide the branch (Step 0 logic)**

Pull + inspect the raw CAPTURE dump:
```bash
scp rock5b:~/vvtest/dumps/v4l2vk_capture_0000.bin artifacts/phase-b0/dumps/
# blank-check the Y plane (coded-normalized) using the stride/sizeimage just logged:
python3 scripts/vvtest/nv12_tool.py normalize --in artifacts/phase-b0/dumps/v4l2vk_capture_0000.bin \
  --stride-y <STRIDE> --stride-uv <STRIDE> --buf-h <ALIGN(720,16)=720> --out-w 1280 --out-h 720 \
  --out artifacts/phase-b0/compare/capture_coded.nv12
python3 scripts/vvtest/nv12_tool.py compare --a artifacts/phase-b0/compare/capture_coded.nv12 --b artifacts/phase-b0/dumps/v4l2vk_capture_0000.bin 2>/dev/null || true
```
Decision:
- **CAPTURE (a) is a plausible frame** (distinct values ≫ 2) → decode-side is OK → skip ahead to COPY2/`vulkandownload` (Task 9 Step 4 b/c) + GStreamer nego (Task 9 Step 5).
- **CAPTURE (a) is blank** (distinct ≤ 2) → bug is decode-side → continue to Step 1 tracer below. (Expected, given the leading H-CONTROL lead.)

- [ ] **Step 4: Step 1 — capture both v4l2-tracer traces (1 AU)**

Run:
```bash
ssh rock5b 'cd ~/vvtest && \
  VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json v4l2-tracer -u trace gst-launch-1.0 \
    filesrc location=case1.h264 ! h264parse config-interval=1 ! \
    video/x-h264,alignment=au,stream-format=byte-stream ! identity eos-after=1 ! \
    vulkanh264dec ! fakesink sync=false && mv trace.json ours.trace.json ; \
  v4l2-tracer -u trace gst-launch-1.0 \
    filesrc location=case1.h264 ! h264parse config-interval=1 ! \
    video/x-h264,alignment=au,stream-format=byte-stream ! identity eos-after=1 ! \
    v4l2slh264dec ! fakesink sync=false && mv trace.json golden.trace.json'
scp rock5b:~/vvtest/ours.trace.json rock5b:~/vvtest/golden.trace.json artifacts/phase-b0/traces/
```
Expected: two trace JSONs. **First confirm the schema**: `python3 -c "import json;d=json.load(open('artifacts/phase-b0/traces/ours.trace.json'));print(type(d));print(str(d)[:400])"` — verify `VIDIOC_S_EXT_CTRLS` appears; if the ioctl key differs, pass `--ioctl <name>` to the next step. (If `identity eos-after=1` produced >1 AU, the diff still uses the first group.)

- [ ] **Step 5: Diff the control VALUES**

Run: `python3 scripts/vvtest/tracer_diff.py --ours artifacts/phase-b0/traces/ours.trace.json --golden artifacts/phase-b0/traces/golden.trace.json | tee artifacts/phase-b0/compare/tracer-diff.txt`
Expected output guides the branch:
- **MATCH** → controls are fine on frame 1 → go to Task 8 (DPB/sequencing).
- **differences** → H-CONTROL/H-PARSER. Look specifically for: a control golden sets that ours omits (e.g. golden has `SLICE_PARAMS` but ours skipped it because `QUERY_EXT_CTRL` failed — the known lead), `level_idc` raw vs enum, `SCALING_MATRIX` presence, `DECODE_PARAMS.flags`. → go to Task 8 Step 3 (parser isolation) if value-level, or record as the H-CONTROL finding.

- [ ] **Step 6: Commit the comparison artifacts**

```bash
git add artifacts/phase-b0/compare/tracer-diff.txt artifacts/phase-b0/metadata/
VERIFIED=1 git commit -m "phase-b0: Step 0 boundary probe + Step 1 tracer control-diff results"
```

---

### Task 8: Step 2 DPB + per-request checks; Step 3 parser isolation (conditional)

- [ ] **Step 1: Step 2 — DPB binding & sequencing (criterion §1.2)**

From `ours.trace.json` + the `V4L2VK_LOG_V4L2=1`/`V4L2VK_LOG_REFS=1` decode log, verify:
```bash
ssh rock5b 'cd ~/vvtest && VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json \
  V4L2VK_LOG_V4L2=1 V4L2VK_LOG_REFS=1 \
  gst-launch-1.0 filesrc location=case1.h264 ! h264parse config-interval=1 ! \
    video/x-h264,alignment=au,stream-format=byte-stream ! vulkanh264dec ! fakesink sync=false \
    2>&1 | grep -E "ts=|dpb_refs|reference_ts|S_EXT_CTRLS|request_fd"' | tee artifacts/phase-b0/compare/dpb-sequencing.txt
```
Check (record pass/fail each): (a) each `dpb[].reference_ts` resolves to a CAPTURE buffer actually DQBUF'd with that timestamp; (b) active refs ≤ `num_ref_frames`; (c) OUTPUT↔CAPTURE timestamp units consistent (no ms/ns/index mismatch — R9). For case-1 (baseline, single ref) the DPB is trivial — a mismatch here on case-1 is a strong signal.

- [ ] **Step 2: Step 2 — per-request V4L2 stateless checks (P1.8)**

From the same log + trace, confirm the request lifecycle: `S_EXT_CTRLS(request_fd=N)` → `QBUF OUTPUT(request_fd=N)` → `MEDIA_REQUEST_IOC_QUEUE` for the SAME N; one request per AU; no request/buffer reused before DQBUF/completion. Record findings in `dpb-sequencing.txt`. (The source flow is `alloc_request → set_h264_controls → queue_output → submit_request → wait_request → dequeue_*`.)

- [ ] **Step 3: Step 3 — parser isolation (ONLY if Step 1/2 didn't localize)**

Compare the raw controls the ICD emits (`V4L2VK_LOG_H264=1` + `V4L2VK_DUMP_BITSTREAM=1`) against an INDEPENDENT parser (not `v4l2slh264dec`, which shares `h264parse`):
```bash
ssh rock5b 'cd ~/vvtest && ffmpeg -v debug -i case1.h264 -frames:v 1 -f null - 2>ffmpeg-parse.txt; \
  VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json V4L2VK_LOG_H264=1 \
  gst-launch-1.0 filesrc location=case1.h264 ! h264parse config-interval=1 ! \
    video/x-h264,alignment=au,stream-format=byte-stream ! identity eos-after=1 ! \
    vulkanh264dec ! fakesink sync=false 2>icd-h264.txt'
```
Compare first-class fields (§6 H-PARSER): `pic_order_cnt_bit_size`, `dec_ref_pic_marking_bit_size`, MMCO presence, `frame_num`, `pic_order_cnt_lsb`, `idr_pic_id`, IDR/reference/field flags. Match → it's the mapping/controls, not the parser; mismatch → parser-stopgap bug.

- [ ] **Step 4: Commit findings**

```bash
git add artifacts/phase-b0/compare/dpb-sequencing.txt
VERIFIED=1 git commit -m "phase-b0: Step 2 DPB+per-request, Step 3 parser-isolation results"
```

---

### Task 9: Step 4 readback bisection (a)/(b)/(c); Step 5 GStreamer nego (conditional)

- [ ] **Step 1: Step 4 — dump (b) VkImage + (c) output, alongside (a)**

Run:
```bash
ssh rock5b 'cd ~/vvtest && \
  VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json \
  V4L2VK_DUMP_CAPTURE=1 V4L2VK_DUMP_VKIMAGE=1 V4L2VK_DUMP_DIR=$PWD/dumps \
  gst-launch-1.0 filesrc location=case1.h264 ! h264parse config-interval=1 ! \
    video/x-h264,alignment=au,stream-format=byte-stream ! identity eos-after=1 ! \
    vulkanh264dec ! vulkandownload ! video/x-raw,format=NV12 ! \
    filesink location=$PWD/dumps/output_f0.nv12 sync=false'
scp rock5b:~/vvtest/dumps/v4l2vk_capture_0000.bin \
    rock5b:~/vvtest/dumps/v4l2vk_vkimage_0000.bin \
    rock5b:~/vvtest/dumps/output_f0.nv12 artifacts/phase-b0/dumps/
```
Expected: three dumps. (a)=raw CAPTURE, (b)=VkImage post-COPY2, (c)=`vulkandownload` output.

- [ ] **Step 2: Localize with the bisection rule**

Run (compare (a)↔(b) flat — COPY2 is a flat memcpy, so they MUST match):
```bash
python3 scripts/vvtest/nv12_tool.py compare \
  --a artifacts/phase-b0/dumps/v4l2vk_capture_0000.bin \
  --b artifacts/phase-b0/dumps/v4l2vk_vkimage_0000.bin
```
Decision (per §6 Step 4):
- **(a) blank** → decode-side (back to Task 7/8). 
- **(a) plausible, (b) != (a)** → COPY2 bug (offset/size) — but COPY2 is flat memcpy(`copy_size`), so check `copy_size` clamp (`dst_mem->size`) and `bound_offset`.
- **(a)==(b) plausible, (c) blank** → H-FORMAT in COPY3/`vulkandownload`: COPY3 hardcodes `stride=ALIGN(width,256)` (device.c:282) — compare against the **measured** `capture_stride` from Task 7 Step 2. Mismatch = the bug (§6.A verdict). Confirm by normalizing (c) and comparing to ref (Step 3).

- [ ] **Step 3: Visible-normalized gate compare of the output (c) vs ffmpeg**

Run (using the measured strides; for case-1 coded==visible 1280×720):
```bash
python3 scripts/vvtest/nv12_tool.py normalize --in artifacts/phase-b0/dumps/output_f0.nv12 \
  --stride-y 1280 --stride-uv 1280 --buf-h 720 --out-w 1280 --out-h 720 \
  --out artifacts/phase-b0/compare/output_visible.nv12
python3 scripts/vvtest/nv12_tool.py compare \
  --a artifacts/phase-b0/compare/output_visible.nv12 \
  --b artifacts/phase-b0/dumps/ref_f0.nv12 | tee artifacts/phase-b0/compare/gate.txt
```
(`vulkandownload` output should already be packed visible NV12 — if so, normalize is identity and the compare is direct.) Expected eventually: `RESULT: PASS (byte-exact)`. Now it will FAIL (blank) — that failure IS the pre-fix baseline.

- [ ] **Step 4: Step 5 — GStreamer negotiation (only if (a)==(b) plausible but (c) blank and COPY3 stride looks right)**

Run: `ssh rock5b 'cd ~/vvtest && VK_ICD_FILENAMES=~/vvtest/v4l2vk_icd.aarch64.json GST_DEBUG=vulkanh264dec:9,vulkandownload:9,vulkandeviceprovider:5 gst-launch-1.0 filesrc location=case1.h264 ! h264parse config-interval=1 ! video/x-h264,alignment=au,stream-format=byte-stream ! vulkanh264dec ! vulkandownload ! fakesink sync=false 2>gst-nego.txt'; scp rock5b:~/vvtest/gst-nego.txt artifacts/phase-b0/compare/`
Inspect chosen profile/format/DPB-mode/memory vs what the ICD exposes. ICD mis-answers a query → blank upstream of the copy → input for Stage-2 (R7). Direct Vulkan program = out-of-scope B0.

- [ ] **Step 5: Commit bisection artifacts**

```bash
git add artifacts/phase-b0/compare/
VERIFIED=1 git commit -m "phase-b0: Step 4 readback bisection + Step 5 nego results (pre-fix baseline)"
```

---

### Task 10: Diagnose → fix → verify → findings

- [ ] **Step 1: State the root cause from the evidence**

Write the diagnosis: which hypothesis (H-FORMAT/H-CONTROL/H-PARSER/H-DPB/H-GST-NEGO) and which confine, citing the artifacts (tracer-diff, dpb-sequencing, bisection). The fix differs by branch:
  - **H-CONTROL (R6 ABI drift / missing SLICE_PARAMS)**: align the ICD's `v4l2_ctrl_h264_*` struct usage / control set to what kernel-7.1rc1 rkvdec accepts (the golden `v4l2slh264dec` trace is the oracle for which controls + sizes the kernel wants). Frame-based rkvdec may not need per-slice `SLICE_PARAMS` — match golden.
  - **H-FORMAT (COPY3 stride)**: if measured `capture_stride` ≠ `ALIGN(width,256)`, fix the readback stride in `v4l2vk_CmdCopyImageToBuffer2` (device.c:282) to use the real stride; or fix COPY2 to translate layout. Record in a fix patch.
  - **H-PARSER**: correct the offending field in `v4l2vk_v4l2_h264.c`.

- [ ] **Step 2: Apply the fix in the volume + capture it as a patch**

Edit the relevant source in the volume (via a container, like Task 5 Step 2 but for the fix), then capture:
```bash
docker run --rm -v mesa-sree-tree:/mesa -v "$(pwd)/deploy/vulkan-v4l2-icd:/deploy" rock5b-dev-serena \
  sh -lc 'cd /mesa/mesa && git diff -- src/vulkan-v4l2/ > /deploy/b0-fix.patch && cat /deploy/b0-fix.patch'
```
(The fix patch is SEPARATE from `debug-instrumentation.patch` and from `compat-mesa26.patch`. Keep instrumentation; it can be reverted before the final fix-only `.so` is packaged.)

- [ ] **Step 3: Rebuild + redeploy + re-run the gate**

Run: `scripts/vvtest/icd-rebuild.sh && scripts/vvtest/icd-deploy.sh` then re-run Task 9 Step 3 (visible-normalized gate compare).
Expected: `RESULT: PASS (byte-exact)` (or PASS-with-fallback PSNR≥50 with the residual explained). Blank = not fixed; iterate.

- [ ] **Step 4: Verify all §1 exit criteria**

Confirm and record evidence for: (1) tracer control-match (re-run Task 7 Step 5 → MATCH); (2) DPB binding & per-request OK (Task 8); (3) output byte-exact visible-normalized (Task 9 Step 3 → PASS); (4) root cause + fix patch recorded. This is the gate — no "fixed" claim without these.

- [ ] **Step 5: Write the vault findings + format/modifier inventory**

Create `OBSIDIAN_Kernel/VulkanVideo/wiki/analyses/phase-b0-h264-correctness-findings.md` (English, YAML frontmatter, confidence H/M/L, §0-ter provenance self-check): root cause, fix, tracer/DPB/byte-exact results, VK-CTS note (stretch — likely deferred to Phase B), Stage-2 inputs, and the rkvdec format/modifier inventory (`pixelformat` hex fourcc, w/h, per-plane bytesperline+sizeimage, num_planes, DRM modifier or "not exposed", tiled vs linear). Update `gap-tracker.md` (Gate 5 CONFIRMED-on-our-build; Gate 2 resolved), `log.md`, `index.md`.

- [ ] **Step 6: Commit fix patch + finish the branch**

```bash
git add deploy/vulkan-v4l2-icd/b0-fix.patch deploy/vulkan-v4l2-icd/libvulkan_v4l2_video.so artifacts/phase-b0/compare/
VERIFIED=1 git commit -m "phase-b0: H264 decode fix (<root-cause>); byte-exact vs ffmpeg on case-1"
```
Then invoke `superpowers:finishing-a-development-branch`. Vault changes are committed+pushed in the `OBSIDIAN_Kernel` repo separately (its own governance). Stage-2 (architecture gate) is a separate gated spec — do not start it here.

---

## Self-Review (spec coverage)

- **§1.1 tracer control-match** → Task 3 (tool) + Task 7 Step 5 (run) + Task 10 Step 4 (verify). ✓
- **§1.2 DPB binding & per-request** → Task 8 Steps 1-2. ✓
- **§1.3 byte-exact primary / PSNR≥50 fallback / blank=fail** → Task 2 (tool, encoded in `compare` exit codes) + Task 9 Step 3. ✓
- **§1.4 root cause documented** → Task 10 Steps 1, 5. ✓
- **Stretch VK-CTS** → noted as deferred in Task 10 Step 5 (not gating). ✓
- **§3 build/deploy isolated, mesa-pin intact** → Task 1 (scripts enforce isolated VK_ICD_FILENAMES, no system icd.d). ✓
- **§4 Precond 0 + Precond 1** → Task 4 (script) + Task 6 Step 1. ✓
- **§5 ref stream + corpus** → Task 6 Steps 2-4 (case-1 minimal; case-2 reuses existing B-frame stream, case-3 crop — best-effort, not blocking). ✓
- **§6 Step 0 fast-probe** → Task 7 Steps 2-3. **Step 1 tracer** → Task 7 Steps 4-5. **Step 2 DPB+per-request** → Task 8 Steps 1-2. **Step 3 parser** → Task 8 Step 3. **Step 4 readback a/b/c** → Task 9 Steps 1-3. **Step 5 nego** → Task 9 Step 4. ✓
- **§6.A multi-planar/format log** → Task 5 DUMP_A logs stride/sizeimage/dmabuf; deeper per-plane G_FMT captured in Task 6 `formats-ext.txt` + Task 7 logs. ✓
- **§6.B coded vs visible, UV row=W, strides from metadata** → Task 2 `normalize_nv12` + tests. ✓
- **R11 cache/DMA sync** → Task 5 helper (DMA_BUF_IOCTL_SYNC). ✓
- **§8 artifacts dir + per-dump metadata** → Task 4 scaffold + commits throughout. ✓
- **§9 Stage-2 gated** → Task 10 Step 6 (explicitly not started). ✓

**Gaps acknowledged (not plan failures):** corpus case-2/case-3 (reordering, crop) are best-effort after case-1 byte-exact, per spec §5. The fix code in Task 10 is intentionally branch-dependent (a debug spike cannot pre-author the fix before diagnosis) — the VERIFY harness and candidate fixes per hypothesis are fully specified; the exact edit is determined by the evidence.

**Type/interface consistency:** `normalize_nv12(raw, stride_y, stride_uv, buf_h, out_w, out_h, uv_offset=None)` and `compare(a,b)` / `psnr(a,b)` / `distinct_values(data)` are used identically in tests, CLI, and Tasks 7/9. `tracer_diff.diff_first_controls(ours, golden, ioctl_name)` + `first_ioctl(obj, name)` consistent across test and CLI. Env flags `V4L2VK_DUMP_CAPTURE`/`V4L2VK_DUMP_VKIMAGE`/`V4L2VK_DUMP_DIR` consistent between Task 5 (C) and Tasks 7/9 (invocation).
