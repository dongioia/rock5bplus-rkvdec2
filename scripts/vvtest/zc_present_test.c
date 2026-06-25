/* Phase-C Step-2 Increment 1: prove the GRAPHICS (fragment-shader) ycbcr path
 * and the padded-resolution import.
 *   sub-gate 2a: import rkvdec NV12 dmabuf -> PanVK -> sample via a fragment
 *     shader (VkSamplerYcbcrConversion) into an OFFSCREEN RGBA color attachment
 *     -> read back -> flat-chroma PSNR vs CPU BT.709 ref. (Stage-1 only proved
 *     the COMPUTE path; a swapchain present uses the graphics path — prove it
 *     off-screen first, isolated from WSI. Swapchain = Increment 2 / sub-gate 2b.)
 *   + meta-aware appsink: a C query-probe adds GST_VIDEO_META_API_TYPE so the
 *     v4l2codecs decoder keeps the PADDED hardware dmabuf at 1080p instead of
 *     copying to system memory (Step-2 Stage-0 finding). Enables 1080p import.
 *
 * Build (SBC):
 *   glslangValidator -V zc_present.vert -o zc_present_vert.spv
 *   glslangValidator -V zc_present.frag -o zc_present_frag.spv
 *   cc zc_present_test.c -o zc_present_test \
 *      $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 \
 *        gstreamer-allocators-1.0 gstreamer-video-1.0) -lvulkan -lm
 * Run:
 *   VK_ICD_FILENAMES=$HOME/mesa-zc/panfrost_icd.json ./zc_present_test <clip> <ref_nv12>
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#define NV12 VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
#define RGBA VK_FORMAT_R8G8B8A8_UNORM

static const char *vkstr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default: return "VK_ERROR_<other>";
    }
}
#define CHECK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FATAL %s:%d %s -> %s\n", __FILE__, __LINE__, #call, vkstr(_r)); \
    exit(3);} } while (0)

/* ---- decoded frame from rkvdec (meta-aware appsink keeps padded hw dmabuf) ---- */
struct frame {
    int fd; uint32_t width, height, n_planes, stride[4]; uint64_t offset[4];
    uint32_t vis_w, vis_h;   /* visible (from caps); width/height are coded (VideoMeta) — may be padded (1080->1088) */
    GstSample *sample;
};

static GstPadProbeReturn meta_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u) {
    (void)pad; (void)u;
    GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);
    /* Advertise GstVideoMeta support so v4l2codecs keeps the padded hardware
     * dmabuf (Step-2 Stage-0: else it copies to packed system memory at 1080p). */
    if (q && GST_QUERY_TYPE(q) == GST_QUERY_ALLOCATION && gst_query_is_writable(q))
        gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, NULL);
    return GST_PAD_PROBE_PASS;
}

