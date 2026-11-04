#include "ui_layout.hpp"

#include "sim/model.hpp"
#include "style.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cctype>
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
    a.cat = alert_category::system;
    items_.insert(items_.begin(), std::move(a));
    ++unread_;
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
    ++unread_;
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
    // Categories for the top-bar filters.
    switch(e.code) {
    case journal_default: case journal_chokepoint: a.cat = alert_category::economic; break;
    case journal_election: case journal_regime_change: case journal_no_confidence: case journal_court: case journal_term_limit:
        a.cat = alert_category::political; break;
    case journal_war: case journal_coup: case journal_cyber: case journal_rebels: a.cat = alert_category::security; break;
    case journal_peace: case journal_sanction: case journal_dispute: case journal_concession: case journal_alliance_call: case journal_agreement:
        a.cat = alert_category::diplomacy; break;
    case journal_variant: a.cat = alert_category::health; break;
    case journal_insurgency: a.cat = alert_category::society; break;
    default: a.cat = alert_category::system; break;
    }
    items_.insert(items_.begin(), std::move(a));
    ++unread_;
}

// --- HUD --------------------------------------------------------------------

namespace {

void separator(ui_context& ui, float x, rect bar) {
    ui.fill(layer::scene, {std::round(x), bar.y + ui.px(8), std::max(1.0f, std::round(ui.px(1))), bar.h - ui.px(16)}, style::separator);
}

// Top bar: returns its rect.
rect top_bar(ui_context& ui, sim_runner& sim, hud_state& st, alert_feed& alerts) {
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
    rect bar{0, 0, std::max(std::round(w), ui.width()), std::round(h)}; // flush with the window's top edge, full width
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
    // Right end: alert category filters and the notification bell.
    {
        static constexpr char const* cat_icon[alert_filter_count] = {"📈", "🏛", "⚔", "🤝", "🌀", "🦠", "✊"};
        static constexpr char const* cat_tip[alert_filter_count] = {
            "Alerts: economy (defaults, shipping)", "Alerts: politics (elections, regime changes, votes, courts)",
            "Alerts: security (wars, coups, rebels, cyber attacks)", "Alerts: diplomacy (peace, sanctions, disputes, agreements, allies)",
            "Alerts: disasters (cyclones, floods, droughts, earthquakes, tsunamis)", "Alerts: health (pathogen variants)",
            "Alerts: society (insurgencies)"};
        std::size_t count[alert_filter_count] = {};
        for(auto const& a : alerts.items()) {
            if(a.cat != alert_category::system) ++count[static_cast<std::size_t>(a.cat)];
        }
        float rx = bar.right() - pad - b;
        // The bell: toggles the list and marks everything seen; a red badge counts the unread.
        if(ui.button("alerts.bell", layer::scene, {rx, by, b, b}, "🔔", icon, !st.alerts_hidden,
                     st.alerts_hidden ? "Show alerts" : "Hide alerts (marks them seen)")) {
            st.alerts_hidden = !st.alerts_hidden;
            alerts.mark_seen();
        }
        if(alerts.unread() > 0) {
            char n[8];
            if(alerts.unread() > 99) std::snprintf(n, sizeof(n), "99+");
            else std::snprintf(n, sizeof(n), "%zu", alerts.unread());
            const float bsz = ui.px(11);
            const float bw = std::max(ui.px(14), ui.text_width(n, bsz, font_weight::semibold) + ui.px(6));
            const rect badge{std::round(rx + b - bw + ui.px(3)), std::round(by - ui.px(1)), std::round(bw), std::round(ui.px(14))};
            ui.fill(layer::scene, badge, hex(0xE0322B));
            ui.text_in(layer::scene, badge, n, bsz, style::text, align::centre, font_weight::semibold);
        }
        rx -= pad;
        separator(ui, rx, bar);
        rx -= pad;
        for(std::size_t k = alert_filter_count; k-- > 0;) {
            rx -= b;
            const bool on = st.alert_filter >> k & 1u;
            char id[40];
            std::snprintf(id, sizeof(id), "alerts.filter.%zu", k);
            if(ui.button(id, layer::scene, {rx, by, b, b}, cat_icon[k], icon, on, cat_tip[k])) st.alert_filter ^= 1u << k;
            if(count[k] > 0) {
                char n[8];
                if(count[k] > 99) std::snprintf(n, sizeof(n), "99+");
                else std::snprintf(n, sizeof(n), "%zu", count[k]);
                ui.text_in(layer::scene, {rx, by + b - ui.px(11), b - ui.px(2), ui.px(11)}, n, ui.px(9), on ? style::text : style::text_dim, align::right);
            }
            rx -= gap;
        }
    }
    return bar;
}

// The agent bars: one continuous pane of glass under the top bar, flush at
// x = 0 and stacked without gaps; a thin white strip marks each shared edge
// (top bar / first bar, bar / bar). Returns the bottom of the last bar.
float agent_bars(ui_context& ui, sim_runner& sim, hud_state& st, float y, float above_w, std::size_t max_bars) {
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
        const float ai_w = ui.text_width("AI ✓", small, font_weight::semibold) + pad * 2, bs = ui.px(24);
        w += ai_w + ui.px(4) + bs + pad;
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
        // AI switch (on: the agent's AI decides, the player advises / forces; off:
        // full control) and removal.
        char id[48];
        std::snprintf(id, sizeof(id), "agent.ai.%u", panel.agent);
        const rect ai{bar.right() - pad - bs - ui.px(4) - ai_w, bar.y + (h - bs) * 0.5f, ai_w, bs};
        if(ui.button(id, layer::overlay, ai, panel.ai ? "AI ✓" : "AI ✗", small, panel.ai,
                     panel.ai ? "AI on: the agent decides; you advise or force levers. Click for full control"
                              : "AI off: you have full control (levers held where they are). Click to hand back to the AI")) {
            st.toggle_ai = panel.agent;
        }
        std::snprintf(id, sizeof(id), "agent.remove.%u", panel.agent);
        if(ui.button(id, layer::overlay, {bar.right() - pad - bs, bar.y + (h - bs) * 0.5f, bs, bs}, "✖", ui.px(12), false, "Stop controlling")) {
            st.remove_agent = panel.agent;
        }
        y = bar.bottom();
    }
    return y;
}

