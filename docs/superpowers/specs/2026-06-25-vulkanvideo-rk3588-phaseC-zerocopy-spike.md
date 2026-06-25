# Phase C — Zero-Copy: Step-1 Foundational Spike (design)

**Date:** 2026-06-25
**Branch:** `spec/vulkanvideo-phaseC-zerocopy`
**Status:** design (pre-plan). Independent adversarial review applied (verdict HOLD-WITH-CHANGES, all findings incorporated).

## Goal

**Phase C objective:** in-browser zero-copy hardware video decode on RK3588 — decode on rkvdec, sample the decoded NV12 on the Mali GPU via PanVK's hardware YUV texturing (Mesa MR !42353, merged into Mesa main 2026-06-24), with no GPU→system-memory readback. This eliminates the `vulkandownload` copy that the current H.264/HEVC browser path does on every frame.

**This spec covers Step 1 only:** a foundational spike that proves (or kills) the building block — can a real rkvdec-decoded NV12 DMA-BUF be imported into PanVK and sampled correctly? It is the B0-equivalent for Phase C: smallest experiment that resolves the make-or-break unknowns before any pipeline or browser work.

## Context

Current working state (Phases A/B0 + Stage-2/3 + HEVC): a standalone V4L2-backed Vulkan Video decode ICD — a **separate `VkPhysicalDevice`** from PanVK — decodes H.264 and HEVC byte-exact on rkvdec. Browser playback works in WebKitGTK via a GStreamer bridge (`vulkanh26Xdec ! vulkandownload`) that reads the decoded `VkImage` back to system-memory NV12. That readback is the per-frame CPU/bandwidth cost zero-copy removes.

## Decomposition (Phase C is three gated steps)

1. **Step 1 — Foundational spike (this spec).** Prove rkvdec DMA-BUF → PanVK import → HW-YUV sample → pixels correct. Resolves OQ2 (rkvdec modifier) + the isolated Mesa+!42353 build + cross-device import.
2. **Step 2 — Standalone no-copy pipeline.** Decode → dmabuf → PanVK sample → on-screen (no readback), measured against the CPU-copy path. Outside the browser. (Own spec after Step 1.)
3. **Step 3 — In-browser zero-copy.** WebKitGTK composites the GPU buffer with no system-memory round-trip. The "no WebKit patch" constraint is **deliberately deferred** to this step, decided with real information from Steps 1–2. (Own spec.)

Each is a prerequisite for the next.

## Architecture

Two `VkPhysicalDevice`s joined by a DMA-BUF: the existing standalone decode ICD produces the decoded NV12 in a V4L2 CAPTURE buffer; PanVK imports that buffer's exported DMA-BUF as a `VkImage` and samples it. This keeps the proven decode path intact and does **not** add a V4L2 video queue inside PanVK (ADR Option A — rated low, invasive, not revisited).

## Step-1 design

### Prerequisite — isolated, pinned-commit Mesa+!42353 PanVK build

Build a Mesa **main checkout pinned to an exact commit** that contains the !42353 merge (the commit carrying `78e55592` "panvk: enable 8bit multiplanar YUV formats on v9+ to v13"), `-Dvulkan-drivers=panfrost`, into a **self-contained parallel prefix**. Select it per-run via `VK_ICD_FILENAMES` (its `panfrost_icd.aarch64.json`) and `LD_LIBRARY_PATH`. Record the exact `git rev` so the spike is reproducible — do **not** track moving `main`.

Isolation rules (mirror the existing ICD discipline):
- The pacman-pinned system `mesa 1:26.0.6-1` stays untouched; verify unchanged after every run (`pacman -Q mesa`).
- The parallel prefix must be self-contained: `LD_LIBRARY_PATH` must resolve `libgallium`/`libvulkan_lvp`/`libdrm` from the **build prefix**, not mix the system copies (the `libgstvulkan.so` ABI-ordering pitfall hit before — order the path so the build prefix wins, and confirm with `ldd` on the built `libvulkan_panfrost.so`).
- Cherry-picking !42353's 16 commits onto 26.0.6 is the rejected alternative — more fragile than a pinned-main build.

