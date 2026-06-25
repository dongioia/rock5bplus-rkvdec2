/* Phase-C Step-2 finish: A/B measurement — zero-copy vs CPU-copy (vulkandownload).
 * Streams the clip and, per frame, does ONE of:
 *   copy     : import rkvdec dmabuf -> vkCmdCopyImageToBuffer NV12 -> HOST buffer
 *              (== what vulkandownload does every frame: read the decoded frame
 *              back to system memory). This is the cost zero-copy eliminates.
 *   zerocopy : import -> ycbcr fragment-sample render to an offscreen RGBA target
 *              (GPU only, NO readback) — the zero-copy display work.
 * Reports wall-clock fps and CPU time (getrusage utime+stime, whole process incl.
 * the gst decode threads — identical in both modes, so the DELTA is the HOST-SIDE
 * CPU difference: map + full-frame consumer read + cache coherency.
 * IMPORTANT (conservative): PanVK's copyImageToBuffer is itself a GPU compute
 * pass, so the readback's memory-BANDWIDTH cost is NOT in getrusage — the measured
 * CPU saving is a LOWER BOUND (real vulkandownload, with a full memcpy+re-upload
 * downstream, costs more). Per-frame vkWaitForFences serializes both modes, so fps
 * is decode-bound here (pipelined throughput is NOT measured). Off-screen, no FIFO.
 *
 * Build (SBC): cc zc_measure.c -o zc_measure \
 *   $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0) -lvulkan -lm
 * Run: VK_ICD_FILENAMES=$HOME/mesa-zc/panfrost_icd.json ./zc_measure <clip> <copy|zerocopy> [maxframes]
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
#include <time.h>
#include <sys/resource.h>

#define NV12 VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
#define RGBA VK_FORMAT_R8G8B8A8_UNORM
#define CHECK(c) do{VkResult _r=(c); if(_r!=VK_SUCCESS){fprintf(stderr,"FATAL %s:%d -> %d\n",__FILE__,__LINE__,_r);exit(3);} }while(0)
static double wall(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static double cpu(void){struct rusage r;getrusage(RUSAGE_SELF,&r);return r.ru_utime.tv_sec+r.ru_utime.tv_usec*1e-6+r.ru_stime.tv_sec+r.ru_stime.tv_usec*1e-6;}

struct frame { int fd; uint32_t width,height,n_planes,stride[4]; uint64_t offset[4],vis_w,vis_h; GstSample *sample; };
static GstPadProbeReturn meta_probe(GstPad *p,GstPadProbeInfo *i,gpointer u){(void)p;(void)u;GstQuery *q=GST_PAD_PROBE_INFO_QUERY(i);
    if(q&&GST_QUERY_TYPE(q)==GST_QUERY_ALLOCATION&&gst_query_is_writable(q))gst_query_add_allocation_meta(q,GST_VIDEO_META_API_TYPE,NULL);return GST_PAD_PROBE_PASS;}
struct decoder{GstElement *pipe,*sink;};
static int dec_start(struct decoder *d,const char *clip){
    const char *pa=g_str_has_suffix(clip,".h265")?"h265parse":"h264parse",*de=g_str_has_suffix(clip,".h265")?"v4l2slh265dec":"v4l2slh264dec";
    gchar *desc=g_strdup_printf("filesrc location=%s ! %s ! %s ! appsink name=s emit-signals=false max-buffers=8 drop=false sync=false",clip,pa,de);
    d->pipe=gst_parse_launch(desc,NULL);g_free(desc);if(!d->pipe)return 1;d->sink=gst_bin_get_by_name(GST_BIN(d->pipe),"s");
    GstPad *sp=gst_element_get_static_pad(d->sink,"sink");gst_pad_add_probe(sp,GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,meta_probe,NULL,NULL);gst_object_unref(sp);
    return gst_element_set_state(d->pipe,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE;}
static int dec_next(struct decoder *d,struct frame *f){
    GstSample *s=gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink),5*GST_SECOND);if(!s)return 1;
    GstBuffer *b=gst_sample_get_buffer(s);GstMemory *m=gst_buffer_peek_memory(b,0);
    if(!gst_is_dmabuf_memory(m)){gst_sample_unref(s);return 2;}GstVideoMeta *vm=gst_buffer_get_video_meta(b);if(!vm){gst_sample_unref(s);return 2;}
    memset(f,0,sizeof(*f));f->fd=dup(gst_dmabuf_memory_get_fd(m));f->width=vm->width;f->height=vm->height;f->n_planes=vm->n_planes;
    for(guint i=0;i<vm->n_planes&&i<4;i++){f->stride[i]=vm->stride[i];f->offset[i]=vm->offset[i];}f->sample=s;return 0;}

struct vk{VkInstance inst;VkPhysicalDevice pd;VkDevice dev;uint32_t qfam;VkQueue q;VkCommandPool pool;PFN_vkGetMemoryFdPropertiesKHR gmfp;
    VkSamplerYcbcrConversion conv;VkSampler samp;VkDescriptorSetLayout dsl;VkPipelineLayout pll;VkPipeline pipe;VkRenderPass rp;VkDescriptorPool dpool;
    VkImage rt;VkDeviceMemory rtmem;VkImageView rtview;VkFramebuffer fb;VkBuffer stg;VkDeviceMemory stgmem;uint32_t rw,rh;};
static int hx(VkExtensionProperties *e,uint32_t n,const char *s){for(uint32_t i=0;i<n;i++)if(!strcmp(e[i].extensionName,s))return 1;return 0;}
static uint32_t mt(struct vk *v,uint32_t b){VkPhysicalDeviceMemoryProperties m;vkGetPhysicalDeviceMemoryProperties(v->pd,&m);for(uint32_t i=0;i<m.memoryTypeCount;i++)if(b&(1u<<i))return i;exit(3);}
static uint32_t mtp(struct vk *v,uint32_t b,VkMemoryPropertyFlags w){VkPhysicalDeviceMemoryProperties m;vkGetPhysicalDeviceMemoryProperties(v->pd,&m);
    for(uint32_t i=0;i<m.memoryTypeCount;i++)if((b&(1u<<i))&&(m.memoryTypes[i].propertyFlags&w)==w)return i;exit(3);}

static void vk_init(struct vk *v){
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};   /* no validation: measurement */
    CHECK(vkCreateInstance(&ic,NULL,&v->inst));
    uint32_t n=0;vkEnumeratePhysicalDevices(v->inst,&n,NULL);VkPhysicalDevice pd[8];if(n>8)n=8;vkEnumeratePhysicalDevices(v->inst,&n,pd);v->pd=pd[0];
    uint32_t ec=0;vkEnumerateDeviceExtensionProperties(v->pd,NULL,&ec,NULL);VkExtensionProperties *e=calloc(ec,sizeof(*e));vkEnumerateDeviceExtensionProperties(v->pd,NULL,&ec,e);
    const char *need[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME};
    for(size_t i=0;i<sizeof(need)/sizeof(*need);i++)if(!hx(e,ec,need[i])){fprintf(stderr,"missing %s\n",need[i]);exit(2);}free(e);
    uint32_t qc=0;vkGetPhysicalDeviceQueueFamilyProperties(v->pd,&qc,NULL);VkQueueFamilyProperties qp[16];if(qc>16)qc=16;vkGetPhysicalDeviceQueueFamilyProperties(v->pd,&qc,qp);
    v->qfam=0;for(uint32_t i=0;i<qc;i++)if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){v->qfam=i;break;}
    float pr=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=v->qfam,.queueCount=1,.pQueuePriorities=&pr};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures yf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,.samplerYcbcrConversion=VK_TRUE};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&yf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=sizeof(need)/sizeof(*need),.ppEnabledExtensionNames=need};
    CHECK(vkCreateDevice(v->pd,&dc,NULL,&v->dev));vkGetDeviceQueue(v->dev,v->qfam,0,&v->q);
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=v->qfam};
    CHECK(vkCreateCommandPool(v->dev,&pc,NULL,&v->pool));
    v->gmfp=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(v->dev,"vkGetMemoryFdPropertiesKHR");
}
/* zerocopy render target + pipeline (hoisted) */
static void vk_render_setup(struct vk *v,uint32_t W,uint32_t H){
    v->rw=W;v->rh=H;
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=RGBA,.extent={W,H,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(v->dev,&ic,NULL,&v->rt));VkMemoryRequirements mr;vkGetImageMemoryRequirements(v->dev,v->rt,&mr);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mtp(v,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(v->dev,&ma,NULL,&v->rtmem));CHECK(vkBindImageMemory(v->dev,v->rt,v->rtmem,0));
    VkImageViewCreateInfo iv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=v->rt,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=RGBA,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(vkCreateImageView(v->dev,&iv,NULL,&v->rtview));
    VkAttachmentDescription at={.format=RGBA,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar={.attachment=0,.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ar};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&at,.subpassCount=1,.pSubpasses=&sd};
    CHECK(vkCreateRenderPass(v->dev,&rp,NULL,&v->rp));
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=v->rp,.attachmentCount=1,.pAttachments=&v->rtview,.width=W,.height=H,.layers=1};
    CHECK(vkCreateFramebuffer(v->dev,&fb,NULL,&v->fb));
    VkSamplerYcbcrConversionCreateInfo cc={.sType=VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,.format=NV12,.ycbcrModel=VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,.ycbcrRange=VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,.xChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN,.yChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN,.chromaFilter=VK_FILTER_NEAREST};
    CHECK(vkCreateSamplerYcbcrConversion(v->dev,&cc,NULL,&v->conv));
    VkSamplerYcbcrConversionInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,.conversion=v->conv};
    VkSamplerCreateInfo sc={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.pNext=&ci,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(v->dev,&sc,NULL,&v->samp));
    VkDescriptorSetLayoutBinding b={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT,.pImmutableSamplers=&v->samp};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&b};CHECK(vkCreateDescriptorSetLayout(v->dev,&dl,NULL,&v->dsl));
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&v->dsl};CHECK(vkCreatePipelineLayout(v->dev,&pl,NULL,&v->pll));
    FILE *fv=fopen("zc_present_vert.spv","rb"),*ff=fopen("zc_present_frag.spv","rb");if(!fv||!ff){fprintf(stderr,"need zc_present_{vert,frag}.spv\n");exit(3);}
    fseek(fv,0,SEEK_END);long vn=ftell(fv);fseek(fv,0,SEEK_SET);uint32_t *vb=malloc(vn);if(fread(vb,1,vn,fv)!=(size_t)vn)exit(3);fclose(fv);
    fseek(ff,0,SEEK_END);long fn=ftell(ff);fseek(ff,0,SEEK_SET);uint32_t *fbuf=malloc(fn);if(fread(fbuf,1,fn,ff)!=(size_t)fn)exit(3);fclose(ff);
    VkShaderModuleCreateInfo vmc={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vn,.pCode=vb},fmc={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=fn,.pCode=fbuf};
    VkShaderModule sv,sf;CHECK(vkCreateShaderModule(v->dev,&vmc,NULL,&sv));CHECK(vkCreateShaderModule(v->dev,&fmc,NULL,&sf));
    VkPipelineShaderStageCreateInfo st[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=sv,.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=sf,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp={0,0,(float)W,(float)H,0,1};VkRect2D scr={{0,0},{W,H}};
    VkPipelineViewportStateCreateInfo vps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&scr};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba={.colorWriteMask=0xF};VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&ba};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=st,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vps,.pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&cb,.layout=v->pll,.renderPass=v->rp,.subpass=0};
    CHECK(vkCreateGraphicsPipelines(v->dev,VK_NULL_HANDLE,1,&gp,NULL,&v->pipe));vkDestroyShaderModule(v->dev,sv,NULL);vkDestroyShaderModule(v->dev,sf,NULL);free(vb);free(fbuf);
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    CHECK(vkCreateDescriptorPool(v->dev,&dp,NULL,&v->dpool));
}
/* copy-mode staging buffer (hoisted) */
static void vk_copy_setup(struct vk *v,uint32_t W,uint32_t H){
    VkDeviceSize sz=(VkDeviceSize)W*H+(VkDeviceSize)W*(H/2);
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sz,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};CHECK(vkCreateBuffer(v->dev,&bc,NULL,&v->stg));
    VkMemoryRequirements mr;vkGetBufferMemoryRequirements(v->dev,v->stg,&mr);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mtp(v,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(v->dev,&ma,NULL,&v->stgmem));CHECK(vkBindBufferMemory(v->dev,v->stg,v->stgmem,0));
}
static VkImage import_frame(struct vk *v,struct frame *f,VkDeviceMemory *mem,VkImageView *yview){
    VkSubresourceLayout pl[2];memset(pl,0,sizeof(pl));pl[0].offset=f->offset[0];pl[0].rowPitch=f->stride[0];pl[1].offset=f->offset[1];pl[1].rowPitch=f->stride[1];
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm={.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,.drmFormatModifier=0,.drmFormatModifierPlaneCount=f->n_planes,.pPlaneLayouts=pl};
    VkExternalMemoryImageCreateInfo em={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.pNext=&drm,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageUsageFlags usage = yview ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&em,.imageType=VK_IMAGE_TYPE_2D,.format=NV12,.extent={f->width,f->height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,.usage=usage,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;if(vkCreateImage(v->dev,&ic,NULL,&img)!=VK_SUCCESS){close(f->fd);return VK_NULL_HANDLE;}
    VkMemoryFdPropertiesKHR mfp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};CHECK(v->gmfp(v->dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,f->fd,&mfp));
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(v->dev,img,&mr);uint32_t b=mr.memoryTypeBits&mfp.memoryTypeBits;if(!b){close(f->fd);vkDestroyImage(v->dev,img,NULL);return VK_NULL_HANDLE;}
    VkMemoryDedicatedAllocateInfo de={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=img};
    VkImportMemoryFdInfoKHR im={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,.pNext=&de,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=f->fd};
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&im,.allocationSize=mr.size,.memoryTypeIndex=mt(v,b)};
    CHECK(vkAllocateMemory(v->dev,&ma,NULL,mem));CHECK(vkBindImageMemory(v->dev,img,*mem,0));
    if(yview){VkSamplerYcbcrConversionInfo cin={.sType=VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,.conversion=v->conv};
        VkImageViewCreateInfo iv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.pNext=&cin,.image=img,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=NV12,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(v->dev,&iv,NULL,yview));}
    return img;
}
static VkCommandBuffer cbeg(struct vk *v){VkCommandBufferAllocateInfo a={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=v->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer c;CHECK(vkAllocateCommandBuffers(v->dev,&a,&c));VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};CHECK(vkBeginCommandBuffer(c,&b));return c;}
static void cend(struct vk *v,VkCommandBuffer c){CHECK(vkEndCommandBuffer(c));VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&c};
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence f;CHECK(vkCreateFence(v->dev,&fi,NULL,&f));CHECK(vkQueueSubmit(v->q,1,&s,f));CHECK(vkWaitForFences(v->dev,1,&f,VK_TRUE,UINT64_MAX));vkDestroyFence(v->dev,f,NULL);vkFreeCommandBuffers(v->dev,v->pool,1,&c);}

