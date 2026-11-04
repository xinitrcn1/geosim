#version 450
// The Politics window's inset map, ID pass (docs/07-map-and-filters.md#electoral-map):
// the nation's ring fans, vertex-pulled like map_ids.vert, in plain Mercator scaled
// from one inset's source rect into its destination rect (drawn once per inset,
// scissored to it). Fragments XOR the ring's code into an R32_UINT image.

layout(std430, set = 0, binding = 0) readonly buffer vertices {
    uvec4 v[]; // lon, lat (float bits, degrees), ring's first vertex, code
} vb;
layout(std430, set = 0, binding = 1) readonly buffer indices {
    uint i[];
} ib;

layout(push_constant) uniform elect_block {
    vec2 viewport;
    float ref_lon; // degrees: longitudes are unwrapped around it
    float pad;
    vec4 dst;      // pixels: x0, y0, x1, y1
    vec4 src;      // Mercator: x0, y0, x1, y1 (x = lon radians, y = ln tan, up)
} pc;

layout(location = 0) flat out uint o_code;

float wrap360(float x) { return x - 360.0 * round(x / 360.0); }

void main() {
    uvec4 a = vb.v[ib.i[gl_VertexIndex]];
    uvec4 f = vb.v[a.z];
    vec2 ll = uintBitsToFloat(a.xy);
    float lon_first = uintBitsToFloat(f.x);
    float lf = pc.ref_lon + wrap360(lon_first - pc.ref_lon);
    float lon = lf + wrap360(ll.x - lon_first);
    float x = radians(lon);
    float y = log(tan(0.78539816 + radians(clamp(ll.y, -85.0, 85.0)) * 0.5));
    vec2 t = vec2((x - pc.src.x) / (pc.src.z - pc.src.x), (pc.src.w - y) / (pc.src.w - pc.src.y));
    vec2 s = mix(pc.dst.xy, pc.dst.zw, t);
    gl_Position = vec4(s / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
    o_code = a.w;
}
