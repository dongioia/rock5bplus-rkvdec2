/*
 * gstv4l2metabridge.c — GStreamer plugin: V4L2H264MetaBridge
 *
 * Phase-C Step-3 Increment A. Wraps `v4l2slh264dec` in a GstBin and injects
 * the GstVideoMeta API into the downstream ALLOCATION query, so a consumer
 * that does NOT advertise GstVideoMeta (e.g. WebKitGTK 2.52's GL video sink)
 * can still negotiate the padded hardware dmabuf.
 *
 * Root cause this fixes (board gst log, 2026-06-27):
 *   gstv4l2codech264dec.c:477 gst_v4l2_codec_h264_dec_decide_allocation:
 *     "DMABuf caps negotiated without the mandatory support of VideoMeta"
 *   -> not-negotiated -> WebKit MediaError ERR4.
 * v4l2slh264dec mandates that a DMABuf consumer support the GstVideoMeta API
 * (rkvdec buffers are padded: stride != width, coded height != visible). The
 * same meta-aware ALLOCATION probe proven in Phase-C Step-2 (zc_*.c), packaged
 * as an autopluggable element. Zero-copy preserved: the dmabuf passes through
 * untouched; only the query is amended.
 *
 * Build on SBC:
 *   gcc -shared -fPIC -O2 -o libgstv4l2metabridge.so gstv4l2metabridge.c \
 *       $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0) \
 *       -Wl,-soname,libgstv4l2metabridge.so
 * Use:
 *   export GST_PLUGIN_PATH=$HOME/vvtest:$GST_PLUGIN_PATH
 *   gst-inspect-1.0 v4l2h264metabridge
 */
#define PACKAGE "v4l2metabridge"
#define PACKAGE_VERSION "1.0"
#include <gst/gst.h>
#include <gst/video/video.h>

#define GST_TYPE_V4L2METABRIDGE   (gst_v4l2metabridge_get_type())
#define GST_V4L2METABRIDGE(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_V4L2METABRIDGE, GstV4l2MetaBridge))

typedef struct _GstV4l2MetaBridge      GstV4l2MetaBridge;
typedef struct _GstV4l2MetaBridgeClass GstV4l2MetaBridgeClass;

struct _GstV4l2MetaBridge {
  GstBin      parent;
  GstElement *decoder;   /* v4l2slh264dec */
  GstPad     *sinkpad;   /* ghost pad on decoder sink */
  GstPad     *srcpad;    /* ghost pad on decoder src */
};

struct _GstV4l2MetaBridgeClass {
  GstBinClass parent_class;
};

GType gst_v4l2metabridge_get_type (void);
G_DEFINE_TYPE (GstV4l2MetaBridge, gst_v4l2metabridge, GST_TYPE_BIN)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE (
  "sink",
  GST_PAD_SINK,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS ("video/x-h264, "
                   "stream-format = (string) { byte-stream, avc, avc3 }, "
                   "alignment = (string) { au, nal }")
);

/* Advertise raw video incl. the DMABuf feature so decodebin links us to the
 * GL/dmabuf consumer (where the zero-copy path lives). */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE (
  "src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS ("video/x-raw(memory:DMABuf); video/x-raw")
);

/* Add GstVideoMeta API to the downstream ALLOCATION query (idempotent). This
 * is the whole point of the element: it compensates for a consumer that omits
 * GstVideoMeta, which v4l2codecs requires for DMABuf output. */
static GstPadProbeReturn
meta_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  (void) pad; (void) user_data;
  GstQuery *q = GST_PAD_PROBE_INFO_QUERY (info);
  if (q && GST_QUERY_TYPE (q) == GST_QUERY_ALLOCATION) {
    if (gst_query_is_writable (q)) {
      guint idx;
      if (!gst_query_find_allocation_meta (q, GST_VIDEO_META_API_TYPE, &idx))
        gst_query_add_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL);
    } else {
      /* Can't amend a non-writable ALLOCATION query -> the GstVideoMeta
       * injection is skipped and negotiation will fail with the original
       * "without the mandatory support of VideoMeta" error. Make it visible
       * instead of regressing to a silent ERR4. */
      GST_WARNING ("v4l2metabridge: ALLOCATION query not writable; "
                   "GstVideoMeta NOT injected (negotiation will fail)");
    }
  }
  /* Non-blocking data probe: OK = continue normally (PASS is for blocking probes). */
  return GST_PAD_PROBE_OK;
}

