#include "hazard_feed.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geosim::client {

namespace {

// Disaster kinds (sim::disaster_kind order): cyclone, flood, drought, earthquake, tsunami.
constexpr char const* kind_icon[] = {"🌀", "🌧", "🏜", "🏚", "🌊"};

std::string people(float v) {
    char buf[32];
    if(v >= 1e6f) std::snprintf(buf, sizeof(buf), "%.1f M", static_cast<double>(v) / 1e6);
    else if(v >= 1e5f) std::snprintf(buf, sizeof(buf), "%.0f k", static_cast<double>(v) / 1e3);
    else if(v >= 1e3f) std::snprintf(buf, sizeof(buf), "%.1f k", static_cast<double>(v) / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    return buf;
}

std::string usd(double v) {
    char buf[32];
    if(v >= 1e9) std::snprintf(buf, sizeof(buf), "$%.1f bn", v / 1e9);
    else if(v >= 1e6) std::snprintf(buf, sizeof(buf), "$%.0f M", v / 1e6);
    else std::snprintf(buf, sizeof(buf), "$%.0f k", v / 1e3);
    return buf;
}

bool notable(disaster_note const& n) { return n.deaths >= 50.0f || n.affected >= 1e6f || n.damage_usd >= 1e9; }

} // namespace

alert hazard_feed::make_alert(disaster_note const& n, bool mine) {
    alert a;
    a.key = n.key;
    a.tick = n.start;
    a.icon = n.kind < std::size(kind_icon) ? kind_icon[n.kind] : "⚠";
    a.title = n.title;
    a.detail = n.place.empty() ? n.date : n.place + " · " + n.date;
    std::string impact;
    auto add = [&](std::string s) { impact += impact.empty() ? s : " · " + s; };
    if(n.deaths >= 0.5f) add(people(n.deaths) + " dead");
    if(n.affected >= 0.5f) add(people(n.affected) + " affected");
    if(n.damage_usd >= 1e5) {
        char pct[32];
        std::snprintf(pct, sizeof(pct), " (%.2g%% of GDP)", static_cast<double>(n.gdp_loss_pct));
        add(usd(n.damage_usd) + (n.gdp_loss_pct > 0.0f ? pct : ""));
    }
    if(impact.empty()) impact = n.potential ? "Impact being assessed" : "No casualties reported";
    a.impact = n.potential ? "Could cause " + impact : impact;
    a.t = mine ? tone::bad : tone::neutral;
    return a;
}

void hazard_feed::poll(sim_runner& sim, alert_feed& alerts, float seconds) {
    if(sim.status() != sim_status::running) return;
    // Keep the ring animation running between snapshots.
    for(auto& q : quakes_) {
        auto it = quake_seen_.find(q.key);
        if(it != quake_seen_.end()) q.shown_seconds = seconds - it->second;
    }
    if(pending_.valid()) {
        if(pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        hazard_snapshot snap = pending_.get();
        storms_ = std::move(snap.storms);
        quakes_ = std::move(snap.quakes);
        for(auto& q : quakes_) {
            auto it = quake_seen_.try_emplace(q.key, seconds).first;
            q.shown_seconds = seconds - it->second;
        }
        if(quake_seen_.size() > 512) quake_seen_.clear();
        // Oldest first so the newest ends up on top.
        std::sort(snap.disasters.begin(), snap.disasters.end(), [](auto const& a, auto const& b) { return a.start < b.start; });
        std::unordered_map<uint32_t, record> live;
        for(auto& n : snap.disasters) {
            auto it = records_.find(n.id);
            if(it != records_.end() && it->second.kind == n.kind && it->second.title == n.title) {
                n.key = it->second.key;
                if(it->second.potential && !n.potential) { // landfall: replace the forecast
                    alerts.dismiss(n.key);
                    n.key = n.key ^ (0x9E3779B97F4A7C15ull * ++serial_);
                }
            } else {
                n.key = n.key ^ (0x9E3779B97F4A7C15ull * ++serial_); // a new record (or a reused id)
            }
            live[n.id] = {n.kind, n.title, n.key, n.potential};
            const bool mine = sim.controls(n.nation);
            if(!mine && !notable(n)) continue;
            alerts.upsert(make_alert(n, mine));
        }
        records_ = std::move(live);
        notes_ = std::move(snap.disasters);
        tick_ = snap.tick;
    }
    const auto now = std::chrono::steady_clock::now();
    if(!pending_.valid() && now - last_request_ > std::chrono::milliseconds(250)) {
        pending_ = sim.request_hazards();
        last_request_ = now;
    }
}

} // namespace geosim::client
