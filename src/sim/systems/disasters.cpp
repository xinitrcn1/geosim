// Natural hazards as a macro climate and tectonic state (docs/03-economy.md#natural-hazards).
// Disasters are not dice rolls on a fixed hazard: they emerge from state variables that
// evolve under physical rules, with weather noise as the only randomness.
//
// ENSO: Jin's (1997) recharge oscillator for the Nino-3.4 SST anomaly T and equatorial
//   heat content h, dT/dt = R T + gamma h + noise, dh/dt = -r h - alpha T, forced by
//   westerly wind bursts (Burgers, Jin & van Oldenborgh 2005). It shifts regional
//   precipitation through teleconnections (Ropelewski & Halpert 1987) and cyclone
//   genesis by basin (Gray 1984; Camargo & Sobel 2005).
// Water balance (monthly, per region): precipitation = climatology (WorldClim: annual
//   total, wettest month, seasonality) x a lognormal anomaly that is persistent (AR(1)),
//   partly national (spatially coherent) and shifted by ENSO; evapotranspiration rises with
//   temperature, so warming dries soils. A bucket soil (Manabe 1969) stores water.
//   SPEI-6, the standardised 6-month water balance (Vicente-Serrano et al. 2010), sets
//   drought: below `drought_spei` crops fail in proportion (Lesk, Rowhani & Ramankutty
//   2016) and famine mortality rises steeply with poverty and conflict (Maxwell & Fitzpatrick
//   2012, Somalia 2011). Floods follow extreme monthly rain on saturated soil over flood
//   plains: exposure x vulnerability (Jongman, Ward & Aerts 2012; Jongman et al. 2015).
// Tropical cyclones: genesis in each basin's season at a rate set by climatology and ENSO;
//   tracks steered west by the trades, recurving past the subtropical ridge (Holland
//   1983); intensity relaxes toward the SST-set potential intensity (Emanuel 1987) and
//   decays over land (Kaplan & DeMaria 1995). Damage follows Emanuel's (2011) cubic wind
//   damage function with vulnerability falling with income (Eberenz, Luthi & Bresch 2021);
//   deaths come from surge in low-lying coasts (Bangladesh 1970, Nargis 2008) and wind,
//   both falling with income (Kahn 2005; Toya & Skidmore 2007). Approaching storms
//   raise warnings with their potential impact.
// Earthquakes: each major fault accumulates strain since its last rupture (elastic rebound;
//   Reid 1910); the rupture hazard is the Brownian Passage Time renewal rate (Matthews,
//   Ellsworth & Reasenberg 2002), so long-quiet faults (seismic gaps) are the likeliest.
//   Magnitude grows with the slip deficit. Shaking attenuates with distance; deaths and
//   losses follow the USGS PAGER empirical models (Jaiswal & Wald 2010, 2013).
// Tsunamis: megathrust ruptures above `tsunami_min_mw` raise waves whose height falls with
//   distance (Abe 1981); low-lying coasts (LECZ; McGranahan, Balk & Anderson 2007) are
//   inundated, with deaths by depth (Koshimura, Oie, Yanagisawa & Imamura 2009) cut by warning
//   systems, which come with income and distance.
// Damage destroys regional capacity (va0); deaths leave the population (society.cpp).

#include "core/calendar.hpp"
#include "core/keys.hpp"
#include "core/rng.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cstdio>
#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace geosim::sim {

namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr float rad = pi / 180.0f;

// Storm names by basin (WMO rotating lists of 2020), reused cyclically by season.
constexpr std::array<char const*, 21> names_natl{"Arthur", "Bertha", "Cristobal", "Dolly", "Edouard", "Fay", "Gonzalo", "Hanna", "Isaias", "Josephine", "Kyle",
                                                  "Laura", "Marco", "Nana", "Omar", "Paulette", "Rene", "Sally", "Teddy", "Vicky", "Wilfred"};
constexpr std::array<char const*, 24> names_epac{"Amanda", "Boris", "Cristina", "Douglas", "Elida", "Fausto", "Genevieve", "Hernan", "Iselle", "Julio", "Karina", "Lowell",
                                                  "Marie", "Norbert", "Odalys", "Polo", "Rachel", "Simon", "Trudy", "Vance", "Winnie", "Xavier", "Yolanda", "Zeke"};
constexpr std::array<char const*, 25> names_wpac{"Vongfong", "Nuri", "Sinlaku", "Hagupit", "Jangmi", "Mekkhala", "Higos", "Bavi", "Maysak", "Haishen", "Noul", "Dolphin", "Kujira",
                                                  "Chan-hom", "Linfa", "Nangka", "Saudel", "Molave", "Goni", "Atsani", "Etau", "Vamco", "Krovanh", "Dujuan", "Surigae"};
constexpr std::array<char const*, 14> names_nio{"Amphan", "Nisarga", "Gati", "Nivar", "Burevi", "Tauktae", "Yaas", "Gulab", "Shaheen", "Jawad", "Asani", "Sitrang", "Mandous", "Mocha"};
constexpr std::array<char const*, 13> names_sio{"Ambali", "Belna", "Calvinia", "Diane", "Esther", "Francisco", "Gabekile", "Herold", "Irondro", "Jeruto", "Kanga", "Ludzi", "Mitsoa"};
constexpr std::array<char const*, 13> names_aus{"Blake", "Claudia", "Damien", "Esther", "Ferdinand", "Gretel", "Harold", "Imogen", "Joshua", "Kimi", "Lucas", "Marian", "Niran"};
constexpr std::array<char const*, 14> names_spac{"Rita", "Sarai", "Tino", "Uesi", "Vicky", "Wasi", "Yasa", "Zazu", "Ana", "Bina", "Cody", "Dovi", "Eva", "Fili"};

char const* storm_name(uint32_t basin_key, uint16_t i) {
    auto pick = [i](auto const& a) { return a[i % a.size()]; };
    if(basin_key == core::pack_key("natl")) return pick(names_natl);
    if(basin_key == core::pack_key("epac")) return pick(names_epac);
    if(basin_key == core::pack_key("wpac")) return pick(names_wpac);
    if(basin_key == core::pack_key("nio")) return pick(names_nio);
    if(basin_key == core::pack_key("sio")) return pick(names_sio);
    if(basin_key == core::pack_key("aus")) return pick(names_aus);
    return pick(names_spac);
}

float norm_cdf(float x) { return 0.5f * std::erfc(-x / std::sqrt(2.0f)); }