static int decode_one(const char *clip, struct frame *f) {
    const char *parser = g_str_has_suffix(clip, ".h265") ? "h265parse" : "h264parse";
    const char *dec    = g_str_has_suffix(clip, ".h265") ? "v4l2slh265dec" : "v4l2slh264dec";
    gchar *desc = g_strdup_printf(
        "filesrc location=%s ! %s ! %s ! appsink name=s emit-signals=false max-buffers=4 drop=false sync=false",
        clip, parser, dec);
    GError *err = NULL;
    GstElement *pipe = gst_parse_launch(desc, &err); g_free(desc);
    if (!pipe) { fprintf(stderr, "FAIL parse_launch: %s\n", err ? err->message : "?"); return 1; }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "s");
    GstPad *sp = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, meta_probe, NULL, NULL);
    gst_object_unref(sp);
    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "FAIL pipeline PLAYING\n"); return 1; }
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
    if (!sample) { fprintf(stderr, "FAIL: no sample in 10s\n"); return 1; }
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem0 = gst_buffer_peek_memory(buf, 0);
    if (!gst_is_dmabuf_memory(mem0)) {
        fprintf(stderr, "FAIL: mem0 not dmabuf (n_memory=%u) — meta-probe not honored?\n",
                gst_buffer_n_memory(buf));
        return 1;
    }
    int fd = gst_dmabuf_memory_get_fd(mem0);
    GstVideoMeta *vm = gst_buffer_get_video_meta(buf);
    if (!vm) { fprintf(stderr, "FAIL: no GstVideoMeta\n"); return 1; }
    memset(f, 0, sizeof(*f));
    f->fd = dup(fd); f->width = vm->width; f->height = vm->height; f->n_planes = vm->n_planes;
    for (guint i = 0; i < vm->n_planes && i < 4; i++) {
        f->stride[i] = (uint32_t)vm->stride[i]; f->offset[i] = (uint64_t)vm->offset[i]; }
    /* VideoMeta width/height are CODED (may be padded, e.g. 1080->1088); the
     * VISIBLE size for the ref/comparison comes from the caps. */
    GstCaps *caps = gst_sample_get_caps(sample);
    GstStructure *st = caps ? gst_caps_get_structure(caps, 0) : NULL;
    int vw = 0, vh = 0;
    if (st) { gst_structure_get_int(st, "width", &vw); gst_structure_get_int(st, "height", &vh); }
    f->vis_w = vw > 0 ? (uint32_t)vw : f->width;
    f->vis_h = vh > 0 ? (uint32_t)vh : f->height;
    f->sample = sample;
    gst_object_unref(sink);
    gst_element_set_state(pipe, GST_STATE_PAUSED);
    printf("decode: coded %ux%u visible %ux%u n_planes=%u fd=%d(dup=%d)\n",
           f->width, f->height, f->vis_w, f->vis_h, f->n_planes, fd, f->fd);
    for (uint32_t i = 0; i < f->n_planes; i++)
        printf("  plane[%u] offset=%lu stride=%u%s\n", i, (unsigned long)f->offset[i], f->stride[i],
               (i == 0 && f->stride[0] != f->width) ? "  (PADDED stride != width)" : "");
    return 0;
}

/* ---- Vulkan (PanVK) ---- */
struct vk {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; uint32_t qfam; VkQueue q; VkCommandPool pool;
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
};
static int has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

static void vk_init(struct vk *v) {
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const char *layers[1]; uint32_t nlayers = 0;
    if (!getenv("ZC_NOVALIDATE")) {
        uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, NULL);
        VkLayerProperties lp[64]; if (lc > 64) lc = 64; vkEnumerateInstanceLayerProperties(&lc, lp);
        for (uint32_t i = 0; i < lc; i++)
            if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) layers[nlayers++] = lp[i].layerName;
    }
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
                                .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers};
    CHECK(vkCreateInstance(&ici, NULL, &v->inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(v->inst, &n, NULL);
    if (!n) { fprintf(stderr, "FAIL: 0 physical devices\n"); exit(2); }
    VkPhysicalDevice pds[8]; if (n > 8) n = 8; vkEnumeratePhysicalDevices(v->inst, &n, pds); v->pd = pds[0];
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(v->pd, &p);
    printf("vk: '%s' api=%u.%u.%u%s\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
           VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), nlayers ? " (+validation)" : "");
    uint32_t ec = 0; vkEnumerateDeviceExtensionProperties(v->pd, NULL, &ec, NULL);
    VkExtensionProperties *ext = calloc(ec, sizeof(*ext)); vkEnumerateDeviceExtensionProperties(v->pd, NULL, &ec, ext);
    const char *need[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                          VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME};
    for (size_t i = 0; i < sizeof(need)/sizeof(*need); i++)
        if (!has_ext(ext, ec, need[i])) { fprintf(stderr, "FAIL: missing %s\n", need[i]); exit(2); }
    free(ext);
    uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &qc, NULL);
    VkQueueFamilyProperties qp[16]; if (qc > 16) qc = 16; vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &qc, qp);
    v->qfam = 0; for (uint32_t i = 0; i < qc; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v->qfam = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = v->qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, .samplerYcbcrConversion = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &ycf,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = sizeof(need)/sizeof(*need), .ppEnabledExtensionNames = need};
    CHECK(vkCreateDevice(v->pd, &dci, NULL, &v->dev));
    vkGetDeviceQueue(v->dev, v->qfam, 0, &v->q);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = v->qfam};
    CHECK(vkCreateCommandPool(v->dev, &pci, NULL, &v->pool));
    v->GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(v->dev, "vkGetMemoryFdPropertiesKHR");
    if (!v->GetMemoryFdPropertiesKHR) { fprintf(stderr, "FAIL: no vkGetMemoryFdPropertiesKHR\n"); exit(2); }
}

