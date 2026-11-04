#pragma once
// Natural hazards as the client shows them (docs/07-map-and-filters.md#hazards):
// live tropical cyclones and recent earthquakes for the map, and disaster
// records for the alert feed. Filled from the simulation by
// sim_runner::request_hazards (sim_hazards.cpp); no Vulkan or sim types here.

#include <cstdint>
#include <string>
#include <vector>

namespace geosim::client {

struct map_storm {
    float lon = 0, lat = 0;   // degrees
    float wind = 0;           // maximum sustained wind, m/s
    float radius_km = 200;    // radius of gale-force winds
};

struct map_quake {
    uint64_t key = 0;         // stable per event (ring animation start)
    float lon = 0, lat = 0;   // epicentre, degrees
    float magnitude = 6;      // Mw
    float age_days = 0;       // sim days since the event (rings fade over ~7)
    float shown_seconds = 0;  // real seconds since it appeared (set by hazard_feed)
};

inline constexpr std::size_t map_max_storms = 64, map_max_quakes = 32;

// One disaster record (sim `disaster` object, text from sim::describe_disaster).
struct disaster_note {
    uint32_t id = 0;          // sim record index (erasable: reused after the record ends)
    uint64_t key = 0;         // stable per record (assigned by hazard_feed)
    uint8_t kind = 0;         // sim::disaster_kind
    uint8_t status = 0;       // sim::disaster_status (0 = warning: potential impact)
    int32_t start = 0;        // tick
    std::string nation;       // ISO3 of the hit nation
    std::string title, place, date;
    float deaths = 0, affected = 0;
    double damage_usd = 0;
    float gdp_loss_pct = 0;
    bool potential = false;   // a forecast (approaching cyclone): "could cause ..."
};

struct hazard_snapshot {
    int32_t tick = 0;
    std::vector<map_storm> storms;
    std::vector<map_quake> quakes;      // earthquakes of the last 7 days
    std::vector<disaster_note> disasters; // every live record
};

} // namespace geosim::client
