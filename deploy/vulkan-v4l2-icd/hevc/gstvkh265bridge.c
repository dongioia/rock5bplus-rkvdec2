/*
 * gstvkh265bridge.c — GStreamer plugin: VulkanH265Bridge
 *
 * Wraps vulkanh265dec ! vulkandownload as a single GstBin element.
 * Exposes video/x-h265 sink + video/x-raw NV12 src so decodebin
 * can auto-plug it without seeing VulkanImage memory caps.
 *
 * Build on SBC:
 *   gcc -shared -fPIC -O2 -o libgstvkh265bridge.so gstvkh265bridge.c \
 *       $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0) \
 *       -Wl,-soname,libgstvkh265bridge.so
 *
 * Use:
 *   export GST_PLUGIN_PATH=$HOME/vvtest:$GST_PLUGIN_PATH
 *   gst-inspect-1.0 vkh265bridge
 */
#define PACKAGE "vkh265bridge"
#define PACKAGE_VERSION "1.0"
#include <gst/gst.h>

#define GST_TYPE_VKH265BRIDGE        (gst_vkh265bridge_get_type())
#define GST_VKH265BRIDGE(obj)        (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VKH265BRIDGE, GstVkH265Bridge))

typedef struct _GstVkH265Bridge       GstVkH265Bridge;
typedef struct _GstVkH265BridgeClass  GstVkH265BridgeClass;

struct _GstVkH265Bridge {
  GstBin  parent;
  GstElement *decoder;   /* vulkanh265dec */
  GstElement *download;  /* vulkandownload */
  GstPad     *sinkpad;   /* ghost pad on decoder sink */
  GstPad     *srcpad;    /* ghost pad on download src */
};

struct _GstVkH265BridgeClass {
  GstBinClass parent_class;
};

GType gst_vkh265bridge_get_type (void);
G_DEFINE_TYPE (GstVkH265Bridge, gst_vkh265bridge, GST_TYPE_BIN)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE (
  "sink",
  GST_PAD_SINK,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS ("video/x-h265, "
                   "stream-format = (string) { hvc1, hev1, byte-stream }, "
                   "alignment = (string) { au, nal }")
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE (
  "src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS ("video/x-raw, format = (string) NV12")
);

static void
gst_vkh265bridge_init (GstVkH265Bridge *self)
{
  GstPad *pad;

  self->decoder  = gst_element_factory_make ("vulkanh265dec",   "vkdec");
  self->download = gst_element_factory_make ("vulkandownload",  "vkdl");

  if (!self->decoder || !self->download) {
    GST_ERROR_OBJECT (self, "Cannot create vulkanh265dec or vulkandownload");
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
gst_vkh265bridge_class_init (GstVkH265BridgeClass *klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_static_metadata (element_class,
      "Vulkan H.265 Bridge Decoder",
      "Codec/Decoder/Video",
      "Wraps vulkanh265dec + vulkandownload; exposes video/x-raw NV12 src",
      "VulkanVideo-RK3588 Project");

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  /* Guard: only register if the Vulkan plugin library is present.
   * gst_element_factory_find() cannot be used here (called during
   * registry scan before other plugins are indexed).  Instead, probe
   * for libgstvulkan.so in the GStreamer system plugin path — if it is
   * absent, vulkanh265dec and vulkandownload will not exist and
   * registering vkh265bridge at rank 258 would shadow working decoders.
   */
  const gchar *sys_path = g_getenv ("GST_PLUGIN_SYSTEM_PATH_1_0");
  if (!sys_path || sys_path[0] == '\0')
    sys_path = "/usr/lib/gstreamer-1.0";

  {
    gchar *probe = g_build_filename (sys_path, "libgstvulkan.so", NULL);
    gboolean found = g_file_test (probe, G_FILE_TEST_EXISTS);
    g_free (probe);
    if (!found) {
      GST_WARNING ("vkh265bridge: libgstvulkan.so not found in %s — not registering", sys_path);
      return FALSE;
    }
  }

  return gst_element_register (plugin, "vkh265bridge",
                               GST_RANK_PRIMARY + 2,   /* 258 */
                               GST_TYPE_VKH265BRIDGE);
}

GST_PLUGIN_DEFINE (
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  vkh265bridge,
  "Vulkan H.265 bridge decoder (vulkanh265dec+vulkandownload bin)",
  plugin_init,
  "1.0",
  "LGPL",
  "vkh265bridge",
  "https://github.com/dongioia/rock5bplus-rkvdec2"
)
