#pragma once
// The UI's only drawing primitive: an instanced screen-space rectangle
// (docs/08-ui.md#rendering). Panels, buttons, borders, glyphs and the map
// placeholder are all quad_instance values; each run of instances shares a
// `material` (a fragment shader + blend state in ui_renderer.cpp).
//
// Two layers per frame:
//  - scene:   drawn into the offscreen scene image (map placeholder, opaque
//             panels and their text). This is what frosted panels blur.
//  - overlay: drawn on the swapchain after the scene copy (translucent
//             panels, their text, tooltips).

#include <cstdint>
#include <vector>

namespace geosim::client {

struct rgba {
    float r = 0, g = 0, b = 0, a = 1;
};

[[nodiscard]] inline uint32_t pack(rgba c) noexcept {
    auto ch = [](float v) {
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    return ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16) | (ch(c.a) << 24);
}

[[nodiscard]] constexpr rgba hex(uint32_t rgb, float alpha = 1.0f) noexcept {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

struct rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool contains(float px, float py) const noexcept { return px >= x && py >= y && px < x + w && py < y + h; }
    [[nodiscard]] float right() const noexcept { return x + w; }
    [[nodiscard]] float bottom() const noexcept { return y + h; }
};

// ui.frag modes (flags bits 0..7). Keep in sync with shaders/common.glsl.
enum class quad_mode : uint32_t { solid = 0, frosted = 1, glyph = 2, scene = 3, arc = 4 };

// Vertex-rate-instance layout (48 bytes). Keep in sync with shaders/quad.vert
// and ui_renderer.cpp (vertex attribute descriptions).
struct quad_instance {
    float x, y, w, h;      // pixels, top-left origin
    float u0, v0, u1, v1;  // atlas UVs for glyphs; free parameters for other materials
    uint32_t color;        // RGBA8 (see pack); frosted: tint colour, alpha = tint strength
    uint32_t border_color; // RGBA8; drawn inside the rect
    float border_width;    // pixels, 0 = none
    uint32_t flags;        // bits 0..7: quad_mode (ui material)
};
static_assert(sizeof(quad_instance) == 48);

// A material is one pipeline: shared vertex shader + instance format, its own
// fragment shader. To add a chart: write shaders/chart_x.frag, add an entry
// here and in ui_renderer.cpp's material table, and push instances with it
// (u0..v1 carry its parameters).
enum class material : uint8_t { ui = 0, map_placeholder = 1, count };

enum class layer : uint8_t { scene = 0, overlay = 1 };

struct draw_batch {
    material mat;
    uint32_t first;
    uint32_t count;
};

class draw_list {
public:
    void clear() {
        quads_.clear();
        batches_.clear();
    }
    void push(material m, quad_instance const& q) {
        if(batches_.empty() || batches_.back().mat != m) {
            batches_.push_back({m, static_cast<uint32_t>(quads_.size()), 0});
        }
        quads_.push_back(q);
        ++batches_.back().count;
    }
    [[nodiscard]] std::vector<quad_instance> const& quads() const noexcept { return quads_; }
    [[nodiscard]] std::vector<draw_batch> const& batches() const noexcept { return batches_; }

private:
    std::vector<quad_instance> quads_;
    std::vector<draw_batch> batches_;
};

} // namespace geosim::client
