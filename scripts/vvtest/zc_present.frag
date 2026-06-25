#version 450
/* Sample the imported NV12 through the immutable VkSamplerYcbcrConversion bound
 * at set0/binding0 — texture() returns RGB already converted by the HW ycbcr
 * sampler. Step-2 sub-gate 2a: this is the GRAPHICS (fragment) ycbcr path, the
 * one a swapchain present uses, vs Stage-1's compute path. */
layout(set = 0, binding = 0) uniform sampler2D ytex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 col;
void main()
{
    col = texture(ytex, uv);
}
