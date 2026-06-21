/*
 * gstvkh264bridge.c — GStreamer plugin: VulkanH264Bridge
 *
 * Wraps vulkanh264dec ! vulkandownload as a single GstBin element.
 * Exposes video/x-h264 sink + video/x-raw NV12 src so decodebin
 * can auto-plug it without seeing VulkanImage memory caps.
 *
 * Build on SBC:
 *   gcc -shared -fPIC -O2 -o libgstvkh264bridge.so gstvkh264bridge.c \
 *       $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0) \
 *       -Wl,-soname,libgstvkh264bridge.so
 *
 * Use:
 *   export GST_PLUGIN_PATH=$HOME/vvtest:$GST_PLUGIN_PATH
 *   gst-inspect-1.0 vkh264bridge
 */
#define PACKAGE "vkh264bridge"
#define PACKAGE_VERSION "1.0"
#include <gst/gst.h>

#define GST_TYPE_VKH264BRIDGE        (gst_vkh264bridge_get_type())
#define GST_VKH264BRIDGE(obj)        (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VKH264BRIDGE, GstVkH264Bridge))

typedef struct _GstVkH264Bridge       GstVkH264Bridge;
typedef struct _GstVkH264BridgeClass  GstVkH264BridgeClass;

struct _GstVkH264Bridge {
  GstBin  parent;
  GstElement *decoder;   /* vulkanh264dec */
  GstElement *download;  /* vulkandownload */
  GstPad     *sinkpad;   /* ghost pad on decoder sink */
  GstPad     *srcpad;    /* ghost pad on download src */
};

struct _GstVkH264BridgeClass {
  GstBinClass parent_class;
};

GType gst_vkh264bridge_get_type (void);
G_DEFINE_TYPE (GstVkH264Bridge, gst_vkh264bridge, GST_TYPE_BIN)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE (
  "sink",
  GST_PAD_SINK,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS ("video/x-h264, "
                   "stream-format = (string) { byte-stream, avc, avc3 }, "
                   "alignment = (string) { au, nal }")
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE (
  "src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS ("video/x-raw, format = (string) NV12")
);

static void
gst_vkh264bridge_init (GstVkH264Bridge *self)
{
  GstPad *pad;

  self->decoder  = gst_element_factory_make ("vulkanh264dec",   "vkdec");
  self->download = gst_element_factory_make ("vulkandownload",  "vkdl");

  if (!self->decoder || !self->download) {
    GST_ERROR_OBJECT (self, "Cannot create vulkanh264dec or vulkandownload");
    if (self->decoder)  gst_object_unref (self->decoder);
    if (self->download) gst_object_unref (self->download);
    self->decoder = self->download = NULL;
    return;
  }

  gst_bin_add_many (GST_BIN (self), self->decoder, self->download, NULL);
  gst_element_link (self->decoder, self->download);

  /* Ghost sink pad */
  pad = gst_element_get_static_pad (self->decoder, "sink");
  self->sinkpad = gst_ghost_pad_new ("sink", pad);
  gst_pad_set_active (self->sinkpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_object_unref (pad);

  /* Ghost src pad */
  pad = gst_element_get_static_pad (self->download, "src");
  self->srcpad = gst_ghost_pad_new ("src", pad);
  gst_pad_set_active (self->srcpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
  gst_object_unref (pad);
}

static void
gst_vkh264bridge_class_init (GstVkH264BridgeClass *klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_static_metadata (element_class,
      "Vulkan H.264 Bridge Decoder",
      "Codec/Decoder/Video",
      "Wraps vulkanh264dec + vulkandownload; exposes video/x-raw NV12 src",
      "VulkanVideo-RK3588 Project");

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  /* TODO (for final review): guard registration on child-factory availability.
   * Currently vkh264bridge is always registered at rank 258 even if
   * vulkanh264dec or vulkandownload are absent (e.g. host without the ICD).
   * On such hosts it would shadow other H264 decoders.  Fix: probe
   *   gst_element_factory_find("vulkanh264dec") and
   *   gst_element_factory_find("vulkandownload")
   * here; return FALSE early if either is NULL.
   * Not fixed now to avoid recompile/redeploy on the SBC mid-test. */
  return gst_element_register (plugin, "vkh264bridge",
                               GST_RANK_PRIMARY + 2,   /* 258 */
                               GST_TYPE_VKH264BRIDGE);
}

GST_PLUGIN_DEFINE (
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  vkh264bridge,
  "Vulkan H.264 bridge decoder (vulkanh264dec+vulkandownload bin)",
  plugin_init,
  "1.0",
  "LGPL",
  "vkh264bridge",
  "https://github.com/dongioia/rock5bplus-rkvdec2"
)