int main(int argc,char **argv){
    if(argc<3){fprintf(stderr,"usage: %s <clip> <copy|zerocopy> [maxframes]\n",argv[0]);return 2;}
    int zerocopy = !strcmp(argv[2],"zerocopy");
    int maxf = argc>3 ? atoi(argv[3]) : 100000;
    gst_init(&argc,&argv);
    struct vk v;memset(&v,0,sizeof(v));vk_init(&v);
    struct decoder d;if(dec_start(&d,argv[1])){fprintf(stderr,"decode fail\n");return 1;}
    int setup=0,presented=0;struct frame f;double t0=0,c0=0;
    while(presented<maxf && dec_next(&d,&f)==0){
        if(!setup){ if(zerocopy)vk_render_setup(&v,f.width,f.height); else vk_copy_setup(&v,f.width,f.height); setup=1; t0=wall(); c0=cpu(); }
        VkDeviceMemory mem;VkImageView yview=VK_NULL_HANDLE;
        VkImage img=import_frame(&v,&f,&mem,zerocopy?&yview:NULL);
        if(img==VK_NULL_HANDLE){gst_sample_unref(f.sample);break;}
        VkCommandBuffer c=cbeg(&v);
        if(zerocopy){
            VkImageMemoryBarrier ab={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&ab);
            VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=v.dpool,.descriptorSetCount=1,.pSetLayouts=&v.dsl};VkDescriptorSet ds;CHECK(vkAllocateDescriptorSets(v.dev,&da,&ds));
            VkDescriptorImageInfo di={.sampler=v.samp,.imageView=yview,.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&di};vkUpdateDescriptorSets(v.dev,1,&w,0,NULL);
            VkClearValue cv={.color={{0,0,0,1}}};VkRenderPassBeginInfo rb={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=v.rp,.framebuffer=v.fb,.renderArea={{0,0},{v.rw,v.rh}},.clearValueCount=1,.pClearValues=&cv};
            vkCmdBeginRenderPass(c,&rb,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,v.pipe);vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,v.pll,0,1,&ds,0,NULL);vkCmdDraw(c,3,1,0,0);vkCmdEndRenderPass(c);
            cend(&v,c);vkFreeDescriptorSets(v.dev,v.dpool,1,&ds);vkDestroyImageView(v.dev,yview,NULL);
        } else {
            VkImageMemoryBarrier ab={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ab);
            VkBufferImageCopy cp[2]={{.bufferOffset=0,.imageSubresource={VK_IMAGE_ASPECT_PLANE_0_BIT,0,0,1},.imageExtent={f.width,f.height,1}},
                {.bufferOffset=(VkDeviceSize)f.width*f.height,.imageSubresource={VK_IMAGE_ASPECT_PLANE_1_BIT,0,0,1},.imageExtent={f.width/2,f.height/2,1}}};
            vkCmdCopyImageToBuffer(c,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,v.stg,2,cp);
            cend(&v,c);
            /* read the WHOLE NV12 buffer (both planes, every cache line) like a real
             * consumer that hands the frame to the next stage — still conservative vs a
             * real memcpy/upload (this only reads). vulkandownload pays at least this. */
            void *m;CHECK(vkMapMemory(v.dev,v.stgmem,0,VK_WHOLE_SIZE,0,&m));volatile uint8_t s=0;const uint8_t *p=m;
            size_t sz=(size_t)f.width*f.height+(size_t)f.width*(f.height/2);for(size_t i=0;i<sz;i+=64)s+=p[i];(void)s;vkUnmapMemory(v.dev,v.stgmem);
        }
        vkDestroyImage(v.dev,img,NULL);vkFreeMemory(v.dev,mem,NULL);gst_sample_unref(f.sample);
        presented++;
    }
    double dt=wall()-t0, dc=cpu()-c0;
    gst_element_set_state(d.pipe,GST_STATE_NULL);
    printf("MODE=%-8s frames=%d wall=%.3fs fps=%.1f cpu=%.3fs cpu_ms/frame=%.3f cpu_util=%.1f%%\n",
           argv[2],presented,dt,presented/dt,dc,1000.0*dc/presented,100.0*dc/dt);
    return presented>0?0:1;
}
