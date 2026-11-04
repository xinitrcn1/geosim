#version 450
// The world map (docs/07-map-and-filters.md#rendering), one fullscreen
// triangle drawn first into the scene image (so the frosted panels blur it).
//
// Per pixel: inverse spherical oblique Mercator (map_view.hpp) -> lon/lat ->
//  - terrain:  rgb = Blue-Marble-like land colour, a = relief code (sea depth ramp)
//  - region:   the region-ID raster around the point, at the mip level where a
//              cell is about one pixel; the region is the ID with the largest
//              bilinear weight, and the distance (in pixels) to the contour
//              where that weight crosses 1/2 gives smooth, constant-width lines:
//              thin for region borders, thick where the nation key changes
//              (national borders; water and unclaimed land have their own keys,
//              so coastlines are thick too)
//  - fill:     the controlling nation's colour from the region table, blended
//              over the terrain (political filter). Flat colour, no lighting.

layout(set = 0, binding = 0) uniform sampler2D u_terrain;
layout(set = 0, binding = 1) uniform usampler2D u_ids;
// Indexed by raster ID (0 = water): x = nation key (borders), y = fill RGBA8 (a = strength).
layout(std430, set = 0, binding = 2) readonly buffer region_table {
    uvec2 info[];
} rt;

layout(push_constant) uniform map_block {
    vec2 viewport;  // pixels
    vec2 centre;    // pixels (the focus)
    vec4 p;         // xyz: focus unit vector, w: scale (pixels per radian)
    vec4 e;         // xyz: local east, w: line scale (UI scale)
    vec4 n;         // xyz: local north, w: unused
    uint flags;     // bit 0: political fill
    uint id_levels; // mip levels of u_ids
    uint id_w, id_h;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

const float PI = 3.14159265358979;
const uint unclaimed = 0xFFFFu;
const uint key_water = 0u;
const uint key_unclaimed = 0xFFFFFFFEu;

uint nation_key(uint id) {
    if(id == 0u) return key_water;
    if(id == unclaimed || id >= uint(rt.info.length())) return key_unclaimed;
    return rt.info[id].x;
}

// Ocean colour from the relief code (build_map.py: 0..127 sea, depth = 11000 m x ((127 - c) / 127)^2).
vec3 ocean(float code) {
    float t = clamp((127.0 - code * 255.0) / 127.0, 0.0, 1.0);
    float depth = 11000.0 * t * t;
    const float d[7] = float[](0.0, 120.0, 500.0, 1800.0, 3800.0, 5800.0, 9000.0);
    const vec3 c[7] = vec3[](vec3(0.388, 0.627, 0.788), vec3(0.282, 0.533, 0.718), vec3(0.200, 0.427, 0.635),
                             vec3(0.137, 0.333, 0.545), vec3(0.102, 0.267, 0.471), vec3(0.078, 0.212, 0.400),
                             vec3(0.055, 0.157, 0.322));
    vec3 col = c[6];
    for(int i = 0; i < 6; ++i) {
        if(depth < d[i + 1]) {
            col = mix(c[i], c[i + 1], smoothstep(0.0, 1.0, (depth - d[i]) / (d[i + 1] - d[i])));
            break;
        }
    }
    return col;
}

// Distance in pixels from the pixel to the 1/2-contour of the bilinear
// indicator `ind` (weights w, corner order 00, 10, 01, 11); huge when uniform.
float contour_distance(vec4 ind, vec4 w, vec2 f, vec2 dx, vec2 dy) {
    float s = dot(ind, w);
    vec2 g = vec2((ind.y - ind.x) * (1.0 - f.y) + (ind.w - ind.z) * f.y, (ind.z - ind.x) * (1.0 - f.x) + (ind.w - ind.y) * f.x);
    vec2 gs = vec2(dot(g, dx), dot(g, dy));
    float len = length(gs);
    if(len < 1e-6) return 1e6;
    return abs(s - 0.5) / len;
}

void main() {
    // Screen -> oblique Mercator (u, v) -> unit vector -> lon/lat.
    vec2 d = gl_FragCoord.xy - pc.centre;
    float scale = pc.p.w;
    float u = mod(d.x / scale + PI, 2.0 * PI) - PI; // the map repeats east-west
    float v = clamp(-d.y / scale, -9.0, 9.0);
    float z = tanh(v);
    float r = 1.0 / cosh(v);
    vec3 x = r * cos(u) * pc.p.xyz + r * sin(u) * pc.e.xyz + z * pc.n.xyz;
    float lon = atan(x.y, x.x);
    float lat = asin(clamp(x.z, -1.0, 1.0));
    vec2 g = vec2(lon / (2.0 * PI) + 0.5, 0.5 - lat / PI);

    // Screen derivatives of the texture coordinate, unwrapped across the antimeridian.
    vec2 gx = dFdx(g), gy = dFdy(g);
    gx.x -= round(gx.x);
    gy.x -= round(gy.x);

    vec4 terrain = textureGrad(u_terrain, g, gx, gy);

    // Region-ID level: about one cell per pixel.
    vec2 size0 = vec2(float(pc.id_w), float(pc.id_h));
    float cells_per_px = max(length(gx * size0), length(gy * size0));
    int level = int(clamp(ceil(log2(max(cells_per_px, 1e-6)) - 0.15), 0.0, float(pc.id_levels - 1u)));
    ivec2 isz = ivec2(int(pc.id_w) >> level, int(pc.id_h) >> level);
    vec2 sz = vec2(isz);
    vec2 tl = g * sz - 0.5;
    ivec2 i0 = ivec2(floor(tl));
    vec2 f = tl - vec2(i0);
    uint id[4];
    for(int k = 0; k < 4; ++k) {
        ivec2 c = i0 + ivec2(k & 1, k >> 1);
        c.x = ((c.x % isz.x) + isz.x) % isz.x;
        c.y = clamp(c.y, 0, isz.y - 1);
        id[k] = texelFetch(u_ids, c, level).r;
    }
    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    uint key[4] = uint[](nation_key(id[0]), nation_key(id[1]), nation_key(id[2]), nation_key(id[3]));

    // The region and nation with the largest bilinear weight.
    uint me = id[0], my_key = key[0];
    float best = -1.0, best_key = -1.0;
    for(int k = 0; k < 4; ++k) {
        float s = 0.0, sk = 0.0;
        for(int j = 0; j < 4; ++j) {
            s += id[j] == id[k] ? w[j] : 0.0;
            sk += key[j] == key[k] ? w[j] : 0.0;
        }
        if(s > best) { best = s; me = id[k]; }
        if(sk > best_key) { best_key = sk; my_key = key[k]; }
    }
    vec4 in_region = vec4(id[0] == me, id[1] == me, id[2] == me, id[3] == me);
    vec4 in_nation = vec4(key[0] == my_key, key[1] == my_key, key[2] == my_key, key[3] == my_key);
    vec2 dx = gx * sz, dy = gy * sz;
    float d_region = contour_distance(in_region, w, f, dx, dy);
    float d_nation = contour_distance(in_nation, w, f, dx, dy);

    // Colour: water by depth, land by terrain, plus the nation fill.
    vec3 col;
    if(me == 0u) {
        col = ocean(terrain.a);
    } else {
        col = terrain.rgb;
        if(me != unclaimed && (pc.flags & 1u) != 0u && me < uint(rt.info.length())) {
            // Tinted relief: the nation's hue at the terrain's lightness (deserts
            // lighter, forests darker), fading out on snow and ice so the
            // Himalaya and the ice caps stay white.
            vec4 fill = unpackUnorm4x8(rt.info[me].y);
            float luma = dot(col, vec3(0.299, 0.587, 0.114));
            vec3 tinted = min(fill.rgb * clamp(0.72 + 0.8 * luma, 0.75, 1.2), vec3(1.0));
            col = mix(col, tinted, fill.a * (1.0 - smoothstep(0.72, 0.9, luma)));
        }
    }

    // Borders: thick black for nations and coasts, thin for regions (fainter when zoomed out).
    float line_scale = max(pc.e.w, 1.0);
    float thick = clamp(1.0 * line_scale + 0.5 - d_nation, 0.0, 1.0);
    float thin = clamp(0.5 * line_scale + 0.5 - d_region, 0.0, 1.0) * 0.55 * pow(0.6, float(level));
    float ink = max(thick * 0.9, thin);
    col = mix(col, vec3(0.0), ink);
    o_color = vec4(col, 1.0);
}
