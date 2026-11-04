#include "action_tabs.hpp"

#include "sim/model.hpp"
#include "style.hpp"
#include "ui_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geosim::client {

namespace {

using steady = std::chrono::steady_clock;
using sim::agent_kind;
using sim::override_mode;

constexpr uint32_t none_id = 0xFFFFFFFFu;
constexpr uint8_t mode_none = static_cast<uint8_t>(override_mode::none);
constexpr uint8_t mode_advise = static_cast<uint8_t>(override_mode::advise);
constexpr uint8_t mode_direct = static_cast<uint8_t>(override_mode::direct);
constexpr uint8_t mode_force = static_cast<uint8_t>(override_mode::force);

// Categorical slots in fixed order (the validated reference palette of the dataviz
// method), then grey for "Other".
constexpr rgba series[] = {hex(0x2A78D6), hex(0xEB6834), hex(0x1BAF7A), hex(0xEDA100), hex(0xE87BA4),
                           hex(0x008300), hex(0x6250D6), hex(0xE34948), hex(0x9AA0A6)};

std::string fmt(char const* f, double v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

// A lever value with as many decimals as its step needs.
std::string fmt_value(float v, float step) {
    if(!std::isfinite(v)) return "-";
    if(step >= 1.0f) return fmt("%.0f", v);
    if(step >= 0.1f - 1e-4f && std::fabs(std::round(step * 10.0f) - step * 10.0f) < 1e-3f) return fmt("%.1f", v);
    return fmt("%.2f", v);
}

float snap(float v, float lo, float hi, float step) {
    if(step > 0.0f) v = lo + std::round((v - lo) / step) * step;
    return std::clamp(v, lo, hi);
}

bool view_eq(tab_request const& a, tab_request const& b) {
    return a.tab == b.tab && a.agent == b.agent && a.nation == b.nation && a.subtab == b.subtab;
}
bool target_eq(counterpart_row const& a, counterpart_row const& b) {
    return a.nation == b.nation && a.agent == b.agent && a.organisation == b.organisation && a.label == b.label;
}

// Party colours by family (sim::party_family): the electoral map's (map_layer.cpp
// party_colour), so the hemicycle and the map agree.
rgba party_colour(std::vector<party_row> const& parties, std::size_t j, std::string const& iso) {
    static constexpr uint32_t family_colour[] = {0x9AA0A6, 0xD6453D, 0x2F64B5, 0xE8B723, 0xE07B24, 0x3A9A48,
                                                 0x2B3A67, 0x8E1B2E, 0xB0162B, 0x7E57A8, 0x8AA834, 0xC2408E};
    static constexpr uint32_t index_colour[] = {0xD6453D, 0xE8B723, 0x2F64B5, 0x3A9A48, 0x7E57A8, 0xE07B24, 0x2B3A67, 0xC2408E};
    party_row const& p = parties[j];
    if(iso == "USA" && p.family == 3) return hex(0x2F64B5);
    if(iso == "USA" && p.family == 2) return hex(0xD6453D);
    bool shared = false;
    for(std::size_t k = 0; k < parties.size(); ++k) shared = shared || (k != j && parties[k].family == p.family);
    if(p.family == 0 && shared) {
        if(p.key == "L") return hex(0xD6453D);
        if(p.key == "C") return hex(0xE8B723);
        if(p.key == "R") return hex(0x2F64B5);
        // local order is lost here; the key hash keeps it stable across refreshes
        uint32_t h = 0;
        for(char c : p.key) h = h * 31u + static_cast<unsigned char>(c);
        return hex(index_colour[h % std::size(index_colour)]);
    }
    return hex(family_colour[p.family < std::size(family_colour) ? p.family : 0]);
}

char const* kind_noun(uint8_t k) {
    static constexpr char const* names[] = {"government", "central bank", "board", "union federation", "military", "governor", "rebel movement"};
    return k < std::size(names) ? names[k] : "agent";
}

// Keeps the open tab's snapshot fresh (a few times a second, at once when the view
// changes); drops local overrides the sim has seen.
void poll(sim_runner& sim, tabs_state& t, tab_request const& rq) {
    if(t.pending.valid() && t.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        t.snap = t.pending.get();
        t.asked = t.pending_req;
        t.snap_serial = t.pending_serial;
        t.have = true;
        t.local.erase(std::remove_if(t.local.begin(), t.local.end(), [&](auto const& l) { return t.snap_serial > l.serial; }), t.local.end());
    }
    const bool fresh_view = !view_eq(t.pending.valid() ? t.pending_req : t.asked, rq) || !target_eq((t.pending.valid() ? t.pending_req : t.asked).target, rq.target);
    if(!t.pending.valid() && (fresh_view || steady::now() - t.last_request > std::chrono::milliseconds(300))) {
        t.pending_req = rq;
        t.pending_serial = ++t.serial;
        t.pending = sim.request_tab(rq);
        t.last_request = steady::now();
    }
}

bool showing(tabs_state const& t, tab_request const& rq) { return t.have && view_eq(t.asked, rq); }

// Small flat text helpers (the windows are flat white: no text backing).
void label(ui_context& ui, rect r, std::string_view s, rgba c = style::flat_text, align a = align::left, font_weight w = font_weight::regular,
           float size = 12) {
    ui.text_in(layer::overlay, r, s, ui.px(size), c, a, w);
}

// A row of stat tiles: label over a value, with a tooltip; returns the bottom.
struct tile {
    std::string label, value, note, tip;
    tone t = tone::neutral;
};
float tiles(ui_context& ui, std::vector<tile> const& items, float x, float y, float w) {
    const float pad = ui.px(style::pad);
    const float th = ui.px(52);
    const float gap = ui.px(4);
    const float tw = std::max(ui.px(110), (w - gap * static_cast<float>(items.size() - 1)) / static_cast<float>(std::max<std::size_t>(1, items.size())));
    float tx = x;
    for(auto const& it : items) {
        if(tx + tw > x + w + 1.0f) break;
        const rect r{std::round(tx), y, std::floor(tw), th};
        ui.fill(layer::overlay, r, hex(0xF2F5F9));
        label(ui, {r.x + pad, r.y + ui.px(4), r.w, ui.px(16)}, it.label, style::flat_dim);
        const rgba vc = it.t == tone::bad ? style::flat_bad : it.t == tone::good ? style::flat_good : style::flat_text;
        const float vw = ui.text_in(layer::overlay, {r.x + pad, r.y + ui.px(20), r.w, ui.px(26)}, it.value, ui.px(18), vc, align::left, font_weight::semibold);
        if(!it.note.empty()) label(ui, {r.x + pad + vw + ui.px(6), r.y + ui.px(24), r.w, ui.px(22)}, it.note, style::flat_dim, align::left, font_weight::regular, 11);
        if(ui.hovered(r) && !it.tip.empty()) ui.tooltip(it.tip);
        tx += tw + gap;
    }
    return y + th;
}

// The window's agent strip: which controlled agent the tab acts for (click to switch).
float agent_strip(ui_context& ui, sim_runner& sim, tabs_state& t, agent_info const* who, float x, float y, float w) {
    const float small = ui.px(style::font_small);
    const float bh = ui.px(22);
    const float pad = ui.px(style::pad);
    auto const& all = sim.controlled_agents();
    label(ui, {x, y, w, bh}, "Acting as", style::flat_dim);
    float bx = x + ui.text_width("Acting as", small) + pad;
    for(auto const& a : all) {
        const float bw = ui.text_width(a.label, small) + pad * 2;
        if(bx + bw > x + w) break;
        char id[40];
        std::snprintf(id, sizeof(id), "tabs.agent.%u", a.agent);
        if(ui.flat_button(id, {bx, y, bw, bh}, a.label, small, who && who->agent == a.agent,
                          sim.ai_on(a.agent) ? "AI on: you advise or force its levers" : "AI off: you direct its levers")) {
            t.agent = a.agent;
            t.scroll = 0;
            t.has_target = false;
        }
        bx += bw + ui.px(4);
    }
    return y + bh + pad;
}

// The lever table: name (bill tag), now, the AI's own choice, a crank and slider for
// the player's target, the unit and the mode (Advise / Force with the AI on, Direct
// with it off). Writes go through sim_runner::set_override.
void lever_table(ui_context& ui, sim_runner& sim, tabs_state& t, agent_info const& who, std::vector<lever_row> const& rows, rect area) {
    const float pad = ui.px(style::pad);
    const float small = ui.px(style::font_small);
    const float rh = ui.px(30);
    const bool ai = sim.ai_on(who.agent);
    const float col_now = ui.px(68), col_ai = ui.px(68), crank = ui.px(22), col_target = ui.px(64), col_unit = ui.px(124), col_mode = ui.px(124);
    const float slider = std::clamp(area.w * 0.16f, ui.px(90), ui.px(220));
    const float fixed = col_now + col_ai + crank * 2 + slider + ui.px(8) + col_target + col_unit + col_mode + pad * 3;
    const float name_w = std::max(ui.px(120), area.w - fixed);
    const float x_now = area.x + name_w + pad, x_ai = x_now + col_now, x_minus = x_ai + col_ai, x_slider = x_minus + crank + ui.px(4);
    const float x_plus = x_slider + slider + ui.px(4), x_target = x_plus + crank + pad, x_unit = x_target + col_target, x_mode = x_unit + col_unit;
    float y = area.y;
    // Header.
    label(ui, {area.x, y, name_w, rh * 0.8f}, "Lever", style::flat_dim, align::left, font_weight::semibold);
    label(ui, {x_now, y, col_now - pad, rh * 0.8f}, "Now", style::flat_dim, align::right, font_weight::semibold);
    label(ui, {x_ai, y, col_ai - pad, rh * 0.8f}, "AI alone", style::flat_dim, align::right, font_weight::semibold);
    label(ui, {x_minus, y, x_target + col_target - x_minus, rh * 0.8f}, "Your target", style::flat_dim, align::centre, font_weight::semibold);
    label(ui, {x_unit, y, col_unit, rh * 0.8f}, "Unit", style::flat_dim, align::left, font_weight::semibold);
    label(ui, {x_mode, y, col_mode, rh * 0.8f}, ai ? "Mode (AI on)" : "Mode (AI off)", style::flat_dim, align::left, font_weight::semibold);
    if(ui.hovered({area.x, y, area.w, rh * 0.8f})) {
        ui.tooltip(ai ? "Now: the lever's value; AI alone: what the agent's AI would choose; Advise: your target is a strong prior it weighs; Force: imposed beyond legal limits, at a political cost"
                      : "AI off: you direct every lever (held within its legal range); Now: the value in force; AI alone: what the AI would choose");
    }
    y += rh * 0.8f;
    ui.fill(layer::overlay, {area.x, y, area.w, std::max(1.0f, std::round(ui.px(1)))}, style::flat_rule);
    const rect list{area.x, y, area.w, area.bottom() - y};
    const auto visible = static_cast<std::size_t>(std::max(1.0f, std::floor(list.h / rh)));
    const float max_scroll = static_cast<float>(rows.size() > visible ? rows.size() - visible : 0);
    t.scroll = std::clamp(t.scroll - ui.take_wheel(list) * 2.0f, 0.0f, max_scroll);
    const auto first = static_cast<std::size_t>(t.scroll);
    if(!ui.input().mouse_down) t.drag = -1;
    for(std::size_t i = first; i < rows.size() && i < first + visible; ++i) {
        lever_row const& row = rows[i];
        // Local (not yet seen) override wins.
        uint8_t mode = row.mode;
        float value = row.value;
        for(auto const& l : t.local) {
            if(l.agent == who.agent && l.index == row.index) {
                mode = l.mode;
                value = l.value;
            }
        }
        const float target = mode != mode_none ? value : row.current;
        const float ry = y + static_cast<float>(i - first) * rh;
        if(i % 2 == 1) ui.fill(layer::overlay, {area.x, ry, area.w, rh}, hex(0xF6F8FB));
        auto write = [&](uint8_t m, float v) {
            sim.set_override(who.agent, row.index, m, v);
            t.local.erase(std::remove_if(t.local.begin(), t.local.end(), [&](auto const& l) { return l.agent == who.agent && l.index == row.index; }), t.local.end());
            t.local.push_back({who.agent, row.index, m, v, t.serial});
            t.last_request = {};
        };
        const uint8_t crank_mode = ai ? (mode == mode_none || mode == mode_direct ? mode_advise : mode) : mode_direct;
        // Name and the bill tag.
        const float nw = ui.text_in(layer::overlay, {area.x + pad, ry, name_w - pad, rh}, row.name, small, style::flat_text);
        if(row.bill) {
            const float bw = ui.text_width("bill", ui.px(10)) + ui.px(8);
            const rect tag{std::round(area.x + pad + nw + ui.px(6)), std::round(ry + (rh - ui.px(16)) * 0.5f), std::round(bw), std::round(ui.px(16))};
            if(tag.right() < area.x + name_w) {
                ui.fill(layer::overlay, tag, hex(0xE3ECF7));
                label(ui, tag, "bill", style::flat_header, align::centre, font_weight::semibold, 10);
                if(ui.hovered(tag)) ui.tooltip("A bill: it must pass the legislature where politics is competitive (Force bypasses it)");
            }
        }
        if(ui.hovered({area.x, ry, name_w, rh}) && !row.bill) ui.tooltip(row.npi ? "Pandemic restriction (OxCGRT ordinal level)" : row.name);
        label(ui, {x_now, ry, col_now - pad, rh}, fmt_value(row.current, row.step), style::flat_text, align::right, font_weight::semibold);
        label(ui, {x_ai, ry, col_ai - pad, rh}, fmt_value(row.ai_choice, row.step), style::flat_dim, align::right);
        // Crank and slider.
        char id[48];
        const float bh = ui.px(20), by = ry + (rh - bh) * 0.5f;
        std::snprintf(id, sizeof(id), "lever.minus.%u", row.index);
        if(ui.flat_button(id, {x_minus, by, crank, bh}, "−", small, false, "One step down")) write(crank_mode, snap(target - row.step, row.lo, row.hi, row.step));
        std::snprintf(id, sizeof(id), "lever.plus.%u", row.index);
        if(ui.flat_button(id, {x_plus, by, crank, bh}, "+", small, false, "One step up")) write(crank_mode, snap(target + row.step, row.lo, row.hi, row.step));
        const rect track{x_slider, ry + rh * 0.5f - ui.px(5), slider, ui.px(10)};
        const float span = std::max(1e-6f, row.hi - row.lo);
        auto frac = [&](float v) { return std::clamp((v - row.lo) / span, 0.0f, 1.0f); };
        ui.fill(layer::overlay, track, hex(0xE6ECF3));
        ui.fill(layer::overlay, {track.x, track.y, std::round(track.w * frac(target)), track.h}, mode != mode_none ? hex(0x86B6EF) : hex(0xC9D6E6));
        const float tick = std::max(2.0f, std::round(ui.px(2)));
        ui.fill(layer::overlay, {std::round(track.x + track.w * frac(row.ai_choice) - tick * 0.5f), track.y - ui.px(3), tick, track.h + ui.px(6)}, series[1]);
        ui.fill(layer::overlay, {std::round(track.x + track.w * frac(row.current) - tick * 0.5f), track.y - ui.px(3), tick, track.h + ui.px(6)}, style::flat_text);
        const rect hit{track.x, ry, track.w, rh};
        if(ui.hovered(hit)) {
            ui.tooltip("Click or drag to set your target (" + fmt_value(row.lo, row.step) + " to " + fmt_value(row.hi, row.step) +
                       "); dark tick: now, orange: the AI alone");
            if(ui.input().pressed) t.drag = row.index;
        }
        if(t.drag == row.index && ui.input().mouse_down) {
            const float v = snap(row.lo + std::clamp((ui.input().mouse_x - track.x) / track.w, 0.0f, 1.0f) * span, row.lo, row.hi, row.step);
            if(std::fabs(v - target) > row.step * 0.25f || mode == mode_none) write(crank_mode, v);
        }
        label(ui, {x_target, ry, col_target - pad, rh}, mode != mode_none ? fmt_value(target, row.step) : "–",
              mode == mode_force ? style::flat_bad : style::flat_header, align::right, font_weight::semibold);
        label(ui, {x_unit, ry, col_unit - pad, rh}, row.unit, style::flat_dim, align::left, font_weight::regular, 11);
        // Mode.
        if(ai) {
            const float mw = (col_mode - ui.px(4)) * 0.5f;
            std::snprintf(id, sizeof(id), "lever.advise.%u", row.index);
            if(ui.flat_button(id, {x_mode, by, mw, bh}, "Advise", ui.px(11), mode == mode_advise,
                              mode == mode_advise ? "Stop advising (the AI decides alone)" : "Advise: the AI weighs your target as a strong prior")) {
                write(mode == mode_advise ? mode_none : mode_advise, target);
            }
            std::snprintf(id, sizeof(id), "lever.force.%u", row.index);
            const rect fr{x_mode + mw + ui.px(4), by, mw, bh};
            if(ui.flat_button(id, fr, "Force", ui.px(11), mode == mode_force,
                              mode == mode_force ? "Stop forcing" : "Force: imposed beyond legal limits and the legislature, at a political cost")) {
                write(mode == mode_force ? mode_none : mode_force, target);
            }
            if(mode == mode_force) ui.fill(layer::overlay, {fr.x, fr.bottom() - ui.px(2), fr.w, ui.px(2)}, style::flat_bad);
        } else {
            label(ui, {x_mode, ry, col_mode, rh}, "Direct", style::flat_header, align::left, font_weight::semibold);
        }
    }
    if(rows.size() > visible) {
        char where[48];
        std::snprintf(where, sizeof(where), "%zu–%zu of %zu (wheel scrolls)", first + 1, std::min(rows.size(), first + visible), rows.size());
        label(ui, {area.x, area.bottom() - ui.px(14), area.w, ui.px(14)}, where, style::flat_dim, align::right, font_weight::regular, 11);
    }
}

// A one-line note in the body.
float note(ui_context& ui, float x, float y, float w, std::string_view s, rgba c = style::flat_dim) {
    label(ui, {x, y, w, ui.px(18)}, s, c);
    return y + ui.px(18);
}

float forced_warning(ui_context& ui, std::vector<lever_row> const& rows, tabs_state const& t, uint32_t agent, float x, float y, float w) {
    bool forced = false;
    for(auto const& r : rows) forced = forced || r.mode == mode_force;
    for(auto const& l : t.local) forced = forced || (l.agent == agent && l.mode == mode_force);
    if(!forced) return y;
    return note(ui, x, y, w, "⚠ Forced levers ignore their legal limits and the legislature; each one costs legitimacy and trust", style::flat_bad);
}

// A flat pie of `slices` (top 8 + other) in a square, with its legend beside it.
void pie(ui_context& ui, std::string_view title, std::vector<trade_slice> const& slices, rect r) {
    const float pad = ui.px(style::pad);
    const float line = ui.px(17);
    label(ui, {r.x, r.y, r.w, ui.px(20)}, title, style::flat_text, align::left, font_weight::semibold, 13);
    const float top = r.y + ui.px(24);
    double total = 0.0;
    for(auto const& s : slices) total += s.value;
    if(total <= 0.0) {
        label(ui, {r.x, top, r.w, ui.px(20)}, "No data", style::flat_dim);
        return;
    }
    const float legend_h = static_cast<float>(slices.size()) * line;
    const float d = std::max(ui.px(60), std::min(r.w, r.bottom() - top - legend_h - pad));
    const rect sq{std::round(r.x + (r.w - d) * 0.5f), std::round(top), std::round(d), std::round(d)};
    // Clockwise from the top; a 1 px white spacer between slices.
    float at = 0.25f;
    int hovered = -1;
    const float mx = ui.input().mouse_x - (sq.x + sq.w * 0.5f), my = -(ui.input().mouse_y - (sq.y + sq.h * 0.5f));
    const bool in_disc = mx * mx + my * my <= sq.w * sq.w * 0.25f;
    float mt = std::atan2(my, mx) / 6.2831853f;
    for(std::size_t i = 0; i < slices.size(); ++i) {
        const float f = static_cast<float>(slices[i].value / total);
        const rgba c = slices[i].label == "Other" ? series[8] : series[std::min<std::size_t>(i, 7)];
        ui.arc(sq, at - f, at, 0.0f, c, slices.size() > 1 ? std::max(1.0f, ui.px(1)) : 0.0f);
        if(in_disc) {
            const float d0 = mt - (at - f);
            if(d0 - std::floor(d0) <= f) hovered = static_cast<int>(i);
        }
        at -= f;
    }
    float ly = sq.bottom() + pad;
    const float sw = ui.px(10);
    for(std::size_t i = 0; i < slices.size(); ++i) {
        const rgba c = slices[i].label == "Other" ? series[8] : series[std::min<std::size_t>(i, 7)];
        ui.fill(layer::overlay, {r.x, std::round(ly + (line - sw) * 0.5f), sw, sw}, c);
        label(ui, {r.x + sw + ui.px(5), ly, r.w - sw - ui.px(5) - ui.px(44), line}, slices[i].label, style::flat_text, align::left,
              static_cast<int>(i) == hovered ? font_weight::semibold : font_weight::regular, 11);
        label(ui, {r.x, ly, r.w, line}, fmt("%.1f%%", slices[i].value / total * 100.0), style::flat_dim, align::right, font_weight::regular, 11);
        ly += line;
    }
    if(hovered >= 0) ui.tooltip(slices[static_cast<std::size_t>(hovered)].label + ": " + fmt("%.1f%%", slices[static_cast<std::size_t>(hovered)].value / total * 100.0));
}

// --- tabs ---------------------------------------------------------------------

void policies_body(ui_context& ui, sim_runner& sim, tabs_state& t, agent_info const& who, rect body) {
    const float pad = ui.px(style::pad);
    float y = body.y;
    auto const& rows = t.snap.levers;
    const bool ai = sim.ai_on(who.agent);
    y = note(ui, body.x, y, body.w,
             ai ? "AI on: the agent decides. Advise a target (a strong prior it weighs) or Force it (beyond legal limits, at a political cost)."
                : "AI off: you direct every lever; values are held within their legal range until you change them.");
    y = forced_warning(ui, rows, t, who.agent, body.x, y, body.w);
    y += ui.px(4);
    if(rows.empty()) {
        note(ui, body.x, y, body.w,
             who.kind == static_cast<uint8_t>(agent_kind::military) ? "Militaries have no policy levers in the simulation: their loyalty decides coups (see Military and Politics)."
             : who.kind == static_cast<uint8_t>(agent_kind::rebel) ? "Rebels have no policy levers; see Diplomacy for a settlement."
                                                                   : "No levers for this agent.");
        return;
    }
    lever_table(ui, sim, t, who, rows, {body.x, y, body.w, body.bottom() - y - pad});
}

void fiscal_body(ui_context& ui, sim_runner& sim, tabs_state& t, agent_info const& who, rect body) {
    const float pad = ui.px(style::pad);
    float y = body.y;
    fiscal_summary const& f = t.snap.fiscal;
    std::vector<tile> items;
    const std::string over = f.days >= 1.0f ? fmt("over the %.0f days so far", f.days) : "";
    if(f.valid) {
        items.push_back({"Revenue", fmt("%.1f%%", f.revenue), "of GDP", "Taxes on households, consumption, firms, resources, tariffs, carbon and wealth, % of GDP " + over});
        items.push_back({"Spending", fmt("%.1f%%", f.spending), "of GDP", "Revenue minus net lending: everything the government paid out net of other receipts, % of GDP " + over});
        items.push_back({f.balance < 0.0f ? "Deficit" : "Surplus", fmt("%.1f%%", std::fabs(f.balance)), "of GDP",
                         "Net lending (change in the government's balance), % of GDP " + over, f.balance < -4.0f ? tone::bad : f.balance > -1.0f ? tone::good : tone::neutral});
    } else {
        items.push_back({"Budget", "-", "", "Shown from the first day of the run"});
    }
    items.push_back({"Debt", fmt("%.0f%%", f.debt), fmt("start %.0f%%", f.debt0), "Gross government debt, % of annual GDP (start of the run in the note)",
                     f.debt > f.debt0 + 10.0f ? tone::bad : f.debt < f.debt0 - 2.0f ? tone::good : tone::neutral});
    items.push_back({"10y yield", fmt("%.2f%%", f.yield10), fmt("spread %.2f", f.spread), "10-year government bond yield and its sovereign spread"});
    y = tiles(ui, items, body.x, y, body.w) + pad;
    if(who.kind != static_cast<uint8_t>(agent_kind::executive)) {
        note(ui, body.x, y, body.w, std::string("Fiscal levers belong to the government; a ") + kind_noun(who.kind) + " can lobby it (Diplomacy → Governments).");
        return;
    }
    y = forced_warning(ui, t.snap.levers, t, who.agent, body.x, y, body.w);
    lever_table(ui, sim, t, who, t.snap.levers, {body.x, y, body.w, body.bottom() - y - pad});
}

void trade_body(ui_context& ui, sim_runner& sim, tabs_state& t, agent_info const& who, rect body) {
    const float pad = ui.px(style::pad);
    trade_snapshot const& tr = t.snap.trade;
    float y = body.y;
    std::vector<tile> items;
    items.push_back({"Exports", fmt("%.1f%%", tr.exports_pct), fmt("%+.1f pp", tr.exports_pct - tr.exports0_pct), "Exports, % of real GDP (change since the start)"});
    items.push_back({"Imports", fmt("%.1f%%", tr.imports_pct), fmt("%+.1f pp", tr.imports_pct - tr.imports0_pct), "Imports, % of real GDP (change since the start)"});
    items.push_back({"Tariff", fmt("%.1f%%", tr.tariff), fmt("start %.1f%%", tr.tariff0), "Average import tariff, ad valorem"});
    items.push_back({"Sanctions", fmt("%.0f", static_cast<double>(tr.sanctions_by.size())) + " / " + fmt("%.0f", static_cast<double>(tr.sanctions_on.size())),
                     "by / on", "Sanctions in force: nations this one sanctions / nations sanctioning it",
                     tr.sanctions_on.empty() ? tone::neutral : tone::bad});
    y = tiles(ui, items, body.x, y, body.w) + pad;
    // Pies.
    const bool exec = who.kind == static_cast<uint8_t>(agent_kind::executive) && !t.snap.levers.empty();
    const float levers_h = exec ? ui.px(30) * 3.0f : 0.0f;
    const float lists_h = ui.px(18) * 2.0f;
    const float pie_h = std::max(ui.px(180), body.bottom() - y - levers_h - lists_h - pad * 2);
    const float pw = (body.w - pad * 4) / 3.0f;
    pie(ui, "Exports by partner", tr.exports, {body.x, y, pw, pie_h});
    pie(ui, "Imports by partner", tr.imports, {body.x + pw + pad * 2, y, pw, pie_h});
    pie(ui, "Exports by sector", tr.sectors, {body.x + (pw + pad * 2) * 2, y, pw, pie_h});
    y += pie_h + pad;
    auto joined = [](std::vector<std::string> const& v) {
        std::string s;
        for(std::size_t i = 0; i < v.size() && i < 12; ++i) s += (i ? ", " : "") + v[i];
        if(v.size() > 12) s += ", …";
        return s.empty() ? std::string("none") : s;
    };
    y = note(ui, body.x, y, body.w, "Sanctioning: " + joined(tr.sanctions_by), style::flat_text);
    y = note(ui, body.x, y, body.w, "Sanctioned by: " + joined(tr.sanctions_on), tr.sanctions_on.empty() ? style::flat_text : style::flat_bad);
    if(exec) lever_table(ui, sim, t, who, t.snap.levers, {body.x, y + pad * 0.5f, body.w, body.bottom() - y - pad});
}

void military_body(ui_context& ui, sim_runner& sim, tabs_state& t, agent_info const& who, rect body) {
    const float pad = ui.px(style::pad);
    military_snapshot const& m = t.snap.military;
    float y = body.y;
    std::vector<tile> items;
    items.push_back({"Loyalty", fmt("%.2f", m.loyalty), "0-1", "The military's loyalty to the government (a coup becomes likely when it is low)",
                     m.loyalty < 0.4f ? tone::bad : tone::neutral});
    items.push_back({"Defence", fmt("%.1f%%", m.defence), fmt("start %.1f%%", m.defence0), "Defence spending, % of GDP"});
    items.push_back({"Power", fmt("%.2f", m.power), fmt("CINC %.3f", m.cinc), "Military power and the Composite Index of National Capability"});
    items.push_back({"Threat", fmt("%.2f", m.threat), "", "Perceived external threat (0-1)", m.threat > 0.6f ? tone::bad : tone::neutral});
    items.push_back({"Nuclear", fmt("%.2f", m.nuclear), "", "Nuclear deterrent (nation.nuclear)"});
    y = tiles(ui, items, body.x, y, body.w) + pad;
    auto joined = [](std::vector<std::string> const& v) {
        std::string s;
        for(std::size_t i = 0; i < v.size() && i < 14; ++i) s += (i ? ", " : "") + v[i];
        if(v.size() > 14) s += ", …";
        return s.empty() ? std::string("none") : s;
    };
    y = note(ui, body.x, y, body.w, "At war with: " + joined(m.wars), m.wars.empty() ? style::flat_text : style::flat_bad);
    y = note(ui, body.x, y, body.w, "Disputes: " + joined(m.disputes), style::flat_text);
    y = note(ui, body.x, y, body.w, "Allies: " + joined(m.allies), style::flat_text);
    y = note(ui, body.x, y, body.w, "War and peace are decided in Diplomacy → Governments.");
    y += pad;
    if(!t.snap.levers.empty()) lever_table(ui, sim, t, who, t.snap.levers, {body.x, y, body.w, body.bottom() - y - pad});
    else if(who.kind == static_cast<uint8_t>(agent_kind::military)) note(ui, body.x, y, body.w, "Militaries have no levers in the simulation yet; defence spending is the government's.");
}

// Diplomacy: subtabs per counterpart kind (and organisations), a counterpart list
// and the options the simulation offers toward the selected one.
void diplomacy_body(ui_context& ui, sim_runner& sim, alert_feed& alerts, tabs_state& t, agent_info const& who, rect body) {
    const float pad = ui.px(style::pad);
    const float small = ui.px(style::font_small);
    float y = body.y;
    // The outcome of an action in flight.
    if(action_outcome r; t.pending_action != 0 && sim.take_result(t.pending_action, r)) {
        t.pending_action = 0;
        t.result = t.pending_label + ": " + r.message;
        t.result_ok = r.accepted;
        alerts.push_local(r.accepted ? "🤝" : "⛔", t.pending_label, r.message.substr(0, 90), r.accepted ? tone::good : tone::bad);
        t.last_request = {};
    }
    static constexpr char const* sub_names[] = {"Governments", "Central banks", "Firms", "Unions", "Militaries", "Governors", "Rebels", "Organisations"};
    static constexpr int sub_order[] = {0, 1, 2, 3, 4, 5, 6, 7};
    float bx = body.x;
    const float bh = ui.px(24);
    for(int k : sub_order) {
        const float bw = ui.text_width(sub_names[k], small) + pad * 2;
        char id[32];
        std::snprintf(id, sizeof(id), "dipl.sub.%d", k);
        if(ui.flat_button(id, {bx, y, bw, bh}, sub_names[k], small, t.subtab == k)) {
            t.subtab = k;
            t.has_target = false;
            t.list_scroll = 0;
            t.result.clear();
        }
        bx += bw + ui.px(4);
    }
    y += bh + pad;
    diplomacy_snapshot const& d = t.snap.diplomacy;
    // Counterpart list (left).
    const float lw = std::min(ui.px(320), body.w * 0.36f);
    const rect list{body.x, y, lw, body.bottom() - y - pad};
    ui.fill(layer::overlay, list, hex(0xF6F8FB));
    const float line = ui.px(24);
    const auto visible = static_cast<std::size_t>(std::max(1.0f, std::floor(list.h / line)));
    const float max_scroll = static_cast<float>(d.counterparts.size() > visible ? d.counterparts.size() - visible : 0);
    t.list_scroll = std::clamp(t.list_scroll - ui.take_wheel(list) * 3.0f, 0.0f, max_scroll);
    const auto first = static_cast<std::size_t>(t.list_scroll);
    if(d.counterparts.empty()) label(ui, list, showing(t, t.pending_req) ? "None in the simulation" : "Loading…", style::flat_dim, align::centre);
    for(std::size_t i = first; i < d.counterparts.size() && i < first + visible; ++i) {
        auto const& c = d.counterparts[i];
        const rect row{list.x, list.y + static_cast<float>(i - first) * line, list.w, line - ui.px(2)};
        const bool sel = t.has_target && target_eq(t.target, c);
        char id[64];
        std::snprintf(id, sizeof(id), "dipl.cp.%d.%zu", t.subtab, i);
        if(ui.flat_button(id, row, "", small, sel)) {
            t.target = c;
            t.has_target = true;
            t.result.clear();
            t.lobby_lever = -1;
        }
        ui.text_in(layer::overlay, {row.x + pad, row.y, row.w - pad * 2, row.h}, c.label, small, sel ? style::text : style::flat_text);
        if(!c.detail.empty()) ui.text_in(layer::overlay, {row.x, row.y, row.w - pad, row.h}, c.detail, ui.px(11), sel ? style::text : style::flat_dim, align::right);
    }
    if(d.counterparts.size() > visible) {
        label(ui, {list.x, list.bottom() + ui.px(1), list.w, ui.px(12)}, fmt("%.0f counterparts, wheel scrolls", static_cast<double>(d.counterparts.size())),
              style::flat_dim, align::right, font_weight::regular, 10);
    }
    // Options (right).
    const float ox = list.right() + pad * 2, ow = body.right() - ox;
    float oy = y;
    if(!t.has_target) {
        note(ui, ox, oy, ow, t.subtab >= 1 && t.subtab <= 5 ? "No diplomatic options are modelled for this kind yet (pick one to see)." : "Pick a counterpart on the left.");
        return;
    }
    label(ui, {ox, oy, ow, ui.px(22)}, t.target.label, style::flat_text, align::left, font_weight::semibold, 15);
    oy += ui.px(24);
    if(!t.target.detail.empty()) oy = note(ui, ox, oy, ow, t.target.detail);
    oy += ui.px(4);
    const bool current = showing(t, t.pending_req) && target_eq(t.asked.target, t.target);
    if(!current && d.actions.empty()) {
        note(ui, ox, oy, ow, "Loading…");
        return;
    }
    if(d.actions.empty()) {
        if(t.subtab >= 1 && t.subtab <= 5) oy = note(ui, ox, oy, ow, "No diplomatic options are modelled for this kind yet.");
        else oy = note(ui, ox, oy, ow, std::string("No options toward this counterpart are modelled for a ") + kind_noun(who.kind) + ".");
    }
    const float rh = ui.px(32);
    const float btn_w = ui.px(230);
    for(std::size_t i = 0; i < d.actions.size(); ++i) {
        auto const& a = d.actions[i];
        const rect b{ox, oy + (rh - ui.px(24)) * 0.5f, btn_w, ui.px(24)};
        char id[48];
        std::snprintf(id, sizeof(id), "dipl.act.%u", a.kind);
        bool clicked = false;
        if(a.available && t.pending_action == 0) {
            clicked = ui.flat_button(id, b, a.label, small, false, a.reason);
        } else {
            ui.fill(layer::overlay, b, hex(0xF0F2F5));
            label(ui, b, a.label, hex(0xA9B2BD), align::centre);
            if(ui.hovered(b)) ui.tooltip(a.available ? "An action is in progress" : "Not available: " + a.reason);
        }
        // The value it takes.
        float ix = b.right() + pad;
        action_order order;
        order.kind = a.kind;
        order.agent = who.agent;
        order.nation = t.target.nation;
        order.organisation = t.target.organisation;
        auto stepper = [&](char const* key, float& v, float lo, float hi, float step, std::string const& shown, std::string const& tip) {
            const float sb = ui.px(22), sy = oy + (rh - sb) * 0.5f;
            char sid[48];
            std::snprintf(sid, sizeof(sid), "dipl.%s.minus", key);
            if(ui.flat_button(sid, {ix, sy, sb, sb}, "−", small)) v = snap(v - step, lo, hi, step);
            const float vw = std::max(ui.px(90), ui.text_width(shown, small) + pad);
            label(ui, {ix + sb, oy, vw, rh}, shown, style::flat_header, align::centre, font_weight::semibold);
            if(ui.hovered({ix + sb, oy, vw, rh})) ui.tooltip(tip);
            std::snprintf(sid, sizeof(sid), "dipl.%s.plus", key);
            if(ui.flat_button(sid, {ix + sb + vw, sy, sb, sb}, "+", small)) v = snap(v + step, lo, hi, step);
            ix += sb * 2 + vw + pad;
        };
        switch(a.input) {
        case action_input::sanction_level:
            stepper("sanction", t.sanction, 0.0f, 1.0f, 0.05f, fmt("level %.2f", t.sanction), "Sanction level 0 (none) to 1 (embargo), held until cleared");
            order.value = t.sanction;
            break;
        case action_input::aid_share:
            stepper("aid", t.aid, 0.05f, 2.0f, 0.05f, fmt("%.2f%% of GDP", t.aid), "A one-off grant, % of your GDP");
            order.value = t.aid;
            break;
        case action_input::pledge:
            stepper("pledge", t.pledge, 0.0f, 300.0f, 5.0f, fmt("%.0f USD/t", t.pledge), "The carbon price you pledge to reach by 2030");
            order.value = t.pledge;
            break;
        case action_input::treaty: {
            static constexpr std::pair<uint8_t, char const*> treaties[] = {{1, "Migration deal"}, {3, "Conditional aid"}, {2, "Debt relief"}};
            for(auto const& [k, name] : treaties) {
                const float tw = ui.text_width(name, ui.px(11)) + pad * 1.5f;
                char sid[40];
                std::snprintf(sid, sizeof(sid), "dipl.treaty.%u", k);
                if(ui.flat_button(sid, {ix, oy + (rh - ui.px(22)) * 0.5f, tw, ui.px(22)}, name, ui.px(11), t.treaty == k)) t.treaty = k;
                ix += tw + ui.px(3);
            }
            order.treaty = t.treaty;
            break;
        }
        case action_input::lobby: {
            auto const& levers = d.lobby_levers;
            if(levers.empty()) break;
            if(t.lobby_lever < 0 || static_cast<std::size_t>(t.lobby_lever) >= levers.size()) {
                t.lobby_lever = 0;
                t.lobby_value = levers[0].current;
            }
            lever_row const& lv = levers[static_cast<std::size_t>(t.lobby_lever)];
            const float sb = ui.px(22), sy = oy + (rh - sb) * 0.5f;
            if(ui.flat_button("dipl.lobby.prev", {ix, sy, sb, sb}, "◀", ui.px(10))) {
                t.lobby_lever = (t.lobby_lever + static_cast<int>(levers.size()) - 1) % static_cast<int>(levers.size());
                t.lobby_value = levers[static_cast<std::size_t>(t.lobby_lever)].current;
            }
            const float nw = ui.px(190);
            label(ui, {ix + sb + ui.px(4), oy, nw, rh}, lv.name, style::flat_text, align::left, font_weight::regular, 11);
            if(ui.flat_button("dipl.lobby.next", {ix + sb + nw + ui.px(8), sy, sb, sb}, "▶", ui.px(10))) {
                t.lobby_lever = (t.lobby_lever + 1) % static_cast<int>(levers.size());
                t.lobby_value = levers[static_cast<std::size_t>(t.lobby_lever)].current;
            }
            ix += sb * 2 + nw + ui.px(8) + pad;
            stepper("lobbyv", t.lobby_value, lv.lo, lv.hi, lv.step, fmt_value(t.lobby_value, lv.step) + " " + lv.unit,
                    "The value you press for (the government's now: " + fmt_value(lv.current, lv.step) + ")");
            order.lever = lv.index;
            order.value = t.lobby_value;
            break;
        }
        default: break;
        }
        // The reason / effect.
        if(ix < body.right() - ui.px(60)) {
            label(ui, {ix, oy, body.right() - ix, rh}, a.reason, a.available ? style::flat_dim : hex(0xA9B2BD), align::left, font_weight::regular, 11);
        }
        if(clicked) {
            t.pending_label = a.label + (t.target.label.empty() ? std::string{} : " → " + t.target.label);
            t.pending_action = sim.perform(order);
            t.result = "…";
        }
        oy += rh;
    }
    if(!t.result.empty()) {
        oy += pad;
        note(ui, ox, oy, ow, t.result, t.result == "…" ? style::flat_dim : t.result_ok ? style::flat_good : style::flat_bad);
    }
}

} // namespace

agent_info const* acting_agent(sim_runner const& sim, tabs_state& tabs) {
    auto const& all = sim.controlled_agents();
    for(auto const& a : all) {
        if(a.agent == tabs.agent) return &a;
    }
    if(all.empty()) return nullptr;
    tabs.agent = all.front().agent;
    return &all.front();
}

void action_tab(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, rect win) {
    tabs_state& t = st.tabs;
    agent_info const* who = sim.status() == sim_status::running ? acting_agent(sim, t) : nullptr;
    tab_kind kind = tab_kind::policies;
    switch(st.open) {
    case hud_action::budget: kind = tab_kind::fiscal; break;
    case hud_action::trade: kind = tab_kind::trade; break;
    case hud_action::diplomacy: kind = tab_kind::diplomacy; break;
    case hud_action::military: kind = tab_kind::military; break;
    default: break;
    }
    std::string title = "Policies";
    if(kind == tab_kind::policies && who) {
        switch(static_cast<agent_kind>(who->kind)) {
        case agent_kind::central_bank: title = "Monetary policy"; break;
        case agent_kind::board: title = "Firm policy"; break;
        case agent_kind::union_federation: title = "Wage bargaining"; break;
        case agent_kind::governor: title = "Regional policy"; break;
        default: break;
        }
    }
    if(kind == tab_kind::fiscal) title = "Fiscal policy";
    if(kind == tab_kind::trade) title = "Trade";
    if(kind == tab_kind::diplomacy) title = "Diplomacy";
    if(kind == tab_kind::military) title = "Military";
    const rect frame = window_frame(ui, st, win, title, who ? who->label : std::string{});
    const float pad = ui.px(style::pad);
    rect body{frame.x + pad * 1.5f, frame.y + pad, frame.w - pad * 3, frame.h - pad * 2};
    ui.set_text_backing(false);
    if(!who) {
        label(ui, body, sim.status() == sim_status::running ? "Control an agent with + (top left) to act" : "Loading…", style::flat_dim, align::centre, font_weight::regular, 14);
        ui.set_text_backing(true);
        return;
    }
    tab_request rq;
    rq.tab = kind;
    rq.agent = who->agent;
    rq.home = who->nation;
    rq.subtab = t.subtab;
    if(kind == tab_kind::diplomacy && t.has_target) rq.target = t.target;
    poll(sim, t, rq);
    const float y = agent_strip(ui, sim, t, who, body.x, body.y, body.w);
    body = {body.x, y, body.w, body.bottom() - y};
    if(!showing(t, rq)) {
        label(ui, body, "Loading…", style::flat_dim, align::centre);
    } else {
        switch(kind) {
        case tab_kind::policies: policies_body(ui, sim, t, *who, body); break;
        case tab_kind::fiscal: fiscal_body(ui, sim, t, *who, body); break;
        case tab_kind::trade: trade_body(ui, sim, t, *who, body); break;
        case tab_kind::diplomacy: diplomacy_body(ui, sim, alerts, t, *who, body); break;
        case tab_kind::military: military_body(ui, sim, t, *who, body); break;
        default: break;
        }
    }
    ui.set_text_backing(true);
}

void power_panel(ui_context& ui, sim_runner& sim, hud_state& st, int nation, bool democracy_hint, rect r) {
    tabs_state& t = st.tabs;
    const float pad = ui.px(style::pad);
    agent_info const* who = sim.status() == sim_status::running ? acting_agent(sim, t) : nullptr;
    tab_request rq;
    rq.tab = tab_kind::politics;
    rq.agent = who ? who->agent : none_id;
    rq.home = who ? who->nation : none_id;
    rq.nation = nation >= 0 ? static_cast<uint32_t>(nation) : none_id;
    if(sim.status() != sim_status::running) return;
    poll(sim, t, rq);
    ui.set_text_backing(false);
    const rect body{r.x + pad * 1.5f, r.y + pad, r.w - pad * 3, r.h - pad * 2};
    if(!showing(t, rq) || !t.snap.power.valid) {
        label(ui, body, "Loading…", style::flat_dim, align::centre);
        ui.set_text_backing(true);
        return;
    }
    power_snapshot const& p = t.snap.power;
    const bool democracy = p.democracy || (democracy_hint && !p.parties.empty() && p.electoral >= 0.5f);
    float y = body.y;
    label(ui, {body.x, y, body.w, ui.px(22)}, democracy ? "Parliament" : "Who holds power", style::flat_text, align::left, font_weight::semibold, 15);
    label(ui, {body.x, y, body.w, ui.px(22)},
          fmt("electoral %.2f", p.electoral) + fmt(" · liberal %.2f", p.liberal) + fmt(" · legitimacy %.2f", p.legitimacy), style::flat_dim, align::right,
          font_weight::regular, 11);
    if(ui.hovered({body.x, y, body.w, ui.px(22)})) {
        ui.tooltip("Electoral and liberal democracy indices (V-Dem style, 0-1), the regime's legitimacy; capture " + fmt("%.2f", p.capture) +
                   ", winning coalition " + fmt("%.2f", p.winning_coalition));
    }
    y += ui.px(26);
    // The acting board first.
    auto cards = [&](std::vector<figure_row> const& rows, float& cy, float bottom) {
        const float ch = ui.px(40);
        for(auto const& f : rows) {
            if(cy + ch > bottom) break;
            const rect card{body.x, cy, body.w, ch - ui.px(4)};
            ui.fill(layer::overlay, card, hex(0xF2F5F9));
            label(ui, {card.x + pad, card.y, ui.px(24), card.h * 0.5f + ui.px(2)}, f.icon, style::flat_header, align::left, font_weight::regular, 14);
            label(ui, {card.x + pad + ui.px(22), card.y, card.w, card.h * 0.5f + ui.px(2)}, f.title, style::flat_text, align::left, font_weight::semibold, 12);
            float ix = card.x + pad + ui.px(22);
            for(auto const& [k, v] : f.items) {
                const float lw = ui.text_width(k, ui.px(10)), vw = ui.text_width(v, ui.px(11), font_weight::semibold);
                if(ix + lw + vw + ui.px(14) > card.right()) break;
                label(ui, {ix, card.y + card.h * 0.5f, lw, card.h * 0.5f}, k, style::flat_dim, align::left, font_weight::regular, 10);
                label(ui, {ix + lw + ui.px(3), card.y + card.h * 0.5f, vw, card.h * 0.5f}, v, style::flat_text, align::left, font_weight::semibold, 11);
                ix += lw + vw + ui.px(14);
            }
            if(ui.hovered(card) && !f.tip.empty()) ui.tooltip(f.tip);
            cy += ch;
        }
    };
    const float leanings_h = ui.px(22) + ui.px(24) * 3.0f + ui.px(20);
    const float content_bottom = body.bottom() - leanings_h - pad;
    if(!p.board.empty()) cards(p.board, y, content_bottom);
    if(democracy) {
        // Hemicycle: arcs by seat share, left to right by economic position; the
        // governing coalition gets an outer band.
        std::vector<std::size_t> order(p.parties.size());
        for(std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return p.parties[a].position[2] < p.parties[b].position[2]; });
        double seats = 0.0;
        for(auto const& q : p.parties) seats += std::max(0.0f, q.seat);
        const float R = std::min(body.w * 0.36f, std::max(ui.px(60), (content_bottom - y) * 0.42f));
        const float cx = body.x + body.w * 0.5f, cy = y + R + ui.px(6);
        const rect sq{std::round(cx - R), std::round(cy - R), std::round(R * 2), std::round(R * 2)};
        const float band = std::round(ui.px(5));
        const rect outer{sq.x - band - ui.px(2), sq.y - band - ui.px(2), sq.w + (band + ui.px(2)) * 2, sq.h + (band + ui.px(2)) * 2};
        float at = 0.5f;
        int hovered = -1;
        const float mx = ui.input().mouse_x - cx, my = -(ui.input().mouse_y - cy);
        const float mr = std::sqrt(mx * mx + my * my);
        const float mt = std::atan2(my, mx) / 6.2831853f;
        if(seats > 0.0) {
            for(std::size_t oi : order) {
                party_row const& q = p.parties[oi];
                const float f = static_cast<float>(std::max(0.0f, q.seat) / seats) * 0.5f;
                if(f <= 0.0f) continue;
                if(q.in_government) ui.arc(outer, at - f, at, 1.0f - band / (outer.w * 0.5f), style::flat_text);
                ui.arc(sq, at - f, at, 0.48f, party_colour(p.parties, oi, p.iso3), std::max(1.0f, ui.px(1)));
                if(my >= 0.0f && mr <= R && mr >= R * 0.48f && mt <= at && mt >= at - f) hovered = static_cast<int>(oi);
                at -= f;
            }
            label(ui, {cx - R, cy - ui.px(34), R * 2, ui.px(18)}, fmt("Government %.0f%%", p.gov_seat_share * 100.0), style::flat_text, align::centre,
                  font_weight::semibold, 13);
            label(ui, {cx - R, cy - ui.px(16), R * 2, ui.px(16)}, "of seats (outer band)", style::flat_dim, align::centre, font_weight::regular, 10);
            if(hovered >= 0) {
                auto const& q = p.parties[static_cast<std::size_t>(hovered)];
                ui.tooltip(q.key + fmt(": %.1f%% of seats", q.seat * 100.0) + fmt(", %.1f%% of the vote", q.vote * 100.0) + (q.in_government ? ", in government" : ""));
            }
        } else {
            label(ui, {body.x, y, body.w, R}, "No seats allocated yet", style::flat_dim, align::centre);
        }
        y = cy + ui.px(8);
        // Party list.
        const float rh = ui.px(19);
        const float c_vote = body.x + body.w * 0.34f, c_seat = body.x + body.w * 0.46f, c_gov = body.x + body.w * 0.56f;
        const float c_ax = body.x + body.w * 0.64f, ax_w = body.w * 0.12f;
        auto head = [&](float x, float w, char const* s, align a) { label(ui, {x, y, w, rh}, s, style::flat_dim, a, font_weight::semibold, 11); };
        head(body.x + ui.px(16), body.w * 0.3f, "Party", align::left);
        head(c_vote, body.w * 0.1f, "Vote", align::right);
        head(c_seat, body.w * 0.09f, "Seats", align::right);
        head(c_gov, body.w * 0.07f, "Gov", align::centre);
        head(c_ax, ax_w, "Secular", align::right);
        head(c_ax + ax_w, ax_w, "Self-expr.", align::right);
        head(c_ax + ax_w * 2, ax_w, "Econ. right", align::right);
        if(ui.hovered({c_ax, y, ax_w * 3, rh})) ui.tooltip("Party positions on the value axes (-1..1): traditional-secular, survival-self-expression, economic left-right");
        y += rh;
        for(std::size_t i = 0; i < p.parties.size() && y + rh <= content_bottom; ++i) {
            party_row const& q = p.parties[i];
            const float sw = ui.px(10);
            ui.fill(layer::overlay, {body.x, std::round(y + (rh - sw) * 0.5f), sw, sw}, party_colour(p.parties, i, p.iso3));
            label(ui, {body.x + ui.px(16), y, body.w * 0.3f, rh}, q.key + (q.incumbent ? "  (head of government)" : ""), style::flat_text, align::left,
                  q.in_government ? font_weight::semibold : font_weight::regular, 11);
            label(ui, {c_vote, y, body.w * 0.1f, rh}, fmt("%.1f%%", q.vote * 100.0), style::flat_text, align::right, font_weight::regular, 11);
            label(ui, {c_seat, y, body.w * 0.09f, rh}, fmt("%.1f%%", q.seat * 100.0), style::flat_text, align::right, font_weight::regular, 11);
            if(q.in_government) label(ui, {c_gov, y, body.w * 0.07f, rh}, "✓", style::flat_good, align::centre, font_weight::semibold, 11);
            for(int ax = 0; ax < 3; ++ax) label(ui, {c_ax + ax_w * static_cast<float>(ax), y, ax_w, rh}, fmt("%+.2f", q.position[ax]), style::flat_dim, align::right, font_weight::regular, 11);
            y += rh;
        }
    } else {
        cards(p.figures, y, content_bottom);
    }
    // Higher echelon leanings: a dot plot per value axis (-1..1), top 10 % / top 25 % /
    // everyone.
    float ly = body.bottom() - leanings_h;
    ui.fill(layer::overlay, {body.x, ly - pad * 0.5f, body.w, std::max(1.0f, std::round(ui.px(1)))}, style::flat_rule);
    label(ui, {body.x, ly, body.w, ui.px(20)}, "Higher echelon leanings", style::flat_text, align::left, font_weight::semibold, 13);
    float lx = body.right();
    for(std::size_t k = p.leanings.size(); k-- > 0;) { // legend, right-aligned
        const float tw = ui.text_width(p.leanings[k].label, ui.px(10));
        lx -= tw;
        label(ui, {lx, ly, tw, ui.px(20)}, p.leanings[k].label, style::flat_dim, align::left, font_weight::regular, 10);
        lx -= ui.px(12);
        ui.fill(layer::overlay, {lx, std::round(ly + ui.px(6)), ui.px(8), ui.px(8)}, k + 1 == p.leanings.size() ? series[8] : series[k]);
        lx -= ui.px(10);
    }
    ly += ui.px(22);
    static constexpr char const* axes[] = {"Traditional – secular", "Survival – self-expression", "Economic left – right"};
    const float lab_w = ui.px(150);
    const rect track_area{body.x + lab_w, 0, body.w - lab_w - ui.px(8), 0};
    for(int ax = 0; ax < 3; ++ax) {
        const float rh = ui.px(24);
        label(ui, {body.x, ly, lab_w, rh}, axes[ax], style::flat_text, align::left, font_weight::regular, 11);
        const float ty = std::round(ly + rh * 0.5f);
        ui.fill(layer::overlay, {track_area.x, ty, track_area.w, std::max(1.0f, std::round(ui.px(1)))}, style::flat_rule);
        ui.fill(layer::overlay, {std::round(track_area.x + track_area.w * 0.5f), ty - ui.px(5), std::max(1.0f, std::round(ui.px(1))), ui.px(10)}, style::flat_dim);
        std::string tip;
        for(std::size_t k = 0; k < p.leanings.size(); ++k) {
            const float v = std::clamp(p.leanings[k].values[ax], -1.0f, 1.0f);
            const float dx = track_area.x + track_area.w * (v + 1.0f) * 0.5f;
            const float dsz = ui.px(9);
            // a 2 px white ring keeps overlapping dots apart
            ui.fill(layer::overlay, {std::round(dx - dsz * 0.5f - ui.px(1)), std::round(ty - dsz * 0.5f - ui.px(1)), std::round(dsz + ui.px(2)), std::round(dsz + ui.px(2))}, style::flat_bg);
            ui.fill(layer::overlay, {std::round(dx - dsz * 0.5f), std::round(ty - dsz * 0.5f), std::round(dsz), std::round(dsz)},
                    k + 1 == p.leanings.size() ? series[8] : series[k]);
            tip += (k ? ", " : "") + p.leanings[k].label + fmt(" %+.2f", p.leanings[k].values[ax]);
        }
        if(ui.hovered({body.x, ly, body.w, rh})) ui.tooltip(std::string(axes[ax]) + ": " + tip + " (pops ranked by income per person)");
        ly += rh;
    }
    label(ui, {track_area.x, ly, track_area.w, ui.px(14)}, "-1", style::flat_dim, align::left, font_weight::regular, 10);
    label(ui, {track_area.x, ly, track_area.w, ui.px(14)}, "+1", style::flat_dim, align::right, font_weight::regular, 10);
    ui.set_text_backing(true);
}

} // namespace geosim::client
