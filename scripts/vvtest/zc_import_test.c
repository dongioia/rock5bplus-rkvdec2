/* Phase-C Stage-1: import a REAL rkvdec NV12 DMA-BUF into PanVK (!42353) and
 * validate the imported pixels. Two gates, both on the SAME import:
 *   1a  per-plane copy (vkCmdCopyImageToBuffer, PLANE_0/PLANE_1 aspects) ->
 *       byte-exact vs an INDEPENDENT canonical-linear NV12 (ffmpeg, not the
 *       GstVideoMeta this program reads). Proves import geometry / offsets.
 *   1b  HW-YUV sample (VkSamplerYcbcrConversion) -> RGBA -> PSNR vs CPU ref.
 *       The path zero-copy actually uses.  [added in v2; v1 = import + 1a]
 *
 * Same process: the rkvdec CAPTURE buffer surfaced by `v4l2slh26Xdec ! appsink`
 * is already dmabuf-backed (Stage-0a), so its fd is valid here -> no SCM_RIGHTS.
 *
 * Build (on the SBC):
 *   cc zc_import_test.c -o zc_import_test \
 *      $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 \
 *        gstreamer-allocators-1.0 gstreamer-video-1.0) -lvulkan -lm
 * Run (pinned !42353 PanVK, not system mesa):
 *   VK_ICD_FILENAMES=$HOME/mesa-zc/panfrost_icd.json \
 *     ./zc_import_test case1.h264 ref_case1_f0.nv12
 *   ZC_NOVALIDATE=1 ...   # skip the Khronos validation layer (mimics the
 *                         # production no-layer pipeline; see import_image note)
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

/* ---- decoded frame surfaced from rkvdec via GStreamer (same process) ---- */
struct frame {
    int      fd;          /* dup'd dmabuf fd, handed to Vulkan (Vulkan owns it) */
    uint32_t width, height;
    uint32_t n_planes;
    uint32_t stride[4];
    uint64_t offset[4];
    GstSample *sample;    /* kept alive until after import */
};

static int decode_one(const char *clip, struct frame *f) {
    const char *parser = g_str_has_suffix(clip, ".h265") ? "h265parse" : "h264parse";
    const char *dec    = g_str_has_suffix(clip, ".h265") ? "v4l2slh265dec" : "v4l2slh264dec";
    gchar *desc = g_strdup_printf(
        "filesrc location=%s ! %s ! %s ! "
        "appsink name=s emit-signals=false max-buffers=4 drop=false sync=false",
        clip, parser, dec);
    GError *err = NULL;
    GstElement *pipe = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!pipe) { fprintf(stderr, "FAIL parse_launch: %s\n", err ? err->message : "?"); return 1; }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "s");
    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "FAIL pipeline PLAYING\n"); return 1;
    }
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
    if (!sample) { fprintf(stderr, "FAIL: no sample in 10s\n"); gst_element_set_state(pipe, GST_STATE_NULL); return 1; }

    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMemory *mem0 = gst_buffer_peek_memory(buf, 0);
    if (!gst_is_dmabuf_memory(mem0)) {
        fprintf(stderr, "FAIL: mem0 not dmabuf (n_memory=%u)\n", gst_buffer_n_memory(buf));
        return 1;
    }
    int fd = gst_dmabuf_memory_get_fd(mem0);
    GstVideoMeta *vm = gst_buffer_get_video_meta(buf);
    if (!vm) { fprintf(stderr, "FAIL: no GstVideoMeta\n"); return 1; }

    memset(f, 0, sizeof(*f));
    f->fd       = dup(fd);          /* Vulkan import takes ownership of this dup */
    f->width    = vm->width;
    f->height   = vm->height;
    f->n_planes = vm->n_planes;
    for (guint i = 0; i < vm->n_planes && i < 4; i++) {
        f->stride[i] = (uint32_t)vm->stride[i];
        f->offset[i] = (uint64_t)vm->offset[i];
    }
    f->sample = sample;             /* hold the ref: keeps the orig fd/buffer valid */
    gst_object_unref(sink);
    gst_element_set_state(pipe, GST_STATE_PAUSED); /* keep buffer alive; full NULL at exit */
    /* leak `pipe` deliberately for the short run; sample ref pins the memory */

    printf("decode: %ux%u n_planes=%u  fd=%d(dup=%d)\n",
           f->width, f->height, f->n_planes, fd, f->fd);
    for (uint32_t i = 0; i < f->n_planes; i++)
        printf("  plane[%u] offset=%lu stride=%u\n", i,
               (unsigned long)f->offset[i], f->stride[i]);
    return 0;
}

