#pragma once
// Immediate-mode UI core (docs/08-ui.md#implementation): input state, hit
// testing and the drawing helpers every widget is built from. The layout code
// (ui_layout.cpp) calls these each frame; nothing is retained except the
// pressed widget id and the hovered tooltip.
//
// Coordinates are framebuffer pixels; metrics in style.hpp are points and
// are multiplied by scale() (display content scale).

#include "text.hpp"
#include "ui_draw.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace geosim::client {

struct ui_input {
    float mouse_x = -1, mouse_y = -1; // pixels; negative = outside the window
    bool mouse_down = false;
    bool pressed = false;   // left button went down this frame
    bool released = false;  // left button went up this frame
    float wheel = 0;        // vertical wheel steps this frame (positive = away from the user)
};

enum class panel_kind : uint8_t {
    opaque,      // flat, always-visible elements (top bar): drawn into the scene layer
    translucent, // dismissable / ignorable elements: frosted glass in the overlay layer, floating card with an edge
    glass_flush, // frosted glass without an edge, stacked flush with its neighbours (agent bars); separated by strips
};

enum class tone : uint8_t { neutral, good, bad, dim };
enum class align : uint8_t { left, centre, right };

class ui_context {
public:
    explicit ui_context(text_system& text) : text_(text) {}

    void begin(ui_input const& in, float scale, float width, float height);
    void end(); // draws the tooltip, if any

    [[nodiscard]] float scale() const noexcept { return scale_; }
    [[nodiscard]] float px(float points) const noexcept { return points * scale_; }
    [[nodiscard]] float width() const noexcept { return width_; }
    [[nodiscard]] float height() const noexcept { return height_; }
    [[nodiscard]] ui_input const& input() const noexcept { return in_; }

    // True when the mouse is over any panel drawn this frame (the future map
    // must ignore clicks then).
    [[nodiscard]] bool wants_mouse() const noexcept { return wants_mouse_; }
    [[nodiscard]] bool hovered(rect r) const noexcept { return r.contains(in_.mouse_x, in_.mouse_y); }
    // Wheel steps over `r`, consumed (returns 0 for every later caller).
    float take_wheel(rect r);

    // Primitives.
    void fill(layer l, rect r, rgba c, material m = material::ui);
    void panel(rect r, panel_kind kind);
    // A flat opaque panel in the overlay layer (tabs, map tooltips): white body,
    // optional blue title strip of `header` pixels. Text on it should be drawn
    // with set_text_backing(false).
    void flat_panel(rect r, float header = 0);
    // The faint dark backing under overlay text (on by default; off on flat panels).
    void set_text_backing(bool on) noexcept { backing_ = on; }
    float text(layer l, float x, float baseline, std::string_view s, float size_px, rgba c, font_weight w = font_weight::regular);
    // Text vertically centred in r, aligned horizontally; returns its width.
    float text_in(layer l, rect r, std::string_view s, float size_px, rgba c, align a = align::left, font_weight w = font_weight::regular);
    float text_width(std::string_view s, float size_px, font_weight w = font_weight::regular) { return text_.measure(s, size_px, w); }

    // A square-cornered button. `active` draws it selected (toggles, current
    // speed). Returns true on click (press and release inside).
    bool button(std::string_view id, layer l, rect r, std::string_view label, float size_px, bool active = false,
                std::string_view tooltip = {});
    void tooltip(std::string_view s) { tooltip_ = s; }

    [[nodiscard]] static rgba colour(tone t) noexcept;

    draw_list scene;
    draw_list overlay;

private:
    text_system& text_;
    ui_input in_;
    float scale_ = 1, width_ = 0, height_ = 0;
    bool wants_mouse_ = false;
    bool backing_ = true;
    bool wheel_taken_ = false;
    uint64_t active_id_ = 0; // button under a press that has not been released yet
    std::string tooltip_;
    std::vector<glyph_quad> glyphs_;
};

} // namespace geosim::client
