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
    float heading = 0;        // degrees (0 = poleward, as in the sim)
    float speed_kmd = 0;      // km per day
    int32_t serial = -1;      // matches region.storm_seen
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
    float lat = 0, lon = 0;   // epicentre / record position
    float magnitude = 0;      // Mw for earthquakes
    uint32_t region = 0xFFFFFFFFu; // sim region index (worst hit)
};

// Per sim region, for the map views (sim_runner::request_regions).
struct region_row {
    float crop_loss = 0, spei = 0, flood_cooldown = 0, soil_frac = 0, floodplain = 0, precip_anom = 0;
    bool drought_record = false, wet_season = false;
    int32_t storm_seen = -1;
    float storm_wind = 0;
    float insurgency = 0, unrest = 0, grievance = 0, protest = 0;
    float infections_100k = 0; // new infections per 100k per day
    float population = 0;
};

// Per sim nation (national values; regions inherit them).
struct nation_row {
    float growth = 0, unemployment = 0, inflation = 0, policy_rate = 0, yield10 = 0, budget = 0, gap = 0;
    float credit_growth = 0, wage_real = 0, house_price = 0, equity_price = 0, gini = 0;
    float exports_pct = 0, imports_pct = 0, tariff = 0, seaborne = 0, freight = 0, logistics_delay = 0;
    float oil_production = 0, oil_demand = 0, fossil_share = 0, carbon_price = 0;
    int sanction_links = 0;
    bool insurgent = false, at_war = false;
    uint16_t partner[5] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // top export destinations
    float partner_share[5] = {};
};

struct fault_row {
    float lat = 0, lon = 0, mw = 0, ratio = 0; // ratio = elapsed / mean recurrence
};

struct region_snapshot {
    int32_t tick = 0;
    int month = 1; // 1..12
    std::vector<region_row> regions;
    std::vector<nation_row> nations;
    std::vector<fault_row> faults;
};

struct hazard_snapshot {
    int32_t tick = 0;
    std::vector<map_storm> storms;
    std::vector<map_quake> quakes;      // earthquakes of the last 7 days
    std::vector<disaster_note> disasters; // every live record
};

} // namespace geosim::client
