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
//  - clouds:   translucent flat cloud cover: 3D value-noise fbm on the sphere,
//              advected by climatological zonal winds (trades, westerlies, polar
//              easterlies) with a two-phase flow map, thresholded by a cover
//              climatology (ITCZ following the season, storm tracks, clear
//              subtropical highs)
//  - cyclones: per storm in the hazard UBO, logarithmic spiral bands, a central
//              dense overcast and a clear eye (hurricane strength), rotating
//              anticlockwise in the north and clockwise in the south
//  - quakes:   news-style rings of red circles expanding from the epicentre,
//              sized by magnitude, fading over the days after the event

layout(set = 0, binding = 0) uniform sampler2D u_terrain;
layout(set = 0, binding = 1) uniform usampler2D u_ids;
// Indexed by raster ID (0 = water): x = nation key (borders), y = fill RGBA8 (a = strength).
layout(std430, set = 0, binding = 2) readonly buffer region_table {
    uvec2 info[];
} rt;

// Hazards (map_renderer::set_hazards).
layout(std140, set = 0, binding = 3) uniform hazard_block {
    vec4 storm[64]; // lon, lat (deg), max wind (m/s), gale radius (km)
    vec4 quake[32]; // lon, lat (deg), Mw, age in sim days
    vec4 quake_t[32]; // x: seconds since shown (animation)
} hz;

layout(push_constant) uniform map_block {
    vec2 viewport;  // pixels
    vec2 centre;    // pixels (the focus)
    vec4 p;         // xyz: focus unit vector, w: scale (pixels per radian)
    vec4 e;         // xyz: local east, w: line scale (UI scale)
    vec4 n;         // xyz: local north, w: unused
    uint flags;     // bit 0: political fill
    uint id_levels; // mip levels of u_ids
    uint id_w, id_h;
    vec4 anim;      // x: cloud time (days), y: real seconds, z: cloud strength (0 = off), w: day of year
    uvec4 counts;   // x: storms, y: quakes
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

// --- clouds ------------------------------------------------------------------

float hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float vnoise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

float fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for(int k = 0; k < 5; ++k) {
        v += a * vnoise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return v / 0.97;
}

vec3 unit(float lon, float lat) { return vec3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat)); }

// Climatological zonal wind (m/s, + = westerly): trades, mid-latitude westerlies, polar easterlies.
float zonal_wind(float alat) {
    return -7.0 * exp(-pow((alat - 13.0) / 11.0, 2.0)) + 13.0 * exp(-pow((alat - 47.0) / 12.0, 2.0)) - 3.0 * exp(-pow((alat - 76.0) / 8.0, 2.0));
}

// Mean cloud cover (0..1) by latitude and season.
float cover_climatology(float lat, float doy) {
    float season = sin(2.0 * PI * (doy - 110.0) / 365.0); // +1 in northern summer
    float alat = abs(lat);
    float itcz = 0.62 * exp(-pow((lat - (4.0 + 7.0 * season)) / 7.5, 2.0));
    float hemi = lat >= 0.0 ? season : -season;
    float tracks = 0.5 * exp(-pow((alat - (52.0 + 4.0 * hemi)) / 13.0, 2.0));
    float polar = 0.3 * smoothstep(62.0, 80.0, alat);
    float subtropic = 0.26 * exp(-pow((alat - 24.0) / 8.0, 2.0));
    return clamp(0.36 + itcz + tracks + polar - subtropic, 0.06, 0.9);
}

// dryness: 0..1, from the land colour (deserts and steppes get little cloud).
float cloud_cover(float lon, float lat, float dryness) {
    float t = pc.anim.x;
    float alat = abs(lat) * 180.0 / PI;
    float omega = zonal_wind(alat) * 86400.0 / (6371000.0 * max(cos(lat), 0.08)); // rad/day
    const float period = 1.5;                                                    // flow-map cycle (days): short, so shear does not streak the noise
    float n = 0.0;
    for(int k = 0; k < 2; ++k) {
        float ph = t / period + 0.5 * float(k);
        float cyc = floor(ph);
        float tp = fract(ph) * period;
        float wgt = 1.0 - abs(2.0 * fract(ph) - 1.0);
        vec3 p = unit(lon - omega * tp, lat) * 11.0 + vec3(cyc * 0.37, cyc * 0.21, cyc * 0.13); // slowly drifting seed: cycles blend smoothly
        n += wgt * fbm(p + vec3(0.0, 0.0, 0.25 * tp));
    }
    n = clamp((n - 0.5) * 2.4 + 0.5, 0.0, 1.0);
    float cover = cover_climatology(lat * 180.0 / PI, pc.anim.w) * (1.0 - 0.75 * dryness);
    return smoothstep(1.0 - cover - 0.06, 1.0 - cover + 0.22, n);
}