float gauss(game_state const& state, uint32_t a, uint32_t b, uint32_t c) {
    const auto s = static_cast<uint32_t>(core::rng_stream::climate);
    const auto t = static_cast<uint32_t>(state.current_tick);
    const float u1 = std::max(1e-7f, core::uniform01(state.rng_seed, s, t, a, b * 4u + c * 64u));
    const float u2 = core::uniform01(state.rng_seed, s, t, a, b * 4u + c * 64u + 1u);
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * pi * u2);
}
float uniform(game_state const& state, uint32_t a, uint32_t b) {
    return core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::climate), static_cast<uint32_t>(state.current_tick), a, b);
}

float distance_km(float la1, float lo1, float la2, float lo2) {
    const float p1 = la1 * rad, p2 = la2 * rad;
    const float dl = (lo2 - lo1) * rad;
    const float h = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0f * 6371.0f * std::asin(std::min(1.0f, std::sqrt(h)));
}

// Regions taking part in the hazard model (the rest of the world in the slices does not).
bool hazard_region(game_state const& state, dcon::region_id r) { return state.world.region_get_precip_mm(r) > 0.0f; }

float region_radius_km(game_state const& state, dcon::region_id r) {
    return std::sqrt(std::max(1.0f, state.world.region_get_area_km2(r)) / pi);
}

float warming_now(game_state const& state) {
    for(auto c : state.world.in_climate) return c.get_temperature() - static_cast<float>(state.params.temperature0);
    return 0.0f;
}

// Income (GDP per head) relative to 10,000 USD, floored.
float income_rel(game_state const& state, dcon::nation_id n, float ref) {
    return std::max(0.05f, state.world.nation_get_gdp_pc0(n) / ref);
}
// 0 (1,000 USD) .. 1 (40,000 USD) on a log scale.
float income_norm(game_state const& state, dcon::nation_id n) {
    return std::clamp(std::log(std::max(1.0f, state.world.nation_get_gdp_pc0(n)) / 1000.0f) / std::log(40.0f), 0.0f, 1.0f);
}

// A region's GDP (USD a year) and capital stock.
double region_gdp(game_state const& state, dcon::region_id r) {
    auto const& w = state.world;
    const auto n = w.region_get_owner(r);
    if(!n) return 0.0;
    double rs = 0.0, ns = 0.0;
    const auto S = state.derived.sector_count;
    for(uint8_t s = 0; s < S; ++s) rs += w.region_get_va0(r, s);
    for(auto own : w.nation_get_region_ownership(n)) {
        for(uint8_t s = 0; s < S; ++s) ns += own.get_region().get_va0(s);
    }
    const double gdp = w.nation_get_gdp0(n) * w.nation_get_core_price(n) * w.nation_get_gf(n);
    return ns > 0.0 ? gdp * rs / ns : 0.0;
}
double nation_gdp(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    return w.nation_get_gdp0(n) * w.nation_get_core_price(n) * w.nation_get_gf(n);
}

// Destroys a share of the region's capacity (every sector).
void destroy_capacity(game_state& state, dcon::region_id r, float share) {
    auto& w = state.world;
    share = std::clamp(share, 0.0f, 0.9f);
    if(share <= 0.0f) return;
    for(uint8_t s = 0; s < state.derived.sector_count; ++s) w.region_set_va0(r, s, w.region_get_va0(r, s) * (1.0f - share));
    const auto n = w.region_get_owner(r);
    if(n) w.nation_set_disaster_losses(n, w.nation_get_disaster_losses(n) + share * static_cast<float>(region_gdp(state, r) / std::max(1.0, nation_gdp(state, n))));
}

void kill(game_state& state, dcon::region_id r, float people) {
    auto& w = state.world;
    people = std::min(people, 0.5f * w.region_get_population(r));
    if(people > 0.0f) w.region_set_war_deaths(r, w.region_get_war_deaths(r) + people); // removed as violent deaths (society.cpp)
}

