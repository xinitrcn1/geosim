// Shared declarations for the quad pipelines (docs/08-ui.md#rendering).
// Every UI element is one instance of a screen-space rectangle; the vertex
// shader expands it from gl_VertexIndex (6 vertices, no vertex buffer).

layout(push_constant) uniform push_block {
    vec2 viewport; // framebuffer size in pixels
    float time;    // seconds since start (animated materials)
    float scale;   // UI scale (display content scale)
} pc;

// quad_instance.flags, bits 0..7: the ui.frag mode (ui_draw.hpp: quad_mode)
const uint mode_solid = 0u;   // flat colour
const uint mode_frosted = 1u; // blurred backdrop under the rect, tinted by colour (alpha = tint strength)
const uint mode_glyph = 2u;   // colour * atlas coverage (text and monochrome emoji)
const uint mode_scene = 3u;   // copy of the scene image (the swapchain pass starts with it)
const uint mode_arc = 4u;     // a ring / pie sector: params = start, end (turns, ccw from +x), inner radius (fraction), gap (px)
