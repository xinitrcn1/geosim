#include "ui_layout.hpp"

#include "sim/model.hpp"
#include "style.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geosim::client {

namespace {

using steady = std::chrono::steady_clock;

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t key_of(journal_entry const& e) {
    uint64_t h = mix(static_cast<uint64_t>(static_cast<uint32_t>(e.tick)), e.code);
    for(char c : e.nation) h = mix(h, static_cast<unsigned char>(c));
    for(char c : e.partner) h = mix(h, static_cast<unsigned char>(c) + 256u);
    h = mix(h, static_cast<uint64_t>(std::lround(e.after * 1000.0f)));
    return h;
}

char const* const month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct speed_button {
    char const* icon;
    char const* tip;
};
// SimCity 4-style animals, slow to fast.
const speed_button speed_buttons[max_speed] = {
    {"🐌", "Speed 1: 1 day/s  [1]"},
    {"🐢", "Speed 2: 2 days/s  [2]"},
    {"🦙", "Speed 3: 4 days/s  [3]"},
    {"🐆", "Speed 4: 8 days/s  [4]"},
    {"🚀", "Speed 5: as fast as the simulation runs  [5]"},
};

struct icon_button {
    char const* icon;
    char const* tip;
};
const icon_button action_buttons[] = {
    {"🏛", "Government policy"}, {"💰", "Budget and taxes"}, {"🤝", "Diplomacy"}, {"⚔", "Military"}, {"🗳", "Politics and elections"},
};
const icon_button filter_buttons[] = {
    {"🗺", "Map: political"}, {"🌦", "Map: climate and hazards"}, {"📈", "Map: economy"}, {"🚢", "Map: trade"},
    {"🛢", "Map: energy"},    {"🦠", "Map: epidemic"},           {"🔥", "Map: unrest and insurgencies"},
};
static_assert(std::size(action_buttons) == 5 && std::size(filter_buttons) == static_cast<std::size_t>(map_filter::count));

} // namespace

std::string format_long_date(core::civil_date d) {
    static char const* const weekdays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"}; // 1970-01-01 was a Thursday
    const int64_t days = core::days_from_civil(d);
    const auto wd = static_cast<std::size_t>(((days % 7) + 7) % 7);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s %u %s %d", weekdays[wd], d.day, month_names[(d.month - 1) % 12], d.year);
    return buf;
}

// --- alerts -----------------------------------------------------------------

void alert_feed::push_local(std::string icon, std::string title, std::string detail, tone t) {
    alert a;
    a.key = mix(0xA1E27ull, ++local_serial_);
    a.icon = std::move(icon);
    a.title = std::move(title);
    a.detail = std::move(detail);
    a.t = t;
    items_.insert(items_.begin(), std::move(a));
}

void alert_feed::upsert(alert a) {
    for(auto& existing : items_) {
        if(existing.key == a.key) {
            existing = std::move(a);
            return;
        }
    }
    if(!seen_.insert(a.key).second) return; // dismissed earlier
    items_.insert(items_.begin(), std::move(a));
    if(items_.size() > 200) items_.resize(200);
}

void alert_feed::dismiss(uint64_t key) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](alert const& a) { return a.key == key; }), items_.end());
}