void journal_disaster_entry(game_state& state, dcon::disaster_id d) {
    auto& w = state.world;
    const auto n = w.disaster_get_nation(d);
    const auto j = w.create_decision();
    if(n) w.decision_set_agent(j, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(j, n);
    w.decision_set_tick(j, state.current_tick);
    w.decision_set_lever(j, journal_disaster);
    w.decision_set_before(j, static_cast<float>(w.disaster_get_kind(d)));
    w.decision_set_after(j, w.disaster_get_deaths(d));
    w.decision_set_pragmatic(j, static_cast<float>(d.index()));
    w.decision_set_ideological(j, NAN);
    w.decision_set_prior(j, NAN);
}

dcon::disaster_id new_disaster(game_state& state, disaster_kind k, disaster_status st, dcon::nation_id n, dcon::region_id r, float lat, float lon,
                               float magnitude, int32_t source) {
    auto& w = state.world;
    const auto d = w.create_disaster();
    w.disaster_set_kind(d, static_cast<uint8_t>(k));
    w.disaster_set_status(d, static_cast<uint8_t>(st));
    w.disaster_set_nation(d, n);
    w.disaster_set_region(d, r);
    w.disaster_set_start(d, state.current_tick);
    w.disaster_set_finish(d, -1);
    w.disaster_set_lat(d, lat);
    w.disaster_set_lon(d, lon);
    w.disaster_set_magnitude(d, magnitude);
    w.disaster_set_deaths(d, 0.0f);
    w.disaster_set_affected(d, 0.0f);
    w.disaster_set_damage_usd(d, 0.0);
    w.disaster_set_gdp_loss_pct(d, 0.0f);
    w.disaster_set_source(d, source);
    w.disaster_set_restore(d, 0.0f);
    return d;
}

// Damage (capital destroyed plus output lost) as a share of the nation's annual GDP.
void add_impact(game_state& state, dcon::disaster_id d, double damage, float deaths, float affected, double lost_output = 0.0) {
    auto& w = state.world;
    w.disaster_set_damage_usd(d, w.disaster_get_damage_usd(d) + damage + lost_output);
    w.disaster_set_deaths(d, w.disaster_get_deaths(d) + deaths);
    w.disaster_set_affected(d, w.disaster_get_affected(d) + affected);
    const double gdp = nation_gdp(state, w.disaster_get_nation(d));
    if(gdp > 0.0) w.disaster_set_gdp_loss_pct(d, w.disaster_get_gdp_loss_pct(d) + static_cast<float>(100.0 * (damage + lost_output) / gdp));
}

// Region monthly climatology (mm): cosine seasonality around the wettest month.
float precip_clim(game_state const& state, dcon::region_id r, int month) {
    auto const& w = state.world;
    const float a = std::min(0.95f, w.region_get_precip_seasonality(r) * std::sqrt(2.0f));
    const float peak = static_cast<float>(std::max<uint8_t>(1, w.region_get_precip_peak_month(r)));
    return w.region_get_precip_mm(r) / 12.0f * (1.0f + a * std::cos(2.0f * pi * (static_cast<float>(month) - peak) / 12.0f));
}
float temp_month(game_state const& state, dcon::region_id r, int month, float warming) {
    auto const& w = state.world;
    const float lat = w.region_get_lat(r);
    const float amp = std::min(18.0f, 0.3f * std::abs(lat)) * (1.0f - 0.5f * std::min(1.0f, w.region_get_coastal(r)));
    const float warm = lat >= 0.0f ? 7.0f : 1.0f;
    return w.region_get_temp_base(r) - 0.0065f * std::max(0.0f, w.region_get_elevation_m(r)) + amp * std::cos(2.0f * pi * (static_cast<float>(month) - warm) / 12.0f) + 1.3f * warming;
}
float pet_month(float t) { return t > 0.0f ? 10.0f + 5.5f * t : 0.0f; } // mm; Thornthwaite-like
float precip_sigma(game_state const& state, dcon::region_id r) {
    return 0.35f + 0.4f * (1.0f - std::min(1.0f, state.world.region_get_precip_mm(r) / 1500.0f)); // dry climates are more variable
}

bool tele_active(int month, uint8_t from, uint8_t to) {
    if(from == 0) return false;
    return from <= to ? (month >= from && month <= to) : (month >= from || month <= to);
}

// ---------------------------------------------------------------- ENSO
void step_enso(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const float dt = 1.0f / 365.0f;
    for(auto c : w.in_climate) {
        const float t = c.get_enso(), h = c.get_enso_heat();
        const float nt = t + dt * (static_cast<float>(P.enso_growth) * t + static_cast<float>(P.enso_coupling) * h)
                         + static_cast<float>(P.enso_noise) * std::sqrt(dt) * gauss(state, 0xE750u, 0, 0);
        const float nh = h + dt * (-static_cast<float>(P.enso_damping) * h - static_cast<float>(P.enso_recharge) * t);
        c.set_enso(std::clamp(nt, -3.0f, 3.5f));
        c.set_enso_heat(std::clamp(nh, -3.0f, 3.0f));
    }
}
float enso_now(game_state const& state) {
    for(auto c : state.world.in_climate) return c.get_enso();
    return 0.0f;
}

// ---------------------------------------------------------------- water balance (monthly)
void monthly_water(game_state& state, int month) {
    auto& w = state.world;
    auto const& P = state.params;
    const float warming = warming_now(state);
    const float enso = enso_now(state);
    const float rho = static_cast<float>(P.precip_persistence);
    const float nat = static_cast<float>(P.precip_national);
    const float cap = static_cast<float>(P.soil_capacity_mm);
    const float lambda = 5.0f / 6.0f;
    const float inflate = (1.0f + rho) / (1.0f - rho);

    struct flood_hit {
        dcon::region_id r;
        float z, severity, share; // share of the region's basins flooded
    };
    std::vector<flood_hit> floods;
    for(auto r : w.in_region) {
        const auto n = r.get_owner();
        if(!n || r.get_precip_mm() <= 0.0f) continue;
        // Weather noise: national and regional parts; persistent; ENSO-shifted.
        const float e = std::sqrt(nat) * gauss(state, 0x9000u + static_cast<uint32_t>(n.id.index()), 0, 0)
                        + std::sqrt(1.0f - nat) * gauss(state, 0xA0000u + static_cast<uint32_t>(r.id.index()), 0, 0);
        float z = rho * r.get_precip_noise() + std::sqrt(1.0f - rho * rho) * e;
        r.set_precip_noise(z);
        if(tele_active(month, r.get_tele_from(), r.get_tele_to())) z += r.get_enso_tele() * enso;
        r.set_precip_anom(z);
        const float sigma = precip_sigma(state, r.id);
        const float clim = precip_clim(state, r.id, month);
        const float rain = clim * std::exp(sigma * z - 0.5f * sigma * sigma);
        const float pet = pet_month(temp_month(state, r.id, month, warming));
        const float pet0 = pet_month(temp_month(state, r.id, month, 0.0f));
        // Bucket soil.
        const float before = r.get_soil_moisture();
        const float soil = before + rain - pet;
        const float runoff = std::max(0.0f, soil - cap);
        r.set_soil_moisture(std::clamp(soil, 0.0f, cap));
        // SPEI-6 as an exponentially weighted standardised water-balance anomaly.
        const float anom = (rain - clim) - (pet - pet0);
        const float sd = sigma * clim;
        const float num = lambda * r.get_spei() * std::sqrt(std::max(1e-6f, r.get_spei_var())) + anom;
        const float var = lambda * lambda * r.get_spei_var() + sd * sd * inflate;
        r.set_spei_var(var);
        r.set_spei(num / std::sqrt(std::max(1e-6f, var)));
        // Floods: extreme rain on saturated soil over flood plains, in the wet season.
        r.set_flood_cooldown(std::max(0.0f, r.get_flood_cooldown() - 1.0f));
        // A large region holds many river basins (~50,000 km2 each); their rain anomalies share
        // the regional one (correlation 0.7), so floods somewhere in a big region are likelier
        // than the regional mean suggests (extreme-value scaling).
        const bool saturated = before >= 0.8f * cap;
        const float zf = static_cast<float>(P.flood_z) + (saturated ? 0.0f : 0.7f);
        if(r.get_flood_cooldown() <= 0.0f && r.get_floodplain_share() > 0.0f && clim >= r.get_precip_mm() / 12.0f) {
            const float basins = std::max(1.0f, r.get_area_km2() / 50000.0f);
            const float c = 0.7f;
            const float p1 = 1.0f - norm_cdf((zf - c * z) / std::sqrt(1.0f - c * c));
            const float expected = basins * p1;
            if(uniform(state, 0xF1000u + static_cast<uint32_t>(r.id.index()), 0) < 1.0f - std::exp(-expected)) {
                const float share = std::min(1.0f, std::max(1.0f, expected) / basins);
                floods.push_back({r.id, std::max(z, zf), (std::max(z, zf) - zf + 0.5f + runoff / 200.0f), share});
                r.set_flood_cooldown(2.0f);
            }
        }
    }

    // Flood records, one per nation and month.
    std::vector<dcon::disaster_id> by_nation(w.nation_size());
    for(auto const& f : floods) {
        const auto n = w.region_get_owner(f.r);
        const float pop = w.region_get_population(f.r);
        const float exposed = std::min(1.0f, w.region_get_floodplain_share(f.r) * f.share * std::min(1.0f, 0.3f + 0.3f * f.severity));
        const float affected = pop * exposed;
        const float deaths = affected * static_cast<float>(P.flood_mortality) * std::pow(income_rel(state, n, 10000.0f), -0.7f) * f.severity;
        const float loss = exposed * std::min(0.5f, static_cast<float>(P.flood_damage) * f.severity);
        const double damage = static_cast<double>(loss) * P.capital_output * region_gdp(state, f.r);
        auto& d = by_nation[n.id.index()];
        if(!d) d = new_disaster(state, disaster_kind::flood, disaster_status::ongoing, n, f.r, w.region_get_lat(f.r), w.region_get_lon(f.r), f.z, -1);
        if(f.z > w.disaster_get_magnitude(d)) {
            w.disaster_set_magnitude(d, f.z);
            w.disaster_set_region(d, f.r);
            w.disaster_set_lat(d, w.region_get_lat(f.r));
            w.disaster_set_lon(d, w.region_get_lon(f.r));
        }
        add_impact(state, d, damage, deaths, affected);
        destroy_capacity(state, f.r, loss);
        kill(state, f.r, deaths);
    }
    for(auto d : by_nation) {
        if(!d) continue;
        w.disaster_set_status(d, static_cast<uint8_t>(disaster_status::over));
        w.disaster_set_finish(d, state.current_tick + 14);
        journal_disaster_entry(state, d);
    }

    // Droughts, one record per nation while any region is in severe drought.
    const float thr = static_cast<float>(P.drought_spei);
    const auto agr = state.derived.agriculture_sector;
    for(auto n : w.in_nation) {
        dcon::disaster_id d;
        for(auto own : n.get_region_ownership()) {
            if(own.get_region().get_drought_disaster()) d = own.get_region().get_drought_disaster();
        }
        float affected = 0.0f, deaths_total = 0.0f, worst = 0.0f;
        double lost = 0.0;
        dcon::region_id worst_r;
        bool any = false;
        const float conflict = 1.0f + 3.0f * n.get_war_effort();
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            if(!hazard_region(state, r.id)) continue;
            const float spei = r.get_spei();
            const bool in = r.get_drought_disaster() ? spei < thr + 0.5f : spei < thr; // hysteresis
            const float target = in ? std::min(0.5f, 0.15f + static_cast<float>(P.drought_crop_loss) * std::max(0.0f, thr - spei)) : 0.0f;
            const float old = r.get_crop_loss();
            if(agr && target != old) {
                const auto s = static_cast<uint8_t>(agr.index());
                r.set_va0(s, r.get_va0(s) * (1.0f - target) / std::max(0.05f, 1.0f - old));
            }
            r.set_crop_loss(target);
            if(!in) {
                r.set_drought_disaster(dcon::disaster_id{});
                continue;
            }
            any = true;
            const float rural = r.get_population() * (1.0f - r.get_urban_share());
            affected += rural;
            const float insurgency = 1.0f + 3.0f * r.get_insurgency();
            // Famine kills where people are poor (below ~2,500 USD a head) and markets or aid
            // fail; irrigation and imports protect richer nations (Sen 1981; Devereux 2009).
            const float poor = std::clamp((2500.0f - n.get_gdp_pc0()) / 1500.0f, 0.0f, 1.0f);
            const float deaths = rural * static_cast<float>(P.famine_mortality) * poor * poor * conflict * insurgency
                                 * (thr - spei + 0.5f) / 12.0f;
            kill(state, r.id, deaths);
            deaths_total += deaths;
            if(agr) {
                const double ag = region_gdp(state, r.id) * r.get_va0(static_cast<uint8_t>(agr.index()))
                                  / std::max(1e-9f, [&] { float t = 0; for(uint8_t s = 0; s < state.derived.sector_count; ++s) t += r.get_va0(s); return t; }());
                lost += ag * target / std::max(0.05f, 1.0f - target) / 12.0;
            }
            if(-spei > worst) {
                worst = -spei;
                worst_r = r.id;
            }
        }
        if(!any) {
            if(d) {
                w.disaster_set_status(d, static_cast<uint8_t>(disaster_status::over));
                w.disaster_set_finish(d, state.current_tick);
            }
            continue;
        }
        if(!d) {
            d = new_disaster(state, disaster_kind::drought, disaster_status::ongoing, n.id, worst_r, w.region_get_lat(worst_r), w.region_get_lon(worst_r), -worst, -1);
            journal_disaster_entry(state, d);
        }
        for(auto own : n.get_region_ownership()) {
            if(own.get_region().get_crop_loss() > 0.0f) own.get_region().set_drought_disaster(d);
        }
        w.disaster_set_magnitude(d, std::min(w.disaster_get_magnitude(d), -worst));
        w.disaster_set_affected(d, std::max(w.disaster_get_affected(d), affected));
        add_impact(state, d, 0.0, deaths_total, 0.0f, lost);
    }
}

