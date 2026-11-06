#version 450
// The flag material (docs/08-ui.md#flags): one quad per flag, its primitives evaluated per
// pixel in painter's order from the flag buffers (core/flags.hpp compile()), antialiased
// over one pixel by signed distance. Params: x = first primitive, y = count, z = opacity.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

struct prim {
    uint kind; // 0 polygon, 1 disc / ring, 2 crescent
    uint colour;
    uint first;
    uint count;
    vec4 a;
    vec4 b;
    vec4 box; // bounds and clip rectangle
};
layout(std430, set = 0, binding = 3) readonly buffer prim_buffer { prim prims[]; };
layout(std430, set = 0, binding = 4) readonly buffer vert_buffer { vec2 verts[]; };

layout(location = 0) in vec2 v_local;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in vec4 v_border_color;
layout(location = 4) flat in vec4 v_rect;
layout(location = 5) flat in float v_border_width;
layout(location = 6) flat in uint v_flags;
layout(location = 7) flat in vec4 v_params;

layout(location = 0) out vec4 o_color;

// Signed distance to a polygon (negative inside, even-odd rule; Quilez).
float sd_poly(uint first, uint n, vec2 p) {
    vec2 v0 = verts[first];
    float d = dot(p - v0, p - v0);
    float s = 1.0;
    for(uint i = 0u, j = n - 1u; i < n; j = i, ++i) {
        vec2 vi = verts[first + i];
        vec2 vj = verts[first + j];
        vec2 e = vj - vi;
        vec2 w = p - vi;
        float ee = max(dot(e, e), 1e-12);
        vec2 q = w - e * clamp(dot(w, e) / ee, 0.0, 1.0);
        d = min(d, dot(q, q));
        bvec3 c = bvec3(p.y >= vi.y, p.y < vj.y, e.x * w.y > e.y * w.x);
        if(all(c) || all(not(c))) s = -s;
    }
    return s * sqrt(d);
}

void main() {
    float h = max(v_rect.w, 1.0);
    vec2 p = v_local / h;  // flag units: height 1
    float px = 1.0 / h;    // one pixel in flag units
    uint first = uint(v_params.x + 0.5);
    uint count = uint(v_params.y + 0.5);
    vec4 acc = vec4(0.0); // premultiplied
    for(uint i = first; i < first + count; ++i) {
        prim q = prims[i];
        if(p.x < q.box.x - px || p.y < q.box.y - px || p.x > q.box.z + px || p.y > q.box.w + px) continue;
        float d;
        if(q.kind == 1u) {
            float r = length(p - q.a.xy);
            d = q.a.w > 0.0 ? abs(r - 0.5 * (q.a.z + q.a.w)) - 0.5 * (q.a.z - q.a.w) : r - q.a.z;
        } else if(q.kind == 2u) {
            d = max(length(p - q.a.xy) - q.a.z, q.b.z - length(p - q.b.xy));
        } else {
            d = sd_poly(q.first, q.count, p);
        }
        // Clip to the box (the bounds, or an inset's rectangle): exact for shapes inside it.
        vec2 out_of_box = max(q.box.xy - p, p - q.box.zw);
        d = max(d, max(out_of_box.x, out_of_box.y));
        float cov = clamp(0.5 - d / px, 0.0, 1.0);
        if(cov <= 0.0) continue;
        vec4 c = unpackUnorm4x8(q.colour);
        c.a *= cov;
        acc = vec4(c.rgb * c.a, c.a) + acc * (1.0 - c.a);
    }
    // A faint inner edge so white and pale flags keep their outline on light pages.
    vec2 edge = min(v_local, v_rect.zw - v_local);
    if(min(edge.x, edge.y) < 1.0 && acc.a > 0.0) acc.rgb *= 0.82;
    float alpha = acc.a * v_params.z;
    o_color = vec4(acc.a > 0.0 ? acc.rgb / acc.a : vec3(0.0), alpha);
}
