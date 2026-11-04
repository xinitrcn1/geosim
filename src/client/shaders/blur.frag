#version 450
// Separable Gaussian blur, one direction per pass. radius = 0 is a single
// bilinear tap, used to downsample (2x2 box at exactly half resolution).

layout(set = 0, binding = 0) uniform sampler2D u_src;

layout(push_constant) uniform blur_block {
    vec2 texel;  // 1 / source size
    vec2 dir;    // (1, 0) horizontal, (0, 1) vertical
    float sigma; // in source texels
    int radius;  // taps on each side
} pb;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    if(pb.radius <= 0) {
        o_color = vec4(texture(u_src, v_uv).rgb, 1.0);
        return;
    }
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for(int i = -pb.radius; i <= pb.radius; ++i) {
        float x = float(i);
        float w = exp(-0.5 * x * x / (pb.sigma * pb.sigma));
        sum += w * texture(u_src, v_uv + pb.dir * pb.texel * x).rgb;
        total += w;
    }
    o_color = vec4(sum / total, 1.0);
}
