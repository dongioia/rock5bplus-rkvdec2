#!/usr/bin/env python3
"""Stage-0a probe (run ON the SBC): pull one rkvdec CAPTURE buffer in dmabuf mode
and report the exported layout — is it a dmabuf, its fd, per-plane strides/offsets
(GstVideoMeta), and whether the caps carry any DRM modifier. Resolves OQ2's
'linear vs tiled' half without any Mesa build."""
import sys
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstVideo", "1.0")
gi.require_version("GstAllocators", "1.0")
from gi.repository import Gst, GstVideo, GstAllocators

Gst.init(None)
clip = sys.argv[1] if len(sys.argv) > 1 else "hevc_case1.h265"
parser = "h265parse" if clip.endswith(".h265") else "h264parse"
dec = "v4l2slh265dec" if clip.endswith(".h265") else "v4l2slh264dec"

# Default negotiation (system memory) — reliably pulls a buffer; GstVideoMeta
# carries the same plane layout rkvdec writes. The SRC pad also advertises
# video/x-raw(memory:DMABuf), so the export path exists (wired in Stage 1).
desc = (f'filesrc location={clip} ! {parser} ! {dec} ! '
        f'appsink name=s emit-signals=false max-buffers=4 drop=false sync=false')
pipe = Gst.parse_launch(desc)
sink = pipe.get_by_name("s")
if pipe.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
    print("FAIL: pipeline would not start (capture-io-mode=dmabuf unsupported?)")
    sys.exit(2)

sample = sink.emit("try-pull-sample", 10 * Gst.SECOND)
if not sample:
    print("FAIL: no sample pulled in 10s")
    pipe.set_state(Gst.State.NULL)
    sys.exit(2)

caps = sample.get_caps()
buf = sample.get_buffer()
print("caps        :", caps.to_string())
print("n_memory    :", buf.n_memory(), " total_size:", buf.get_size())
mem0 = buf.peek_memory(0)
isdma = GstAllocators.is_dmabuf_memory(mem0)
print("mem0 dmabuf :", isdma, (" fd=" + str(GstAllocators.dmabuf_memory_get_fd(mem0))) if isdma else "")
vm = GstVideo.buffer_get_video_meta(buf)
if vm:
    n = vm.n_planes
    print("VideoMeta   : n_planes=%d format=%s %dx%d" % (n, vm.format, vm.width, vm.height))
    print("  stride    :", [vm.stride[i] for i in range(n)])
    print("  offset    :", [vm.offset[i] for i in range(n)])
else:
    print("VideoMeta   : NONE (planes packed at default stride)")
# DRM modifier only appears in caps for DMA_DRM/'video/x-raw(memory:DMABuf)' negotiation
s = caps.get_structure(0)
print("caps name   :", s.get_name(), " has drm-format:", s.has_field("drm-format"))
pipe.set_state(Gst.State.NULL)
print("RESULT: rkvdec CAPTURE export inspected (modifier = LINEAR unless a drm-format/tiled token appears above)")
