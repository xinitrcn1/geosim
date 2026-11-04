#version 450
// The Politics window's inset map (material::electoral, drawn by map_renderer):
// the region in the electoral ID image at this pixel (XOR codes, map_elect.vert)
// filled with its electoral-mode colour, white lines between regions and dark
// grey coasts; empty pixels stay transparent (the white window shows).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform usampler2D u_elect;
layout(std430, set = 0, binding = 1) readonly buffer colours {
    uint c[]; // RGBA8 per raster region id
} col;

// quad.vert's outputs (unused here, declared so the interfaces match).
layout(location = 0) in vec2 v_local;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in vec4 v_border_color;
layout(location = 4) flat in vec4 v_rect;
layout(location = 5) flat in float v_border_width;
layout(location = 6) flat in uint v_flags;
layout(location = 7) flat in vec4 v_params;

layout(location = 0) out vec4 o_color;

uint region_at(ivec2 p) {
    ivec2 sz = textureSize(u_elect, 0);
    if(p.x < 0 || p.y < 0 || p.x >= sz.x || p.y >= sz.y) return 0u;
    uint code = texelFetch(u_elect, p, 0).r;
    uint id = code & 0xFFFFu;
    if((code & 0x80000000u) != 0u || id == 0u || (code >> 16) != (((id * 2654435761u) >> 17) & 0x7FFFu)) return 0u;
    return id;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint me = region_at(p);
    uint r = region_at(p + ivec2(1, 0)), d = region_at(p + ivec2(0, 1));
    uint l = region_at(p - ivec2(1, 0)), u = region_at(p - ivec2(0, 1));
    bool coast = (me == 0u) != (r == 0u) || (me == 0u) != (d == 0u) || (me == 0u) != (l == 0u) || (me == 0u) != (u == 0u);
    if(me == 0u) {
        if(!coast) discard;
        o_color = vec4(0.36, 0.41, 0.47, 1.0);
        return;
    }
    if(coast) {
        o_color = vec4(0.36, 0.41, 0.47, 1.0);
        return;
    }
    vec4 fill = me < uint(col.c.length()) ? unpackUnorm4x8(col.c[me]) : vec4(0.84, 0.86, 0.9, 1.0);
    bool border = (r != me && r != 0u) || (d != me && d != 0u);
    o_color = border ? vec4(1.0) : vec4(fill.rgb, 1.0);
}
