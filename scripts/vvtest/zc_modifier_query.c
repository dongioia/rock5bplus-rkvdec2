/* Stage-0b query: does this PanVK advertise importing NV12 with a given DRM
 * modifier (we care about LINEAR=0) + SAMPLED + ycbcr-conversion features?
 * Build on the SBC: cc zc_modifier_query.c -o zc_modifier_query -lvulkan
 * Run:  VK_ICD_FILENAMES=$HOME/mesa-zc/panfrost_icd.json ./zc_modifier_query */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdint.h>

#define NV12 VK_FORMAT_G8_B8R8_2PLANE_420_UNORM

int main(void)
{
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &ai};
    VkInstance inst;
    if (vkCreateInstance(&ici, 0, &inst) != VK_SUCCESS) { printf("FAIL createInstance\n"); return 2; }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, 0);
    if (!n) { printf("FAIL: 0 physical devices\n"); return 2; }
    VkPhysicalDevice pds[8]; if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
    printf("device: %s  driverVersion=0x%x\n", p.deviceName, p.driverVersion);

    /* two-call: count, then fill */
    VkDrmFormatModifierPropertiesListEXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
    vkGetPhysicalDeviceFormatProperties2(pd, NV12, &fp);
    uint32_t mc = ml.drmFormatModifierCount;
    printf("NV12 advertised DRM modifiers: %u\n", mc);
    if (!mc) { printf("STAGE0B: FAIL (no modifiers advertised for NV12)\n"); return 1; }
    VkDrmFormatModifierPropertiesEXT mods[64]; if (mc > 64) mc = 64;
    ml.pDrmFormatModifierProperties = mods;
    ml.drmFormatModifierCount = mc;
    vkGetPhysicalDeviceFormatProperties2(pd, NV12, &fp);

    int linear_sampled = 0, linear_ycbcr = 0;
    for (uint32_t i = 0; i < mc; i++) {
        VkFormatFeatureFlags f = mods[i].drmFormatModifierTilingFeatures;
        int sampled = !!(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        int yc = !!(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT);
        int mid = !!(f & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT);
        int cos = !!(f & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT);
        printf("  mod 0x%016llx planes=%u sampled=%d ycbcrLinFilter=%d midpoint=%d cosited=%d feats=0x%08x\n",
               (unsigned long long)mods[i].drmFormatModifier, mods[i].drmFormatModifierPlaneCount,
               sampled, yc, mid, cos, f);
        if (mods[i].drmFormatModifier == 0 /* DRM_FORMAT_MOD_LINEAR */) {
            linear_sampled = sampled;
            linear_ycbcr = yc;
        }
    }

    /* the actual import tuple: NV12 + LINEAR + SAMPLED + DRM_MODIFIER tiling */
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT di = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = 0, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceImageFormatInfo2 ifi = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &di,
        .format = NV12, .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageFormatProperties2 ifp = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);

    printf("\nLINEAR(0): sampled=%d ycbcr-conversion=%d ; image-format tuple(LINEAR,SAMPLED)=%s\n",
           linear_sampled, linear_ycbcr, r == VK_SUCCESS ? "SUPPORTED" : "NOT-SUPPORTED");
    if (linear_sampled && r == VK_SUCCESS)
        printf("STAGE0B: PASS%s\n", linear_ycbcr ? " (HW-YUV sampling features present)" : " — but ycbcr features ABSENT on LINEAR -> 1b via SW-CSC");
    else
        printf("STAGE0B: FAIL (LINEAR not importable+sampled)\n");
    vkDestroyInstance(inst, 0);
    return 0;
}