// ---------------------------------------------------------------- tropical cyclones
float season_pdf(float month, float peak, float sd) {
    float d = std::fmod(std::abs(month - peak), 12.0f);
    d = std::min(d, 12.0f - d);
    return std::exp(-0.5f * d * d / (sd * sd)) / (std::sqrt(2.0f * pi) * sd);
}


float sst_at(float lat, float month, float warming) {
    const bool north = lat >= 0.0f;
    const float peak = north ? 8.5f : 2.5f;
    const float season = std::cos(2.0f * pi * (month - peak) / 12.0f);
    const float eff = std::max(0.0f, std::abs(lat) - 5.0f - 5.0f * season);
    return 29.0f - 0.009f * eff * eff + 0.8f * warming;
}

// The region a storm sits over, if any (centroid disks).
dcon::region_id region_under(game_state const& state, float lat, float lon) {
    auto const& w = state.world;
    dcon::region_id best;
    float bd = 1e9f;
    for(auto r : w.in_region) {
        if(std::abs(r.get_lat() - lat) > 12.0f || !hazard_region(state, r.id)) continue;
        const float d = distance_km(lat, lon, r.get_lat(), r.get_lon());
        if(d < region_radius_km(state, r.id) && d < bd) {
            bd = d;
            best = r.id;
        }
    }
    return best;
}

float wind_damage_ratio(float v, float v_half) {
    const float x = std::max(0.0f, v - 25.7f) / std::max(1.0f, v_half - 25.7f);
    return x * x * x / (1.0f + x * x * x);
}

dcon::disaster_id storm_record(game_state& state, int32_t serial, dcon::nation_id n) {
    for(auto d : state.world.in_disaster) {
        if(d.get_source() == serial && d.get_nation() == n && d.get_kind() == static_cast<uint8_t>(disaster_kind::cyclone)) return d.id;
    }
    return {};
}