void alert_feed::poll(sim_runner& sim) {
    if(sim.status() != sim_status::running) return;
    if(pending_.valid()) {
        if(pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        journal_batch batch = pending_.get();
        // Oldest first so that the newest ends up on top.
        std::stable_sort(batch.entries.begin(), batch.entries.end(), [](auto const& a, auto const& b) { return a.tick < b.tick; });
        for(auto const& e : batch.entries) add(e, sim);
        scanned_to_ = std::max(scanned_to_, batch.scanned_to - 1); // re-scan the boundary day; keys dedupe
        if(items_.size() > 200) items_.resize(200);
    }
    const auto now = steady::now();
    if(sim.tick() > scanned_to_ && now - last_request_ > std::chrono::milliseconds(250)) {
        pending_ = sim.request_journal(scanned_to_);
        last_request_ = now;
    }
}

void alert_feed::add(journal_entry const& e, sim_runner const& sim) {
    const uint64_t key = key_of(e);
    if(!seen_.insert(key).second) return;
    using namespace sim;
    const bool mine = sim.controls(e.nation) || sim.controls(e.partner);
    const std::string pair = e.partner.empty() ? e.nation : e.nation + " – " + e.partner;
    alert a;
    a.key = key;
    a.tick = e.tick;
    a.t = tone::neutral;
    switch(e.code) {
    case journal_war: a.icon = "⚔"; a.title = "War: " + pair; a.t = mine ? tone::bad : tone::neutral; break;
    case journal_peace: a.icon = "🕊"; a.title = "Peace: " + pair; a.t = mine ? tone::good : tone::neutral; break;
    case journal_coup: a.icon = "💥"; a.title = "Coup in " + e.nation; a.t = mine ? tone::bad : tone::neutral; break;
    case journal_regime_change: a.icon = "🏛"; a.title = "Regime change in " + e.nation; a.t = mine ? tone::bad : tone::neutral; break;
    case journal_election: a.icon = "🗳"; a.title = "Election in " + e.nation; break;
    case journal_no_confidence:
        if(!mine) return;
        a.icon = "✋"; a.title = "No-confidence vote"; a.t = tone::bad; break;
    case journal_default: a.icon = "💸"; a.title = "Sovereign default: " + e.nation; a.t = mine ? tone::bad : tone::neutral; break;
    case journal_sanction: a.icon = "🚫"; a.title = "Sanctions involving " + e.nation; a.t = mine ? tone::bad : tone::neutral; break;
    case journal_insurgency:
        a.icon = "🔥";
        a.title = (e.after > 0.5f ? "Insurgency in " : "Insurgency ends in ") + e.nation;
        a.t = mine ? (e.after > 0.5f ? tone::bad : tone::good) : tone::neutral;
        break;
    case journal_rebels:
        a.icon = "🏴";
        a.title = (e.after > 0.5f ? "Rebel movement in " : e.after < -0.5f ? "Rebels defeated in " : "Settlement with rebels in ") + e.nation;
        a.t = mine ? (e.after > 0.5f ? tone::bad : tone::good) : tone::neutral;
        break;
    // journal_disaster: replaced by the disaster records (hazard_feed).
    case journal_chokepoint: a.icon = "🚢"; a.title = "Shipping chokepoint blocked"; break;
    case journal_variant: a.icon = "🦠"; a.title = "New pathogen variant"; break;
    case journal_cyber: a.icon = "💻"; a.title = "Cyber attack: " + pair; a.t = mine ? tone::bad : tone::neutral; break;
    case journal_dispute:
        if(!mine) return;
        a.icon = "⚠"; a.title = (e.after > 0.5f ? "Dispute: " : "Dispute ended: ") + pair; a.t = e.after > 0.5f ? tone::bad : tone::good;
        break;
    case journal_concession:
        if(!mine) return;
        a.icon = "🏳"; a.title = "Concession: " + pair; a.t = sim.controls(e.nation) ? tone::bad : tone::good; break;
    case journal_alliance_call:
        if(!mine) return;
        a.icon = "📯"; a.title = (e.after > 0.5f ? "Ally joined: " : "Ally declined: ") + pair; a.t = e.after > 0.5f ? tone::good : tone::bad;
        break;
    case journal_agreement:
        if(!mine) return;
        a.icon = "📜";
        a.title = (e.after > 1.5f ? "Agreement ended: " : e.after > 0.5f ? "Agreement signed: " : "Agreement broken: ") + pair;
        a.t = e.after > 0.5f && e.after < 1.5f ? tone::good : tone::bad;
        break;
    case journal_court:
        if(!mine) return;
        a.icon = "⚖"; a.title = "Court strikes down a policy"; a.t = tone::bad; break;
    case journal_term_limit:
        if(!mine) return;
        a.icon = "⏳"; a.title = e.after > 0.5f ? "Term limit evaded" : "Term limit respected"; break;
    default:
        return;
    }
    items_.insert(items_.begin(), std::move(a));
}

// --- HUD --------------------------------------------------------------------

namespace {

void separator(ui_context& ui, float x, rect bar) {
    ui.fill(layer::scene, {std::round(x), bar.y + ui.px(8), std::max(1.0f, std::round(ui.px(1))), bar.h - ui.px(16)}, style::separator);
}

// Top bar: returns its rect.
rect top_bar(ui_context& ui, sim_runner& sim, hud_state& st) {
    const float h = ui.px(style::top_bar_height);
    const float b = ui.px(style::button_size);
    const float icon = ui.px(style::icon_size);
    const float font = ui.px(style::font_size);
    const float pad = ui.px(style::pad);
    const float gap = ui.px(2);

    // Measure first (the panel goes under its contents). No flag or nation
    // here: the player may control several nations; each agent bar names its own.
    const float date_w = ui.text_width("Wed 30 Sep 2020", font, font_weight::semibold);
    const float buttons = static_cast<float>(1 + max_speed) * (b + gap) + static_cast<float>(std::size(action_buttons) + std::size(filter_buttons)) * (b + gap);
    const float w = pad + date_w + pad * 2 + buttons + pad * 4;
    rect bar{0, 0, std::round(w), std::round(h)}; // flush with the window's top-left corner
    ui.panel(bar, panel_kind::opaque);

    float x = bar.x + pad;
    const float by = bar.y + (h - b) * 0.5f;
    // Date (fixed width so the controls do not jitter).
    std::string date = sim.status() == sim_status::running ? format_long_date(sim.date()) : (sim.status() == sim_status::loading ? "Loading…" : "No simulation");
    ui.text_in(layer::scene, {x, bar.y, date_w, h}, date, font, style::text, align::left, font_weight::semibold);
    x += date_w + pad;
    separator(ui, x, bar);
    x += pad;

    // Time controls: pause + speeds 1..5 (SimCity 4 style).
    const int speed = sim.speed();
    if(ui.button("time.pause", layer::scene, {x, by, b, b}, "⏸", icon, speed == 0, "Pause  [Space]")) st.requested_speed = 0;
    x += b + gap;
    for(int s = 1; s <= max_speed; ++s) {
        char id[16];
        std::snprintf(id, sizeof(id), "time.%d", s);
        std::string tip = speed_buttons[s - 1].tip;
        if(speed == s && sim.measured_days_per_second() > 0) {
            char rate[48];
            std::snprintf(rate, sizeof(rate), "  (now %.1f days/s)", sim.measured_days_per_second());
            tip += rate;
        }
        if(ui.button(id, layer::scene, {x, by, b, b}, speed_buttons[s - 1].icon, icon, speed == s, tip)) st.requested_speed = s;
        x += b + gap;
    }
    x += pad - gap;
    separator(ui, x, bar);
    x += pad;

    // Important actions (panels are stubs for now: the button toggles the request).
    for(std::size_t i = 0; i < std::size(action_buttons); ++i) {
        const auto act = static_cast<hud_action>(i + 1);
        if(ui.button(action_buttons[i].tip, layer::scene, {x, by, b, b}, action_buttons[i].icon, icon, st.open == act, action_buttons[i].tip)) {
            st.open = st.open == act ? hud_action::none : act;
        }
        x += b + gap;
    }
    x += pad - gap;
    separator(ui, x, bar);
    x += pad;

    // Map filters (one active at a time).
    for(std::size_t i = 0; i < std::size(filter_buttons); ++i) {
        const auto f = static_cast<map_filter>(i);
        if(ui.button(filter_buttons[i].tip, layer::scene, {x, by, b, b}, filter_buttons[i].icon, icon, st.filter == f, filter_buttons[i].tip)) {
            st.filter = f;
        }
        x += b + gap;
    }
    return bar;
}

// The agent bars: one continuous pane of glass under the top bar, flush at
// x = 0 and stacked without gaps; a thin white strip marks each shared edge
// (top bar / first bar, bar / bar). Returns the bottom of the last bar.
float agent_bars(ui_context& ui, sim_runner& sim, float y, float above_w, std::size_t max_bars) {
    const float strip = std::max(1.0f, std::round(ui.px(style::separator_strip)));
    const float h = ui.px(style::bar_height);
    const float pad = ui.px(style::pad);
    const float font = ui.px(style::font_size);
    const float small = ui.px(style::font_small);
    const float icon = ui.px(style::icon_size);
    std::size_t shown = 0;
    for(auto const& panel : sim.read_agents()) {
        if(shown++ >= max_bars) break;
        // Measure.
        // flag of the agent's nation, the agent's icon, "<Nation> <agent>"
        float w = pad + ui.text_width(panel.flag, icon) + ui.px(6) + ui.text_width(panel.icon, icon) + ui.px(6) +
                  ui.text_width(panel.title, font, font_weight::semibold) + pad * 2;
        for(auto const& it : panel.items) {
            w += ui.text_width(it.label, small) + ui.px(5) + ui.text_width(it.value, font, font_weight::semibold) + pad * 2;
        }
        rect bar{0, y, std::round(w), std::round(h)};
        ui.panel(bar, panel_kind::glass_flush);
        ui.fill(layer::overlay, {0, y, std::min(above_w, bar.w), strip}, style::glass_separator);
        above_w = bar.w;
        float x = bar.x + pad;
        x += ui.text_in(layer::overlay, {x, bar.y, w, h}, panel.flag, icon, style::text);
        x += ui.px(6);
        x += ui.text_in(layer::overlay, {x, bar.y, w, h}, panel.icon, icon, style::text);
        x += ui.px(6);
        x += ui.text_in(layer::overlay, {x, bar.y, w, h}, panel.title, font, style::text, align::left, font_weight::semibold);
        x += pad * 2;
        for(auto const& it : panel.items) {
            const float x0 = x;
            x += ui.text_in(layer::overlay, {x, bar.y, w, h}, it.label, small, style::text_dim);
            x += ui.px(5);
            x += ui.text_in(layer::overlay, {x, bar.y, w, h}, it.value, font, ui_context::colour(it.t), align::left, font_weight::semibold);
            if(ui.hovered({x0, bar.y, x - x0, h})) ui.tooltip(it.tip);
            x += pad * 2;
        }
        y = bar.bottom();
    }
    return y;
}

void alert_list(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, float y) {
    auto const& items = alerts.items();
    if(items.empty()) return;
    const float pad = ui.px(style::pad);
    const float w = ui.px(style::alert_width);
    const float h = ui.px(style::alert_height);
    const float gap = ui.px(style::gap);
    const float font = ui.px(14);
    const float small = ui.px(style::font_small);
    const float icon = ui.px(20);
    const float x = ui.width() - w - gap; // the right side of the window

    // Header: count + dismiss all.
    const float hh = ui.px(26);
    rect header{x, y, w, hh};
    ui.panel(header, panel_kind::translucent);
    const float h_impact = ui.px(style::alert_height + 16); // a card with a third (impact) line
    const auto visible = static_cast<std::size_t>(std::max(1.0f, std::floor((ui.height() - y - hh - gap * 2) / (h + gap))));
    const float max_scroll = static_cast<float>(items.size() > visible ? items.size() - visible : 0);
    st.alert_scroll = std::clamp(st.alert_scroll, 0.0f, max_scroll);
    const auto first = static_cast<std::size_t>(st.alert_scroll);
    char title[64];
    if(items.size() > visible) {
        std::snprintf(title, sizeof(title), "Alerts %zu–%zu of %zu", first + 1, std::min(items.size(), first + visible), items.size());
    } else {
        std::snprintf(title, sizeof(title), "Alerts (%zu)", items.size());
    }
    ui.text_in(layer::overlay, {x + pad, y, w, hh}, title, small, style::text_dim, align::left, font_weight::semibold);
    const float cw = ui.text_width("Dismiss all", small) + pad * 2;
    if(ui.button("alerts.clear", layer::overlay, {header.right() - cw, y, cw, hh}, "Dismiss all", small)) {
        alerts.dismiss_all();
        st.alert_scroll = 0;
        return;
    }
    y += hh + gap;

    rect column{x, y, w, static_cast<float>(visible) * (h + gap)};
    // Wheel scrolls the list; the new offset applies from the next frame.
    st.alert_scroll = std::clamp(st.alert_scroll - ui.take_wheel(column), 0.0f, max_scroll);

    uint64_t dismiss = 0;
    const float bottom = ui.height() - gap;
    for(std::size_t i = first; i < items.size() && i < first + visible; ++i) {
        alert const& a = items[i];
        const float ch = a.impact.empty() ? h : h_impact;
        if(i > first && y + ch > bottom) break;
        rect card{x, y, w, ch};
        ui.panel(card, panel_kind::translucent);
        ui.text_in(layer::overlay, {x + pad, y, icon, ch}, a.icon, icon, style::text, align::centre);
        const float tx = x + pad + icon + pad;
        const float asc = ui.px(14) * 0.75f;
        ui.text(layer::overlay, tx, y + ui.px(7) + asc, a.title, font, ui_context::colour(a.t), font_weight::semibold);
        std::string detail = a.detail;
        if(detail.empty() && sim.status() == sim_status::running) {
            detail = format_long_date(core::date_from_tick(a.tick, sim.epoch_days()));
        }
        ui.text(layer::overlay, tx, y + h - ui.px(9), detail, small, style::text_dim);
        if(!a.impact.empty()) ui.text(layer::overlay, tx, y + ch - ui.px(9), a.impact, small, ui_context::colour(a.t == tone::bad ? tone::bad : tone::neutral));
        const float bs = ui.px(24);
        char id[40];
        std::snprintf(id, sizeof(id), "alert.%llx", static_cast<unsigned long long>(a.key));
        if(ui.button(id, layer::overlay, {card.right() - bs - ui.px(6), y + (ch - bs) * 0.5f, bs, bs}, "✖", ui.px(13), false, "Dismiss")) {
            dismiss = a.key;
        }
        y += ch + gap;
    }
    if(dismiss) alerts.dismiss(dismiss);
}

} // namespace

// The open view's tab: flat white, a blue title strip, below the top bar and
// left of the alert column.
void view_tab(ui_context& ui, map_tab_info const& tab, hud_state& st, float y, bool alerts_shown) {
    const float pad = ui.px(style::pad);
    const float gap = ui.px(style::gap);
    const float w = ui.px(style::tab_width);
    const float hh = ui.px(style::tab_header);
    const float font = ui.px(14);
    const float small = ui.px(style::font_small);
    const float line = ui.px(19);
    const float right = alerts_shown ? ui.width() - ui.px(style::alert_width) - gap * 2 : ui.width() - gap;
    const float x = right - w;
    float h = hh + pad + static_cast<float>(tab.legend.size()) * line + pad;
    if(!tab.note.empty()) h += line;
    if(!tab.summary.empty()) h += pad + static_cast<float>(tab.summary.size()) * line;
    rect r{x, y, w, h};
    ui.flat_panel(r, hh);
    ui.text_in(layer::overlay, {x + pad, y, w, hh}, tab.title, font, style::text, align::left, font_weight::semibold);
    const float bs = ui.px(22);
    if(ui.button("tab.close", layer::overlay, {r.right() - bs - ui.px(3), y + (hh - bs) * 0.5f, bs, bs}, "✖", ui.px(12), false,
                 "Close (political view)")) {
        st.filter = map_filter::political;
    }
    ui.set_text_backing(false);
    float cy = y + hh + pad;
    const float sw = ui.px(14);
    for(auto const& item : tab.legend) {
        ui.fill(layer::overlay, {x + pad, std::round(cy + (line - sw) * 0.5f), sw, sw}, item.colour);
        ui.text_in(layer::overlay, {x + pad + sw + pad, cy, w, line}, item.label, small, style::flat_text);
        cy += line;
    }
    if(!tab.note.empty()) {
        ui.text_in(layer::overlay, {x + pad, cy, w, line}, tab.note, small, style::flat_dim);
        cy += line;
    }
    if(!tab.summary.empty()) {
        cy += pad * 0.5f;
        ui.fill(layer::overlay, {x + pad, std::round(cy), w - pad * 2, std::max(1.0f, std::round(ui.px(1)))}, style::flat_rule);
        cy += pad * 0.5f;
        for(auto const& s : tab.summary) {
            ui.text_in(layer::overlay, {x + pad, cy, w - pad * 2, line}, s, small, style::flat_text);
            cy += line;
        }
    }
    ui.set_text_backing(true);
}

// The Politics tab: the electoral tile map of one nation, its mode buttons and
// the national result (or regime indicators), flat white like the view tabs.
void politics_tab(ui_context& ui, electoral_view_info const& info, hud_state& st, float y, bool alerts_shown) {
    const float pad = ui.px(style::pad);
    const float gap = ui.px(style::gap);
    const float hh = ui.px(style::tab_header);
    const float font = ui.px(14);
    const float small = ui.px(style::font_small);
    const float line = ui.px(18);
    const float right = alerts_shown ? ui.width() - ui.px(style::alert_width) - gap * 2 : ui.width() - gap;
    const float w = std::min(ui.px(560), right - gap);
    const float x = right - w;
    // Mode buttons, wrapped into rows.
    const float bh = ui.px(22);
    std::vector<rect> buttons;
    float bx = x + pad, by = 0;
    for(auto const& m : info.modes) {
        const float bw = ui.text_width(m, small) + pad * 2;
        if(bx + bw > x + w - pad && bx > x + pad) {
            bx = x + pad;
            by += bh + ui.px(4);
        }
        buttons.push_back({bx, by, bw, bh});
        bx += bw + ui.px(4);
    }
    const float rows_h = info.modes.empty() ? 0.0f : by + bh;
    const float map_w = w - pad * 2;
    const float map_h = std::min(ui.px(320), std::max(ui.px(140), map_w / std::max(0.4f, info.aspect)));
    // The national strip, word-wrapped to the tab.
    std::vector<std::string> strip;
    for(auto const& sline : info.strip) {
        std::string cur;
        std::size_t at = 0;
        while(at < sline.size()) {
            const std::size_t sp = std::min(sline.find(' ', at), sline.size());
            const std::string word = sline.substr(at, sp - at);
            const std::string next = cur.empty() ? word : cur + " " + word;
            if(!cur.empty() && ui.text_width(next, small) > w - pad * 2) {
                strip.push_back(cur);
                cur = "   " + word;
            } else {
                cur = next;
            }
            at = sp + 1;
        }
        if(!cur.empty()) strip.push_back(cur);
    }
    const float h = hh + pad + rows_h + pad + map_h + pad + (info.legend.empty() ? 0.0f : line) +
                    static_cast<float>(strip.size()) * line + pad;
    rect r{x, y, w, h};
    ui.flat_panel(r, hh);
    float tw = ui.text_in(layer::overlay, {x + pad, y, w, hh}, "Politics", font, style::text, align::left, font_weight::semibold);
    tw += ui.text_in(layer::overlay, {x + pad + tw + pad, y, w, hh}, info.nation, font, style::text_dim);
    const float bs = ui.px(22);
    if(ui.button("politics.close", layer::overlay, {r.right() - bs - ui.px(3), y + (hh - bs) * 0.5f, bs, bs}, "✖", ui.px(12), false, "Close")) {
        st.open = hud_action::none;
    }
    if(ui.button("politics.next", layer::overlay, {r.right() - 2 * bs - ui.px(6), y + (hh - bs) * 0.5f, bs, bs}, "▶", ui.px(12), false,
                 "Next nation (or click a nation on the map)")) {
        st.politics_step = 1;
    }
    if(ui.button("politics.prev", layer::overlay, {r.right() - 3 * bs - ui.px(9), y + (hh - bs) * 0.5f, bs, bs}, "◀", ui.px(12), false,
                 "Previous nation")) {
        st.politics_step = -1;
    }
    float cy = y + hh + pad;
    for(std::size_t i = 0; i < buttons.size(); ++i) {
        rect b = buttons[i];
        b.y += cy;
        char id[48];
        std::snprintf(id, sizeof(id), "politics.mode.%zu", i);
        if(ui.flat_button(id, b, info.modes[i], small, static_cast<int>(i) == info.mode)) st.politics_mode = static_cast<int>(i);
    }
    cy += rows_h + pad;
    // Tiles, fitted into the map area with the layout's aspect.
    float lw = map_w, lh = map_w / std::max(0.05f, info.aspect);
    if(lh > map_h) {
        lh = map_h;
        lw = map_h * info.aspect;
    }
    const float ox = x + pad + (map_w - lw) * 0.5f, oy = cy + (map_h - lh) * 0.5f;
    st.politics_tile = -1;
    ui.set_text_backing(false);
    electoral_raster const* ras = info.raster.get();
    if(!ras || ras->runs.empty()) {
        ui.text_in(layer::overlay, {x + pad, cy, map_w, map_h}, info.valid ? "No regions" : "Loading…", small, style::flat_dim, align::centre);
    } else {
        // The packed inset map: run-length quads scaled into the area, white region
        // borders, grey inset frames.
        const float sx = lw / static_cast<float>(ras->w), sy = lh / static_cast<float>(ras->h);
        for(auto const& run : ras->runs) {
            rgba c = style::flat_rule;
            if(run.value >= 0) c = static_cast<std::size_t>(run.value) < info.colours.size() ? info.colours[static_cast<std::size_t>(run.value)] : style::flat_rule;
            else if(run.value == electoral_raster::border) c = style::flat_bg;
            else if(run.value == electoral_raster::frame) c = style::flat_dim;
            const float x0 = std::round(ox + static_cast<float>(run.x) * sx), x1 = std::round(ox + static_cast<float>(run.x + run.len) * sx);
            const float y0 = std::round(oy + static_cast<float>(run.row) * sy), y1 = std::round(oy + static_cast<float>(run.row + 1) * sy);
            if(x1 > x0 && y1 > y0) ui.fill(layer::overlay, {x0, y0, x1 - x0, y1 - y0}, c);
        }
        // Hover: the cell under the pointer -> its region.
        rect area{ox, oy, lw, lh};
        if(ui.hovered(area)) {
            const int ci = std::clamp(static_cast<int>((ui.input().mouse_x - ox) / sx), 0, ras->w - 1);
            const int cj = std::clamp(static_cast<int>((ui.input().mouse_y - oy) / sy), 0, ras->h - 1);
            const int32_t v = ras->cell[static_cast<std::size_t>(cj) * static_cast<std::size_t>(ras->w) + static_cast<std::size_t>(ci)];
            if(v >= 0 && static_cast<std::size_t>(v) < ras->regions.size()) st.politics_tile = ras->regions[static_cast<std::size_t>(v)];
        }
    }
    cy += map_h + pad;
    if(!info.legend.empty()) {
        float lx = x + pad;
        const float sw = ui.px(12);
        for(auto const& item : info.legend) {
            const float iw = sw + ui.px(4) + ui.text_width(item.label, small) + pad;
            if(lx + iw > x + w) break;
            ui.fill(layer::overlay, {lx, std::round(cy + (line - sw) * 0.5f), sw, sw}, item.colour);
            ui.text_in(layer::overlay, {lx + sw + ui.px(4), cy, iw, line}, item.label, small, style::flat_text);
            lx += iw;
        }
        cy += line;
    }
    for(auto const& sline : strip) {
        ui.text_in(layer::overlay, {x + pad, cy, w - pad * 2, line}, sline, small, style::flat_text);
        cy += line;
    }
    ui.set_text_backing(true);
}

void build_hud(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, map_tab_info const* tab,
               electoral_view_info const* politics) {
    st.requested_speed = -1;
    st.politics_step = 0;
    const rect bar = top_bar(ui, sim, st);
    const bool politics_open = politics && st.open == hud_action::politics;
    const bool tab_open = !politics_open && tab && st.filter != map_filter::political;
    agent_bars(ui, sim, bar.bottom(), bar.w, tab_open || politics_open ? 1 : static_cast<std::size_t>(-1));
    alert_list(ui, sim, alerts, st, bar.bottom() + ui.px(style::gap)); // right side, below the top bar; floating cards keep their margins
    if(politics_open) politics_tab(ui, *politics, st, bar.bottom() + ui.px(style::gap), !alerts.items().empty());
    else if(tab_open) view_tab(ui, *tab, st, bar.bottom() + ui.px(style::gap), !alerts.items().empty());
    if(!politics_open) st.politics_tile = -1;
}

void draw_map_tooltip(ui_context& ui, map_tooltip_info const& info, hud_state& st) {
    if(!info.valid) {
        st.tooltip_rect = {};
        return;
    }
    const float pad = ui.px(style::pad);
    const float font = ui.px(14);
    const float small = ui.px(style::font_small);
    const float line = ui.px(18);
    const bool expanded = st.tooltip_expanded && !info.details.empty();
    float w = ui.px(expanded ? 300 : 240);
    w = std::max(w, ui.text_width(info.title, font, font_weight::semibold) + pad * 2);
    const std::string headline = info.value.empty() ? info.label : info.label + ": " + info.value;
    w = std::max(w, ui.text_width(headline, small, font_weight::semibold) + pad * 2);
    const float hh = ui.px(style::tab_header);
    float h = hh + line * 1.2f + pad;
    if(expanded) h += pad + static_cast<float>(info.details.size()) * line;
    else if(!info.details.empty()) h += line;
    float x = std::min(st.tooltip_x + ui.px(14), ui.width() - w - ui.px(2));
    float y = std::min(st.tooltip_y + ui.px(16), ui.height() - h - ui.px(2));
    x = std::max(0.0f, x);
    y = std::max(0.0f, y);
    rect r{std::round(x), std::round(y), std::round(w), std::round(h)};
    st.tooltip_rect = r;
    ui.flat_panel(r, hh);
    // Title strip: region, nation.
    float tw = ui.text_in(layer::overlay, {r.x + pad, r.y, w, hh}, info.title, font, style::text, align::left, font_weight::semibold);
    if(!info.subtitle.empty()) ui.text_in(layer::overlay, {r.x + pad + tw + pad, r.y, w, hh}, info.subtitle, small, style::text_dim);
    ui.set_text_backing(false);
    float cy = r.y + hh + pad * 0.5f;
    const rgba vc = info.t == tone::bad ? style::flat_bad : info.t == tone::good ? style::flat_good : style::flat_text;
    ui.text_in(layer::overlay, {r.x + pad, cy, w, line * 1.2f}, headline, small, vc, align::left, font_weight::semibold);
    cy += line * 1.2f;
    if(expanded) {
        ui.fill(layer::overlay, {r.x + pad, std::round(cy + pad * 0.5f), w - pad * 2, std::max(1.0f, std::round(ui.px(1)))}, style::flat_rule);
        cy += pad;
        for(auto const& [label, value] : info.details) {
            if(value.empty()) {
                ui.text_in(layer::overlay, {r.x + pad, cy, w, line}, label, small, style::flat_header, align::left, font_weight::semibold);
            } else {
                ui.text_in(layer::overlay, {r.x + pad, cy, w, line}, label, small, style::flat_dim);
                ui.text_in(layer::overlay, {r.x, cy, w - pad, line}, value, small, style::flat_text, align::right);
            }
            cy += line;
        }
    } else if(!info.details.empty()) {
        ui.text_in(layer::overlay, {r.x + pad, cy, w, line}, "Point here for details", small, style::flat_dim);
    }
    ui.set_text_backing(true);
}

} // namespace geosim::client
