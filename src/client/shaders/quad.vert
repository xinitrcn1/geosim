#version 450
// Instanced rectangle: one instance = one UI element (panel, button, glyph...).
// Shared by every quad material; materials differ only in the fragment shader.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec4 i_rect;         // x, y, w, h (pixels, top-left origin)
layout(location = 1) in vec4 i_uv;           // u0, v0, u1, v1 (atlas) or material parameters
layout(location = 2) in vec4 i_color;        // RGBA8 unorm
layout(location = 3) in vec4 i_border_color; // RGBA8 unorm
layout(location = 4) in float i_border_width; // pixels; 0 = none
layout(location = 5) in uint i_flags;

layout(location = 0) out vec2 v_local;       // pixel offset inside the rect
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_color;
layout(location = 3) flat out vec4 v_border_color;
layout(location = 4) flat out vec4 v_rect;
layout(location = 5) flat out float v_border_width;
layout(location = 6) flat out uint v_flags;
layout(location = 7) flat out vec4 v_params;

const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));

void main() {
    vec2 c = corners[gl_VertexIndex];
    vec2 p = i_rect.xy + c * i_rect.zw;
    gl_Position = vec4(p / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
    v_local = c * i_rect.zw;
    v_uv = mix(i_uv.xy, i_uv.zw, c);
    v_color = i_color;
    v_border_color = i_border_color;
    v_rect = i_rect;
    v_border_width = i_border_width;
    v_flags = i_flags;
    v_params = i_uv;
}