/* ---- Vulkan: pick PanVK, enable import extensions ---- */
struct vk {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    uint32_t qfam;
    VkQueue q;
    VkCommandPool pool;
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
};

static int has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void vk_init(struct vk *v) {
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_3};
    /* enable the validation layer if present (debug import/view VUs), unless
     * ZC_NOVALIDATE is set. Note: with the layer ON, count=2 (see import_image)
     * trips VUID-...-02265; the no-layer path mimics the production pipeline. */
    const char *layers[1]; uint32_t nlayers = 0;
    if (!getenv("ZC_NOVALIDATE")) {
        uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, NULL);
        VkLayerProperties lp[64]; if (lc > 64) lc = 64;
        vkEnumerateInstanceLayerProperties(&lc, lp);
        for (uint32_t i = 0; i < lc; i++)
            if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) { layers[nlayers++] = lp[i].layerName; }
    }
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &ai,
                                .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers};
    CHECK(vkCreateInstance(&ici, NULL, &v->inst));
    printf("vk: instance up%s\n", nlayers ? " (+validation)" : "");

    uint32_t n = 0; vkEnumeratePhysicalDevices(v->inst, &n, NULL);
    if (!n) { fprintf(stderr, "FAIL: 0 physical devices (VK_ICD_FILENAMES set?)\n"); exit(2); }
    VkPhysicalDevice pds[8]; if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(v->inst, &n, pds);
    v->pd = pds[0];                 /* only our ICD is loaded -> [0] is PanVK */
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(v->pd, &p);
    printf("vk: device '%s' driverVersion=0x%x api=%u.%u.%u\n", p.deviceName, p.driverVersion,
           VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));

    uint32_t ec = 0; vkEnumerateDeviceExtensionProperties(v->pd, NULL, &ec, NULL);
    VkExtensionProperties *ext = calloc(ec, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(v->pd, NULL, &ec, ext);
    const char *need[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,  /* 1b HW-YUV sample (core 1.1; PanVK lists it) */
    };
    for (size_t i = 0; i < sizeof(need)/sizeof(*need); i++)
        if (!has_ext(ext, ec, need[i])) { fprintf(stderr, "FAIL: missing %s\n", need[i]); exit(2); }
    free(ext);

    /* queue family with graphics (covers transfer for 1a; render for 1b) */
    uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &qc, NULL);
    VkQueueFamilyProperties qp[16]; if (qc > 16) qc = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &qc, qp);
    v->qfam = 0;
    for (uint32_t i = 0; i < qc; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v->qfam = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = v->qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        .samplerYcbcrConversion = VK_TRUE};   /* required before creating a ycbcr conversion (1b) */
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = &ycf,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = sizeof(need)/sizeof(*need),
                              .ppEnabledExtensionNames = need};
    CHECK(vkCreateDevice(v->pd, &dci, NULL, &v->dev));
    vkGetDeviceQueue(v->dev, v->qfam, 0, &v->q);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = v->qfam};
    CHECK(vkCreateCommandPool(v->dev, &pci, NULL, &v->pool));
    v->GetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(v->dev, "vkGetMemoryFdPropertiesKHR");
    if (!v->GetMemoryFdPropertiesKHR) { fprintf(stderr, "FAIL: no vkGetMemoryFdPropertiesKHR\n"); exit(2); }
}

/* Query PanVK's plane count + tiling features for NV12 + LINEAR modifier(0). */
static uint32_t panvk_linear_plane_count(struct vk *v) {
    VkDrmFormatModifierPropertiesListEXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
    vkGetPhysicalDeviceFormatProperties2(v->pd, NV12, &fp);
    uint32_t mc = ml.drmFormatModifierCount;
    VkDrmFormatModifierPropertiesEXT mods[64]; if (mc > 64) mc = 64;
    ml.pDrmFormatModifierProperties = mods; ml.drmFormatModifierCount = mc;
    vkGetPhysicalDeviceFormatProperties2(v->pd, NV12, &fp);
    for (uint32_t i = 0; i < mc; i++)
        if (mods[i].drmFormatModifier == 0) {
            printf("vk: NV12 LINEAR modifier planes=%u feats=0x%08x\n",
                   mods[i].drmFormatModifierPlaneCount, mods[i].drmFormatModifierTilingFeatures);
            return mods[i].drmFormatModifierPlaneCount;
        }
    fprintf(stderr, "FAIL: PanVK does not advertise NV12 LINEAR\n"); exit(2);
}