bool shown(alert const& a, hud_state const& st) {
    return a.cat == alert_category::system || (st.alert_filter >> static_cast<uint32_t>(a.cat) & 1u);
}

// The alert column: cards only (the count, filters and the bell live in the top
// bar), newest first, filtered by category; the wheel scrolls; a small
// "Dismiss all" at its end.
void alert_list(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, float y, float bottom) {
    if(st.alerts_hidden) return;
    std::vector<alert const*> items;
    for(auto const& a : alerts.items()) {
        if(shown(a, st)) items.push_back(&a);
    }
    if(items.empty()) return;
    const float pad = ui.px(style::pad);
    const float w = ui.px(style::alert_width);
    const float h = ui.px(style::alert_height);
    const float gap = ui.px(style::gap);
    const float font = ui.px(14);
    const float small = ui.px(style::font_small);
    const float icon = ui.px(20);
    const float x = ui.width() - w - gap; // the right side of the window
    const float foot = ui.px(20);         // "Dismiss all"
    const float h_impact = ui.px(style::alert_height + 16); // a card with a third (impact) line
    bottom -= foot + gap;
    const auto visible = static_cast<std::size_t>(std::max(1.0f, std::floor((bottom - y + gap) / (h + gap))));
    const float max_scroll = static_cast<float>(items.size() > visible ? items.size() - visible : 0);
    st.alert_scroll = std::clamp(st.alert_scroll, 0.0f, max_scroll);
    const auto first = static_cast<std::size_t>(st.alert_scroll);
    rect column{x, y, w, bottom - y};
    // Wheel scrolls the list; the new offset applies from the next frame.
    st.alert_scroll = std::clamp(st.alert_scroll - ui.take_wheel(column), 0.0f, max_scroll);

    uint64_t dismiss = 0;
    std::size_t drawn = 0;
    for(std::size_t i = first; i < items.size(); ++i) {
        alert const& a = *items[i];
        const float ch = a.impact.empty() ? h : h_impact;
        if(y + ch > bottom) break;
        ++drawn;
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
    // Footer: position in the list and "Dismiss all" (small, unobtrusive).
    char where[48];
    std::snprintf(where, sizeof(where), "%zu–%zu of %zu", first + 1, first + drawn, items.size());
    rect footer{x, y, w, foot};
    ui.panel(footer, panel_kind::translucent);
    ui.text_in(layer::overlay, {x + pad, y, w, foot}, where, small, style::text_dim);
    const float cw = ui.text_width("Dismiss all", small) + pad * 2;
    if(ui.button("alerts.clear", layer::overlay, {footer.right() - cw, y, cw, foot}, "Dismiss all", small, false,
                 "Dismiss every alert (all categories)")) {
        alerts.dismiss_all();
        st.alert_scroll = 0;
        return;
    }
    if(dismiss) alerts.dismiss(dismiss);
}

} // namespace

