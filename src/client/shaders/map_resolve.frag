#version 450
// Vector-map resolve pass: the XOR ID image -> one region per pixel (R32_UINT:
// 0 water, 0xFFFF land of no region, else the raster region id).
//  - Detail: the lookup is displaced by a deterministic fractal noise in world
//    space (amplitude ~ the simplification tolerance on screen), so simplified
//    borders gain a natural roughness when zoomed in. Fill, borders and
//    picking all read this image, so they stay aligned and crack-free.
//  - Overlaps (two codes XORed: the checksum fails) and cracks (no code) between
//    land regions take the nearest valid region within a few pixels when there
//    is one on both sides; otherwise the coarse raster decides between water
//    and land of no region (Antarctica).

layout(set = 0, binding = 0) uniform usampler2D u_ids;    // XOR ID image
layout(set = 0, binding = 1) uniform usampler2D u_raster; // region raster (fallback)
layout(set = 0, binding = 2) uniform sampler2D u_terrain;  // a = relief code (>= 128: land)

layout(push_constant) uniform resolve_block {
    vec2 viewport;
    vec2 centre;
    vec4 p; // focus, w: scale
    vec4 e; // east, w: simplification tolerance (km)
    vec4 n; // north, w: unused
    uvec4 raster; // w, h
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out uint o_region;

const float PI = 3.14159265358979;
const uint unclaimed = 0xFFFFu;

uint decode(uint c) {
    if((c & 0x80000000u) != 0u) return 0xFFFFFFFEu; // lake
    uint id = c & 0xFFFFu;
    if(id == 0u || (c >> 16) != (((id * 2654435761u) >> 17) & 0x7FFFu)) return 0xFFFFFFFFu; // empty or clash
    return id;
}

uint fetch(ivec2 q) {
    ivec2 sz = textureSize(u_ids, 0);
    if(q.x < 0 || q.y < 0 || q.x >= sz.x || q.y >= sz.y) return 0xFFFFFFFFu;
    return decode(texelFetch(u_ids, q, 0).r);
}

float hash3(vec3 q) {
    q = fract(q * 0.3183099 + vec3(0.71, 0.113, 0.419));
    q *= 17.0;
    return fract(q.x * q.y * q.z * (q.x + q.y + q.z));
}
float vnoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm3(vec3 x) {
    float s = 0.0, a = 0.5;
    for(int k = 0; k < 3; ++k) {
        s += a * vnoise(x);
        x = x * 2.1 + 3.7;
        a *= 0.5;
    }
    return s / 0.875;
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec2 d = gl_FragCoord.xy - pc.centre;
    float scale = pc.p.w;
    float u = mod(d.x / scale + PI, 2.0 * PI) - PI;
    float v = clamp(-d.y / scale, -9.0, 9.0);
    float z = tanh(v), r = 1.0 / cosh(v);
    vec3 x = r * cos(u) * pc.p.xyz + r * sin(u) * pc.e.xyz + z * pc.n.xyz;
    // Displace by up to half the tolerance (on screen), at a wavelength of a few tolerances.
    float px_per_km = scale * cosh(v) / 6371.0;
    float amp = min(10.0, 0.6 * pc.e.w * px_per_km);
    ivec2 q = px;
    if(amp > 0.75) {
        float k = 6371.0 / max(0.1, 2.5 * pc.e.w);
        vec2 off = vec2(fbm3(x * k), fbm3(x * k + vec3(17.3, 5.1, 9.7))) * 2.0 - 1.0;
        q += ivec2(round(off * amp));
        q = clamp(q, ivec2(0), textureSize(u_ids, 0) - 1); // the edges of the screen are not water
    }
    uint c = fetch(q);
    if(c == 0xFFFFFFFEu) { // lake
        o_region = 0u;
        return;
    }
    if(c != 0xFFFFFFFFu) {
        o_region = c;
        return;
    }
    // Crack or overlap between regions: the nearest region if there is land on both sides.
    const ivec2 dirs[4] = ivec2[](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    uint found[4] = uint[](0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    int dist[4] = int[](99, 99, 99, 99);
    for(int k = 0; k < 4; ++k) {
        for(int s = 1; s <= 4; ++s) {
            uint t = fetch(q + dirs[k] * s);
            if(t != 0xFFFFFFFFu && t != 0xFFFFFFFEu) {
                found[k] = t;
                dist[k] = s;
                break;
            }
        }
    }
    bool across = (found[0] != 0xFFFFFFFFu && found[1] != 0xFFFFFFFFu) || (found[2] != 0xFFFFFFFFu && found[3] != 0xFFFFFFFFu);
    if(across) {
        int best = 0;
        for(int k = 1; k < 4; ++k) if(dist[k] < dist[best]) best = k;
        o_region = found[best];
        return;
    }
    // Otherwise the coarse data: where the relief says land, the raster's region (wide
    // cracks between nations' polygons from different sources) or land of no region;
    // else water (the polygon coast wins).
    float lon = atan(x.y, x.x), lat = asin(clamp(x.z, -1.0, 1.0));
    vec2 g = vec2(lon / (2.0 * PI) + 0.5, 0.5 - lat / PI);
    ivec2 rc = ivec2(int(clamp(g.x * float(pc.raster.x), 0.0, float(pc.raster.x - 1u))), int(clamp(g.y * float(pc.raster.y), 0.0, float(pc.raster.y - 1u))));
    uint rr = texelFetch(u_raster, rc, 0).r;
    bool land = textureLod(u_terrain, g, 0.0).a * 255.0 >= 127.5;
    o_region = rr == unclaimed ? unclaimed : (land ? rr : 0u);
}