static uint32_t panvk_linear_plane_count(struct vk *v) {
    VkDrmFormatModifierPropertiesListEXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
    vkGetPhysicalDeviceFormatProperties2(v->pd, NV12, &fp);
    uint32_t mc = ml.drmFormatModifierCount; VkDrmFormatModifierPropertiesEXT mods[64]; if (mc > 64) mc = 64;
    ml.pDrmFormatModifierProperties = mods; ml.drmFormatModifierCount = mc;
    vkGetPhysicalDeviceFormatProperties2(v->pd, NV12, &fp);
    for (uint32_t i = 0; i < mc; i++) if (mods[i].drmFormatModifier == 0) return mods[i].drmFormatModifierPlaneCount;
    fprintf(stderr, "FAIL: PanVK does not advertise NV12 LINEAR\n"); exit(2);
}
static uint32_t pick_memtype(struct vk *v, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (bits & (1u << i)) return i;
    fprintf(stderr, "FAIL: no memtype 0x%x\n", bits); exit(3);
}
static uint32_t prop_memtype(struct vk *v, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "FAIL: no memtype bits=0x%x want=0x%x\n", bits, want); exit(3);
}

/* import the dmabuf into a PanVK VkImage (LINEAR, explicit per-plane layout) */
static VkImage import_image(struct vk *v, struct frame *f, uint32_t mod_planes, VkDeviceMemory *out_mem) {
    /* drmFormatModifierPlaneCount = format plane count (2): PanVK indexes
     * pPlaneLayouts by image->plane_count and the validation safe-copy truncates
     * to the declared count, so it must be 2 (see STAGE1a). */
    VkSubresourceLayout pl[2]; memset(pl, 0, sizeof(pl));
    pl[0].offset = f->offset[0]; pl[0].rowPitch = f->stride[0];
    pl[1].offset = f->offset[1]; pl[1].rowPitch = f->stride[1];
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = 0, .drmFormatModifierPlaneCount = f->n_planes, .pPlaneLayouts = pl};
    (void)mod_planes;
    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi,
        .imageType = VK_IMAGE_TYPE_2D, .format = NV12, .extent = {f->width, f->height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; VkResult r = vkCreateImage(v->dev, &ici, NULL, &img);
    if (r == VK_ERROR_INITIALIZATION_FAILED) {
        fprintf(stderr, "STRICT_IMPORT rejected (pl[0]={off=%lu,pitch=%lu} pl[1]={off=%lu,pitch=%lu})\n",
                (unsigned long)pl[0].offset,(unsigned long)pl[0].rowPitch,
                (unsigned long)pl[1].offset,(unsigned long)pl[1].rowPitch);
        close(f->fd); exit(4);
    }
    CHECK(r);
    VkMemoryFdPropertiesKHR mfp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    CHECK(v->GetMemoryFdPropertiesKHR(v->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, f->fd, &mfp));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(v->dev, img, &mr);
    uint32_t bits = mr.memoryTypeBits & mfp.memoryTypeBits;
    if (!bits) { fprintf(stderr, "FAIL: no shared memtype\n"); close(f->fd); exit(3); }
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = f->fd};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
        .allocationSize = mr.size, .memoryTypeIndex = pick_memtype(v, bits)};
    CHECK(vkAllocateMemory(v->dev, &mai, NULL, out_mem));
    CHECK(vkBindImageMemory(v->dev, img, *out_mem, 0));
    printf("vk: imported dmabuf -> VkImage (memReq.size=%lu)\n", (unsigned long)mr.size);
    return img;
}

