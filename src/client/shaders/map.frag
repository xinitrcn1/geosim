#version 450
// Placeholder for the world map (docs/07): a procedural ocean / land pattern
// with a graticule, so the frosted panels have something to blur. It is also
// the reference example of a per-element material: same vertex shader and
// instance format as the UI, a different fragment shader
// (ui_renderer.cpp: material table). Future charts follow the same pattern.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 v_local;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in vec4 v_border_color;
layout(location = 4) flat in vec4 v_rect;
layout(location = 5) flat in float v_border_width;
layout(location = 6) flat in uint v_flags;
layout(location = 7) flat in vec4 v_params; // unused for now (future: pan x, pan y, zoom)

layout(location = 0) out vec4 o_color;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for(int k = 0; k < 5; ++k) {
        v += a * noise(p);
        p *= 2.03;
        a *= 0.5;
    }
    return v;
}

void main() {
    // Equirectangular-like coordinates: x in [0, 360), y in [-90, 90].
    vec2 size = max(v_rect.zw, vec2(1.0));
    float aspect = size.x / size.y;
    vec2 q = v_local / size;
    vec2 geo = vec2(q.x * 360.0, (0.5 - q.y) * 180.0 * 2.0 / aspect);

    float h = fbm(geo / 38.0 + vec2(3.1, 7.7));
    float land = smoothstep(0.52, 0.55, h);

    vec3 ocean_deep = vec3(0.065, 0.125, 0.220);
    vec3 ocean_shelf = vec3(0.10, 0.20, 0.32);
    vec3 ocean = mix(ocean_deep, ocean_shelf, smoothstep(0.35, 0.52, h));
    vec3 ground = mix(vec3(0.22, 0.31, 0.27), vec3(0.38, 0.40, 0.35), smoothstep(0.55, 0.75, h));
    vec3 c = mix(ocean, ground, land);

    // Graticule every 15 degrees, one pixel wide.
    vec2 g = abs(fract(geo / 15.0 + 0.5) - 0.5) * 15.0;
    vec2 px_per_deg = size / vec2(360.0, 360.0 / aspect);
    vec2 line = 1.0 - smoothstep(vec2(0.0), vec2(1.0) / px_per_deg, g);
    c = mix(c, vec3(0.55, 0.70, 0.90), 0.12 * max(line.x, line.y));

    o_color = vec4(c, 1.0);
}
