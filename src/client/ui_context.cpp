#include "ui_context.hpp"

#include "style.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::client {

namespace {

uint64_t hash_id(std::string_view s) {
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for(char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h | 1u; // never 0 (0 = no active widget)
}

quad_instance make_quad(rect r, rgba c, quad_mode mode) {
    return {r.x, r.y, r.w, r.h, 0, 0, 1, 1, pack(c), 0, 0, static_cast<uint32_t>(mode)};
}

} // namespace

void ui_context::begin(ui_input const& in, float scale, float width, float height) {
    in_ = in;
    scale_ = scale;
    width_ = width;
    height_ = height;
    wants_mouse_ = false;
    wheel_taken_ = false;
    backing_ = true;
    tooltip_.clear();
    scene.clear();
    overlay.clear();
    if(!in_.mouse_down && !in_.released) {
        active_id_ = 0;
    }
}

float ui_context::take_wheel(rect r) {
    if(wheel_taken_ || in_.wheel == 0 || !hovered(r)) return 0;
    wheel_taken_ = true;
    return in_.wheel;
}

rgba ui_context::colour(tone t) noexcept {
    switch(t) {
    case tone::good: return style::good;
    case tone::bad: return style::bad;
    case tone::dim: return style::text_dim;
    default: return style::text;
    }
}

void ui_context::fill(layer l, rect r, rgba c, material m) {
    (l == layer::scene ? scene : overlay).push(m, make_quad(r, c, quad_mode::solid));
}

void ui_context::panel(rect r, panel_kind kind) {
    // Snap to whole pixels so borders stay crisp.
    r = {std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h)};
    const float b = std::max(1.0f, std::round(px(style::border)));
    if(kind == panel_kind::opaque) {
        scene.push(material::ui, make_quad(r, style::opaque_bg, quad_mode::solid));
    } else {
        auto q = make_quad(r, style::glass_tint, quad_mode::frosted);
        if(kind == panel_kind::translucent) {
            q.border_color = pack(style::glass_border);
            q.border_width = b;
        }
        overlay.push(material::ui, q);
    }
    if(hovered(r)) wants_mouse_ = true;
}

void ui_context::flat_panel(rect r, float header) {
    r = {std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h)};
    overlay.push(material::ui, make_quad(r, style::flat_bg, quad_mode::solid));
    if(header > 0) overlay.push(material::ui, make_quad({r.x, r.y, r.w, std::round(header)}, style::flat_header, quad_mode::solid));
    if(hovered(r)) wants_mouse_ = true;
}

float ui_context::text(layer l, float x, float baseline, std::string_view s, float size_px, rgba c, font_weight w) {
    glyphs_.clear();
    const float adv = text_.shape(s, size_px, w, std::round(x), std::round(baseline), &glyphs_);
    auto& list = l == layer::scene ? scene : overlay;
    const uint32_t colour = pack(c);
    if(l == layer::overlay && backing_) {
        // Text on glass gets a faint dark backing (offset copy) so it stays
        // legible over bright parts of the map.
        const float o = std::max(1.0f, std::round(px(1)));
        rgba back = style::text_backing;
        back.a *= c.a;
        const uint32_t bc = pack(back);
        for(auto const& g : glyphs_) {
            list.push(material::ui, {g.x0 + o, g.y0 + o, g.x1 - g.x0, g.y1 - g.y0, g.u0, g.v0, g.u1, g.v1, bc, 0, 0,
                                     static_cast<uint32_t>(quad_mode::glyph)});
        }
    }
    for(auto const& g : glyphs_) {
        list.push(material::ui, {g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0, g.u0, g.v0, g.u1, g.v1, colour, 0, 0,
                                 static_cast<uint32_t>(quad_mode::glyph)});
    }
    return adv;
}

float ui_context::text_in(layer l, rect r, std::string_view s, float size_px, rgba c, align a, font_weight w) {
    const float tw = text_.measure(s, size_px, w);
    float x = r.x;
    if(a == align::centre) x = r.x + (r.w - tw) * 0.5f;
    if(a == align::right) x = r.right() - tw;
    // Centre the cap height: baseline sits ascender-ish below the middle.
    const float asc = text_.ascender(size_px), desc = text_.descender(size_px);
    const float baseline = r.y + (r.h + asc - desc) * 0.5f;
    text(l, x, baseline, s, size_px, c, w);
    return tw;
}

bool ui_context::button(std::string_view id_text, layer l, rect r, std::string_view label, float size_px, bool active,
                        std::string_view tip) {
    const uint64_t id = hash_id(id_text);
    const bool hot = hovered(r);
    if(hot) wants_mouse_ = true;
    if(hot && in_.pressed) active_id_ = id;
    const bool held = active_id_ == id && in_.mouse_down;
    const bool clicked = active_id_ == id && in_.released && hot;
    if(active) fill(l, r, style::button_active);
    if(held && hot) {
        fill(l, r, style::button_pressed);
    } else if(hot) {
        fill(l, r, style::button_hover);
    }
    text_in(l, r, label, size_px, style::text, align::centre);
    if(hot && !tip.empty()) tooltip_ = tip;
    return clicked;
}

void ui_context::end() {
    if(tooltip_.empty() || in_.mouse_x < 0) return;
    const float size = px(style::font_small);
    const float w = text_.measure(tooltip_, size) + px(style::pad) * 2;
    const float h = px(24);
    float x = in_.mouse_x + px(14);
    float y = in_.mouse_y + px(18);
    x = std::min(x, width_ - w - px(2));
    y = std::min(y, height_ - h - px(2));
    rect r{std::round(x), std::round(y), std::round(w), std::round(h)};
    auto q = quad_instance{r.x, r.y, r.w, r.h, 0, 0, 1, 1, pack(style::tooltip_tint), pack(style::glass_border),
                           std::max(1.0f, std::round(px(style::border))), static_cast<uint32_t>(quad_mode::frosted)};
    overlay.push(material::ui, q);
    text_in(layer::overlay, {r.x + px(style::pad), r.y, r.w, r.h}, tooltip_, size, style::text);
}

} // namespace geosim::client
