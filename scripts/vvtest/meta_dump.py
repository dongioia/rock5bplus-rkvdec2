#!/usr/bin/env python3
# Dump the GstVideoMeta (coded geometry) + caps (visible) of a v4l2 stateless
# decoder's first output buffer. Meta-aware: a QUERY_DOWNSTREAM probe adds
# GST_VIDEO_META_API so the padded hardware dmabuf is kept (else appsink gets a
# visible-cropped copy and the padding is hidden).
import sys, gi
gi.require_version('Gst', '1.0'); gi.require_version('GstVideo', '1.0')
from gi.repository import Gst, GstVideo
Gst.init(None)

clip = sys.argv[1] if len(sys.argv) > 1 else 'vp9_1280.webm'
dec  = sys.argv[2] if len(sys.argv) > 2 else 'v4l2slvp9dec'
demux = 'matroskademux' if clip.endswith('.webm') else 'qtdemux'
parse = {'v4l2slvp9dec':'vp9parse','v4l2slh264dec':'h264parse',
         'v4l2slh265dec':'h265parse','v4l2slav1dec':'av1parse'}[dec]
# Use the meta-bridge element (adds GstVideoMeta to the ALLOCATION query in C,
# so the padded hardware dmabuf is kept).
bridge = {'v4l2slvp9dec':'v4l2vp9metabridge','v4l2slh264dec':'v4l2h264metabridge',
          'v4l2slh265dec':'v4l2h265metabridge','v4l2slav1dec':'v4l2av1metabridge'}[dec]

pipe = Gst.parse_launch(
  f"filesrc location={clip} ! {demux} ! {parse} ! {bridge} ! "
  f"appsink name=s sync=false max-buffers=3 drop=false")
sink = pipe.get_by_name('s')
pipe.set_state(Gst.State.PLAYING)
sample = sink.emit('try-pull-sample', Gst.SECOND * 5)
if not sample:
    print("NO SAMPLE"); pipe.set_state(Gst.State.NULL); sys.exit(1)
caps = sample.get_caps()
buf  = sample.get_buffer()
s = caps.get_structure(0)
print("CAPS visible :", s.get_value('width'), "x", s.get_value('height'),
      "fmt", s.get_value('format'))
vm = GstVideo.buffer_get_video_meta(buf)
if vm:
    print("VIDEOMETA    :", vm.width, "x", vm.height, "n_planes", vm.n_planes)
    print("  strides    :", [vm.stride[i] for i in range(vm.n_planes)])
    print("  offsets    :", [vm.offset[i] for i in range(vm.n_planes)])
else:
    print("VIDEOMETA    : NONE (got a copied/visible buffer)")
print("BUFFER size  :", buf.get_size())
pipe.set_state(Gst.State.NULL)