// The map mode's legend: a compact flat box in the bottom-right corner, under the
// alert column (which ends above it). Returns its top edge.
float legend_box(ui_context& ui, map_tab_info const& tab) {
    const float pad = ui.px(style::pad);
    const float gap = ui.px(style::gap);
    const float small = ui.px(style::font_small);
    const float line = ui.px(18);
    const float hh = ui.px(22);
    const float sw = ui.px(12);
    float w = ui.text_width(tab.title, ui.px(13), font_weight::semibold) + pad * 2;
    for(auto const& item : tab.legend) w = std::max(w, pad + sw + pad + ui.text_width(item.label, small) + pad);
    if(!tab.note.empty()) w = std::max(w, ui.text_width(tab.note, small) + pad * 2);
    w = std::min(w, ui.px(style::alert_width));
    const float h = hh + pad * 0.5f + static_cast<float>(tab.legend.size()) * line + (tab.note.empty() ? 0.0f : line) + pad * 0.5f;
    rect r{ui.width() - w - gap, ui.height() - h - gap, w, h};
    ui.flat_panel(r, hh);
    ui.text_in(layer::overlay, {r.x + pad, r.y, w, hh}, tab.title, ui.px(13), style::text, align::left, font_weight::semibold);
    ui.set_text_backing(false);
    float cy = r.y + hh + pad * 0.5f;
    for(auto const& item : tab.legend) {
        ui.fill(layer::overlay, {r.x + pad, std::round(cy + (line - sw) * 0.5f), sw, sw}, item.colour);
        ui.text_in(layer::overlay, {r.x + pad + sw + pad, cy, w, line}, item.label, small, style::flat_text);
        cy += line;
    }
    if(!tab.note.empty()) ui.text_in(layer::overlay, {r.x + pad, cy, w - pad * 2, line}, tab.note, small, style::flat_dim);
    ui.set_text_backing(true);
    return r.y;
}

// A window (Politics, Government policy, Diplomacy...): flat white with a blue
// title strip, filling the space below the top bar and the first agent bar,
// left of the alert column, down to the bottom margin. Returns the body rect.
rect window_frame(ui_context& ui, hud_state& st, rect win, std::string_view title, std::string_view subtitle) {
    const float pad = ui.px(style::pad);
    const float hh = ui.px(style::tab_header);
    ui.flat_panel(win, hh);
    const float tw = ui.text_in(layer::overlay, {win.x + pad, win.y, win.w, hh}, title, ui.px(14), style::text, align::left, font_weight::semibold);
    if(!subtitle.empty()) ui.text_in(layer::overlay, {win.x + pad + tw + pad, win.y, win.w, hh}, subtitle, ui.px(14), style::text_dim);
    const float bs = ui.px(22);
    if(ui.button("window.close", layer::overlay, {win.right() - bs - ui.px(3), win.y + (hh - bs) * 0.5f, bs, bs}, "✖", ui.px(12), false, "Close")) {
        st.open = hud_action::none;
    }
    return {win.x, win.y + hh, win.w, win.h - hh};
}