static uint32_t pick_memtype(struct vk *v, uint32_t typebits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (typebits & (1u << i)) return i;
    fprintf(stderr, "FAIL: no memory type for bits 0x%x\n", typebits); exit(3);
}

/* import the dmabuf fd into a VkImage (LINEAR DRM-modifier, single bind). */
static VkImage import_image(struct vk *v, struct frame *f, uint32_t mod_planes, VkDeviceMemory *out_mem) {
    /* Explicit per-plane layout from the rkvdec GstVideoMeta.
     *
     * PanVK BUG (!42353, pinned e5ec9502) — advertise/consume mismatch:
     *   - The LINEAR modifier advertises drmFormatModifierPlaneCount=1 for NV12
     *     (one memory plane). Per the Vulkan spec a conformant app therefore
     *     supplies pPlaneLayouts of length 1 (VU 02265).
     *   - But the import path consumes 2 entries: get_plane_count (panvk_image.c
     *     :121) returns vk_format_get_plane_count(NV12)=2 -> image->plane_count
     *     =2 -> the layout loop (:482) reads pPlaneLayouts[0] AND [1]. CONFIRMED
     *     by driver instrumentation: it logged plane=0 (off 0) and plane=1.
     *   With the Khronos validation layer active, its safe-copy of the pNext
     *   chain truncates pPlaneLayouts to the *declared* count, so declaring 1
     *   makes the driver read [1] from a 1-long copy -> garbage (measured
     *   off=65, pitch=3.8e9) -> "WSI offset not properly aligned".
     *
     * Workaround: declare 2 and supply both Y+UV. The driver gets correct
     * entries with the layer ON (copy carries 2) and OFF (driver reads our
     * array). Cost: declaring 2 violates VU 02265 (a REAL spec violation, the
     * symptom of the PanVK bug — to be reported upstream, not glossed over).
     * The spec-conformant count=1 only works with the layer OFF (ZC_NOVALIDATE),
     * because then nothing truncates our 2-entry array — but it is still a
     * driver over-read of an array the spec says is length 1. */
    VkSubresourceLayout pl[2]; memset(pl, 0, sizeof(pl));
    pl[0].offset = f->offset[0]; pl[0].rowPitch = f->stride[0];
    pl[1].offset = f->offset[1]; pl[1].rowPitch = f->stride[1];

    VkImageDrmFormatModifierExplicitCreateInfoEXT drm = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = 0,
        .drmFormatModifierPlaneCount = f->n_planes,  /* 2: what the driver consumes, see above */
        .pPlaneLayouts = pl};
    (void)mod_planes;  /* queried memory-plane count (1); kept for the log only */
    VkExternalMemoryImageCreateInfo emi = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &emi,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = NV12,
        .extent = {f->width, f->height, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkResult r = vkCreateImage(v->dev, &ici, NULL, &img);
    if (r == VK_ERROR_INITIALIZATION_FAILED) {
        /* strict_import: our rowPitch != PanVK's computed pitch. Report what we
         * supplied so the caller can retry with the driver-reported pitch. */
        fprintf(stderr, "STRICT_IMPORT: image-create rejected layout "
                "(supplied pl[0]={off=%lu,pitch=%lu}). pitch mismatch.\n",
                (unsigned long)pl[0].offset, (unsigned long)pl[0].rowPitch);
        close(f->fd); exit(4);
    }
    CHECK(r);  /* other create errors: process-exit reclaims f->fd (spike). For
                * Step-2's long-running loop, close f->fd on every error path. */

    /* import the fd as the backing memory (dedicated alloc for the image) */
    VkMemoryFdPropertiesKHR mfp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    CHECK(v->GetMemoryFdPropertiesKHR(v->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, f->fd, &mfp));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(v->dev, img, &mr);
    uint32_t bits = mr.memoryTypeBits & mfp.memoryTypeBits;
    if (!bits) { fprintf(stderr, "FAIL: no shared memtype (img 0x%x dmabuf 0x%x)\n",
                         mr.memoryTypeBits, mfp.memoryTypeBits); close(f->fd); exit(3); }

    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                         .image = img};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .pNext = &ded,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   .fd = f->fd};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = &imp,
                                .allocationSize = mr.size,
                                .memoryTypeIndex = pick_memtype(v, bits)};
    CHECK(vkAllocateMemory(v->dev, &mai, NULL, out_mem));  /* Vulkan now owns f->fd */
    CHECK(vkBindImageMemory(v->dev, img, *out_mem, 0));
    printf("vk: imported dmabuf -> VkImage (mod_planes=%u, memReq.size=%lu)\n",
           mod_planes, (unsigned long)mr.size);
    return img;
}