// Impact of storm s on region r with local wind v (incremental over what it has already done).
void storm_impact(game_state& state, dcon::storm_id s, dcon::region_id r, float v) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto n = w.region_get_owner(r);
    if(!n) return;
    const int32_t serial = w.storm_get_serial(s);
    float seen = 0.0f;
    const bool first = w.region_get_storm_seen(r) != serial;
    if(!first) seen = w.region_get_storm_wind(r);
    if(v <= seen) return;
    w.region_set_storm_seen(r, serial);
    w.region_set_storm_wind(r, v);
    const float inc = income_norm(state, n);
    const float v_half = static_cast<float>(P.cyclone_v_half) * (0.85f + 0.15f * inc); // vulnerability varies little with income (Eberenz et al. 2021)
    // Share of the region inside the damaging wind field (gales beyond 25.7 m/s): a large
    // region is hit only in part.
    const float wind = w.storm_get_wind(s);
    const float r_damage = std::min(w.storm_get_radius_km(s), 40.0f * (wind / 25.7f) * (wind / 25.7f));
    const float footprint = std::min(1.0f, pi * r_damage * r_damage / std::max(1.0f, w.region_get_area_km2(r)));
    const float exposure = (w.region_get_coastal(r) > 0.0f ? 1.0f : 0.6f) * footprint;
    const float dl = (wind_damage_ratio(v, v_half) - wind_damage_ratio(seen, v_half)) * exposure;
    const double damage = static_cast<double>(dl) * P.capital_output * region_gdp(state, r);
    const float pop = w.region_get_population(r);
    const float poverty = std::pow(income_rel(state, n, 10000.0f), -0.7f);
    auto surge = [&](float x) { return w.region_get_lecz_share(r) * static_cast<float>(P.cyclone_surge_mortality) * norm_cdf((x - 55.0f) / 10.0f); };
    auto windd = [&](float x) { const float y = std::max(0.0f, (x - 33.0f) / 30.0f); return 2e-6f * y * y; };
    const float deaths = pop * footprint * poverty * std::max(0.0f, surge(v) - surge(seen) + windd(v) - windd(seen));
    const float radius = w.storm_get_radius_km(s);
    const float affected = first ? pop * std::min(1.0f, radius * radius / std::max(1.0f, w.region_get_area_km2(r) / pi)) : 0.0f;
    auto d = storm_record(state, serial, n);
    if(!d) {
        d = new_disaster(state, disaster_kind::cyclone, disaster_status::ongoing, n, r, w.region_get_lat(r), w.region_get_lon(r), v, serial);
        w.disaster_set_name_index(d, w.storm_get_name_index(s));
        w.disaster_set_basin(d, w.storm_get_basin(s));
        journal_disaster_entry(state, d);
    } else if(w.disaster_get_status(d) == static_cast<uint8_t>(disaster_status::warning)) {
        // The warning comes true: the forecast impact is replaced by the actual one.
        w.disaster_set_status(d, static_cast<uint8_t>(disaster_status::ongoing));
        w.disaster_set_deaths(d, 0.0f);
        w.disaster_set_affected(d, 0.0f);
        w.disaster_set_damage_usd(d, 0.0);
        w.disaster_set_gdp_loss_pct(d, 0.0f);
        w.disaster_set_magnitude(d, 0.0f);
        journal_disaster_entry(state, d);
    }
    if(v > w.disaster_get_magnitude(d)) {
        w.disaster_set_magnitude(d, v);
        w.disaster_set_region(d, r);
        w.disaster_set_lat(d, w.region_get_lat(r));
        w.disaster_set_lon(d, w.region_get_lon(r));
    }
    add_impact(state, d, damage, deaths, affected);
    w.storm_set_deaths(s, w.storm_get_deaths(s) + deaths);
    w.storm_set_damage_usd(s, w.storm_get_damage_usd(s) + damage);
    destroy_capacity(state, r, static_cast<float>(dl));
    kill(state, r, deaths);
}

// Warning with the potential impact when a strong storm is a day and a half from a coast.
void storm_warning(game_state& state, dcon::storm_id s, float vx_kmd, float vy_kmd) {
    auto& w = state.world;
    auto const& P = state.params;
    if(w.storm_get_wind(s) < 33.0f || w.storm_get_over_land(s)) return;
    const float lat = w.storm_get_lat(s), lon = w.storm_get_lon(s);
    const float plat = lat + vy_kmd * 1.5f / 111.0f;
    const float plon = lon + vx_kmd * 1.5f / (111.0f * std::max(0.2f, std::cos(lat * rad)));
    const auto r = region_under(state, plat, plon);
    if(!r) return;
    const auto n = w.region_get_owner(r);
    if(!n || storm_record(state, w.storm_get_serial(s), n)) return;
    const float v = w.storm_get_wind(s);
    const float inc = income_norm(state, n);
    const float v_half = static_cast<float>(P.cyclone_v_half) * (0.85f + 0.15f * inc); // vulnerability varies little with income (Eberenz et al. 2021)
    const float r_damage = std::min(w.storm_get_radius_km(s), 40.0f * (v / 25.7f) * (v / 25.7f));
    const float footprint = std::min(1.0f, pi * r_damage * r_damage / std::max(1.0f, w.region_get_area_km2(r)));
    const double damage = static_cast<double>(wind_damage_ratio(v, v_half) * footprint) * P.capital_output * region_gdp(state, r);
    const float pop = w.region_get_population(r) * footprint;
    const float deaths = pop * std::pow(income_rel(state, n, 10000.0f), -0.7f)
                         * (w.region_get_lecz_share(r) * static_cast<float>(P.cyclone_surge_mortality) * norm_cdf((v - 55.0f) / 10.0f)
                            + 2e-6f * std::pow(std::max(0.0f, (v - 33.0f) / 30.0f), 2.0f));
    const auto d = new_disaster(state, disaster_kind::cyclone, disaster_status::warning, n, r, w.region_get_lat(r), w.region_get_lon(r), v, w.storm_get_serial(s));
    w.disaster_set_name_index(d, w.storm_get_name_index(s));
    w.disaster_set_basin(d, w.storm_get_basin(s));
    add_impact(state, d, damage, deaths, pop);
    journal_disaster_entry(state, d);
}