static void
gst_v4l2metabridge_init (GstV4l2MetaBridge *self)
{
  GstPad *pad;

  self->decoder = gst_element_factory_make ("v4l2slh264dec", "v4l2dec");
  if (!self->decoder) {
    GST_ERROR_OBJECT (self, "Cannot create v4l2slh264dec");
    return;
  }

  gst_bin_add (GST_BIN (self), self->decoder);

  /* Ghost sink pad (v4l2slh264dec is a GstVideoDecoder: sink/src are static
   * ALWAYS pads, so get_static_pad is non-NULL right after factory_make). */
  pad = gst_element_get_static_pad (self->decoder, "sink");
  if (!pad) { GST_ERROR_OBJECT (self, "no decoder sink pad"); return; }
  self->sinkpad = gst_ghost_pad_new ("sink", pad);
  gst_pad_set_active (self->sinkpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_object_unref (pad);

  /* Ghost src pad + the meta-injecting probe. The probe MUST sit on the
   * decoder's internal real src pad: the GstVideoDecoder base class issues the
   * ALLOCATION query via gst_pad_peer_query(decoder->srcpad, ...) on THIS pad
   * (not the ghost), and a QUERY_DOWNSTREAM probe here fires on that outgoing
   * query before decide_allocation reads it back. */
  pad = gst_element_get_static_pad (self->decoder, "src");
  if (!pad) { GST_ERROR_OBJECT (self, "no decoder src pad"); return; }
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, meta_probe, NULL, NULL);
  self->srcpad = gst_ghost_pad_new ("src", pad);
  gst_pad_set_active (self->srcpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
  gst_object_unref (pad);
}

static void
gst_v4l2metabridge_class_init (GstV4l2MetaBridgeClass *klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_static_metadata (element_class,
      "V4L2 H.264 Meta Bridge Decoder",
      "Codec/Decoder/Video/Hardware",
      "Wraps v4l2slh264dec; injects GstVideoMeta into the ALLOCATION query so "
      "GstVideoMeta-omitting consumers (WebKit) can negotiate the HW dmabuf",
      "VulkanVideo-RK3588 Project");

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  /* Only register if v4l2slh264dec is actually available, else registering at
   * rank 258 would shadow the working plain decoder (257) with a wrapper that
   * can't make its inner element. Filename scan order favours us here:
   * "libgstv4l2codecs.so" sorts before "libgstv4l2metabridge.so", so the
   * factory is registered by the time this runs -> factory_find is reliable.
   * Disk-stat kept only as a fallback if the lookup is somehow inconclusive. */
  GstElementFactory *f = gst_element_factory_find ("v4l2slh264dec");
  if (f) {
    gst_object_unref (f);
  } else {
    const gchar *sys_path = g_getenv ("GST_PLUGIN_SYSTEM_PATH_1_0");
    if (!sys_path || sys_path[0] == '\0')
      sys_path = "/usr/lib/gstreamer-1.0";
    gchar *probe = g_build_filename (sys_path, "libgstv4l2codecs.so", NULL);
    gboolean found = g_file_test (probe, G_FILE_TEST_EXISTS);
    g_free (probe);
    if (!found) {
      GST_WARNING ("v4l2metabridge: v4l2slh264dec not found — not registering");
      return FALSE;
    }
  }

  /* Rank 258 (PRIMARY+2): beat plain v4l2slh264dec (PRIMARY+1, 257) so
   * decodebin auto-plugs the meta-injecting wrapper. */
  return gst_element_register (plugin, "v4l2h264metabridge",
                               GST_RANK_PRIMARY + 2,
                               GST_TYPE_V4L2METABRIDGE);
}

GST_PLUGIN_DEFINE (
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  v4l2metabridge,
  "V4L2 H.264 meta-bridge decoder (v4l2slh264dec + GstVideoMeta ALLOCATION injection)",
  plugin_init,
  "1.0",
  "LGPL",
  "v4l2metabridge",
  "https://github.com/dongioia/rock5bplus-rkvdec2"
)