/* one-shot command buffer helper */
static VkCommandBuffer begin_cmd(struct vk *v) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = v->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(v->dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}
static void end_cmd(struct vk *v, VkCommandBuffer cb) {
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CHECK(vkCreateFence(v->dev, &fci, NULL, &fence));
    CHECK(vkQueueSubmit(v->q, 1, &si, fence));
    CHECK(vkWaitForFences(v->dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(v->dev, fence, NULL);
    vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
}

/* GATE 1a: copy each format-plane to a host buffer; byte-compare vs ref NV12. */
static int gate_1a(struct vk *v, VkImage img, struct frame *f, const char *ref_path) {
    uint32_t W = f->width, H = f->height;
    VkDeviceSize ysz = (VkDeviceSize)W * H;
    VkDeviceSize uvsz = (VkDeviceSize)W * (H / 2);   /* interleaved CbCr: W bytes/row, H/2 rows */
    VkDeviceSize total = ysz + uvsz;

    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = total, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer stg; CHECK(vkCreateBuffer(v->dev, &bci, NULL, &stg));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(v->dev, stg, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mti = i; break; }
    if (mti == UINT32_MAX) { fprintf(stderr, "FAIL: no host-visible memtype\n"); return 3; }
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = mr.size, .memoryTypeIndex = mti};
    VkDeviceMemory stgmem; CHECK(vkAllocateMemory(v->dev, &mai, NULL, &stgmem));
    CHECK(vkBindBufferMemory(v->dev, stg, stgmem, 0));

    VkCommandBuffer cb = begin_cmd(v);
    /* acquire imported content: UNDEFINED -> TRANSFER_SRC. Linear modifier: no
     * retiling, bytes preserved. (If this discards on some driver, the gate
     * catches it and we switch to a FOREIGN-queue acquire / PREINITIALIZED.) */
    VkImageMemoryBarrier acq = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &acq);
    VkBufferImageCopy cpy[2] = {
        {.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
         .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
         .imageOffset = {0,0,0}, .imageExtent = {W, H, 1}},
        {.bufferOffset = ysz, .bufferRowLength = 0, .bufferImageHeight = 0,
         .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
         .imageOffset = {0,0,0}, .imageExtent = {W/2, H/2, 1}}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg, 2, cpy);
    end_cmd(v, cb);

    void *map; CHECK(vkMapMemory(v->dev, stgmem, 0, total, 0, &map));

    FILE *rf = fopen(ref_path, "rb");
    if (!rf) { fprintf(stderr, "FAIL: cannot open ref %s\n", ref_path); return 3; }
    uint8_t *ref = malloc(total);
    size_t got = fread(ref, 1, total, rf); fclose(rf);
    if (got != total) { fprintf(stderr, "FAIL: ref size %zu != %lu\n", got, (unsigned long)total); return 3; }

    int exact = memcmp(map, ref, total) == 0;
    size_t first = total;
    double mse = 0;
    const uint8_t *m = map;
    for (size_t i = 0; i < total; i++) {
        int d = (int)m[i] - (int)ref[i];
        if (d && first == total) first = i;
        mse += (double)d * d;
    }
    mse /= total;
    double dpsnr = (mse == 0) ? 1e9 : 10.0 * log10(255.0 * 255.0 / mse);
    /* distinct values in ours (catch the blank-output failure mode) */
    int seen[256] = {0}, distinct = 0;
    for (size_t i = 0; i < ysz; i++) if (!seen[m[i]]) { seen[m[i]] = 1; distinct++; }

    printf("\n=== GATE 1a (import geometry, byte-exact) ===\n");
    printf("  bytes=%lu  Y-distinct=%d  first_diff=%s%zu  PSNR=%.1f dB\n",
           (unsigned long)total, distinct,
           first == total ? "(none) " : "", first == total ? 0 : first, dpsnr);
    printf("  RESULT: %s\n", exact ? "PASS (byte-exact)" : "FAIL (not byte-exact)");

    free(ref);
    vkUnmapMemory(v->dev, stgmem);
    vkDestroyBuffer(v->dev, stg, NULL);
    vkFreeMemory(v->dev, stgmem, NULL);
    return exact ? 0 : 1;
}

