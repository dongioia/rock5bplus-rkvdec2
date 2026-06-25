# Stage-1b result — PanVK HW-YUV sample of the imported rkvdec NV12 → STAGE 1 PASS

**Date:** 2026-06-25. **Board:** Rock 5B+, kernel 7.1.0-rc1+, rkvdec `/dev/video0`.
**PanVK:** Mesa 26.2.0-devel (git-`e5ec9502`, !42353), isolated `~/mesa-zc/` (system mesa `1:26.0.6-1` untouched, verified pre/post).
**Artifact:** `scripts/vvtest/zc_import_test.c` (`gate_1b`, `chroma_flat`) + `zc.comp` (compute) + `zc-run.sh`.
**Reviewed:** two independent adversarial reviews of 1b (pre-deploy + post-methodology-change); findings folded; root cause source-verified.

## Gate 1b = PASS (HW ycbcr CSC correct), 2 resolutions

The same imported VkImage (1a) is sampled through a `VkSamplerYcbcrConversion` (BT.709, ITU_NARROW, COSITED_EVEN, NEAREST) bound as an immutable combined sampler; a compute shader writes RGBA8 to an SSBO; compared to a CPU BT.709-limited reference.

| clip | res | FLAT PSNR (gate) | flat-% | flat-distinct | whole-frame | 1a |
|---|---|---|---|---|---|---|
| case1 | 1280×720 | **82.31 dB** (max\|d\|=3) | 86.4% | 54 | 26.92 dB | byte-exact |
| t1088 | 1280×1088 | **79.52 dB** (max\|d\|=3) | 86.4% | 95 | 28.17 dB | byte-exact |

Gate = `flat_psnr ≥ 40 ∧ flat ≥ 50% frame ∧ flat-distinct ≥ 16`.

## Why the gate is flat-chroma PSNR (not whole-frame)

Whole-frame PSNR is only ~27 dB — **not** a CSC error. Root cause (source-verified in `/work/mesa-pin`):
- `panvk_image_use_yuv_tex` is true for NV12 on Valhall arch 9 (G610), so the conversion runs on the **Mali texture unit** (`panvk_vX_image_view.c get_yuv_cr_siting` → COSITED_EVEN → `MALI_YUV_CR_SITING_CO_SITED`, OR'd into the HW pixel-format word), **not** the generic NIR lowering.
- Mali's **fixed-function chroma upsampling** at chroma edges is not bit-reproducible by any CPU model (nearest/cosited/bilinear all capped ~33 dB).
- Partitioning by chroma flatness isolates it: **flat-chroma 86.4% → 82 dB, max|d|=3** (CSC correct, rounding only); **chroma-edge 13.6% → 17 dB, max|d|=255** (HW reconstruction).
- In flat regions the conversion is the only variable, and the CSC is a spatially-invariant affine map, so a flat-region match proves the matrix/range/component-map are correct. Verified directly: at hand-checked edge pixels the HW value equals the BT.709 conversion of a *single* neighbouring chroma texel — i.e. HW converts a given chroma correctly; it merely selects a different texel at edges.

## Honest scope of "PASS" (review caveat)
- PASS means **the HW ycbcr CSC produces correct RGB** — NOT that zero-copy output is byte-identical to software decode. Chroma-edge upsampling is HW-defined (Mali fixed-function) and intentionally **not gated**.
- The gate does **not** formally exclude one narrow residual: a CSC error correlated *only* with chroma triples that occur exclusively at chroma edges. The empirical signature (flat max|d|=3 vs edge max|d|=255) is the expected fixed-function-reconstruction pattern, not such a bug; but it is asserted, not proven, that no edge-exclusive-value CSC error exists.
- COSITED is correct siting for H.264/MPEG content (default chroma sample location type 0). Step-2/3 (on-screen / browser) will use Mali's own reconstruction end-to-end, so the un-gated edge path is exactly what production uses — its correctness is validated visually there, not by CPU PSNR.

## Stage-1 conclusion
Both building blocks proven on RK3588: **(1a)** real rkvdec NV12 DMA-BUF imports into PanVK byte-exact (cross-IOMMU, explicit LINEAR layout); **(1b)** the Mali HW ycbcr sampler converts it to correct RGB with no system-memory readback. **Zero-copy via PanVK is feasible AND pixel-correct.** Proceed to Step 2 (standalone no-copy on-screen pipeline, its own spec).

## Review fixes folded (1b)
- ycbcr device feature + extension enabled; immutable combined sampler + matching conversion on sampler & view (VU-correct, source-confirmed PanVK consumes per-plane internally).
- CPU ref BT.709 limited, NV12 Cb/Cr→B/R mapping confirmed (no swap), packing LE-correct, alpha=1.
- gate guard hardened: was whole-output `distinct>2` (wrong set) → now flat-set richness (`fdistinct≥16`) + `flat≥50%`.
- `chroma_flat` 3×3 indexing verified no-OOB.

## PanVK bug carried from 1a (still relevant)
Explicit DRM-modifier import needs `drmFormatModifierPlaneCount=2` (format planes) though the modifier advertises 1 — see `STAGE1a-import-bytexact.md`. Upstream-reportable.
