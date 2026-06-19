# Phase B0 artifacts (reproducible)

- `input/`     stream(s) + SHA256 (gitignored — binaries)
- `traces/`    v4l2-tracer ours + golden (gitignored)
- `dumps/`     readback (a)/(b)/(c) (gitignored)
- `metadata/`  driver identity, G_FMT per-plane, strides (committed — text)
- `compare/`   normalized + diff/PSNR summaries (committed — text)

Per dump record: SHA256 + frame-index + PTS + buffer-index + pixelformat +
w/h + visible-crop + strides(y,uv) + num_planes + bytesused.
Findings live in the vault: OBSIDIAN_Kernel/VulkanVideo/wiki/analyses/phase-b0-h264-correctness-findings.md
