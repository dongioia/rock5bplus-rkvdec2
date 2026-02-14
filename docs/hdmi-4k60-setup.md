# HDMI 2.0 / 4K@60Hz - Rock 5B+ Mainline Kernel

## Problem

The mainline DW HDMI QP bridge driver (`dw-hdmi-qp.c`) limits TMDS character rate to 340 MHz (HDMI 1.4b), blocking 4K@60Hz which requires 594 MHz (HDMI 2.0). The Samsung HDPTX PHY supports up to 600 MHz but the bridge driver doesn't implement SCDC scrambling.

## Solution

Applied Cristian Ciocaltea's (Collabora) HDMI 2.0 scrambling patch series v3, with manual adaptation for kernel 6.19-rc8.

### Patch series: "Add HDMI 2.0 support to DW HDMI QP TX"

- **Author**: Cristian Ciocaltea <cristian.ciocaltea@collabora.com>
- **Version**: v3, 19 January 2026
- **Source**: https://lore.kernel.org/r/20260119-dw-hdmi-qp-scramb-v3-0-bd8611730fc1@collabora.com/
- **Target tree**: `drm-misc-next` (not yet merged as of Feb 2026)

### Patches applied

| # | Patch | Applied via | Notes |
|---|-------|------------|-------|
| 1/4 | `drm/bridge: Add ->detect_ctx hook and drm_bridge_detect_ctx()` | `git am` | Clean apply |
| 2/4 | `drm/bridge-connector: Switch to using ->detect_ctx hook` | `git am` | Clean apply |
| 3/4 | `drm/bridge: dw-hdmi-qp: Add high TMDS clock ratio and scrambling support` | Manual | Conflict resolution needed (see below) |
| 4/4 | `drm/rockchip: dw_hdmi_qp: Do not send HPD events for all connectors` | `git am` | Clean apply |

### Manual adaptation for patch 3/4

The v3 patch targets `drm-misc-next` which diverges from kernel 6.19-rc8. Changes made manually:

1. **Struct additions** to `dw_hdmi_qp`: `curr_conn`, `scramb_work`, `scramb_enabled`
2. **New functions**: `dw_hdmi_qp_supports_scrambling()`, `dw_hdmi_qp_set_scramb()`, `dw_hdmi_qp_scramb_work()`, `dw_hdmi_qp_enable_scramb()`, `dw_hdmi_qp_disable_scramb()`, `dw_hdmi_qp_reset_crtc()`
3. **Modified functions**: `bridge_atomic_enable()` (adds scrambling on TMDS > 340 MHz), `bridge_atomic_disable()` (disables scrambling), `bridge_detect()` → `bridge_detect_ctx()` (handles SCDC reset on reconnect)
4. **Rate validation**: `tmds_char_rate_valid()` raised from 340 MHz to 600 MHz
5. **Removed**: `no_hpd` checks (field doesn't exist in 6.19-rc8, not needed for Rock 5B+ which always has HPD)

### Result

- **3840x2160@60Hz** (594 MHz TMDS) — working, preferred mode
- **3840x2160@50Hz** (594 MHz TMDS) — available
- **3840x2160@30Hz** (297 MHz TMDS) — still available
- **1920x1080@120Hz** (297 MHz TMDS) — available
- SCDC scrambling enabled automatically for modes above 340 MHz

### Module deployment

Only kernel modules need replacement (no full kernel rebuild required):
- `drm.ko` — DRM core (detect_ctx infrastructure)
- `drm_kms_helper.ko` — KMS helper
- `drm_display_helper.ko` — bridge-connector detect_ctx
- `dw-hdmi-qp.ko` — HDMI 2.0 scrambling
- `rockchipdrm.ko` — HPD fix

Modules are at `/lib/modules/$(uname -r)/kernel/drivers/gpu/drm/`. Backups saved with `.bak` suffix.

### Tested by

- Maud Spierings (1440p@100Hz, 4K@60Hz) — on v1
- Diederik de Haas — on v1
- This project (Rock 5B+ / LG TV, 4K@60Hz) — on v3 adapted for 6.19-rc8