Op note: the V4L2 CAPTURE/EXPBUF allocation code lives in the `mesa-sree-tree` Docker volume (`/work/mesa-sree`), not in this repo. Confirm the EXPBUF path (CAPTURE MMAP buffers EXPBUF'd to fds, plane layout) in-container before implementing Stage 0.

### Stage 0 — Falsify "linear" first (the cheap kill-switch)

The single highest-information, lowest-cost experiment. Do this before writing any import/sampling code.

1. Decode one frame and **EXPBUF a real rkvdec CAPTURE buffer** (`gst … ! v4l2slh265dec` with CAPTURE `io-mode=dmabuf ! appsink`, pull the first buffer's DMA-BUF fd; or drive V4L2 directly). Read its **actual** DRM format modifier and per-plane offsets/strides — not the CPU mmap stride.
2. On the pinned-commit !42353 PanVK, query whether it advertises the tuple `(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, <that modifier>, VK_IMAGE_USAGE_SAMPLED_BIT)`: enumerate `VkDrmFormatModifierPropertiesListEXT` for the format and check the modifier is present, and confirm via `vkGetPhysicalDeviceImageFormatProperties2` chained with `VkPhysicalDeviceImageDrmFormatModifierInfoEXT`.

**Why this is the lead:** "rkvdec exports linear NV12" is unproven. The Stage-3 byte-exact-at-`align16` evidence is about the ICD's **internal memcpy target**, not the exported buffer's modifier. Prior chromium-fourier work on this exact SoC+GPU (claude-mem #5564 "NV12 modifiers external-only on RK3588", #5700 "modifier forced to LINEAR for V4L2 rkvdec buffers") shows the modifier story is non-trivial in the GL/EGL path; applicability to the PanVK Vulkan import path is unproven either way. So measure it.

**Stage-0 gate:** the modifier is documented, and PanVK advertises the `(NV12, modifier, SAMPLED)` tuple. If **not advertised**, zero-copy via PanVK is blocked at the import boundary → the spike fails fast and cheap; we keep the proven CPU-copy path and record the finding. No further Stage-1 work.

### Stage 1 — Import + dual-check (only if Stage 0 passes)

Import the **real** rkvdec DMA-BUF into a PanVK `VkImage`. Required extension set (all must be enumerated on the !42353 PanVK; verify before use):
- `VK_EXT_external_memory_dma_buf` + `VK_KHR_external_memory_fd` — import the fd to `VkDeviceMemory`.
- `VK_EXT_image_drm_format_modifier` — bind it to an **image** with a known layout: chain `VkImageDrmFormatModifierExplicitCreateInfoEXT` (the Stage-0 modifier + per-plane `VkSubresourceLayout`) and `VkExternalMemoryImageCreateInfo` onto `VkImageCreateInfo`. (`external_memory_dma_buf` alone only suffices for a `VkBuffer`, per the Khronos spec — an image needs this layer.)
- `VK_KHR_sampler_ycbcr_conversion` (core 1.1) — for the sampling check.

Create the image with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` + `VK_IMAGE_CREATE_EXTENDED_USAGE_BIT` (+ `DISJOINT` if the modifier/layout require per-plane memory), so two different views are legal on the same import.

Two **explicit Vulkan operations on the same import** (not an mmap of the V4L2 fd — that proves nothing about PanVK):

- **1a — layout check (byte-exact).** A **non-converting** per-plane view: plane 0 as `VK_FORMAT_R8_UNORM` (`VK_IMAGE_ASPECT_PLANE_0_BIT`), plane 1 as `VK_FORMAT_R8G8_UNORM` (`PLANE_1_BIT`). `vkCmdCopyImageToBuffer` each plane to a host-visible buffer; byte-compare against the **canonical linear NV12** for this frame (the same bytes the CPU-copy path / ffmpeg produce — which we already know our decode matches byte-exact). Note: `copyImageToBuffer` de-tiles per the image's modifier, so the reference is the canonical *linear* frame regardless of how rkvdec stores it; a match proves PanVK interprets the import + modifier + per-plane layout correctly (not the raw V4L2 byte pattern, which differs under a tiled modifier).
- **1b — sample check (PSNR).** A `VkSamplerYcbcrConversion` combined-sampler view; sample to an RGB render target; read back; compare to a CPU NV12→RGB reference **within a PSNR/tolerance** (HW fixed-point CSC ≠ bit-exact). Pin the conversion explicitly: `ycbcrModel` (BT.601 vs 709 per the stream VUI), `ycbcrRange` (limited vs full), and `xChromaOffset`/`yChromaOffset` — and make the CPU reference use the **identical** parameters, including PanVK's overridden chroma siting (merged commit `2fe1fdc9`). Mismatched siting sags PSNR for reasons unrelated to correctness.

**Cross-IOMMU + sync:** the buffer is exported from the rkvdec V4L2 IOMMU domain and imported into Mali's. Confirm the import succeeds (non-null `VkDeviceMemory`, `dma_buf_attach` into the Mali device works). For the 1a CPU readback, do it through a **Vulkan staging copy** to a host-visible buffer (above), not by mmap'ing the dma-buf directly; if a direct mmap is ever used, bracket it with `DMA_BUF_IOCTL_SYNC`.

### Plan-B (precise — two distinct failure modes)

- HW-YUV texturing **rejects an importable modifier** → fall back to the SW-CSC path (merged commit `a7d741eb`, "panvk: lower YUV texturing to do SW CSC") on the **same import**. Still zero-copy on the buffer; CSC in a shader.
- Modifier **not importable at all** (Stage-0 fail) → the zero-copy-via-PanVK thesis is dead for this buffer; keep the proven CPU-copy Stage-2/3 path. SW-CSC does **not** help here — it samples an imported image; it is not an import fallback.

## Verification gates (summary)

| Gate | Pass criterion |
|---|---|
| Mesa build | pinned-commit panfrost build runs; `ldd` shows build-prefix libs; system `mesa 26.0.6` unchanged |
| Stage 0 | rkvdec modifier documented; PanVK advertises `(NV12, modifier, SAMPLED)` |
| Stage 1a | per-plane non-converting readback **byte-exact** vs V4L2 buffer |
| Stage 1b | ycbcr-sampled RGB within PSNR tolerance of CPU ref at pinned model/range/siting |
| Isolation | `pacman -Q mesa` == `1:26.0.6-1` after the run |

## Key assumptions to falsify (not to trust)

- "rkvdec exports linear NV12" — **unproven**; Stage 0 measures it. Prior GL evidence is mixed/non-trivial.
- "PanVK advertises rkvdec's modifier for NV12+SAMPLED" — unproven; Stage-0 gate-zero.
- Cross-device dma-buf import succeeds across the two IOMMU domains — assumed, confirmed in Stage 1.
- G610 is inside PanVK's arch 9–13 HW-YUV gate — **sufficient** (verified: `pan_props.c` AFRC≥v10 + `PER_ARCH_FUNCS(10)`); exact "=10" is inference but does not gate anything.

## Out of scope (Step 1)

Any GStreamer element; the standalone display pipeline (Step 2); the browser (Step 3); the "no WebKit patch" decision; Main10/10-bit; codecs beyond what already decodes (H.264/HEVC 8-bit 4:2:0).

## Open questions touched

- **OQ2 (rkvdec DMA-BUF modifier)** — Stage 0 resolves it by measurement. Gap-tracker to be updated: "preliminary linear" downgraded to "unproven; GL-path counter-evidence; measure via EXPBUF + PanVK modifier list."
- **OQ14 (IOMMU prerequisite)** — Stage 1 documents whether the cross-device import maps cleanly; partial answer for the embedded Vulkan-Video IOMMU question.

## References

- Mesa MR !42353 (merged 2026-06-24): commits `78e55592` (enable 8bit multiplanar YUV v9–v13), `a7d741eb` (SW CSC lowering), `47e8d5c1` (reject INTERLEAVED_64K/16x16 for multiplanar YUV), `2fe1fdc9` (override chroma siting). Vault: `Source-2026-06-24-panvk-yuv-texturing-merged.md`.
- Khronos: `VK_EXT_external_memory_dma_buf` (image-insufficient), `VK_EXT_image_drm_format_modifier` (explicit-layout import), `VK_KHR_sampler_ycbcr_conversion`.
- Vault: `gap-tracker.md` (OQ2, OQ14), `analyses/architecture-decision-record.md` (two-device + DMA-BUF; Option A rejected).
- Prior art: claude-mem #5564 / #5700 / #5546 (chromium-fourier NV12 modifier handling on RK3588 Panfrost, GL/EGL).
- Independent review verdict HOLD-WITH-CHANGES (F1 false-green gate, F2 missing modifier ext, F3 linear-unproven, F4 support-query gate, F5 IOMMU/sync, F7 plan-B precision, F8 pinned build) — all incorporated above.