void step_cyclones(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto date = core::date_from_tick(state.current_tick, state.epoch_days);
    const float month = static_cast<float>(date.month) - 0.5f + static_cast<float>(date.day) / 30.4f;
    const float warming = warming_now(state);
    const float enso = enso_now(state);

    // Genesis.
    for(auto b : w.in_cyclone_basin) {
        if(b.get_names_year() != date.year) {
            b.set_names_year(date.year);
            b.set_names_used(0);
        }
        const float rate = b.get_annual_count() * season_pdf(month, b.get_peak_month(), b.get_season_sd_months()) / 30.44f
                           * std::max(0.1f, 1.0f + b.get_enso_effect() * enso);
        const auto bi = static_cast<uint32_t>(b.id.index());
        if(uniform(state, 0x57000u + bi, 0) >= rate) continue;
        if(w.storm_size() >= 120) continue;
        const auto s = w.create_storm();
        const float u = uniform(state, 0x57000u + bi, 1), v = uniform(state, 0x57000u + bi, 2);
        float lon = b.get_lon_min() + u * (b.get_lon_max() >= b.get_lon_min() ? b.get_lon_max() - b.get_lon_min() : b.get_lon_max() + 360.0f - b.get_lon_min());
        if(lon > 180.0f) lon -= 360.0f;
        w.storm_set_basin(s, static_cast<uint8_t>(bi));
        w.storm_set_name_index(s, b.get_names_used());
        b.set_names_used(static_cast<uint16_t>(b.get_names_used() + 1));
        w.storm_set_lat(s, b.get_lat_min() + v * (b.get_lat_max() - b.get_lat_min()));
        w.storm_set_lon(s, lon);
        w.storm_set_wind(s, 18.0f);
        w.storm_set_peak_wind(s, 18.0f);
        w.storm_set_radius_km(s, 120.0f);
        w.storm_set_heading(s, 0.0f);
        w.storm_set_noise(s, 0.0f);
        w.storm_set_speed_kmd(s, 0.0f);
        w.storm_set_age_days(s, 0.0f);
        w.storm_set_over_land(s, 0);
        w.storm_set_deaths(s, 0.0f);
        w.storm_set_damage_usd(s, 0.0);
        w.storm_set_serial(s, state.current_tick * 128 + static_cast<int32_t>(s.index()));
    }

    // Motion, intensity and impact.
    const int sub = std::max(1, static_cast<int>(P.cyclone_substeps));
    const float dt = 1.0f / static_cast<float>(sub);
    std::vector<dcon::storm_id> dead;
    for(auto s : w.in_storm) {
        const auto basin = dcon::cyclone_basin_id{static_cast<dcon::cyclone_basin_id::value_base_t>(s.get_basin())};
        const float ridge = std::abs(w.cyclone_basin_get_ridge_lat(basin));
        // Persistent steering noise (degrees of heading).
        const float noise = 0.8f * s.get_noise() + 0.6f * 25.0f * gauss(state, 0x58000u + static_cast<uint32_t>(s.id.index()), 0, 0);
        s.set_noise(noise);
        float vx = 0.0f, vy = 0.0f;
        for(int k = 0; k < sub; ++k) {
            const float lat = s.get_lat(), lon = s.get_lon();
            const float hemi = lat >= 0.0f ? 1.0f : -1.0f;
            // Steering: easterly trades below the ridge, westerlies beyond it (recurvature).
            const float t = 1.0f / (1.0f + std::exp(-(std::abs(lat) - ridge) / 3.0f));
            const float u = (1.0f - t) * -450.0f + t * 750.0f;
            const float vp = 180.0f + 250.0f * t; // beta drift and steering (Holland 1983)
            const float c = std::cos(noise * rad), sn = std::sin(noise * rad);
            vx = u * c - vp * sn;
            vy = (u * sn + vp * c) * hemi;
            float nlat = lat + vy * dt / 111.0f;
            float nlon = lon + vx * dt / (111.0f * std::max(0.2f, std::cos(lat * rad)));
            if(nlon > 180.0f) nlon -= 360.0f;
            if(nlon < -180.0f) nlon += 360.0f;
            s.set_lat(nlat);
            s.set_lon(nlon);
            const auto under = region_under(state, nlat, nlon);
            s.set_over_land(under ? 1 : 0);
            float wind = s.get_wind();
            if(under) {
                wind = 13.6f + (wind - 13.6f) * std::exp(-static_cast<float>(P.cyclone_land_decay) * dt);
            } else {
                const float sst = sst_at(nlat, month, warming);
                const float vpot = sst > 26.0f ? std::min(90.0f, 18.0f + static_cast<float>(P.cyclone_pi_slope) * (sst - 26.0f)) : 0.0f;
                wind += static_cast<float>(P.cyclone_intensify) * (vpot - wind) * dt;
            }
            s.set_wind(wind);
            s.set_peak_wind(std::max(s.get_peak_wind(), wind));
            s.set_age_days(s.get_age_days() + dt);
            s.set_radius_km(std::min(450.0f, 120.0f + 12.0f * s.get_age_days()));
            s.set_heading(std::atan2(vx, vy * hemi) / rad);
            s.set_speed_kmd(std::sqrt(vx * vx + vy * vy));
            // Impact on regions within reach.
            if(wind >= 17.0f) {
                const float radius = s.get_radius_km();
                for(auto r : w.in_region) {
                    if(std::abs(r.get_lat() - nlat) > 8.0f || !hazard_region(state, r.id)) continue;
                    const float rr = region_radius_km(state, r.id);
                    const float d = distance_km(nlat, nlon, r.get_lat(), r.get_lon());
                    const float de = std::max(0.0f, d - 0.5f * rr);
                    if(de >= radius) continue;
                    const float vr = wind * (de <= 40.0f ? 1.0f : std::sqrt(40.0f / de));
                    if(vr >= 25.7f || (vr >= 17.0f && r.get_storm_seen() != s.get_serial())) storm_impact(state, s.id, r.id, vr);
                }
            }
        }
        storm_warning(state, s.id, vx, vy);
        if(s.get_wind() < 15.0f || s.get_age_days() > 16.0f || std::abs(s.get_lat()) > 50.0f) dead.push_back(s.id);
    }
    for(auto s : dead) {
        const int32_t serial = w.storm_get_serial(s);
        for(auto d : w.in_disaster) {
            if(d.get_source() != serial || d.get_kind() != static_cast<uint8_t>(disaster_kind::cyclone)) continue;
            d.set_finish(state.current_tick);
            // A warning that never came true lapses.
            d.set_status(static_cast<uint8_t>(disaster_status::over));
        }
        w.delete_storm(s);
    }
}

// ---------------------------------------------------------------- earthquakes and tsunamis
double bpt_hazard(double t, double mu, double alpha) {
    if(t <= 0.0 || mu <= 0.0) return 0.0;
    const double a2 = alpha * alpha;
    const double pdf = std::sqrt(mu / (2.0 * std::numbers::pi * a2 * t * t * t)) * std::exp(-(t - mu) * (t - mu) / (2.0 * mu * a2 * t));
    const double s1 = std::sqrt(t / mu), s2 = std::sqrt(mu / t);
    auto phi = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
    const double cdf = phi((s1 - s2) / alpha) + std::exp(std::min(700.0, 2.0 / a2)) * phi(-(s1 + s2) / alpha);
    const double surv = std::max(1e-12, 1.0 - std::min(1.0, cdf));
    return pdf / surv;
}