/* ---- Stage 1b: HW-YUV sample via VkSamplerYcbcrConversion ---- */

static uint32_t *load_spv(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL: cannot open shader %s (runner compiles it)\n", path); exit(3); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || (n & 3)) { fprintf(stderr, "FAIL: bad SPIR-V size %ld in %s\n", n, path); exit(3); }
    uint32_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "FAIL: read %s\n", path); exit(3); }
    fclose(f); *size_out = (size_t)n; return buf;
}

static uint32_t host_memtype(struct vk *v, uint32_t typebits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typebits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return i;
    fprintf(stderr, "FAIL: no host-visible memtype\n"); exit(3);
}

/* CPU NV12 -> packed RGBA8 (r|g<<8|b<<16|a<<24), BT.709 limited range, NEAREST
 * chroma — IDENTICAL params to the VkSamplerYcbcrConversion below, so the PSNR
 * isolates HW fixed-point CSC error, not a parameter mismatch. Source NV12 is
 * the INDEPENDENT ffmpeg ref (byte-identical to the import per gate 1a). */
/* Plain nearest chroma (x/2,y/2). Chroma-edge reconstruction is Mali fixed-
 * function HW (get_yuv_cr_siting -> MALI_YUV_CR_SITING_*) and is NOT bit-
 * matchable by any CPU model — so 1b gates on FLAT-chroma regions, where the
 * reconstruction is irrelevant and this plain index is exactly correct. */
static void cpu_nv12_to_rgba(const uint8_t *nv12, uint32_t W, uint32_t H, uint32_t *out) {
    const uint8_t *Y = nv12;
    const uint8_t *UV = nv12 + (size_t)W * H;        /* packed: UV row = W bytes */
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            double yv = ((double)Y[(size_t)y * W + x] - 16.0) / 219.0;
            size_t ci = (size_t)(y / 2) * W + (x / 2) * 2;
            double cb = ((double)UV[ci]     - 128.0) / 224.0;
            double cr = ((double)UV[ci + 1] - 128.0) / 224.0;
            double r = yv + 1.5748 * cr;                       /* BT.709 limited */
            double g = yv - 0.1873 * cb - 0.4681 * cr;
            double b = yv + 1.8556 * cb;
#define C8(t) ((uint32_t)((t) < 0 ? 0 : ((t) > 1 ? 255 : lround((t) * 255.0))))
            out[(size_t)y * W + x] = C8(r) | (C8(g) << 8) | (C8(b) << 16) | (255u << 24);
#undef C8
        }
}

/* A pixel is "reconstruction-insensitive" if the 3x3 chroma neighbourhood around
 * its chroma texel is uniform (<=1) in both Cb and Cr — then every upsampling
 * model agrees, so any HW/CPU mismatch there is a CSC error, not reconstruction. */
static int chroma_flat(const uint8_t *UV, uint32_t W, uint32_t cw, uint32_t ch,
                       uint32_t cx, uint32_t cy) {
    int cbmin = 255, cbmax = 0, crmin = 255, crmax = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int yy = (int)cy + dy, xx = (int)cx + dx;
            if (yy < 0) yy = 0; if (yy > (int)ch - 1) yy = ch - 1;
            if (xx < 0) xx = 0; if (xx > (int)cw - 1) xx = cw - 1;
            size_t o = (size_t)yy * W + (size_t)xx * 2;
            int cb = UV[o], cr = UV[o + 1];
            if (cb < cbmin) cbmin = cb; if (cb > cbmax) cbmax = cb;
            if (cr < crmin) crmin = cr; if (cr > crmax) crmax = cr;
        }
    return (cbmax - cbmin) <= 1 && (crmax - crmin) <= 1;
}

