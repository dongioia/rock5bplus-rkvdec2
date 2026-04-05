# Kernel 7.0 Performance Tuning — Proposal for Beryllium

## Context

With 7.0 final expected mid-April, I've been benchmarking kernel config optimizations on a Rock 5B+ (RS129-D24E0, 24GB LPDDR5) running 7.0-rc3+. All changes were tested incrementally with before/after benchmarks using openssl, sysbench, tinymembench.

Starting point: the current BredOS/Beryllium 6.19.1 config.

## Results

Compared to the stock 6.19.1 config on the same hardware:

| Benchmark | Before | After | Change |
|-----------|--------|-------|--------|
| OpenSSL SHA256 8-thread | 7,113 MB/s | 8,449 MB/s | **+18.8%** |
| OpenSSL AES-256-CBC 8-thread | 7,056 MB/s | 8,280 MB/s | **+17.3%** |
| tinymembench memcpy | 11,423 MB/s | 12,125 MB/s | **+6.1%** |
| tinymembench NEON copy | 12,077 MB/s | 12,476 MB/s | **+3.3%** |
| sysbench CPU / memory | — | — | no regression |

Thermal: +1.9°C under sustained load (56.4°C vs 54.5°C). Acceptable.

## What changed

### 1. KCFLAGS targeting ARMv8.2-A (biggest single win)

```
KCFLAGS='-march=armv8.2-a+crypto+fp16+dotprod -mtune=cortex-a76'
```

RK3588 has A76+A55, both ARMv8.2-A. The default kernel build uses generic `armv8-a`. Targeting the actual ISA enables:
- **Hardware AES/SHA instructions** → explains the +17% AES jump
- **LSE atomics** → faster spinlocks and atomic ops under contention
- **FP16 + dot-product** → used by some kernel math paths

This is safe for any RK3588 board. No risk — these are the actual hardware capabilities.

### 2. schedutil governor (default instead of performance)

Tested both governors with identical workloads:
- SHA256 8T: schedutil **8,449 MB/s** vs performance 7,677 MB/s (+10%)
- All other benchmarks: identical

The counterintuitive result is because `performance` locks all cores at max frequency, accumulating heat during sustained workloads. `schedutil` starts lower and scales up, keeping thermals better managed. In idle, cores drop to 1.2 GHz (vs always 2.4 GHz), saving significant power.

Note: `beryllium-govctl` overrides the kernel default to `performance`. It doesn't support `schedutil` as an option. Worth considering adding it, or letting the kernel default take effect.

### 3. ARM64 errata cleanup (31 → 9)

The stock config enables 31 ARM64 errata workarounds + all vendor-specific ones (Cavium, Qualcomm, HiSilicon, Fujitsu, Nvidia). On RK3588 only A55 r2p0 and A76 r4p0 are present.

Kept (actually needed):
- A55: 1024718, 1530923
- A76: 1418040, 1165522, 1286807, 1463225, 3194386
- Kconfig deps: 845719, 843419 (harmless — MIDR-matched at runtime)
- Rockchip: 3568002, 3588001

Disabled: all A53/A57/A72/A73/A77/A510/A710/A715/A520/Neoverse errata, all vendor-specific.

For a distro supporting multiple SoCs this needs to be per-board, but for RK3588-specific images it reduces branch overhead in hot paths.

### 4. THP madvise mode

`CONFIG_TRANSPARENT_HUGEPAGE=y` with madvise default. With 24GB RAM, reduces TLB pressure for memory-intensive workloads. Applications must explicitly request it via `madvise()`, so no compaction latency surprises.

### 5. ZRAM: LZ4 instead of lzo-rle, built-in

LZ4 is ~30% faster than lzo-rle on ARM64 with comparable compression ratios. Also disabled ZSWAP (competes with ZRAM for the same compressed pages).

### 6. sched-ext (CONFIG_SCHED_CLASS_EXT=y)

Enabled with DEBUG_INFO_BTF. Zero runtime overhead unless a BPF scheduler is explicitly loaded. Enables future experimentation with scx_bpfland, scx_lavd, etc. Requires `dwarves` (pahole) at build time.

### 7. Debug overhead removed

Disabled `SOFTLOCKUP_DETECTOR` and `DETECT_HUNG_TASK` — useful for development but unnecessary overhead for production images.

## Config fragment for merge_config.sh

```
# Beryllium 7.0.y RK3588 performance fragment
# Usage: scripts/kconfig/merge_config.sh .config this_file

# Governor
CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y
# CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE is not set

# THP
CONFIG_TRANSPARENT_HUGEPAGE=y
CONFIG_TRANSPARENT_HUGEPAGE_MADVISE=y

# ZRAM LZ4
CONFIG_ZRAM=y
CONFIG_ZRAM_BACKEND_LZ4=y
CONFIG_ZRAM_DEF_COMP_LZ4=y
CONFIG_ZRAM_DEF_COMP="lz4"
# CONFIG_ZSWAP is not set

# sched-ext
CONFIG_SCHED_CLASS_EXT=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_DEBUG_INFO_BTF_MODULES=y

# Debug reduction
# CONFIG_SOFTLOCKUP_DETECTOR is not set
# CONFIG_DETECT_HUNG_TASK is not set
```

Note: errata cleanup and KCFLAGS are not in the fragment — errata need per-SoC handling, and KCFLAGS belong in the build system (PKGBUILD or Makefile).

## Questions for the team

1. Should `beryllium-govctl` add `schedutil` support? Or should it be disabled by default for 7.0?
2. Is the errata cleanup worth maintaining per-board, or too fragile for a multi-SoC distro?
3. Any concerns about enabling BTF/sched-ext by default? It adds ~10% build time and some kernel size.

Happy to share the full config diff or test additional changes.

— Sav