// The Politics tab: the electoral tile map of one nation, its mode buttons and
// the national result (or regime indicators), flat white like the view tabs.
void politics_tab(ui_context& ui, electoral_view_info const& info, hud_state& st, rect win) {
    const float pad = ui.px(style::pad);
    const float hh = ui.px(style::tab_header);
    const float small = ui.px(style::font_small);
    const float line = ui.px(18);
    const float w = win.w;
    const float x = win.x, y = win.y;
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
    // The map takes the rest of the window.
    const float map_h = std::max(ui.px(80), win.h - (hh + pad + rows_h + pad + pad + (info.legend.empty() ? 0.0f : line) +
                                                     static_cast<float>(strip.size()) * line + pad));
    rect r = win;
    window_frame(ui, st, win, "Politics", info.nation);
    const float bs = ui.px(22);
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

// The Control window ('+'): every controllable agent, one list per kind, with a
// search field (typing filters by name); click to control.
void control_window(ui_context& ui, sim_runner& sim, std::vector<agent_info> const* agents, hud_state& st, rect win) {
    static constexpr char const* kind_name[] = {"Governments", "Central banks", "Boards", "Unions", "Militaries", "Governors", "Rebels"};
    static constexpr int kind_order[] = {0, 1, 3, 4, 2, 5, 6}; // sim::agent_kind values, in display order
    const float pad = ui.px(style::pad);
    const float small = ui.px(style::font_small);
    const float line = ui.px(24);
    const rect body = window_frame(ui, st, win, "Control", "pick the agents you play");
    ui.set_text_backing(false);
    float y = body.y + pad;
    // Kind buttons with counts.
    float x = body.x + pad;
    const float bh = ui.px(24);
    for(int k : kind_order) {
        std::size_t n = 0;
        if(agents) {
            for(auto const& a : *agents) n += a.kind == k;
        }
        const std::string label = std::string(kind_name[k]) + " (" + std::to_string(n) + ")";
        const float bw = ui.text_width(label, small) + pad * 2;
        if(x + bw > body.right() - pad) {
            x = body.x + pad;
            y += bh + ui.px(4);
        }
        char id[32];
        std::snprintf(id, sizeof(id), "control.kind.%d", k);
        if(ui.flat_button(id, {x, y, bw, bh}, label, small, st.control_kind == k)) {
            st.control_kind = k;
            st.control_scroll = 0;
        }
        x += bw + ui.px(4);
    }
    y += bh + pad;
    // Search field (keyboard input goes here while the window is open).
    const rect field{body.x + pad, y, std::min(ui.px(360), body.w - pad * 2), bh};
    ui.fill(layer::overlay, field, hex(0xEEF2F7));
    ui.fill(layer::overlay, {field.x, field.bottom() - std::max(1.0f, std::round(ui.px(1))), field.w, std::max(1.0f, std::round(ui.px(1)))}, style::flat_header);
    const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
    ui.text_in(layer::overlay, {field.x + pad, field.y, field.w, field.h}, st.search.empty() ? (blink ? "Search…|" : "Search…") : st.search + (blink ? "|" : ""),
               small, st.search.empty() ? style::flat_dim : style::flat_text);
    y += bh + pad;
    // The list.
    std::string needle = st.search;
    for(char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<int> rows;
    if(agents) {
        for(std::size_t i = 0; i < agents->size(); ++i) {
            auto const& a = (*agents)[i];
            if(a.kind != st.control_kind) continue;
            if(!needle.empty()) {
                std::string hay = a.label + " " + a.iso3;
                for(char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if(hay.find(needle) == std::string::npos) continue;
            }
            rows.push_back(static_cast<int>(i));
        }
    }
    const rect list{body.x + pad, y, body.w - pad * 2, body.bottom() - y - pad};
    const auto visible = static_cast<std::size_t>(std::max(1.0f, std::floor(list.h / line)));
    const float max_scroll = static_cast<float>(rows.size() > visible ? rows.size() - visible : 0);
    st.control_scroll = std::clamp(st.control_scroll - ui.take_wheel(list) * 3.0f, 0.0f, max_scroll);
    const auto first = static_cast<std::size_t>(st.control_scroll);
    if(!agents) ui.text_in(layer::overlay, list, "Loading…", small, style::flat_dim, align::centre);
    else if(rows.empty()) ui.text_in(layer::overlay, list, "No match", small, style::flat_dim, align::centre);
    for(std::size_t r = first; r < rows.size() && r < first + visible; ++r) {
        auto const& a = (*agents)[static_cast<std::size_t>(rows[r])];
        const rect row{list.x, list.y + static_cast<float>(r - first) * line, list.w, line - ui.px(2)};
        const bool mine = sim.controls_agent(a.agent);
        char id[40];
        std::snprintf(id, sizeof(id), "control.row.%u", a.agent);
        if(ui.flat_button(id, row, "", small, mine) && !mine) st.add_agent = rows[r];
        ui.text_in(layer::overlay, {row.x + pad, row.y, row.w, row.h}, a.label, small, mine ? style::text : style::flat_text);
        ui.text_in(layer::overlay, {row.x, row.y, row.w - pad, row.h}, mine ? "controlled ✓" : a.iso3, small, mine ? style::text : style::flat_dim, align::right);
    }
    if(rows.size() > visible) {
        char where[48];
        std::snprintf(where, sizeof(where), "%zu–%zu of %zu (wheel scrolls)", first + 1, std::min(rows.size(), first + visible), rows.size());
        ui.text_in(layer::overlay, {list.x, body.bottom() - pad - ui.px(14), list.w, ui.px(14)}, where, small, style::flat_dim, align::right);
    }
    ui.set_text_backing(true);
}

void build_hud(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, map_tab_info const* tab,
               electoral_view_info const* politics, std::vector<agent_info> const* agents) {
    st.requested_speed = -1;
    st.politics_step = 0;
    const rect bar = top_bar(ui, sim, st, alerts);
    const float gap = ui.px(style::gap);
    const bool window_open = st.open != hud_action::none;
    st.toggle_ai = st.remove_agent = -1;
    st.add_agent = -1;
    float bars_bottom = agent_bars(ui, sim, st, bar.bottom(), bar.w, window_open ? 1 : static_cast<std::size_t>(-1));
    // '+': control another agent (top left, under the top bar and the agent bars).
    if(sim.status() == sim_status::running) {
        const float ps = ui.px(28);
        const rect plus{0, bars_bottom, ps, ps};
        ui.panel(plus, panel_kind::glass_flush);
        ui.fill(layer::overlay, {0, bars_bottom, ps, std::max(1.0f, std::round(ui.px(style::separator_strip)))}, style::glass_separator);
        if(ui.button("agents.add", layer::overlay, plus, "+", ui.px(18), st.open == hud_action::control,
                     "Control an agent: governments, central banks, unions, militaries, boards, governors, rebels")) {
            st.open = st.open == hud_action::control ? hud_action::none : hud_action::control;
            st.search.clear();
            st.control_scroll = 0;
        }
        bars_bottom += ps; // windows start below it
    }
    // Bottom-right: the map mode's legend; the alert column ends above it.
    float alerts_bottom = ui.height() - gap;
    if(tab && st.filter != map_filter::political) alerts_bottom = legend_box(ui, *tab) - gap;
    alert_list(ui, sim, alerts, st, bar.bottom() + gap, alerts_bottom); // right side, below the top bar; floating cards keep their margins
    // A window fills the space left of the alert column, below the first agent bar.
    st.politics_tile = -1;
    if(window_open) {
        const float right = ui.width() - ui.px(style::alert_width) - gap * 2;
        const rect win{gap, bars_bottom + gap, std::max(ui.px(200), right - gap), std::max(ui.px(120), ui.height() - bars_bottom - gap * 2)};
        if(st.open == hud_action::politics && politics) {
            politics_tab(ui, *politics, st, win);
        } else if(st.open == hud_action::control) {
            control_window(ui, sim, agents, st, win);
        } else {
            const auto i = std::min(static_cast<std::size_t>(st.open) - 1, std::size(action_buttons) - 1);
            const rect body = window_frame(ui, st, win, action_buttons[i].tip, "");
            ui.set_text_backing(false);
            ui.text_in(layer::overlay, {body.x, body.y, body.w, body.h}, "Not available yet", ui.px(14), style::flat_dim, align::centre);
            ui.set_text_backing(true);
        }
    }
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
    }
    // Compact: a thin bar under the title strip fills over the 2 s until it expands.
    if(!expanded && !info.details.empty() && st.tooltip_progress > 0.05f) {
        const float bar = std::max(1.0f, std::round(ui.px(2)));
        ui.fill(layer::overlay, {r.x, r.y + hh, std::round(r.w * std::min(1.0f, st.tooltip_progress)), bar}, hex(0x8FB4E3));
    }
    ui.set_text_backing(true);
}

// --- main menu ----------------------------------------------------------------

namespace {

// A flat blue menu button with white text (lighter when hovered).
bool menu_button(ui_context& ui, rect r, std::string_view label) {
    const bool hot = ui.hovered(r);
    ui.fill(layer::overlay, r, hot ? hex(0x3F80C8) : style::flat_header);
    ui.text_in(layer::overlay, {r.x + ui.px(16), r.y, r.w, r.h}, label, ui.px(18), style::text, align::left, font_weight::semibold);
    return hot && ui.input().released;
}

} // namespace

menu_choice build_menu(ui_context& ui, menu_state& st, client_settings& settings, std::vector<save_entry> const& saves) {
    menu_choice out;
    const float x = ui.px(56);
    const float title_size = ui.px(84);
    const float top = std::max(ui.px(40), ui.height() * 0.16f);
    ui.text(layer::overlay, x, top + title_size * 0.8f, "geosim", title_size, style::text, font_weight{900});
    const float bw = ui.px(280), bh = ui.px(48), gap = ui.px(10);
    float y = top + title_size + ui.px(36);
    static constexpr char const* labels[] = {"New session", "Load session", "Multiplayer", "Settings", "Quit"};
    for(int k = 0; k < 5; ++k) {
        const rect r{x, y, bw, bh};
        if(menu_button(ui, r, labels[k])) {
            switch(k) {
            case 0: out.what = menu_choice::kind::new_session; break;
            case 1: st.open = st.open == menu_state::window::load ? menu_state::window::none : menu_state::window::load; break;
            case 2: st.open = st.open == menu_state::window::multiplayer ? menu_state::window::none : menu_state::window::multiplayer; break;
            case 3: st.open = st.open == menu_state::window::settings ? menu_state::window::none : menu_state::window::settings; break;
            default: out.what = menu_choice::kind::quit; break;
            }
        }
        y += bh + gap;
    }
    if(st.open == menu_state::window::none) return out;
    // The window beside the column.
    const float pad = ui.px(style::pad);
    const float hh = ui.px(style::tab_header);
    const float small = ui.px(style::font_small);
    const float font = ui.px(14);
    const float wx = x + bw + ui.px(32);
    const rect win{wx, top + title_size + ui.px(36), std::min(ui.px(560), ui.width() - wx - ui.px(32)), 5 * bh + 4 * gap};
    char const* title = st.open == menu_state::window::load ? "Load session" : st.open == menu_state::window::settings ? "Settings" : "Multiplayer";
    ui.flat_panel(win, hh);
    ui.text_in(layer::overlay, {win.x + pad, win.y, win.w, hh}, title, font, style::text, align::left, font_weight::semibold);
    const float bs = ui.px(22);
    if(ui.button("menu.close", layer::overlay, {win.right() - bs - ui.px(3), win.y + (hh - bs) * 0.5f, bs, bs}, "✖", ui.px(12))) st.open = menu_state::window::none;
    ui.set_text_backing(false);
    const rect body{win.x + pad, win.y + hh + pad, win.w - pad * 2, win.h - hh - pad * 2};
    if(st.open == menu_state::window::multiplayer) {
        ui.text_in(layer::overlay, body, "Not available yet", font, style::flat_dim, align::centre);
    } else if(st.open == menu_state::window::load) {
        if(saves.empty()) {
            ui.text_in(layer::overlay, body, "No saved sessions yet (Ctrl+S saves during a session)", small, style::flat_dim, align::centre);
        }
        const float line = ui.px(30);
        const auto visible = static_cast<std::size_t>(std::max(1.0f, std::floor(body.h / line)));
        const float max_scroll = static_cast<float>(saves.size() > visible ? saves.size() - visible : 0);
        st.scroll = std::clamp(st.scroll - ui.take_wheel(body), 0.0f, max_scroll);
        for(std::size_t i = static_cast<std::size_t>(st.scroll), r = 0; i < saves.size() && r < visible; ++i, ++r) {
            const rect row{body.x, body.y + static_cast<float>(r) * line, body.w, line - ui.px(3)};
            char id[32];
            std::snprintf(id, sizeof(id), "menu.save.%zu", i);
            if(ui.flat_button(id, row, "", small)) {
                out.what = menu_choice::kind::load;
                out.path = saves[i].path;
            }
            ui.text_in(layer::overlay, {row.x + pad, row.y, row.w, row.h}, saves[i].name, font, style::flat_text);
        }
    } else { // settings
        const float line = ui.px(34);
        float cy = body.y;
        auto row_label = [&](char const* label) { ui.text_in(layer::overlay, {body.x, cy, body.w, line}, label, font, style::flat_text); };
        const float vx = body.right() - ui.px(200);
        row_label("Music volume");
        char v[16];
        std::snprintf(v, sizeof(v), "%.0f%%", static_cast<double>(settings.music_volume) * 100.0);
        if(ui.flat_button("menu.vol.down", {vx, cy + ui.px(5), ui.px(32), line - ui.px(10)}, "−", font)) {
            settings.music_volume = std::max(0.0f, settings.music_volume - 0.1f);
            out.what = menu_choice::kind::settings_changed;
        }
        ui.text_in(layer::overlay, {vx + ui.px(36), cy, ui.px(60), line}, v, font, style::flat_text, align::centre);
        if(ui.flat_button("menu.vol.up", {vx + ui.px(100), cy + ui.px(5), ui.px(32), line - ui.px(10)}, "+", font)) {
            settings.music_volume = std::min(1.0f, settings.music_volume + 0.1f);
            out.what = menu_choice::kind::settings_changed;
        }
        cy += line;
        auto toggle = [&](char const* label, char const* id, bool& value) {
            row_label(label);
            if(ui.flat_button(id, {vx, cy + ui.px(5), ui.px(132), line - ui.px(10)}, value ? "On" : "Off", font, value)) {
                value = !value;
                out.what = menu_choice::kind::settings_changed;
            }
            cy += line;
        };
        toggle("Music", "menu.music", settings.music);
        toggle("Clouds (climate view)", "menu.clouds", settings.clouds);
        toggle("Fullscreen", "menu.fullscreen", settings.fullscreen);
        ui.text_in(layer::overlay, {body.x, body.bottom() - ui.px(16), body.w, ui.px(16)}, settings_path().string(), small, style::flat_dim);
    }
    ui.set_text_backing(true);
    return out;
}

void draw_notice(ui_context& ui, std::string_view title, std::string_view text) {
    const float pad = ui.px(style::pad);
    const float hh = ui.px(style::tab_header);
    const float w = std::max(ui.px(320), ui.text_width(text, ui.px(14)) + pad * 4);
    const rect r{std::round((ui.width() - w) * 0.5f), std::round(ui.height() * 0.45f), std::round(w), std::round(hh + ui.px(44))};
    ui.flat_panel(r, hh);
    ui.text_in(layer::overlay, {r.x + pad, r.y, r.w, hh}, title, ui.px(14), style::text, align::left, font_weight::semibold);
    ui.set_text_backing(false);
    ui.text_in(layer::overlay, {r.x + pad, r.y + hh, r.w - pad * 2, r.h - hh}, text, ui.px(14), style::flat_text);
    ui.set_text_backing(true);
}

} // namespace geosim::client