// Area of the intersection of a circle of radius a with a disk of radius b whose centre is
// at distance d (the part of a region within distance a of the epicentre).
float circle_overlap(float a, float b, float d) {
    if(a <= 0.0f || b <= 0.0f) return 0.0f;
    if(d >= a + b) return 0.0f;
    if(d <= std::abs(a - b)) return pi * std::min(a, b) * std::min(a, b);
    const float a2 = a * a, b2 = b * b, d2 = d * d;
    const float x = std::clamp((d2 + a2 - b2) / (2.0f * d * a), -1.0f, 1.0f);
    const float y = std::clamp((d2 + b2 - a2) / (2.0f * d * b), -1.0f, 1.0f);
    const float k = std::sqrt(std::max(0.0f, (-d + a + b) * (d + a - b) * (d - a + b) * (d + a + b)));
    return a2 * std::acos(x) + b2 * std::acos(y) - 0.5f * k;
}

float mmi_at(float mw, float dist_km, float depth_km, float c) {
    const float r = std::sqrt(dist_km * dist_km + depth_km * depth_km + 100.0f);
    return std::clamp(1.5f * mw - 3.0f * std::log10(r) + c, 1.0f, 12.0f);
}

void rupture(game_state& state, dcon::fault_id f, float mw) {
    auto& w = state.world;
    auto const& P = state.params;
    const float lat = w.fault_get_lat(f), lon = w.fault_get_lon(f), depth = w.fault_get_depth_km(f);
    // Rupture length grows with magnitude (Wells & Coppersmith 1994), within the fault.
    const float half = 0.5f * std::min(w.fault_get_length_km(f), std::pow(10.0f, -2.44f + 0.59f * mw));
    const float reach = std::pow(10.0f, (1.5f * mw + static_cast<float>(P.quake_mmi_c) - 5.5f) / 3.0f) + half;
    const bool tsunami = w.fault_get_subduction(f) && mw >= static_cast<float>(P.tsunami_min_mw);
    const int32_t source = static_cast<int32_t>(f.index());
    std::vector<dcon::disaster_id> quake(w.nation_size()), wave(w.nation_size());
    for(auto r : w.in_region) {
        const auto n = r.get_owner();
        if(!n || !hazard_region(state, r.id)) continue;
        const float d = distance_km(lat, lon, r.get_lat(), r.get_lon());
        const float rr = region_radius_km(state, r.id);
        const float pop = r.get_population();
        const double rgdp = region_gdp(state, r.id);
        // Shaking: population spread over the region's disk, sampled at five distances.
        if(d - rr < reach) {
            const float inc = income_norm(state, n);
            const float theta = static_cast<float>(P.quake_theta_poor + (P.quake_theta_rich - P.quake_theta_poor) * inc);
            // People spread over the region's disk; rings around the rupture (log-spaced).
            float fatal = 0.0f, loss = 0.0f, felt = 0.0f, peak = 0.0f;
            const float region_area = pi * rr * rr;
            float inner = 0.0f, covered = 0.0f;
            for(int k = 0; k < 14; ++k) {
                const float outer = 5.0f * std::pow(reach / 5.0f, static_cast<float>(k + 1) / 14.0f);
                const float area = circle_overlap(outer, rr, d);
                const float share = std::max(0.0f, area - covered) / region_area;
                covered = std::max(covered, area);
                if(share <= 0.0f) {
                    inner = outer;
                    continue;
                }
                const float mid = 0.5f * (inner + outer);
                const float mmi = mmi_at(mw, std::max(0.0f, mid - half), depth, static_cast<float>(P.quake_mmi_c));
                peak = std::max(peak, mmi);
                fatal += share * norm_cdf(std::log(mmi / theta) / static_cast<float>(P.quake_beta));
                loss += share * norm_cdf(std::log(mmi / (static_cast<float>(P.quake_loss_theta) + 3.5f * inc)) / 0.2f); // sturdier buildings in rich nations
                felt += mmi >= 6.0f ? share : 0.0f;
                inner = outer;
            }
            if(felt > 0.0f) {
                const float deaths = pop * fatal;
                const double damage = static_cast<double>(loss) * P.capital_output * rgdp;
                auto& q = quake[n.id.index()];
                if(!q) {
                    q = new_disaster(state, disaster_kind::earthquake, disaster_status::over, n, r.id, lat, lon, mw, source);
                    w.disaster_set_finish(q, state.current_tick);
                }
                if(peak > w.disaster_get_restore(q)) {
                    w.disaster_set_restore(q, peak); // the strongest shaking (MMI) in the nation
                    w.disaster_set_region(q, r.id);
                }
                add_impact(state, q, damage, deaths, pop * felt);
                destroy_capacity(state, r.id, loss);
                kill(state, r.id, deaths);
            }
        }
        // Tsunami: coastal regions; height falls with distance (Abe 1981).
        if(tsunami && r.get_coastal() > 0.0f && d < (mw >= 8.8f ? 12000.0f : 4000.0f)) {
            const float h = std::pow(10.0f, mw - 5.55f) / std::max(50.0f, d);
            if(h >= 0.5f) {
                const float inund = r.get_lecz_share() * std::min(1.0f, h / 10.0f) * std::min(1.0f, r.get_coastal());
                const float inc = income_norm(state, n);
                const float warning = std::min(0.97f, 0.2f + 0.7f * inc + 0.5f * std::min(1.0f, d / 3000.0f));
                const float exposed = pop * inund;
                const float deaths = exposed * norm_cdf(std::log(h / static_cast<float>(P.tsunami_depth_median)) / 0.8f) * (1.0f - warning);
                const float loss = inund * norm_cdf(std::log(h / 2.0f) / 0.7f);
                const double damage = static_cast<double>(loss) * P.capital_output * rgdp;
                auto& t = wave[n.id.index()];
                if(!t) {
                    t = new_disaster(state, disaster_kind::tsunami, disaster_status::over, n, r.id, r.get_lat(), r.get_lon(), h, source);
                    w.disaster_set_finish(t, state.current_tick);
                }
                if(h > w.disaster_get_magnitude(t)) {
                    w.disaster_set_magnitude(t, h);
                    w.disaster_set_region(t, r.id);
                    w.disaster_set_lat(t, r.get_lat());
                    w.disaster_set_lon(t, r.get_lon());
                }
                add_impact(state, t, damage, deaths, exposed);
                destroy_capacity(state, r.id, loss);
                kill(state, r.id, deaths);
            }
        }
    }
    for(auto q : quake) {
        if(q) journal_disaster_entry(state, q);
    }
    for(auto t : wave) {
        if(t && (w.disaster_get_deaths(t) >= 1.0f || w.disaster_get_damage_usd(t) > 1e7)) journal_disaster_entry(state, t);
        else if(t) w.delete_disaster(t);
    }
}

