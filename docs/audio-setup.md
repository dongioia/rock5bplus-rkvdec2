# Audio Setup - Rock 5B+ Mainline Kernel

## Overview

HDMI audio and analog (3.5mm jack) output work on mainline kernel 6.19+ with two configuration fixes.

## Hardware

- **HDMI audio**: DW HDMI QP codec via I2S5 (hdmi0) / I2S6 (hdmi1)
- **Analog audio**: Everest ES8316 codec via I2S0 (3.5mm headphone/line out + headset mic)
- **Kernel modules**: `snd_soc_hdmi_codec`, `snd_soc_rockchip_i2s_tdm`, `snd_soc_es8316`, `snd_soc_simple_card`

## Fix 1: WirePlumber device naming

PipeWire/WirePlumber labels all three audio cards as "Audio interno" with analog icon. This makes HDMI output indistinguishable from analog in desktop UIs.

**File**: `~/.config/wireplumber/wireplumber.conf.d/51-hdmi-rename.conf`

```lua
monitor.alsa.rules = [
  {
    matches = [
      {
        device.name = "alsa_card.platform-hdmi0-sound"
      }
    ]
    actions = {
      update-props = {
        device.description = "HDMI 0"
        device.form-factor = "hdmi"
        device.icon-name = "video-display"
      }
    }
  }
  {
    matches = [
      {
        device.name = "alsa_card.platform-hdmi1-sound"
      }
    ]
    actions = {
      update-props = {
        device.description = "HDMI 1"
        device.form-factor = "hdmi"
        device.icon-name = "video-display"
      }
    }
  }
  {
    matches = [
      {
        device.name = "alsa_card.platform-analog-sound"
      }
    ]
    actions = {
      update-props = {
        device.description = "Cuffie / Line Out"
      }
    }
  }
  {
    matches = [
      {
        node.name = "alsa_output.platform-hdmi0-sound.stereo-fallback"
      }
    ]
    actions = {
      update-props = {
        node.description = "HDMI 0 (LG TV)"
        node.nick = "HDMI 0"
      }
    }
  }
]
```

## Fix 2: Disable jack detection for analog output

The ES8316 UCM profile ties the Headphones output port to `JackControl "Headphones Jack"`. When nothing is plugged in, the port is marked "not available" and desktop UIs (Cinnamon, GNOME) hide it from the output list.

**Fix**: Remove the `JackControl` line from the UCM HiFi profile.

**File**: `/usr/share/alsa/ucm2/Rockchip/rk3588-es8316/HiFi.conf`

Remove this line from the `SectionDevice."Headphones"` → `Value` block:
```
JackControl "Headphones Jack"
```

Then restart PipeWire:
```bash
systemctl --user restart pipewire wireplumber
```

**Note**: This file may be overwritten by `alsa-ucm-conf` package updates. Re-apply after updating.

## Result

After both fixes, three audio outputs are available:
- **HDMI 0** — HDMI audio to connected display (LPCM, AC-3, E-AC-3, TrueHD)
- **HDMI 1** — second HDMI output (if connected)
- **Cuffie / Line Out** — 3.5mm analog jack (always visible)

## Supported audio formats (from TV EDID/ELD)

| Format | Channels | Sample Rates |
|--------|----------|-------------|
| LPCM | 2 | 32/44.1/48/96/192 kHz, 16/20/24-bit |
| AC-3 | 6 | 32/44.1/48 kHz, max 640 kbps |
| E-AC-3 (DD+) | 8 | 32/44.1/48 kHz |
| MLP (TrueHD) | 8 | 48 kHz |

## Known issues

- `HDMI: Unknown ELD version 0` / `ASoC error (-19)` in dmesg at boot — harmless, caused by HDMI1 port (not connected). Audio works fine on connected port.
- HDMI audio sink uses `stereo-fallback` profile name (cosmetic, doesn't affect functionality).
