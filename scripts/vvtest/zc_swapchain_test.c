/* Phase-C Step-2 Increment-2 (sub-gate 2b): on-screen ZERO-COPY present.
 * decode (rkvdec, meta-aware dmabuf) -> import into PanVK -> sample via the
 * fragment-shader VkSamplerYcbcrConversion -> render into a Wayland SWAPCHAIN
 * image -> vkQueuePresentKHR. NO vulkandownload / NO per-frame readback.
 *
 * Streaming is SYNCHRONOUS per frame (import -> render -> present -> queue-wait-
 * idle -> destroy import + close fd + unref GstSample) so there is no buffer-
 * recycle race and no object leak in the loop (the reviewer's streaming hazards).
 * NO readback anywhere in the loop (that is the whole point vs vulkandownload).
 * CONTENT correctness of the rendered pixels is established by sub-gate 2a
 * (zc_present_test.c: identical import + fragment-ycbcr render path, byte-exact
 * 80 dB); 2b proves the on-screen PRESENT plumbing (window mapped + frames
 * presented, no readback) — visual confirmation by the user on sway.
 * Window via SDL2 (handles wl_surface + xdg-shell).
 *
 * Build (SBC):
 *   glslangValidator -V zc_present.vert -o zc_present_vert.spv
 *   glslangValidator -V zc_present.frag -o zc_present_frag.spv
 *   cc zc_swapchain_test.c -o zc_swapchain_test \
 *     $(pkg-config --cflags --libs sdl2 gstreamer-1.0 gstreamer-app-1.0 \
 *       gstreamer-allocators-1.0 gstreamer-video-1.0) -lvulkan -lm
 * Run (under sway):
 *   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 \
 *   VK_ICD_FILENAMES=$HOME/mesa-zc/panfrost_icd.json ./zc_swapchain_test <clip> <ref_nv12>
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
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
#include <time.h>

#define NV12 VK_FORMAT_G8_B8R8_2PLANE_420_UNORM

static const char *vkstr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    default: return "VK_ERROR_<other>";
    }
}
#define CHECK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FATAL %s:%d %s -> %s\n", __FILE__, __LINE__, #call, vkstr(_r)); exit(3);} } while (0)

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* ---- GStreamer streaming decode (meta-aware: keeps padded hw dmabuf) ---- */
struct frame { int fd; uint32_t width, height, n_planes, stride[4]; uint64_t offset[4]; uint32_t vis_w, vis_h; GstSample *sample; };

static GstPadProbeReturn meta_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u) {
    (void)pad; (void)u;
    GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);
    if (q && GST_QUERY_TYPE(q) == GST_QUERY_ALLOCATION && gst_query_is_writable(q))
        gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, NULL);
    return GST_PAD_PROBE_PASS;
}
struct decoder { GstElement *pipe, *sink; };
static int dec_start(struct decoder *d, const char *clip) {
    const char *parser = g_str_has_suffix(clip, ".h265") ? "h265parse" : "h264parse";
    const char *dec = g_str_has_suffix(clip, ".h265") ? "v4l2slh265dec" : "v4l2slh264dec";
    gchar *desc = g_strdup_printf("filesrc location=%s ! %s ! %s ! appsink name=s emit-signals=false max-buffers=8 drop=false sync=false", clip, parser, dec);
    d->pipe = gst_parse_launch(desc, NULL); g_free(desc);
    if (!d->pipe) return 1;
    d->sink = gst_bin_get_by_name(GST_BIN(d->pipe), "s");
    GstPad *sp = gst_element_get_static_pad(d->sink, "sink");
    gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, meta_probe, NULL, NULL);
    gst_object_unref(sp);
    return gst_element_set_state(d->pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
}
/* pull next frame; returns 0 on frame, 1 on EOS/none. caller owns f->sample + f->fd. */
static int dec_next(struct decoder *d, struct frame *f) {
    GstSample *s = gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink), 5 * GST_SECOND);
    if (!s) return 1;
    GstBuffer *buf = gst_sample_get_buffer(s);
    GstMemory *m0 = gst_buffer_peek_memory(buf, 0);
    if (!gst_is_dmabuf_memory(m0)) { fprintf(stderr, "FAIL: not dmabuf (meta probe?)\n"); gst_sample_unref(s); return 2; }
    GstVideoMeta *vm = gst_buffer_get_video_meta(buf);
    if (!vm) { fprintf(stderr, "FAIL: no VideoMeta\n"); gst_sample_unref(s); return 2; }
    memset(f, 0, sizeof(*f));
    f->fd = dup(gst_dmabuf_memory_get_fd(m0));
    f->width = vm->width; f->height = vm->height; f->n_planes = vm->n_planes;
    for (guint i = 0; i < vm->n_planes && i < 4; i++) { f->stride[i] = vm->stride[i]; f->offset[i] = vm->offset[i]; }
    GstCaps *caps = gst_sample_get_caps(s); GstStructure *st = caps ? gst_caps_get_structure(caps, 0) : NULL;
    int vw = 0, vh = 0; if (st) { gst_structure_get_int(st, "width", &vw); gst_structure_get_int(st, "height", &vh); }
    f->vis_w = vw > 0 ? (uint32_t)vw : f->width; f->vis_h = vh > 0 ? (uint32_t)vh : f->height;
    f->sample = s;
    return 0;
}
static void frame_release(struct frame *f) { if (f->sample) gst_sample_unref(f->sample); /* fd owned by Vulkan import */ }