void step_faults(game_state& state) {
    auto& w = state.world;
    const double dt = 1.0 / 365.0;
    for(auto f : w.in_fault) {
        const double t = f.get_elapsed_years() + dt;
        f.set_elapsed_years(static_cast<float>(t));
        // Moderate earthquakes between characteristic ruptures: Gutenberg-Richter (b = 1) below
        // the characteristic magnitude, Poisson in time; they do not reset the cycle.
        {
            const float mmin = 6.0f, mmax = f.get_mw_char() - 0.5f;
            if(mmax > mmin) {
                const double rate = (1.0 / std::max(1.0f, f.get_recurrence_years())) * std::pow(10.0, f.get_mw_char() - mmin) * 0.3;
                if(uniform(state, 0x5F000u + static_cast<uint32_t>(f.id.index()), 0) < rate * dt) {
                    const float u = uniform(state, 0x5F000u + static_cast<uint32_t>(f.id.index()), 1);
                    const float span = 1.0f - std::pow(10.0f, -(mmax - mmin));
                    const float mw = mmin - std::log10(1.0f - u * span); // truncated G-R
                    rupture(state, f.id, mw);
                }
            }
        }
        const double p = bpt_hazard(t, f.get_recurrence_years(), std::max(0.2f, f.get_aperiodicity())) * dt;
        if(uniform(state, 0x5E000u + static_cast<uint32_t>(f.id.index()), 0) >= p) continue;
        // Magnitude grows with the slip deficit: moment ~ slip ~ elapsed time.
        const float ratio = std::clamp(static_cast<float>(t / std::max(1.0f, f.get_recurrence_years())), 0.1f, 2.0f);
        const float mw = std::max(6.0f, f.get_mw_char() + (2.0f / 3.0f) * std::log10(ratio) + 0.15f * gauss(state, 0x5E000u + static_cast<uint32_t>(f.id.index()), 1, 0));
        f.set_elapsed_years(0.0f);
        rupture(state, f.id, mw);
    }
}

} // namespace

void init_disasters(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    for(auto c : w.in_climate) {
        c.set_enso(static_cast<float>(P.enso0));
        c.set_enso_heat(static_cast<float>(P.enso_heat0));
    }
    const float cap = static_cast<float>(P.soil_capacity_mm);
    const auto date = core::date_from_tick(state.current_tick, state.epoch_days);
    const float inflate = (1.0f + static_cast<float>(P.precip_persistence)) / (1.0f - static_cast<float>(P.precip_persistence));
    for(auto r : w.in_region) {
        if(!hazard_region(state, r.id)) continue;
        const float pclim = r.get_precip_mm() / 12.0f;
        const float pet = pet_month(temp_month(state, r.id, static_cast<int>(date.month), 0.0f));
        r.set_soil_moisture(cap * std::clamp(pclim / std::max(1.0f, pet), 0.0f, 1.0f));
        const float sd = precip_sigma(state, r.id) * pclim;
        r.set_spei_var(sd * sd * inflate / (1.0f - (25.0f / 36.0f)));
        r.set_spei(0.0f);
        r.set_precip_noise(0.0f);
        r.set_precip_anom(0.0f);
        r.set_crop_loss(0.0f);
        r.set_flood_cooldown(0.0f);
        r.set_storm_seen(-1);
        r.set_storm_wind(0.0f);
        r.set_drought_disaster(dcon::disaster_id{});
    }
    const int year = date.year;
    for(auto f : w.in_fault) f.set_elapsed_years(std::max(0.0f, static_cast<float>(year) - f.get_elapsed_years())); // loaded as the year of the last rupture
}

void update_disasters(game_state& state) {
    step_enso(state);
    const auto date = core::date_from_tick(state.current_tick, state.epoch_days);
    if(date.day == 1) monthly_water(state, static_cast<int>(date.month));
    step_cyclones(state);
    step_faults(state);
    // Records of disasters long over are dropped (the journal keeps the history).
    auto& w = state.world;
    std::vector<dcon::disaster_id> old;
    for(auto d : w.in_disaster) {
        if(d.get_status() == static_cast<uint8_t>(disaster_status::over) && d.get_finish() >= 0 && state.current_tick - d.get_finish() > 730) old.push_back(d.id);
    }
    for(auto d : old) w.delete_disaster(d);
}

disaster_text describe_disaster(game_state const& state, dcon::disaster_id d) {
    auto const& w = state.world;
    disaster_text t;
    const auto kind = static_cast<disaster_kind>(w.disaster_get_kind(d));
    char buf[96];
    switch(kind) {
    case disaster_kind::cyclone: {
        const uint32_t basin_key = w.cyclone_basin_get_key(dcon::cyclone_basin_id{static_cast<dcon::cyclone_basin_id::value_base_t>(w.disaster_get_basin(d))});
        const float v = w.disaster_get_magnitude(d);
        char const* type = v < 33.0f ? "Tropical Storm"
                           : basin_key == core::pack_key("natl") || basin_key == core::pack_key("epac") ? "Hurricane"
                           : basin_key == core::pack_key("wpac")                                         ? "Typhoon"
                                                                                                         : "Cyclone";
        std::snprintf(buf, sizeof(buf), "%s %s", type, storm_name(basin_key, w.disaster_get_name_index(d)));
        t.title = buf;
        break;
    }
    case disaster_kind::flood: t.title = "Floods"; break;
    case disaster_kind::drought: t.title = "Drought"; break;
    case disaster_kind::earthquake:
        std::snprintf(buf, sizeof(buf), "Magnitude %.1f earthquake", w.disaster_get_magnitude(d));
        t.title = buf;
        break;
    case disaster_kind::tsunami: t.title = "Tsunami"; break;
    default: t.title = "Disaster"; break;
    }
    const auto when = core::date_from_tick(w.disaster_get_start(d), state.epoch_days);
    t.date = core::format_date(when);
    t.kind = kind;
    t.status = static_cast<disaster_status>(w.disaster_get_status(d));
    t.nation = w.disaster_get_nation(d);
    t.region = w.disaster_get_region(d);
    t.deaths = w.disaster_get_deaths(d);
    t.affected = w.disaster_get_affected(d);
    t.damage_usd = w.disaster_get_damage_usd(d);
    t.gdp_loss_pct = w.disaster_get_gdp_loss_pct(d);
    t.potential = t.status == disaster_status::warning;
    return t;
}

} // namespace geosim::sim
