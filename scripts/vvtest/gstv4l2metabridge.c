/*
 * gstv4l2metabridge.c — GStreamer plugin: V4L2 stateless codec meta-bridge
 *
 * Phase-C Step-3 Increment A. Wraps each available v4l2codecs stateless decoder
 * (`v4l2sl{h264,h265,vp8,vp9,mpeg2,av1}dec`) in a GstBin and injects the
 * GstVideoMeta API into the downstream ALLOCATION query, so a consumer that
 * does NOT advertise GstVideoMeta (e.g. WebKitGTK 2.52's GL video sink) can
 * still negotiate the padded hardware dmabuf.
 *
 * Root cause this fixes (board gst log, 2026-06-27):
 *   gstv4l2codec*dec.c decide_allocation:
 *     "DMABuf caps negotiated without the mandatory support of VideoMeta"
 *   -> not-negotiated -> WebKit MediaError ERR4.
 * v4l2codecs mandates that a DMABuf consumer support the GstVideoMeta API
 * (rkvdec buffers are padded: stride != width, coded height != visible). The
 * same meta-aware ALLOCATION probe proven in Phase-C Step-2 (zc_*.c), packaged
 * as autopluggable elements. The gap is codec-agnostic, so one probe serves
 * every stateless codec. Zero-copy preserved: the dmabuf passes through
 * untouched; only the query is amended.
 *
 * A bridge for ALL six stateless codecs is registered whenever the v4l2codecs
 * plugin is present (per-codec gating via gst_element_factory_find is unreliable
 * during the registry scan — see plugin_init). On RK3588 all six v4l2sl*dec
 * decoders are present (verified), so no plain decoder is shadowed by a wrapper
 * that can't make its inner element. Caveats (NOT bridge-specific — they apply
 * equally to using v4l2sl*dec directly): a codec whose v4l2sl decoder is absent,
 * or a device that is busy, fails at link/STREAMON with no guaranteed decodebin
 * fallback. Only H264 and HEVC are end-to-end verified here; VP8/VP9/MPEG2/AV1
 * use the identical mechanism but are untested in-browser.
 *
 * Build on SBC:
 *   gcc -shared -fPIC -O2 -Wall -o libgstv4l2metabridge.so gstv4l2metabridge.c \
 *       $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0) \
 *       -Wl,-soname,libgstv4l2metabridge.so
 * Use:
 *   export GST_PLUGIN_PATH=$HOME/vvtest:$GST_PLUGIN_PATH
 *   gst-inspect-1.0 | grep metabridge
 */
#define PACKAGE "v4l2metabridge"
#define PACKAGE_VERSION "1.2"
#include <gst/gst.h>
#include <gst/video/video.h>

/* Per-codec description, carried as GObject class_data (static -> stable ptr). */
typedef struct {
  const gchar *elem_name;   /* registered element name        */
  const gchar *type_name;   /* GType name                     */
  const gchar *factory;     /* wrapped v4l2codecs decoder     */
  const gchar *long_name;   /* gst-inspect Long-name          */
  const gchar *sink_caps;   /* sink pad template caps string  */
} BridgeDesc;

/* Every v4l2codecs stateless decoder. Only those present on the running
 * hardware are registered (see plugin_init). */