/* ---- Vulkan ---- */
struct vk {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; uint32_t qfam; VkQueue q; VkCommandPool pool;
    VkSurfaceKHR surf; VkSwapchainKHR swap; VkFormat scfmt; VkExtent2D scext;
    uint32_t nimg; VkImage *imgs; VkImageView *views; VkFramebuffer *fbs;
    VkRenderPass rp; VkSampler samp; VkSamplerYcbcrConversion conv;
    VkDescriptorSetLayout dsl; VkPipelineLayout pll; VkPipeline pipe; VkDescriptorPool dpool;
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
};
static int has_ext(VkExtensionProperties *e, uint32_t n, const char *s) { for (uint32_t i=0;i<n;i++) if(!strcmp(e[i].extensionName,s)) return 1; return 0; }

static uint32_t pick_memtype(struct vk *v, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (bits & (1u << i)) return i;
    fprintf(stderr, "FAIL memtype\n"); exit(3);
}
static uint32_t prop_memtype(struct vk *v, uint32_t bits, VkMemoryPropertyFlags w) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&w)==w) return i;
    fprintf(stderr, "FAIL memtype prop\n"); exit(3);
}

static void vk_setup(struct vk *v, SDL_Window *win, const char *vspv_path, const char *fspv_path) {
    /* instance: SDL surface exts (+ validation) */
    unsigned ec = 0; SDL_Vulkan_GetInstanceExtensions(win, &ec, NULL);
    const char **iexts = calloc(ec + 1, sizeof(char *)); SDL_Vulkan_GetInstanceExtensions(win, &ec, iexts);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const char *layers[1]; uint32_t nl = 0;
    if (!getenv("ZC_NOVALIDATE")) { uint32_t lc=0; vkEnumerateInstanceLayerProperties(&lc,NULL); VkLayerProperties lp[64]; if(lc>64)lc=64; vkEnumerateInstanceLayerProperties(&lc,lp);
        for (uint32_t i=0;i<lc;i++) if(!strcmp(lp[i].layerName,"VK_LAYER_KHRONOS_validation")) layers[nl++]=lp[i].layerName; }
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&ai,
        .enabledExtensionCount=ec, .ppEnabledExtensionNames=iexts, .enabledLayerCount=nl, .ppEnabledLayerNames=layers};
    CHECK(vkCreateInstance(&ici, NULL, &v->inst));
    uint32_t n=0; vkEnumeratePhysicalDevices(v->inst,&n,NULL); if(!n){fprintf(stderr,"no PD\n");exit(2);}
    VkPhysicalDevice pds[8]; if(n>8)n=8; vkEnumeratePhysicalDevices(v->inst,&n,pds); v->pd=pds[0];
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(v->pd,&p);
    printf("vk: '%s' api=%u.%u.%u%s\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), nl?" (+val)":"");
    if (!SDL_Vulkan_CreateSurface(win, v->inst, &v->surf)) { fprintf(stderr, "FAIL SDL surface: %s\n", SDL_GetError()); exit(2); }

    uint32_t dec=0; vkEnumerateDeviceExtensionProperties(v->pd,NULL,&dec,NULL);
    VkExtensionProperties *ext=calloc(dec,sizeof(*ext)); vkEnumerateDeviceExtensionProperties(v->pd,NULL,&dec,ext);
    const char *need[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME};
    for (size_t i=0;i<sizeof(need)/sizeof(*need);i++) if(!has_ext(ext,dec,need[i])){fprintf(stderr,"missing %s\n",need[i]);exit(2);}
    free(ext);
    uint32_t qc=0; vkGetPhysicalDeviceQueueFamilyProperties(v->pd,&qc,NULL); VkQueueFamilyProperties qp[16]; if(qc>16)qc=16;
    vkGetPhysicalDeviceQueueFamilyProperties(v->pd,&qc,qp); v->qfam=UINT32_MAX;
    for (uint32_t i=0;i<qc;i++){ VkBool32 pres=0; vkGetPhysicalDeviceSurfaceSupportKHR(v->pd,i,v->surf,&pres);
        if ((qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) && pres){ v->qfam=i; break; } }
    if (v->qfam==UINT32_MAX){fprintf(stderr,"no graphics+present queue\n");exit(2);}
    float prio=1.0f; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=v->qfam,.queueCount=1,.pQueuePriorities=&prio};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures yf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,.samplerYcbcrConversion=VK_TRUE};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&yf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,
        .enabledExtensionCount=sizeof(need)/sizeof(*need),.ppEnabledExtensionNames=need};
    CHECK(vkCreateDevice(v->pd,&dci,NULL,&v->dev));
    vkGetDeviceQueue(v->dev,v->qfam,0,&v->q);
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=v->qfam};
    CHECK(vkCreateCommandPool(v->dev,&pci,NULL,&v->pool));
    v->GetMemoryFdPropertiesKHR=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(v->dev,"vkGetMemoryFdPropertiesKHR");

    /* swapchain */
    VkSurfaceCapabilitiesKHR sc; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->pd,v->surf,&sc);
    uint32_t fc=0; vkGetPhysicalDeviceSurfaceFormatsKHR(v->pd,v->surf,&fc,NULL); VkSurfaceFormatKHR sf[64]; if(fc>64)fc=64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v->pd,v->surf,&fc,sf);
    v->scfmt = sf[0].format; VkColorSpaceKHR cs = sf[0].colorSpace;
    for (uint32_t i=0;i<fc;i++) if (sf[i].format==VK_FORMAT_B8G8R8A8_UNORM||sf[i].format==VK_FORMAT_R8G8B8A8_UNORM){v->scfmt=sf[i].format;cs=sf[i].colorSpace;break;}
    v->scext = sc.currentExtent.width!=0xFFFFFFFF ? sc.currentExtent : (VkExtent2D){1280,720};
    uint32_t want = sc.minImageCount+1; if (sc.maxImageCount && want>sc.maxImageCount) want=sc.maxImageCount;
    int can_copy = (sc.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR sci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=v->surf,.minImageCount=want,
        .imageFormat=v->scfmt,.imageColorSpace=cs,.imageExtent=v->scext,.imageArrayLayers=1,
        .imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|(can_copy?VK_IMAGE_USAGE_TRANSFER_SRC_BIT:0),
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=sc.currentTransform,
        .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    CHECK(vkCreateSwapchainKHR(v->dev,&sci,NULL,&v->swap));
    vkGetSwapchainImagesKHR(v->dev,v->swap,&v->nimg,NULL);
    v->imgs=calloc(v->nimg,sizeof(VkImage)); vkGetSwapchainImagesKHR(v->dev,v->swap,&v->nimg,v->imgs);
    v->views=calloc(v->nimg,sizeof(VkImageView)); v->fbs=calloc(v->nimg,sizeof(VkFramebuffer));
    printf("vk: swapchain %ux%u fmt=%d images=%u present=FIFO copy=%d\n", v->scext.width,v->scext.height,v->scfmt,v->nimg,can_copy);

    /* render pass (load CLEAR, final PRESENT_SRC) */
    VkAttachmentDescription at={.format=v->scfmt,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference ar={.attachment=0,.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ar};
    VkSubpassDependency dep={.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask=0,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&at,.subpassCount=1,.pSubpasses=&sub,.dependencyCount=1,.pDependencies=&dep};
    CHECK(vkCreateRenderPass(v->dev,&rpci,NULL,&v->rp));
    for (uint32_t i=0;i<v->nimg;i++){
        VkImageViewCreateInfo iv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=v->imgs[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=v->scfmt,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(v->dev,&iv,NULL,&v->views[i]));
        VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=v->rp,.attachmentCount=1,.pAttachments=&v->views[i],.width=v->scext.width,.height=v->scext.height,.layers=1};
        CHECK(vkCreateFramebuffer(v->dev,&fb,NULL,&v->fbs[i]));
    }

    /* ycbcr conversion + immutable sampler (frame-invariant, hoisted) */
    VkSamplerYcbcrConversionCreateInfo cci={.sType=VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,.format=NV12,
        .ycbcrModel=VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,.ycbcrRange=VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        .xChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN,.yChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN,.chromaFilter=VK_FILTER_NEAREST};
    CHECK(vkCreateSamplerYcbcrConversion(v->dev,&cci,NULL,&v->conv));
    VkSamplerYcbcrConversionInfo cin={.sType=VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,.conversion=v->conv};
    VkSamplerCreateInfo sca={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.pNext=&cin,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(v->dev,&sca,NULL,&v->samp));
    VkDescriptorSetLayoutBinding b={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT,.pImmutableSamplers=&v->samp};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&b};
    CHECK(vkCreateDescriptorSetLayout(v->dev,&dl,NULL,&v->dsl));
    VkPushConstantRange pcr={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.offset=0,.size=8};   /* vec2 uvscale (visible/coded crop) */
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&v->dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&pcr};
    CHECK(vkCreatePipelineLayout(v->dev,&pl,NULL,&v->pll));

    /* graphics pipeline */
    FILE *fv=fopen(vspv_path,"rb"),*ff=fopen(fspv_path,"rb"); if(!fv||!ff){fprintf(stderr,"FAIL open spv\n");exit(3);}
    fseek(fv,0,SEEK_END);long vn=ftell(fv);fseek(fv,0,SEEK_SET);uint32_t*vb=malloc(vn);size_t rv=fread(vb,1,vn,fv);fclose(fv);
    fseek(ff,0,SEEK_END);long fn=ftell(ff);fseek(ff,0,SEEK_SET);uint32_t*fb2=malloc(fn);size_t rf=fread(fb2,1,fn,ff);fclose(ff);
    if(rv!=(size_t)vn||rf!=(size_t)fn){fprintf(stderr,"FAIL read spv\n");exit(3);}
    VkShaderModuleCreateInfo vm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vn,.pCode=vb};
    VkShaderModuleCreateInfo fm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=fn,.pCode=fb2};
    VkShaderModule sv,sfm; CHECK(vkCreateShaderModule(v->dev,&vm,NULL,&sv)); CHECK(vkCreateShaderModule(v->dev,&fm,NULL,&sfm));
    VkPipelineShaderStageCreateInfo st2[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=sv,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=sfm,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vis={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp={0,0,(float)v->scext.width,(float)v->scext.height,0,1}; VkRect2D scr={{0,0},v->scext};
    VkPipelineViewportStateCreateInfo vps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&scr};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo msa={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xF};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=st2,.pVertexInputState=&vis,
        .pInputAssemblyState=&ia,.pViewportState=&vps,.pRasterizationState=&rs,.pMultisampleState=&msa,.pColorBlendState=&cb,.layout=v->pll,.renderPass=v->rp,.subpass=0};
    CHECK(vkCreateGraphicsPipelines(v->dev,VK_NULL_HANDLE,1,&gp,NULL,&v->pipe));
    vkDestroyShaderModule(v->dev,sv,NULL); vkDestroyShaderModule(v->dev,sfm,NULL); free(vb); free(fb2);

    /* descriptor pool (1 set re-updated per frame; reset each frame) */
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    CHECK(vkCreateDescriptorPool(v->dev,&dp,NULL,&v->dpool));
    free(iexts);
}