static VkCommandBuffer begin_cmd(struct vk *v) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(v->dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static void end_cmd(struct vk *v, VkCommandBuffer cb) {
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CHECK(vkCreateFence(v->dev, &fci, NULL, &fence));
    CHECK(vkQueueSubmit(v->q, 1, &si, fence));
    CHECK(vkWaitForFences(v->dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(v->dev, fence, NULL); vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
}

static uint32_t *load_spv(const char *path, size_t *sz) {
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "FAIL: open %s\n", path); exit(3); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || (n & 3)) { fprintf(stderr, "FAIL: bad spv %s\n", path); exit(3); }
    uint32_t *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "FAIL read %s\n", path); exit(3); }
    fclose(f); *sz = (size_t)n; return b;
}

/* CPU NV12 -> packed RGBA8, BT.709 limited, plain nearest chroma (flat-region gate). */
static void cpu_nv12_to_rgba(const uint8_t *nv12, uint32_t W, uint32_t H, uint32_t *out) {
    const uint8_t *Y = nv12, *UV = nv12 + (size_t)W * H;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        double yv = ((double)Y[(size_t)y*W+x]-16.0)/219.0;
        size_t ci = (size_t)(y/2)*W + (x/2)*2;
        double cb = ((double)UV[ci]-128.0)/224.0, cr = ((double)UV[ci+1]-128.0)/224.0;
        double r = yv+1.5748*cr, g = yv-0.1873*cb-0.4681*cr, b = yv+1.8556*cb;
#define C8(t) ((uint32_t)((t)<0?0:((t)>1?255:lround((t)*255.0))))
        out[(size_t)y*W+x] = C8(r) | (C8(g)<<8) | (C8(b)<<16) | (255u<<24);
#undef C8
    }
}
static int chroma_flat(const uint8_t *UV, uint32_t W, uint32_t cw, uint32_t ch, uint32_t cx, uint32_t cy) {
    int cbmin=255,cbmax=0,crmin=255,crmax=0;
    for (int dy=-1; dy<=1; dy++) for (int dx=-1; dx<=1; dx++) {
        int yy=(int)cy+dy, xx=(int)cx+dx;
        if (yy<0) yy=0; if (yy>(int)ch-1) yy=ch-1; if (xx<0) xx=0; if (xx>(int)cw-1) xx=cw-1;
        size_t o=(size_t)yy*W+(size_t)xx*2; int cb=UV[o],cr=UV[o+1];
        if(cb<cbmin)cbmin=cb; if(cb>cbmax)cbmax=cb; if(cr<crmin)crmin=cr; if(cr>crmax)crmax=cr;
    }
    return (cbmax-cbmin)<=1 && (crmax-crmin)<=1;
}