// Tropical cyclone cloud: spiral bands, central dense overcast, eye.
float storm_cloud(vec3 x, vec4 s) {
    float slon = radians(s.x), slat = radians(s.y);
    vec3 c = unit(slon, slat);
    float cosd = dot(x, c);
    float d_km = acos(clamp(cosd, -1.0, 1.0)) * 6371.0;
    float radius = max(s.w, 60.0);
    if(d_km > radius * 2.4) return 0.0;
    vec3 e = vec3(-sin(slon), cos(slon), 0.0);
    vec3 nn = cross(c, e);
    vec3 v = x - c * cosd;
    float ang = atan(dot(v, nn), dot(v, e));
    float sgn = slat >= 0.0 ? 1.0 : -1.0; // anticlockwise in the north
    float r = d_km / radius;
    float intensity = clamp(s.z / 60.0, 0.25, 1.0);
    float arms = 2.0;
    float phase = arms * (ang + sgn * 3.2 * log(max(r, 0.02))) - sgn * pc.anim.x * 4.0;
    float band = smoothstep(0.25, 0.95, 0.5 + 0.5 * cos(phase));
    band *= 0.6 + 0.4 * vnoise(x * 140.0);
    float outer = band * smoothstep(2.4, 0.5, r);
    float cdo = smoothstep(0.75, 0.25, r) * (0.85 + 0.15 * vnoise(x * 260.0));
    float dens = max(outer, cdo) * (0.55 + 0.45 * intensity);
    if(s.z >= 33.0) { // hurricane / typhoon: a clear eye in a bright eyewall
        float eye = 9.0 + 0.05 * radius * (1.2 - intensity);
        dens *= smoothstep(eye * 0.7, eye * 1.5, d_km);
    }
    return dens;
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

    // Clouds above the political map, translucent so it stays readable.
    float strength = pc.anim.z;
    if(strength > 0.0) {
        // Sandy land colour (red well above blue, bright) marks a dry climate.
        float dryness = 0.0;
        if(me != 0u) {
            float l = dot(terrain.rgb, vec3(0.299, 0.587, 0.114));
            dryness = smoothstep(0.12, 0.22, terrain.r - terrain.b) * smoothstep(0.4, 0.6, l);
        }
        float cloud = cloud_cover(lon, lat, dryness);
        col = mix(col, vec3(0.97, 0.98, 1.0) * (0.86 + 0.14 * cloud), cloud * strength);
        // Cyclones are denser than the background cover (but the map still shows through).
        float storm = 0.0;
        for(uint i = 0u; i < pc.counts.x; ++i) storm = max(storm, storm_cloud(x, hz.storm[i]));
        col = mix(col, vec3(0.98, 0.99, 1.0), storm * min(1.0, strength * 1.7));
    }

    // Earthquakes: rings of red circles expanding from the epicentre.
    float px_per_km = scale * cosh(v) / 6371.0;
    float line_px = 1.1 * line_scale;
    for(uint i = 0u; i < pc.counts.y; ++i) {
        vec4 q = hz.quake[i];
        float d_px = acos(clamp(dot(x, unit(radians(q.x), radians(q.y))), -1.0, 1.0)) * 6371.0 * px_per_km;
        float big = max(40.0 * pow(2.0, q.z - 5.5) * px_per_km, (14.0 + 7.0 * (q.z - 5.0)) * line_scale);
        if(d_px > big * 1.05) continue;
        float fade = 1.0 - smoothstep(3.0, 7.0, q.w);
        float grow = fract(hz.quake_t[i].x * 0.4);
        float a = 0.0;
        for(int k = 0; k < 4; ++k) {
            float rk = big * (float(k) + grow) / 4.0;
            float ring = clamp(line_px + 0.5 - abs(d_px - rk), 0.0, 1.0);
            a = max(a, ring * (1.0 - (float(k) + grow) / 4.0 * 0.7));
        }
        a = max(a, clamp(2.5 * line_scale + 0.5 - d_px, 0.0, 1.0)); // epicentre dot
        col = mix(col, vec3(0.88, 0.08, 0.06), a * fade);
    }
    o_color = vec4(col, 1.0);
}
