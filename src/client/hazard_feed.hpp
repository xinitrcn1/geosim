#pragma once
// Natural hazards from the simulation to the map and the alert feed
// (docs/07-map-and-filters.md#hazards, docs/08-ui.md#layout).
//
// Every 250 ms it requests a hazard_snapshot (sim_runner::request_hazards):
//  - storms and the earthquakes of the last week go to the map renderer
//    (cyclone swirls, red rings; a ring's animation starts when first seen);
//  - disaster records become alerts: kind icon, title, "place · date", and an
//    impact line with deaths, people affected and damage in USD and % of GDP
//    ("Could cause ..." while a cyclone is only forecast). Records are updated
//    in place as their toll grows. Disasters in a nation the player controls
//    are always shown (red); elsewhere only notable ones (>= 50 deaths,
//    >= 1 million affected or >= USD 1 bn damage).

#include "map_hazards.hpp"
#include "sim_runner.hpp"
#include "ui_layout.hpp"

#include <chrono>
#include <future>
#include <unordered_map>
#include <vector>

namespace geosim::client {

class hazard_feed {
public:
    void poll(sim_runner& sim, alert_feed& alerts, float seconds);
    [[nodiscard]] std::vector<map_storm> const& storms() const noexcept { return storms_; }
    [[nodiscard]] std::vector<map_quake> const& quakes() const noexcept { return quakes_; }

    // Text of a disaster alert (exposed for tests).
    static alert make_alert(disaster_note const& n, bool mine);

private:
    std::future<hazard_snapshot> pending_;
    std::chrono::steady_clock::time_point last_request_{};
    std::vector<map_storm> storms_;
    std::vector<map_quake> quakes_;
    std::unordered_map<uint64_t, float> quake_seen_; // key -> first-seen real time
};

} // namespace geosim::client
