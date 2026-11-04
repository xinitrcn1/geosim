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
    {"🗺", "Map: political"}, {"📈", "Map: economy"}, {"🦠", "Map: epidemic"}, {"🛢", "Map: energy and trade"}, {"🔥", "Map: unrest"},
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

void alert_feed::dismiss(uint64_t key) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](alert const& a) { return a.key == key; }), items_.end());
}

void alert_feed::poll(sim_runner& sim) {
    if(sim.status() != sim_status::running) return;
    if(pending_.valid()) {
        if(pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        journal_batch batch = pending_.get();
        // Oldest first so that the newest ends up on top.
        std::sort(batch.entries.begin(), batch.entries.end(), [](auto const& a, auto const& b) { return a.tick < b.tick; });
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
    case journal_disaster: a.icon = "🌪"; a.title = "Disaster in " + e.nation; a.t = mine ? tone::bad : tone::neutral; break;
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
float agent_bars(ui_context& ui, sim_runner& sim, float y, float above_w) {
    const float strip = std::max(1.0f, std::round(ui.px(style::separator_strip)));
    const float h = ui.px(style::bar_height);
    const float pad = ui.px(style::pad);
    const float font = ui.px(style::font_size);
    const float small = ui.px(style::font_small);
    const float icon = ui.px(style::icon_size);
    for(auto const& panel : sim.read_agents()) {
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
    const float x = ui.px(style::gap);

    // Header: count + dismiss all.
    const float hh = ui.px(26);
    rect header{x, y, w, hh};
    ui.panel(header, panel_kind::translucent);
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
    for(std::size_t i = first; i < items.size() && i < first + visible; ++i) {
        alert const& a = items[i];
        rect card{x, y, w, h};
        ui.panel(card, panel_kind::translucent);
        ui.text_in(layer::overlay, {x + pad, y, icon, h}, a.icon, icon, style::text, align::centre);
        const float tx = x + pad + icon + pad;
        const float asc = ui.px(14) * 0.75f;
        ui.text(layer::overlay, tx, y + ui.px(7) + asc, a.title, font, ui_context::colour(a.t), font_weight::semibold);
        std::string detail = a.detail;
        if(detail.empty() && sim.status() == sim_status::running) {
            detail = format_long_date(core::date_from_tick(a.tick, sim.epoch_days()));
        }
        ui.text(layer::overlay, tx, y + h - ui.px(9), detail, small, style::text_dim);
        const float bs = ui.px(24);
        char id[40];
        std::snprintf(id, sizeof(id), "alert.%llx", static_cast<unsigned long long>(a.key));
        if(ui.button(id, layer::overlay, {card.right() - bs - ui.px(6), y + (h - bs) * 0.5f, bs, bs}, "✖", ui.px(13), false, "Dismiss")) {
            dismiss = a.key;
        }
        y += h + gap;
    }
    if(dismiss) alerts.dismiss(dismiss);
}

} // namespace

void build_hud(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st) {
    st.requested_speed = -1;
    const rect bar = top_bar(ui, sim, st);
    float y = agent_bars(ui, sim, bar.bottom(), bar.w);
    alert_list(ui, sim, alerts, st, y + ui.px(style::gap)); // alerts float: they keep their margins
}

} // namespace geosim::client