/* import one frame's dmabuf into a VkImage + ycbcr view (per-frame; destroyed after) */
static VkImage import_frame(struct vk *v, struct frame *f, VkDeviceMemory *mem, VkImageView *yview) {
    VkSubresourceLayout pl[2]; memset(pl,0,sizeof(pl));
    pl[0].offset=f->offset[0]; pl[0].rowPitch=f->stride[0]; pl[1].offset=f->offset[1]; pl[1].rowPitch=f->stride[1];
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm={.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,.drmFormatModifier=0,.drmFormatModifierPlaneCount=f->n_planes,.pPlaneLayouts=pl};
    VkExternalMemoryImageCreateInfo emi={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.pNext=&drm,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ici={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&emi,.imageType=VK_IMAGE_TYPE_2D,.format=NV12,.extent={f->width,f->height,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; VkResult r=vkCreateImage(v->dev,&ici,NULL,&img);
    if (r!=VK_SUCCESS){ fprintf(stderr,"import create fail %s\n",vkstr(r)); close(f->fd); return VK_NULL_HANDLE; }
    VkMemoryFdPropertiesKHR mfp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    CHECK(v->GetMemoryFdPropertiesKHR(v->dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,f->fd,&mfp));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(v->dev,img,&mr);
    uint32_t bits=mr.memoryTypeBits&mfp.memoryTypeBits; if(!bits){fprintf(stderr,"no memtype\n");close(f->fd);vkDestroyImage(v->dev,img,NULL);return VK_NULL_HANDLE;}
    VkMemoryDedicatedAllocateInfo ded={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=img};
    VkImportMemoryFdInfoKHR imp={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,.pNext=&ded,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=f->fd};
    VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imp,.allocationSize=mr.size,.memoryTypeIndex=pick_memtype(v,bits)};
    CHECK(vkAllocateMemory(v->dev,&mai,NULL,mem)); CHECK(vkBindImageMemory(v->dev,img,*mem,0));
    VkSamplerYcbcrConversionInfo cin={.sType=VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,.conversion=v->conv};
    VkImageViewCreateInfo iv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.pNext=&cin,.image=img,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=NV12,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(vkCreateImageView(v->dev,&iv,NULL,yview));
    return img;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <clip>\n", argv[0]); return 2; }
    gst_init(&argc, &argv);
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    SDL_Window *win = SDL_CreateWindow("zc zero-copy present", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 2; }

    struct vk v; memset(&v, 0, sizeof(v));
    vk_setup(&v, win, "zc_sc_vert.spv", "zc_present_frag.spv");

    /* per-frame sync */
    VkSemaphoreCreateInfo semci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acq_sem, done_sem; CHECK(vkCreateSemaphore(v.dev,&semci,NULL,&acq_sem)); CHECK(vkCreateSemaphore(v.dev,&semci,NULL,&done_sem));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=v.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(v.dev,&cbai,&cmd));

    struct decoder d; if (dec_start(&d, argv[1])) { fprintf(stderr, "decode start fail\n"); return 1; }

    double t0 = now_s(); int presented = 0; struct frame f;
    int rc;
    while ((rc = dec_next(&d, &f)) == 0) {
        SDL_Event e; while (SDL_PollEvent(&e)) { if (e.type == SDL_QUIT) goto done; }
        VkDeviceMemory imem; VkImageView yview;
        VkImage img = import_frame(&v, &f, &imem, &yview);
        if (img == VK_NULL_HANDLE) { frame_release(&f); break; }

        uint32_t idx;
        VkResult ar = vkAcquireNextImageKHR(v.dev, v.swap, UINT64_MAX, acq_sem, VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { vkDestroyImageView(v.dev,yview,NULL); vkDestroyImage(v.dev,img,NULL); vkFreeMemory(v.dev,imem,NULL); frame_release(&f); continue; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) CHECK(ar);   /* SUBOPTIMAL = image acquired, proceed */

        /* descriptor set for this frame's ycbcr view */
        VkDescriptorSetAllocateInfo dsa={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=v.dpool,.descriptorSetCount=1,.pSetLayouts=&v.dsl};
        VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(v.dev,&dsa,&ds));
        VkDescriptorImageInfo dii={.sampler=v.samp,.imageView=yview,.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&dii};
        vkUpdateDescriptorSets(v.dev,1,&w,0,NULL);

        vkResetCommandBuffer(cmd,0);
        VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cmd,&bi));
        VkImageMemoryBarrier acq={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&acq);
        VkClearValue clr={.color={{0,0,0,1}}};
        VkRenderPassBeginInfo rb={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=v.rp,.framebuffer=v.fbs[idx],.renderArea={{0,0},v.scext},.clearValueCount=1,.pClearValues=&clr};
        vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,v.pipe);
        float uvscale[2]={(float)f.vis_w/(float)f.width,(float)f.vis_h/(float)f.height};   /* crop coded padding */
        vkCmdPushConstants(cmd,v.pll,VK_SHADER_STAGE_VERTEX_BIT,0,8,uvscale);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,v.pll,0,1,&ds,0,NULL);
        vkCmdDraw(cmd,3,1,0,0);
        vkCmdEndRenderPass(cmd);
        CHECK(vkEndCommandBuffer(cmd));

        VkPipelineStageFlags wstage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acq_sem,.pWaitDstStageMask=&wstage,
            .commandBufferCount=1,.pCommandBuffers=&cmd,.signalSemaphoreCount=1,.pSignalSemaphores=&done_sem};
        CHECK(vkQueueSubmit(v.q,1,&si,VK_NULL_HANDLE));
        VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&done_sem,.swapchainCount=1,.pSwapchains=&v.swap,.pImageIndices=&idx};
        VkResult pr=vkQueuePresentKHR(v.q,&pi);
        if (pr!=VK_SUCCESS && pr!=VK_SUBOPTIMAL_KHR && pr!=VK_ERROR_OUT_OF_DATE_KHR) CHECK(pr);
        CHECK(vkQueueWaitIdle(v.q));   /* synchronous: drains submit + present so sems/cmd/import reuse next frame is safe */
        presented++;

        vkFreeDescriptorSets(v.dev,v.dpool,1,&ds);
        vkDestroyImageView(v.dev,yview,NULL); vkDestroyImage(v.dev,img,NULL); vkFreeMemory(v.dev,imem,NULL);
        frame_release(&f);   /* unref GstSample (and orig fd) AFTER the GPU is done */
    }
done:;
    double dt = now_s() - t0;
    vkDeviceWaitIdle(v.dev);
    gst_element_set_state(d.pipe, GST_STATE_NULL);
    printf("\n=== SUB-GATE 2b (on-screen zero-copy present) ===\n");
    printf("  presented %d frames in %.2fs = %.1f fps (FIFO/vsync); per-frame readback: NONE\n", presented, dt, presented/dt);
    printf("  RESULT: %s (swapchain present succeeded; visual correctness = user confirms on sway)\n", presented > 0 ? "PASS(present)" : "FAIL");
    int hold = getenv("ZC_HOLD") ? atoi(getenv("ZC_HOLD")) : 2500;   /* keep last frame on screen for the user */
    for (int t = 0; t < hold; t += 50) { SDL_Event e; while (SDL_PollEvent(&e)) {} SDL_Delay(50); }
    return presented > 0 ? 0 : 1;
}