static int gate_1b(struct vk *v, VkImage img, struct frame *f, const char *ref_path) {
    uint32_t W = f->width, H = f->height;
    VkDeviceSize obytes = (VkDeviceSize)W * H * 4;

    VkSamplerYcbcrConversionCreateInfo cci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = NV12,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,   /* 720p HD */
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,             /* limited */
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN,
        .yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN,
        /* ZC_LINEAR toggles chroma filter to diagnose what PanVK actually does
         * (nearest siting combos all mismatched ~33dB -> suspect HW linear). */
        .chromaFilter = getenv("ZC_LINEAR") ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .forceExplicitReconstruction = VK_FALSE};
    VkSamplerYcbcrConversion conv; CHECK(vkCreateSamplerYcbcrConversion(v->dev, &cci, NULL, &conv));
    VkSamplerYcbcrConversionInfo cinfo = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                                          .conversion = conv};
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = &cinfo,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .unnormalizedCoordinates = VK_FALSE};
    VkSampler samp; CHECK(vkCreateSampler(v->dev, &sci, NULL, &samp));

    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &cinfo,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = NV12,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CHECK(vkCreateImageView(v->dev, &vci, NULL, &view));

    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = obytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer ob; CHECK(vkCreateBuffer(v->dev, &bci, NULL, &ob));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(v->dev, ob, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = host_memtype(v, mr.memoryTypeBits)};
    VkDeviceMemory om; CHECK(vkAllocateMemory(v->dev, &mai, NULL, &om));
    CHECK(vkBindBufferMemory(v->dev, ob, om, 0));

    VkDescriptorSetLayoutBinding bind[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .pImmutableSamplers = &samp},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dlci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bind};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(v->dev, &dlci, NULL, &dsl));
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 8};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout pllayout; CHECK(vkCreatePipelineLayout(v->dev, &plci, NULL, &pllayout));

    size_t spvsz; uint32_t *spv = load_spv("zc_comp.spv", &spvsz);
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spvsz, .pCode = spv};
    VkShaderModule sm; CHECK(vkCreateShaderModule(v->dev, &smci, NULL, &sm));
    VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
        .layout = pllayout};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(v->dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(v->dev, &dsai, &ds));
    VkDescriptorImageInfo dii = {.sampler = samp, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo dbi = {.buffer = ob, .offset = 0, .range = obytes};
    VkWriteDescriptorSet wds[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi}};
    vkUpdateDescriptorSets(v->dev, 2, wds, 0, NULL);

    VkCommandBuffer cb = begin_cmd(v);
    /* acquire imported content for sampling (UNDEFINED ok for LINEAR; see 1a) */
    VkImageMemoryBarrier acq = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &acq);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pllayout, 0, 1, &ds, 0, NULL);
    uint32_t pcv[2] = {W, H};
    vkCmdPushConstants(cb, pllayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pcv);
    vkCmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
    VkBufferMemoryBarrier bbar = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = ob, .offset = 0, .size = obytes};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bbar, 0, NULL);
    end_cmd(v, cb);

    void *map; CHECK(vkMapMemory(v->dev, om, 0, obytes, 0, &map));
    VkDeviceSize nv12sz = (VkDeviceSize)W * H + (VkDeviceSize)W * (H / 2);
    FILE *rf = fopen(ref_path, "rb");
    if (!rf) { fprintf(stderr, "FAIL: cannot open ref %s\n", ref_path); return 3; }
    uint8_t *nv12 = malloc(nv12sz);
    if (fread(nv12, 1, nv12sz, rf) != nv12sz) { fprintf(stderr, "FAIL: ref short read\n"); fclose(rf); return 3; }
    fclose(rf);
    uint32_t *cpu = malloc(obytes);
    cpu_nv12_to_rgba(nv12, W, H, cpu);

    const uint8_t *hb = map, *cbb = (const uint8_t *)cpu;
    const uint8_t *UV = nv12 + (size_t)W * H;
    uint32_t cw = W / 2, ch = H / 2;
    double mse = 0, fmse = 0; int maxd = 0, fmaxd = 0;
    size_t fcount = 0, edge_bad = 0;
    int fseen[256] = {0}, fdistinct = 0;   /* richness WITHIN the gated (flat) set */
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            int flat = chroma_flat(UV, W, cw, ch, x / 2, y / 2);
            size_t p = ((size_t)y * W + x) * 4;
            int pmax = 0;
            for (int k = 0; k < 4; k++) {
                int d = (int)hb[p + k] - (int)cbb[p + k]; if (d < 0) d = -d;
                if (d > pmax) pmax = d;
                mse += (double)d * d;
                if (flat) fmse += (double)d * d;
            }
            if (pmax > maxd) maxd = pmax;
            if (flat) {
                fcount++; if (pmax > fmaxd) fmaxd = pmax;
                if (!fseen[hb[p]]) { fseen[hb[p]] = 1; fdistinct++; }   /* R-byte variety in flat set */
            } else if (pmax > 32) edge_bad++;
        }
    mse /= obytes;
    double fmse_avg = fcount ? fmse / ((double)fcount * 4) : 0;
    double whole_psnr = (mse == 0) ? 1e9 : 10.0 * log10(65025.0 / mse);
    double flat_psnr  = (fcount == 0) ? 0 : (fmse_avg == 0 ? 1e9 : 10.0 * log10(65025.0 / fmse_avg));
    /* Gate on flat-chroma regions: there the conversion is the ONLY variable, so
     * a match proves the HW ycbcr CSC (matrix+range+component map) is correct.
     * Chroma-edge pixels exercise Mali's fixed-function upsampling, which no CPU
     * model bit-matches — reported as informational, not gated. Guard the gated
     * set itself: it must be a large (>=50% frame) and non-degenerate (>=16
     * distinct flat-pixel values) sample, else 'flat PSNR' would be meaningless. */
    int flat_big = fcount >= (size_t)W * H / 2;
    int pass = flat_psnr >= 40.0 && flat_big && fdistinct >= 16;
    printf("\n=== GATE 1b (HW-YUV ycbcr sample; CSC validated on flat-chroma regions) ===\n");
    printf("  %ux%u  flat-chroma=%.1f%% (>=50%%:%d)  flat-distinct=%d  FLAT PSNR=%.2f dB (max|d|=%d)   [GATE]\n",
           W, H, 100.0 * (double)fcount / ((double)W * H), flat_big, fdistinct, flat_psnr, fmaxd);
    printf("  whole-frame PSNR=%.2f dB (max|d|=%d); edge pixels>32=%zu (Mali HW chroma upsampling, expected)\n",
           whole_psnr, maxd, edge_bad);
    printf("  RESULT: %s (flat-chroma PSNR>=40 proves CSC; edges = HW fixed-function reconstruction)\n",
           pass ? "PASS" : "FAIL");
    if (getenv("ZC_DUMP")) {   /* dump real HW RGBA for offline siting/filter analysis */
        const char *dn = getenv("ZC_LINEAR") ? "hw_rgba_lin.bin" : "hw_rgba.bin";
        FILE *d = fopen(dn, "wb");
        if (d) { fwrite(map, 1, obytes, d); fclose(d); printf("  dumped %s (%llu B)\n", dn, (unsigned long long)obytes); }
    }

    free(spv); free(nv12); free(cpu);
    vkDestroyPipeline(v->dev, pipe, NULL); vkDestroyShaderModule(v->dev, sm, NULL);
    vkDestroyDescriptorPool(v->dev, dp, NULL); vkDestroyPipelineLayout(v->dev, pllayout, NULL);
    vkDestroyDescriptorSetLayout(v->dev, dsl, NULL);
    vkDestroyImageView(v->dev, view, NULL); vkDestroySampler(v->dev, samp, NULL);
    vkDestroySamplerYcbcrConversion(v->dev, conv, NULL);
    vkUnmapMemory(v->dev, om); vkDestroyBuffer(v->dev, ob, NULL); vkFreeMemory(v->dev, om, NULL);
    return pass ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <clip> <ref_nv12>\n", argv[0]); return 2; }
    gst_init(&argc, &argv);

    struct frame f;
    if (decode_one(argv[1], &f)) return 1;

    struct vk v;
    vk_init(&v);
    uint32_t mod_planes = panvk_linear_plane_count(&v);

    VkDeviceMemory mem;
    VkImage img = import_image(&v, &f, mod_planes, &mem);

    int rc1 = gate_1a(&v, img, &f, argv[2]);   /* import geometry, byte-exact */
    int rc2 = gate_1b(&v, img, &f, argv[2]);   /* HW-YUV sample, PSNR (same import) */

    /* isolation reminder lives in the runner (pacman -Q mesa pre/post) */
    return rc1 | rc2;
}
