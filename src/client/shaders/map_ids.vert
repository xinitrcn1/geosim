#version 450
// Vector-map ID pass (docs/07-map-and-filters.md#vector-regions): region rings
// drawn as triangle fans with vertex pulling. Each vertex fetches itself and
// its ring's first vertex, projects both with the oblique Mercator, and
// unwraps its oblique longitude next to the first vertex's, so a ring crossing
// the projection's seam (the antipode of the focus) stays whole; instance 1
// draws every ring again one turn (2 pi) to the other side, filling the seam.
// The fragment shader XORs the ring's code into an R32_UINT target: even-odd
// filling without triangulation (holes and enclaves come out right).

layout(std430, set = 0, binding = 0) readonly buffer vertices {
    uvec4 v[]; // lon, lat (float bits, degrees), ring's first vertex, code
} vb;
layout(std430, set = 0, binding = 1) readonly buffer indices {
    uint i[];
} ib;

layout(push_constant) uniform ids_block {
    vec2 viewport;
    vec2 centre;
    vec4 p; // focus, w: scale (pixels per radian)
    vec4 e; // east
    vec4 n; // north
} pc;

layout(location = 0) flat out uint o_code;

const float PI = 3.14159265358979;

vec2 project(vec2 lonlat_deg) {
    vec2 r = radians(lonlat_deg);
    vec3 x = vec3(cos(r.y) * cos(r.x), cos(r.y) * sin(r.x), sin(r.y));
    float u = atan(dot(x, pc.e.xyz), dot(x, pc.p.xyz));
    float v = clamp(atanh(clamp(dot(x, pc.n.xyz), -0.99999994, 0.99999994)), -9.0, 9.0);
    return vec2(u, v);
}

void main() {
    uvec4 a = vb.v[ib.i[gl_VertexIndex]];
    uvec4 f = vb.v[a.z];
    vec2 q = project(uintBitsToFloat(a.xy));
    vec2 q0 = project(uintBitsToFloat(f.xy));
    float du = q.x - q0.x;
    du -= 2.0 * PI * round(du / (2.0 * PI));
    float u = q0.x + du;
    if(gl_InstanceIndex == 1) u += q0.x > 0.0 ? -2.0 * PI : 2.0 * PI;
    vec2 s = pc.centre + pc.p.w * vec2(u, -q.y);
    gl_Position = vec4(s / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
    o_code = a.w;
}