/* sub-gate 2a: fragment-shader ycbcr -> offscreen RGBA -> flat-chroma PSNR */
static int gate_2a(struct vk *v, VkImage img, struct frame *f, const char *ref_path) {
    uint32_t W = f->width, H = f->height;

    /* ycbcr conversion + immutable sampler + view (same params as Stage-1b) */
    VkSamplerYcbcrConversionCreateInfo cci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = NV12, .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY},
        .xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN, .yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN,
        .chromaFilter = VK_FILTER_NEAREST};
    VkSamplerYcbcrConversion conv; CHECK(vkCreateSamplerYcbcrConversion(v->dev, &cci, NULL, &conv));
    VkSamplerYcbcrConversionInfo cinfo = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = conv};
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = &cinfo,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .unnormalizedCoordinates = VK_FALSE};
    VkSampler samp; CHECK(vkCreateSampler(v->dev, &sci, NULL, &samp));
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &cinfo,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = NV12,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView yview; CHECK(vkCreateImageView(v->dev, &vci, NULL, &yview));

    /* offscreen RGBA color target */
    VkImageCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = RGBA, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage rt; CHECK(vkCreateImage(v->dev, &rci, NULL, &rt));
    VkMemoryRequirements rmr; vkGetImageMemoryRequirements(v->dev, rt, &rmr);
    VkMemoryAllocateInfo rmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rmr.size,
        .memoryTypeIndex = prop_memtype(v, rmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory rmem; CHECK(vkAllocateMemory(v->dev, &rmai, NULL, &rmem));
    CHECK(vkBindImageMemory(v->dev, rt, rmem, 0));
    VkImageViewCreateInfo rvci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = RGBA, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView rtview; CHECK(vkCreateImageView(v->dev, &rvci, NULL, &rtview));

    /* render pass: 1 color attachment, clear -> store, final TRANSFER_SRC */
    VkAttachmentDescription att = {.format = RGBA, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ar = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ar};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
    VkRenderPass rp; CHECK(vkCreateRenderPass(v->dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &rtview, .width = W, .height = H, .layers = 1};
    VkFramebuffer fb; CHECK(vkCreateFramebuffer(v->dev, &fbci, NULL, &fb));

    /* descriptor set layout: combined image sampler (immutable ycbcr) in FRAGMENT */
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .pImmutableSamplers = &samp};
    VkDescriptorSetLayoutCreateInfo dlci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(v->dev, &dlci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout pll; CHECK(vkCreatePipelineLayout(v->dev, &plci, NULL, &pll));

    /* shaders */
    size_t vsz, fsz; uint32_t *vspv = load_spv("zc_present_vert.spv", &vsz), *fspv = load_spv("zc_present_frag.spv", &fsz);
    VkShaderModuleCreateInfo vmci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsz, .pCode = vspv};
    VkShaderModuleCreateInfo fmci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fsz, .pCode = fspv};
    VkShaderModule vm, fm; CHECK(vkCreateShaderModule(v->dev, &vmci, NULL, &vm)); CHECK(vkCreateShaderModule(v->dev, &fmci, NULL, &fm));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba};
    VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .layout = pll, .renderPass = rp, .subpass = 0};
    VkPipeline pipe; CHECK(vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    /* descriptor set */
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(v->dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(v->dev, &dsai, &ds));
    VkDescriptorImageInfo dii = {.sampler = samp, .imageView = yview, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
    vkUpdateDescriptorSets(v->dev, 1, &wds, 0, NULL);

    /* staging readback buffer */
    VkDeviceSize obytes = (VkDeviceSize)W * H * 4;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = obytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer stg; CHECK(vkCreateBuffer(v->dev, &bci, NULL, &stg));
    VkMemoryRequirements smr; vkGetBufferMemoryRequirements(v->dev, stg, &smr);
    VkMemoryAllocateInfo smai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = smr.size,
        .memoryTypeIndex = prop_memtype(v, smr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory smem; CHECK(vkAllocateMemory(v->dev, &smai, NULL, &smem)); CHECK(vkBindBufferMemory(v->dev, stg, smem, 0));

    /* record: acquire import -> SHADER_READ; render pass draw; copy RT -> staging */
    VkCommandBuffer cmd = begin_cmd(v);
    VkImageMemoryBarrier acq = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &acq);
    VkClearValue clear = {.color = {{0, 0, 0, 1}}};
    VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
        .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pll, 0, 1, &ds, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);  /* RT now in TRANSFER_SRC_OPTIMAL (renderpass final layout) */
    /* PanVK CSF does NOT auto-sync the fragment STORE against the downstream copy
     * (verified: CmdEndRendering emits no consumer wait). Without this barrier the
     * copy races the tiler -> partially-stored tiles (alpha=0 / garbage). dst
     * stage covers both TRANSFER and COMPUTE because PanVK's copyImageToBuffer is
     * a compute meta-dispatch. */
    VkImageMemoryBarrier rt2copy = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = rt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &rt2copy);
    VkBufferImageCopy cpy = {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg, 1, &cpy);
    end_cmd(v, cmd);

    /* compare flat-chroma PSNR vs CPU ref (same gate as Stage-1b) */
    /* Compare only the VISIBLE region (ref is visible) against the rendered RT
     * (rows stride = coded W; the bottom coded-padding rows are not compared). */
    uint32_t VW = f->vis_w, VH = f->vis_h;
    void *map; CHECK(vkMapMemory(v->dev, smem, 0, obytes, 0, &map));
    VkDeviceSize nv12sz = (VkDeviceSize)VW*VH + (VkDeviceSize)VW*(VH/2);
    FILE *rf = fopen(ref_path, "rb"); if (!rf) { fprintf(stderr, "FAIL open ref %s\n", ref_path); return 3; }
    uint8_t *nv12 = malloc(nv12sz); if (fread(nv12, 1, nv12sz, rf) != nv12sz) { fprintf(stderr, "FAIL ref read (want %lu)\n", (unsigned long)nv12sz); fclose(rf); return 3; }
    fclose(rf);
    uint32_t *cpu = malloc((size_t)VW*VH*4); cpu_nv12_to_rgba(nv12, VW, VH, cpu);
    const uint8_t *hb = map, *cbb = (const uint8_t *)cpu, *UV = nv12 + (size_t)VW*VH;
    uint32_t cw = VW/2, ch = VH/2; double fmse = 0; int fmaxd = 0; size_t fcount = 0, edge_bad = 0;
    int fseen[256] = {0}, fdistinct = 0;
    for (uint32_t y = 0; y < VH; y++) for (uint32_t x = 0; x < VW; x++) {
        int flat = chroma_flat(UV, VW, cw, ch, x/2, y/2);
        size_t hp = ((size_t)y*W + x)*4;    /* rendered: coded-width stride */
        size_t cp = ((size_t)y*VW + x)*4;   /* cpu ref: visible-width stride */
        int pmax = 0;
        for (int k = 0; k < 4; k++) { int d = (int)hb[hp+k]-(int)cbb[cp+k]; if (d<0) d=-d; if (d>pmax) pmax=d; if (flat) fmse += (double)d*d; }
        if (flat) { fcount++; if (pmax>fmaxd) fmaxd=pmax; if (!fseen[hb[hp]]) { fseen[hb[hp]]=1; fdistinct++; } }
        else if (pmax>32) edge_bad++;
    }
    double favg = fcount ? fmse/((double)fcount*4) : 0;
    double fpsnr = (fcount==0)?0:(favg==0?1e9:10.0*log10(65025.0/favg));
    int flat_big = fcount >= (size_t)VW*VH/2;
    int pass = fpsnr >= 40.0 && flat_big && fdistinct >= 16;
    printf("\n=== SUB-GATE 2a (graphics fragment ycbcr -> offscreen RGBA) ===\n");
    printf("  visible %ux%u (coded %ux%u)  flat-chroma=%.1f%% (>=50%%:%d)  flat-distinct=%d  FLAT PSNR=%.2f dB (max|d|=%d)  edge>32=%zu\n",
           VW, VH, W, H, 100.0*(double)fcount/((double)VW*VH), flat_big, fdistinct, fpsnr, fmaxd, edge_bad);
    printf("  RESULT: %s (graphics-path CSC correct; edges = HW reconstruction)\n", pass ? "PASS" : "FAIL");
    printf("  px[0,0] hw=%d,%d,%d cpu=%d,%d,%d ; px[c] hw=%d,%d,%d cpu=%d,%d,%d\n",
           hb[0],hb[1],hb[2], cbb[0],cbb[1],cbb[2],
           hb[(((size_t)(VH/2)*W)+(VW/2))*4+0],hb[(((size_t)(VH/2)*W)+(VW/2))*4+1],hb[(((size_t)(VH/2)*W)+(VW/2))*4+2],
           cbb[(((size_t)(VH/2)*VW)+(VW/2))*4+0],cbb[(((size_t)(VH/2)*VW)+(VW/2))*4+1],cbb[(((size_t)(VH/2)*VW)+(VW/2))*4+2]);
    if (getenv("ZC_DUMP")) { FILE *d = fopen("present_rgba.bin","wb"); if (d) { fwrite(map,1,obytes,d); fclose(d); printf("  dumped present_rgba.bin\n"); } }
    free(vspv); free(fspv); free(nv12); free(cpu);
    vkUnmapMemory(v->dev, smem);
    return pass ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <clip> <ref_nv12>\n", argv[0]); return 2; }
    gst_init(&argc, &argv);
    struct frame f; if (decode_one(argv[1], &f)) return 1;
    struct vk v; vk_init(&v);
    uint32_t mp = panvk_linear_plane_count(&v);
    VkDeviceMemory mem; VkImage img = import_image(&v, &f, mp, &mem);
    return gate_2a(&v, img, &f, argv[2]);
}