static const BridgeDesc CODECS[] = {
  { "v4l2h264metabridge",  "GstV4l2H264MetaBridge",  "v4l2slh264dec",
    "V4L2 H.264 Meta Bridge Decoder",
    "video/x-h264, stream-format = (string) { byte-stream, avc, avc3 }, "
    "alignment = (string) { au, nal }" },
  { "v4l2h265metabridge",  "GstV4l2H265MetaBridge",  "v4l2slh265dec",
    "V4L2 H.265 Meta Bridge Decoder",
    "video/x-h265, stream-format = (string) { byte-stream, hvc1, hev1 }, "
    "alignment = (string) { au, nal }" },
  { "v4l2vp8metabridge",   "GstV4l2Vp8MetaBridge",   "v4l2slvp8dec",
    "V4L2 VP8 Meta Bridge Decoder",   "video/x-vp8" },
  { "v4l2vp9metabridge",   "GstV4l2Vp9MetaBridge",   "v4l2slvp9dec",
    "V4L2 VP9 Meta Bridge Decoder",   "video/x-vp9, alignment = (string) frame" },
  { "v4l2mpeg2metabridge", "GstV4l2Mpeg2MetaBridge", "v4l2slmpeg2dec",
    "V4L2 MPEG2 Meta Bridge Decoder",
    "video/mpeg, mpegversion = (int) 2, systemstream = (boolean) false, "
    "parsed = (boolean) true" },
  /* v4l2sl{vp9,av1}dec require alignment=frame on their sink; pin it so the
   * parser negotiates frame alignment and the decoder accepts the caps (bare
   * caps let the parser default to tu/obu/super-frame -> "does not accept
   * caps" -> SW fallback). */
  { "v4l2av1metabridge",   "GstV4l2Av1MetaBridge",   "v4l2slav1dec",
    "V4L2 AV1 Meta Bridge Decoder",   "video/x-av1, alignment = (string) frame" },
};
#define N_CODECS (G_N_ELEMENTS (CODECS))

typedef struct {
  GstBin      parent;
  GstElement *decoder;
  GstPad     *sinkpad;
  GstPad     *srcpad;
} GstMetaBridge;

typedef struct {
  GstBinClass       parent_class;
  const BridgeDesc *desc;   /* filled in class_init from class_data */
} GstMetaBridgeClass;

#define META_BRIDGE(obj) ((GstMetaBridge *) (obj))

/* Advertise raw video incl. the DMABuf feature so decodebin links us to the
 * GL/dmabuf consumer (where the zero-copy path lives). */
static const gchar SRC_CAPS[] = "video/x-raw(memory:DMABuf); video/x-raw";

/* Add GstVideoMeta API to the downstream ALLOCATION query (idempotent). The
 * whole point of the element: compensate for a consumer that omits GstVideoMeta,
 * which v4l2codecs requires for DMABuf output. Codec-agnostic. */
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
      GST_WARNING ("v4l2metabridge: ALLOCATION query not writable; "
                   "GstVideoMeta NOT injected (negotiation will fail)");
    }
  }
  /* Non-blocking data probe: OK = continue normally (PASS is for blocking probes). */
  return GST_PAD_PROBE_OK;
}

