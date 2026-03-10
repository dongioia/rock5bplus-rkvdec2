# Benchmark Baseline — Rock 5B+ Kernel 6.19.1 Custom

**Data**: 28 Febbraio 2026
**Kernel**: 6.19.1-00005-g8a580710d406-dirty
**Build**: #2 SMP PREEMPT Tue Feb 17 07:51:20 UTC 2026
**CPU Governor**: performance (entrambi i cluster)
**CPU Freq**: A55 @ 1800 MHz, A76 @ 2400 MHz

---

## CPU — OpenSSL Speed

### Multi-thread (8 thread)

| Algorithm | 16B | 64B | 256B | 1024B | 8192B | 16384B |
|-----------|-----|-----|------|-------|-------|--------|
| sha256 | 302K | 1,093K | 3,140K | 5,926K | 8,250K | **8,491K** |
| aes-256-cbc | 2,947K | 5,515K | 7,275K | 8,028K | 8,253K | **8,320K** |

### Single-thread

| Algorithm | 16B | 64B | 256B | 1024B | 8192B | 16384B |
|-----------|-----|-----|------|-------|-------|--------|
| sha256 | 59K | 211K | 579K | 1,026K | 1,330K | **1,359K** |
| aes-256-cbc | 582K | 984K | 1,160K | 1,217K | 1,237K | **1,239K** |

**Scaling multi/single**: sha256 6.25x, aes 6.72x (su 8 core)

## GPU — glmark2-es2-wayland (offscreen 1920x1080)

**Score: 107**

(Note: il punteggio di 2550 documentato altrove è con display attivo, test diverso)

## Storage — dd (SD card)

| Test | Velocità |
|------|----------|
| Write (512MB, fdatasync) | **2.1 GB/s** |
| Read (256MB, drop caches) | **4.5 GB/s** |

(Nota: velocità alta perché include page cache; la SD card fisica è ~100 MB/s)

## Memory Bandwidth — dd /dev/zero → /dev/null

**10.9 GB/s** (4 GiB block, single-threaded)

## Thermal (°C)

| Sensore | Pre-bench | Post-bench |
|---------|-----------|------------|
| package | 47.2 | 50.8 |
| bigcore0 | 47.2 | 49.9 |
| bigcore2 | 47.2 | 49.9 |
| littlecore | 47.2 | 50.8 |
| center | 46.2 | 49.0 |
| gpu | 45.3 | 49.0 |
| npu | 46.2 | 49.9 |

Delta: +3-4°C dopo benchmark (buon raffreddamento)

## VPU Devices Attivi

| Device | Driver | Video Node |
|--------|--------|------------|
| Hantro VPU (fdb50000) | hantro_vpu | /dev/video2 |
| VEPU121 encoder (fdba0000) | rockchip,rk3588-vepu121-enc | /dev/video3 |
| AV1 VPU (fdc70000) | rockchip,rk3588-av1-vpu-dec | /dev/video4 |
| RKVDEC2 (rkvdec) | rockchip_vdec | /dev/video5 |
| HDMI-RX (fdee0000) | snps_hdmirx | /dev/video1 |
| RGA (rga) | rockchip-rga | /dev/video0 |

## Moduli Chiave Caricati

- `rockchip_vdec` (106K) — RKVDEC2 H.264/HEVC/VP9
- `hantro_vpu` (274K) — Hantro decoder (VP8, MPEG-2/4, JPEG)
- `panthor` (147K) — Mali-G610 GPU driver
- `rockchipdrm` (200K) — Display controller (VOP2)
- `rocket` — NPU driver (RKNN)

---

*Benchmark da usare come baseline per confronto con kernel BredOS 6.19.1 e Linux 7.0-rc1*
