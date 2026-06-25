#version 450
/* Fullscreen triangle, but scale uv by visible/coded so the swapchain shows only
 * the VISIBLE region (crops rkvdec coded padding rows, e.g. 1080 of 1088). */
layout(push_constant) uniform PC { vec2 uvscale; } pc;
layout(location = 0) out vec2 uv;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p * pc.uvscale;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