static void
meta_bridge_instance_init (GTypeInstance *instance, gpointer g_class)
{
  GstMetaBridge      *self = META_BRIDGE (instance);
  GstMetaBridgeClass *klass = (GstMetaBridgeClass *) g_class;
  const BridgeDesc   *d = klass->desc;
  GstPad *pad;

  self->decoder = gst_element_factory_make (d->factory, "dec");
  if (!self->decoder) {
    GST_ERROR_OBJECT (self, "Cannot create %s", d->factory);
    return;
  }
  gst_bin_add (GST_BIN (self), self->decoder);

  /* Ghost sink pad (v4l2sl*dec are GstVideoDecoder: sink/src are static ALWAYS
   * pads, non-NULL right after factory_make). */
  pad = gst_element_get_static_pad (self->decoder, "sink");
  if (!pad) { GST_ERROR_OBJECT (self, "no decoder sink pad"); return; }
  self->sinkpad = gst_ghost_pad_new ("sink", pad);
  gst_pad_set_active (self->sinkpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_object_unref (pad);

  /* Ghost src pad + meta-injecting probe on the decoder's INTERNAL real src pad:
   * the GstVideoDecoder base class issues the ALLOCATION query via
   * gst_pad_peer_query(decoder->srcpad, ...) on this pad (not the ghost), and a
   * QUERY_DOWNSTREAM probe here fires on that outgoing query before
   * decide_allocation reads it back (confirmed via gstpad.c peer_query path). */
  pad = gst_element_get_static_pad (self->decoder, "src");
  if (!pad) { GST_ERROR_OBJECT (self, "no decoder src pad"); return; }
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, meta_probe, NULL, NULL);
  self->srcpad = gst_ghost_pad_new ("src", pad);
  gst_pad_set_active (self->srcpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
  gst_object_unref (pad);
}

static void
meta_bridge_class_init (gpointer g_class, gpointer class_data)
{
  GstElementClass    *ec = GST_ELEMENT_CLASS (g_class);
  GstMetaBridgeClass *bc = (GstMetaBridgeClass *) g_class;
  const BridgeDesc   *d  = (const BridgeDesc *) class_data;
  GstCaps *caps;

  bc->desc = d;

  caps = gst_caps_from_string (d->sink_caps);
  gst_element_class_add_pad_template (ec,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  caps = gst_caps_from_string (SRC_CAPS);
  gst_element_class_add_pad_template (ec,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  gst_element_class_set_static_metadata (ec,
      d->long_name,
      "Codec/Decoder/Video/Hardware",
      "Wraps a v4l2codecs decoder; injects GstVideoMeta into the ALLOCATION "
      "query so GstVideoMeta-omitting consumers (WebKit) can negotiate the HW dmabuf",
      "VulkanVideo-RK3588 Project");
}

static GType
meta_bridge_register_type (const BridgeDesc *desc)
{
  GTypeInfo info = { 0 };
  info.class_size    = sizeof (GstMetaBridgeClass);
  info.class_init    = meta_bridge_class_init;
  info.class_data    = desc;
  info.instance_size = sizeof (GstMetaBridge);
  info.instance_init = meta_bridge_instance_init;
  return g_type_register_static (GST_TYPE_BIN, desc->type_name, &info, 0);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  /* Gate on the v4l2codecs plugin .so being present. gst_element_factory_find()
   * is UNRELIABLE during a registry rebuild — this plugin_init can run before
   * v4l2codecs' features are indexed, and even force-loading the plugin does not
   * make per-codec factory_find succeed mid-scan (both verified 2026-06-27).
   * Disk-stat the .so instead and register the full stateless codec set; on the
   * RK3588 target all six v4l2sl*dec decoders are present, so none of the plain
   * decoders is shadowed by a wrapper that can't make its inner element.
   * (Cross-hardware caveat: on a board missing a given v4l2sl*dec, that bridge
   * would shadow the SW decoder for that codec; revisit with runtime device
   * probing if this is ever run on non-RK3588 hardware.) */
  const gchar *sys_path = g_getenv ("GST_PLUGIN_SYSTEM_PATH_1_0");
  if (!sys_path || sys_path[0] == '\0')
    sys_path = "/usr/lib/gstreamer-1.0";
  gchar *probe = g_build_filename (sys_path, "libgstv4l2codecs.so", NULL);
  gboolean found = g_file_test (probe, G_FILE_TEST_EXISTS);
  g_free (probe);
  if (!found) {
    GST_WARNING ("v4l2metabridge: libgstv4l2codecs.so not in %s — not registering", sys_path);
    return FALSE;
  }

  gboolean ok = TRUE;
  for (guint i = 0; i < N_CODECS; i++) {
    /* Rank 258 (PRIMARY+2): beat the plain decoder (257) so decodebin
     * auto-plugs the meta-injecting wrapper. */
    ok &= gst_element_register (plugin, CODECS[i].elem_name, GST_RANK_PRIMARY + 2,
                                meta_bridge_register_type (&CODECS[i]));
  }
  return ok;
}

GST_PLUGIN_DEFINE (
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  v4l2metabridge,
  "V4L2 stateless codec meta-bridge (GstVideoMeta ALLOCATION injection for all codecs)",
  plugin_init,
  "1.2",
  "LGPL",
  "v4l2metabridge",
  "https://github.com/dongioia/rock5bplus-rkvdec2"
)
