#pragma once
// Visual style (docs/08-ui.md#visual-style): square panels, soft blue scheme,
// white text and white (monochrome) emoji icons. Opaque panels for elements
// that must always be visible; frosted translucent panels for anything that
// can be dismissed or ignored. Red / green text only for indicators that are
// bad / good for the player. All metrics are in points at UI scale 1.

#include "ui_draw.hpp"

namespace geosim::client::style {

// Opaque panels (top bar): flat, no border, flush with the window's corner.
inline constexpr rgba opaque_bg = hex(0x22416A);

// Translucent (frosted) panels: a light, cool tint over the blurred backdrop
// (alpha = tint strength), so the map clearly shows through like frosted glass.
inline constexpr rgba glass_tint = hex(0x6E9FD8, 0.20f);
inline constexpr rgba glass_border = hex(0xFFFFFF, 0.24f);    // edge of a floating card (alerts)
inline constexpr rgba glass_separator = hex(0xFFFFFF, 0.55f); // strip between flush, stacked panes
inline constexpr rgba tooltip_tint = hex(0x1C3A60, 0.55f);
// Faint dark backing under text on glass (a 1 px drop shadow), for bright backdrops.
inline constexpr rgba text_backing = hex(0x08121F, 0.42f);

inline constexpr rgba button_hover = hex(0xFFFFFF, 0.12f);
inline constexpr rgba button_pressed = hex(0xFFFFFF, 0.22f);
inline constexpr rgba button_active = hex(0x5B95D6);
inline constexpr rgba separator = hex(0xFFFFFF, 0.18f);

inline constexpr rgba text = hex(0xFFFFFF);
inline constexpr rgba text_dim = hex(0xFFFFFF, 0.68f);
inline constexpr rgba good = hex(0x7BE3A0); // green: good for the player
inline constexpr rgba bad = hex(0xFF8080);  // red: bad for the player

inline constexpr rgba scene_clear = hex(0x102035);

// Metrics (points).
inline constexpr float top_bar_height = 40.0f;
inline constexpr float bar_height = 34.0f;
inline constexpr float gap = 6.0f;
inline constexpr float pad = 8.0f;
inline constexpr float button_size = 32.0f;
inline constexpr float font_size = 15.0f;
inline constexpr float font_small = 12.0f;
inline constexpr float icon_size = 18.0f;
inline constexpr float border = 1.0f;
inline constexpr float separator_strip = 1.0f;
inline constexpr float alert_width = 340.0f;
inline constexpr float alert_height = 44.0f;

} // namespace geosim::client::style
