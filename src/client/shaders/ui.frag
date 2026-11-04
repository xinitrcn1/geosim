#version 450
// The UI material: flat rectangles, frosted glass, glyphs and the scene copy.
// Square corners only; an optional inner border of v_border_width pixels.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_atlas; // R8 glyph coverage
layout(set = 0, binding = 1) uniform sampler2D u_blur;  // blurred scene (quarter resolution)
layout(set = 0, binding = 2) uniform sampler2D u_scene; // full-resolution scene (map + opaque UI)

layout(location = 0) in vec2 v_local;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in vec4 v_border_color;
layout(location = 4) flat in vec4 v_rect;
layout(location = 5) flat in float v_border_width;
layout(location = 6) flat in uint v_flags;
layout(location = 7) flat in vec4 v_params;

layout(location = 0) out vec4 o_color;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    uint mode = v_flags & 0xFFu;
    vec2 screen_uv = gl_FragCoord.xy / pc.viewport;
    vec4 c;
    if(mode == mode_frosted) {
        vec3 backdrop = texture(u_blur, screen_uv).rgb;
        // Frosted glass: the blurred backdrop, slightly desaturated and
        // dimmed, under a light cool tint (colour.a is the tint strength),
        // with a faint sheen towards the top edge. A little dither hides
        // banding from the quarter-resolution blur.
        float luma = dot(backdrop, vec3(0.299, 0.587, 0.114));
        backdrop = mix(backdrop, vec3(luma), 0.18) * 0.90;
        vec3 tinted = mix(backdrop, v_color.rgb, v_color.a);
        tinted += 0.035 * (1.0 - clamp(v_local.y / max(v_rect.w, 1.0), 0.0, 1.0));
        tinted += (hash(gl_FragCoord.xy) - 0.5) / 255.0;
        c = vec4(tinted, 1.0);
    } else if(mode == mode_glyph) {
        float coverage = texture(u_atlas, v_uv).r;
        c = vec4(v_color.rgb, v_color.a * coverage);
    } else if(mode == mode_arc) {
        // A sector of the disc inscribed in the rect (pie slices, hemicycle arcs): angles in
        // turns counter-clockwise from +x (screen y up), an inner radius for rings and a gap
        // in pixels on both straight edges; antialiased over one pixel.
        vec2 halfsz = v_rect.zw * 0.5;
        vec2 p = v_local - halfsz;
        p.y = -p.y;
        float R = min(halfsz.x, halfsz.y);
        float r = length(p);
        float cov = clamp(R - r + 0.5, 0.0, 1.0) * clamp(r - v_params.z * R + 0.5, 0.0, 1.0);
        float span = v_params.y - v_params.x;
        if(span < 0.9999) {
            float t = atan(p.y, p.x) / 6.2831853;
            float d0 = fract(t - v_params.x);
            float ang = d0 <= span ? min(d0, span - d0) : -min(d0 - span, 1.0 - d0);
            cov *= clamp(ang * 6.2831853 * r - v_params.w + 0.5, 0.0, 1.0);
        }
        c = vec4(v_color.rgb, v_color.a * cov);
    } else if(mode == mode_scene) {
        c = vec4(texture(u_scene, screen_uv).rgb, 1.0);
    } else {
        c = v_color;
    }
    if(v_border_width > 0.0) {
        vec2 d = min(v_local, v_rect.zw - v_local);
        if(min(d.x, d.y) < v_border_width) {
            c.rgb = mix(c.rgb, v_border_color.rgb, v_border_color.a);
            c.a = max(c.a, v_border_color.a);
        }
    }
    o_color = c;
}
