#include "map_layer.hpp"

#include "core/csv.hpp"
#include "inset_pack.hpp"
#include "core/diagnostics.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace geosim::client {

namespace {

constexpr uint32_t key_unsimulated = 0x10000u; // + asset nation index: borders between nations the scenario lacks
constexpr double deg = 3.14159265358979 / 180.0;

uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) { return r | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16); }
uint32_t pack_rgb(uint32_t hex) { return pack_rgb(static_cast<uint8_t>(hex >> 16), static_cast<uint8_t>(hex >> 8), static_cast<uint8_t>(hex)); }

uint32_t with_alpha(uint32_t rgb, float a) {
    return (rgb & 0xFFFFFFu) | (static_cast<uint32_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f)) << 24);
}

// A stable colour for a nation the asset does not know (e.g. a new state).
uint32_t hash_rgb(std::string const& iso) {
    uint32_t h = 2166136261u;
    for(char c : iso) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    const float hue = static_cast<float>(h % 360u) / 60.0f;
    const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    switch(static_cast<int>(hue)) {
    case 0: r = 1, g = x; break;
    case 1: r = x, g = 1; break;
    case 2: g = 1, b = x; break;
    case 3: g = x, b = 1; break;
    case 4: r = x, b = 1; break;
    default: r = 1, b = x; break;
    }
    auto ch = [](float v) { return static_cast<uint8_t>(std::lround(150.0f + 90.0f * v)); }; // pastel
    return pack_rgb(ch(r), ch(g), ch(b));
}

float distance_km(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * deg, p2 = lat2 * deg, dl = (lon2 - lon1) * deg;
    const double h = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return static_cast<float>(2.0 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h))));
}

// Hazard colours (sRGB): dark / saturated = happening now, light = potential.
struct hazard_colour {
    uint32_t now, potential;
    char const* label;
};
constexpr hazard_colour hazard_colours[] = {
    {0, 0, ""},
    {0xD0307E, 0xF2A9CC, "Earthquake"}, // pink
    {0x4E5866, 0xAEB5BE, "Cyclone"},    // grey
    {0x1F5CC8, 0x92B8F0, "Flood"},      // blue
    {0xD9A400, 0xF4E27C, "Drought"},    // yellow
};
constexpr float alpha_now = 0.85f, alpha_potential = 0.5f;

// Diverging red / green colour for a signed value scaled to [-1, 1].
uint32_t diverging(float t, float& alpha) {
    t = std::clamp(t, -1.0f, 1.0f);
    alpha = 0.2f + 0.65f * std::fabs(t);
    return t < 0.0f ? pack_rgb(0xD0473A) : pack_rgb(0x2E9E58);
}

std::string fmt(char const* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

std::string people(double v) {
    if(v >= 1e6) return fmt("%.1f M", v / 1e6);
    if(v >= 1e3) return fmt("%.0f k", v / 1e3);
    return fmt("%.0f", v);
}

char const* view_title(map_filter v) {
    switch(v) {
    case map_filter::political: return "Political";
    case map_filter::climate: return "Climate";
    case map_filter::economy: return "Economy";
    case map_filter::trade: return "Trade";
    case map_filter::energy: return "Energy";
    case map_filter::epidemic: return "Epidemic";
    case map_filter::unrest: return "Unrest";
    default: return "";
    }
}

rgba colour_of(uint32_t hex) { return client::hex(hex); }

} // namespace

void map_layer::init(map_data& data, std::string const& assets) {
    nations_ = data.nations;
    regions_ = data.regions;
    sim_region_of_.assign(regions_.size(), -1);
    if(!data.ids.empty()) {
        ids_w_ = data.ids[0].width;
        ids_h_ = data.ids[0].height;
        ids_ = std::move(data.ids[0].texels);
    }
    boxes_ = data.boxes;
    names_.assign(regions_.size(), std::string{});
    core::diagnostics diag;
    const auto t = core::csv_table::load(assets + "/scenario/world2020/regions.csv", diag);
    const auto col = t.column("name");
    for(std::size_t r = 0; col && r < t.rows() && r < names_.size(); ++r) names_[r] = std::string(t.get(r, *col));
    rebuild(map_filter::political, hazard_feed{});
}

int map_layer::pick(double lon_deg, double lat_deg) const noexcept {
    if(ids_.empty()) return -1;
    double u = (lon_deg + 180.0) / 360.0;
    u -= std::floor(u);
    const double v = std::clamp((90.0 - lat_deg) / 180.0, 0.0, 0.999999);
    const auto x = std::min<uint32_t>(ids_w_ - 1, static_cast<uint32_t>(u * ids_w_));
    const auto y = std::min<uint32_t>(ids_h_ - 1, static_cast<uint32_t>(v * ids_h_));
    const uint16_t id = ids_[std::size_t{y} * ids_w_ + x];
    if(id == map_water || id == map_unclaimed || id > regions_.size()) return -1;
    return static_cast<int>(id) - 1;
}

int map_layer::sim_region(int raster) const noexcept {
    if(!bound_ || raster < 0 || static_cast<std::size_t>(raster) >= sim_region_of_.size()) return -1;
    return sim_region_of_[static_cast<std::size_t>(raster)];
}

int map_layer::owner_of(int raster) const noexcept {
    const int sr = sim_region(raster);
    if(sr < 0 || static_cast<std::size_t>(sr) >= owners_.size()) return -1;
    const uint16_t o = owners_[static_cast<std::size_t>(sr)];
    return o < sim_nation_rgb_.size() ? static_cast<int>(o) : -1;
}

bool map_layer::update(sim_runner& sim, hazard_feed const& hazards, map_filter view) {
    if(sim.status() != sim_status::running) {
        if(view != last_view_) {
            rebuild(view, hazards);
            return true;
        }
        return false;
    }
    if(!requested_) {
        pending_ = sim.request_map_world();
        requested_ = true;
        return false;
    }
    bool changed = false;
    if(!bound_) {
        if(pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        const sim_map_world world = pending_.get();
        std::map<std::string, std::vector<uint32_t>> by_nation;
        for(std::size_t r = 0; r < world.owner.size(); ++r) {
            if(world.owner[r] < world.nation_iso3.size()) by_nation[world.nation_iso3[world.owner[r]]].push_back(static_cast<uint32_t>(r));
        }
        std::size_t bound = 0;
        for(std::size_t i = 0; i < regions_.size(); ++i) {
            auto it = by_nation.find(nations_[regions_[i].nation].iso3.data());
            if(it == by_nation.end()) continue;
            int32_t best = -1;
            double best_d = 1e30;
            const double clat = std::cos(regions_[i].lat * deg);
            for(uint32_t r : it->second) {
                const double dlon = std::remainder(static_cast<double>(world.lon[r]) - regions_[i].lon, 360.0) * clat;
                const double dlat = static_cast<double>(world.lat[r]) - regions_[i].lat;
                const double d = dlon * dlon + dlat * dlat;
                if(d < best_d) {
                    best_d = d;
                    best = static_cast<int32_t>(r);
                }
            }
            sim_region_of_[i] = best;
            bound += best >= 0;
        }
        sim_nation_rgb_.resize(world.nation_iso3.size());
        for(std::size_t n = 0; n < world.nation_iso3.size(); ++n) {
            bool found = false;
            for(auto const& an : nations_) {
                if(world.nation_iso3[n] == an.iso3.data()) {
                    sim_nation_rgb_[n] = pack_rgb(an.r, an.g, an.b);
                    found = true;
                    break;
                }
            }
            if(!found) sim_nation_rgb_[n] = hash_rgb(world.nation_iso3[n]);
        }
        owners_ = world.owner;
        nation_iso3_ = world.nation_iso3;
        bound_ = true;
        SDL_Log("map: %zu of %zu map regions bound to %zu scenario regions", bound, regions_.size(), world.owner.size());
        last_read_ = std::chrono::steady_clock::now();
        changed = true;
    }
    const auto now = std::chrono::steady_clock::now();
    // Region / nation state for the thematic views, once a second.
    if(pending_regions_.valid() && pending_regions_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        snap_ = pending_regions_.get();
        if(!snap_.faults.empty()) faults_ = std::move(snap_.faults);
        if(exports0_.empty()) {
            for(auto const& n : snap_.nations) exports0_.push_back(n.exports_pct);
        }
        // Grievance threshold for "potential unrest": the 90th percentile of the regions.
        std::vector<float> g;
        for(auto const& r : snap_.regions) g.push_back(r.grievance);
        if(!g.empty()) {
            std::nth_element(g.begin(), g.begin() + static_cast<std::ptrdiff_t>(g.size() * 9 / 10), g.end());
            grievance_q90_ = std::max(0.05f, g[g.size() * 9 / 10]);
        }
        have_snap_ = true;
        changed = true;
    }
    if(!pending_regions_.valid() && now - last_snapshot_ > std::chrono::seconds(1)) {
        pending_regions_ = sim.request_regions(faults_.empty());
        last_snapshot_ = now;
    }
    if(now - last_read_ >= std::chrono::milliseconds(250)) {
        last_read_ = now;
        const std::vector<uint16_t> before = owners_;
        sim.read_region_owners(owners_);
        changed = changed || owners_ != before;
    }
    // Hazards move every snapshot; the climate view follows them.
    if(view == map_filter::climate && have_snap_) changed = true;
    if(changed || view != last_view_) {
        rebuild(view, hazards);
        return true;
    }
    return false;
}

map_layer::hazard_state map_layer::climate_state(int i, hazard_feed const& hz) const {
    hazard_state best;
    const int sr = sim_region(i);
    region_row const* row = have_snap_ && sr >= 0 && static_cast<std::size_t>(sr) < snap_.regions.size() ? &snap_.regions[static_cast<std::size_t>(sr)] : nullptr;
    const double lat = regions_[static_cast<std::size_t>(i)].lat, lon = regions_[static_cast<std::size_t>(i)].lon;
    bool now[5] = {}, pot[5] = {};
    // Earthquakes: shaking of the last week within the felt radius; potential near
    // faults late in their cycle.
    for(auto const& q : hz.quakes()) {
        const double felt = std::pow(10.0, (1.5 * q.magnitude + 2.0 - 6.0) / 3.0);
        if(std::fabs(q.lat - lat) < felt / 100.0 && distance_km(lat, lon, q.lat, q.lon) < felt) now[quake] = true;
    }
    if(static_cast<std::size_t>(i) < fault_risk_.size() && fault_risk_[static_cast<std::size_t>(i)]) pot[quake] = true;
    // Cyclones: under a live storm's gale radius (or hit by it), potential on its
    // projected two-day path or under a warning.
    for(auto const& s : hz.storms()) {
        if(std::fabs(s.lat - lat) > 25.0) continue;
        const float d = distance_km(lat, lon, s.lat, s.lon);
        if(d < s.radius_km || (row && row->storm_seen == s.serial && row->storm_wind >= 17.0f)) now[storm] = true;
        const float hemi = s.lat >= 0.0f ? 1.0f : -1.0f;
        const double vx = s.speed_kmd * std::sin(s.heading * deg), vy = s.speed_kmd * std::cos(s.heading * deg) * hemi;
        for(double t = 0.5; t <= 2.0 && !pot[storm]; t += 0.5) {
            const double plat = s.lat + vy * t / 111.0;
            const double plon = s.lon + vx * t / (111.0 * std::max(0.2, std::cos(s.lat * deg)));
            if(distance_km(lat, lon, plat, plon) < s.radius_km * 1.2f) pot[storm] = true;
        }
    }
    for(auto const& n : hz.disasters()) {
        if(sr < 0 || n.region != static_cast<uint32_t>(sr)) continue;
        if(n.kind == 0 && n.potential) pot[storm] = true;
        if(n.kind == 1 && hz.tick() - n.start <= 30) now[flood] = true;
    }
    if(row) {
        if(row->flood_cooldown > 0.0f) now[flood] = true;
        if(row->floodplain > 0.0f && ((row->soil_frac >= 0.8f && row->wet_season) || row->precip_anom > 1.5f)) pot[flood] = true;
        if(row->crop_loss > 0.0f || row->drought_record) now[drought] = true;
        if(row->spei < -0.8f) pot[drought] = true;
    }
    for(hazard k : {quake, storm, flood, drought}) {
        if(now[k]) return {k, true};
    }
    for(hazard k : {quake, storm, flood, drought}) {
        if(pot[k]) return {k, false};
    }
    return best;
}

void map_layer::rebuild(map_filter view, hazard_feed const& hz) {
    last_view_ = view;
    // Fault proximity is static (recurrence ratios move over years): computed once.
    if(fault_risk_.empty() && !faults_.empty()) {
        fault_risk_.assign(regions_.size(), 0);
        for(std::size_t i = 0; i < regions_.size(); ++i) {
            for(auto const& f : faults_) {
                if(f.ratio <= 0.8f) continue;
                const double radius = 150.0 + 60.0 * std::max(0.0f, f.mw - 6.0f);
                if(std::fabs(f.lat - regions_[i].lat) > radius / 100.0) continue;
                if(distance_km(regions_[i].lat, regions_[i].lon, f.lat, f.lon) < radius) {
                    fault_risk_[i] = 1;
                    break;
                }
            }
        }
    }
    table_.assign((regions_.size() + 1) * 2, 0u);
    for(std::size_t i = 0; i < regions_.size(); ++i) {
        uint32_t key = 0, rgb = 0;
        float alpha = fill_strength;
        const int owner = owner_of(static_cast<int>(i));
        if(!bound_) { // the asset's world2020 owners until the scenario is bound
            auto const& n = nations_[regions_[i].nation];
            key = 1u + regions_[i].nation;
            rgb = pack_rgb(n.r, n.g, n.b);
        } else if(owner >= 0) {
            key = 1u + static_cast<uint32_t>(owner);
            rgb = sim_nation_rgb_[static_cast<std::size_t>(owner)];
        } else { // a nation the scenario does not simulate: its world2020 owner
            auto const& n = nations_[regions_[i].nation];
            key = key_unsimulated + regions_[i].nation;
            rgb = pack_rgb(n.r, n.g, n.b);
        }
        if(view != map_filter::political) {
            alpha = 0.0f;
            const int sr = sim_region(static_cast<int>(i));
            region_row const* row = have_snap_ && sr >= 0 && static_cast<std::size_t>(sr) < snap_.regions.size() ? &snap_.regions[static_cast<std::size_t>(sr)] : nullptr;
            nation_row const* nat = have_snap_ && owner >= 0 && static_cast<std::size_t>(owner) < snap_.nations.size() ? &snap_.nations[static_cast<std::size_t>(owner)] : nullptr;
            switch(view) {
            case map_filter::climate: {
                const hazard_state h = climate_state(static_cast<int>(i), hz);
                if(h.kind != none) {
                    rgb = pack_rgb(h.now ? hazard_colours[h.kind].now : hazard_colours[h.kind].potential);
                    alpha = h.now ? alpha_now : alpha_potential;
                }
                break;
            }
            case map_filter::economy:
                if(nat) rgb = diverging(nat->growth / 6.0f, alpha);
                break;
            case map_filter::trade:
                if(nat && static_cast<std::size_t>(owner) < exports0_.size()) rgb = diverging((nat->exports_pct - exports0_[static_cast<std::size_t>(owner)]) / 4.0f, alpha);
                break;
            case map_filter::energy:
                if(nat && nat->oil_demand > 0.0f) {
                    const float ratio = nat->oil_production / nat->oil_demand;
                    if(ratio >= 1.0f) {
                        rgb = pack_rgb(0x1F7A6E); // net exporter
                        alpha = 0.3f + 0.5f * std::min(1.0f, std::log10(ratio) / 1.0f);
                    } else {
                        rgb = pack_rgb(0xE08A2C); // importer: share of demand imported
                        alpha = 0.2f + 0.6f * (1.0f - ratio);
                    }
                }
                break;
            case map_filter::epidemic:
                if(row && row->infections_100k > 0.01f) {
                    rgb = pack_rgb(0x7A3FB0);
                    alpha = std::clamp(0.15f + 0.3f * std::log10(1.0f + row->infections_100k * 10.0f), 0.0f, 0.85f);
                }
                break;
            case map_filter::unrest:
                if(row) {
                    if(row->insurgency > 0.0f) {
                        rgb = pack_rgb(0xB8201A);
                        alpha = alpha_now;
                    } else if(row->unrest > 1.5f || row->protest > 0.2f || row->grievance >= grievance_q90_) {
                        rgb = pack_rgb(0xF0A08E);
                        alpha = alpha_potential;
                    }
                }
                break;
            default: break;
            }
        }
        table_[2 * (i + 1)] = key;
        table_[2 * (i + 1) + 1] = with_alpha(rgb, alpha);
    }
}

map_tooltip_info map_layer::describe(int i, map_filter view, sim_runner const& sim) const {
    map_tooltip_info out;
    if(i < 0 || static_cast<std::size_t>(i) >= regions_.size()) return out;
    out.valid = true;
    const auto ui = static_cast<std::size_t>(i);
    const int owner = owner_of(i);
    const int sr = sim_region(i);
    out.title = names_[ui].empty() ? regions_[ui].code.data() : names_[ui];
    out.subtitle = owner >= 0 ? sim.nation_name(static_cast<uint32_t>(owner)) : nations_[regions_[ui].nation].iso3.data();
    region_row const* row = have_snap_ && sr >= 0 && static_cast<std::size_t>(sr) < snap_.regions.size() ? &snap_.regions[static_cast<std::size_t>(sr)] : nullptr;
    nation_row const* nat = have_snap_ && owner >= 0 && static_cast<std::size_t>(owner) < snap_.nations.size() ? &snap_.nations[static_cast<std::size_t>(owner)] : nullptr;
    auto add = [&](std::string l, std::string v) { out.details.emplace_back(std::move(l), std::move(v)); };
    auto heading = [&](std::string l) { out.details.emplace_back(std::move(l), std::string{}); };
    if(!nat && view != map_filter::political) {
        out.label = "Not simulated";
        out.value = "";
        return out;
    }
    switch(view) {
    case map_filter::political:
        out.label = "Controlled by";
        out.value = out.subtitle;
        if(row) add("Population", people(row->population));
        break;
    case map_filter::climate: {
        out.label = "Hazard";
        out.value = "None";
        if(row) {
            if(row->crop_loss > 0.0f || row->drought_record) out.value = "Drought";
            else if(row->flood_cooldown > 0.0f) out.value = "Flooding";
            else if(row->spei < -0.8f) out.value = "Drought risk";
            else if(row->floodplain > 0.0f && ((row->soil_frac >= 0.8f && row->wet_season) || row->precip_anom > 1.5f)) out.value = "Flood risk";
            out.t = out.value == "None" ? tone::neutral : tone::bad;
            heading("Water balance");
            add("SPEI (drought index)", fmt("%+.2f", row->spei));
            add("Soil moisture", fmt("%.0f%% of capacity", row->soil_frac * 100.0));
            add("Rain anomaly", fmt("%+.1f sd", row->precip_anom));
            add("Crop loss", fmt("%.0f%%", row->crop_loss * 100.0));
            add("Flood plain", fmt("%.0f%% of land", row->floodplain * 100.0));
            add("Wet season", row->wet_season ? "yes" : "no");
            heading("Storms and faults");
            add("Last storm wind", row->storm_wind > 0.0f ? fmt("%.0f m/s", row->storm_wind) : "-");
            add("Near a fault late in its cycle", ui < fault_risk_.size() && fault_risk_[ui] ? "yes" : "no");
        }
        break;
    }
    case map_filter::economy:
        out.label = "GDP vs 2019";
        out.value = fmt("%+.1f%%", nat->growth);
        out.t = nat->growth < 0.0f ? tone::bad : tone::good;
        heading("National economy");
        add("Real GDP (run mean vs 2019)", fmt("%+.1f%%", nat->growth));
        add("Output gap", fmt("%+.1f%%", nat->gap));
        add("Unemployment", fmt("%.1f%%", nat->unemployment));
        add("Inflation (run mean vs 2019)", fmt("%.1f%%", nat->inflation));
        add("Policy rate", fmt("%.2f%%", nat->policy_rate));
        add("10-year yield", fmt("%.2f%%", nat->yield10));
        add("Budget balance change", fmt("%+.1f%% of GDP", nat->budget));
        add("Credit growth", fmt("%+.1f%%", nat->credit_growth));
        add("Real wage index", fmt("%.3f", nat->wage_real));
        add("House prices (index)", fmt("%.3f", nat->house_price));
        add("Equity prices (index)", fmt("%.3f", nat->equity_price));
        add("Gini", fmt("%.3f", nat->gini));
        if(row) {
            heading("Region");
            add("Population", people(row->population));
        }
        break;
    case map_filter::trade: {
        const float x0 = static_cast<std::size_t>(owner) < exports0_.size() ? exports0_[static_cast<std::size_t>(owner)] : nat->exports_pct;
        out.label = "Exports";
        out.value = fmt("%.1f%% of GDP", nat->exports_pct) + fmt(" (%+.1f pt)", nat->exports_pct - x0);
        out.t = nat->exports_pct < x0 - 0.5f ? tone::bad : (nat->exports_pct > x0 + 0.5f ? tone::good : tone::neutral);
        heading("Trade");
        add("Exports", fmt("%.1f%% of GDP", nat->exports_pct));
        add("Exports at the start", fmt("%.1f%% of GDP", x0));
        add("Imports", fmt("%.1f%% of GDP", nat->imports_pct));
        add("Average tariff", fmt("%.1f%%", nat->tariff));
        add("Sanction links", fmt("%.0f", nat->sanction_links));
        add("Seaborne share", fmt("%.0f%%", nat->seaborne));
        add("Freight cost (index)", fmt("%.2f", nat->freight));
        add("Logistics delay", fmt("%.1f days", nat->logistics_delay));
        if(nat->partner[0] != 0xFFFF) heading("Top export destinations");
        for(int k = 0; k < 5; ++k) {
            if(nat->partner[k] == 0xFFFF) continue;
            add(sim.nation_name(nat->partner[k]), fmt("%.1f%%", nat->partner_share[k]));
        }
        break;
    }
    case map_filter::energy: {
        const float ratio = nat->oil_demand > 0.0f ? nat->oil_production / nat->oil_demand : 0.0f;
        out.label = "Oil production";
        out.value = fmt("%.0f%% of demand", ratio * 100.0);
        heading("Energy");
        add("Oil production", fmt("%.3g", nat->oil_production));
        add("Oil demand", fmt("%.3g", nat->oil_demand));
        add("Fossil share of energy", fmt("%.0f%%", nat->fossil_share));
        add("Carbon price", fmt("$%.0f / t", nat->carbon_price));
        add("At war", nat->at_war ? "yes" : "no");
        add("Sanction links", fmt("%.0f", nat->sanction_links));
        break;
    }
    case map_filter::epidemic:
        out.label = "New infections";
        out.value = row ? fmt("%.1f per 100k / day", row->infections_100k) : "-";
        if(row) add("Population", people(row->population));
        break;
    case map_filter::unrest:
        if(row) {
            out.label = row->insurgency > 0.0f ? "Insurgency" : "Unrest";
            out.value = row->insurgency > 0.0f ? fmt("active (%.2f)", row->insurgency) : fmt("%.2f", row->unrest);
            out.t = row->insurgency > 0.0f || row->unrest > 1.5f ? tone::bad : tone::neutral;
            heading("Region");
            add("Insurgency", fmt("%.2f", row->insurgency));
            add("Unrest", fmt("%.2f", row->unrest));
            add("Protest", fmt("%.2f", row->protest));
            add("Grievance", fmt("%.2f", row->grievance));
        }
        heading("Nation");
        add("Rebel movement", nat->insurgent ? "active" : "none");
        add("At war", nat->at_war ? "yes" : "no");
        add("Unemployment", fmt("%.1f%%", nat->unemployment));
        break;
    default: break;
    }
    return out;
}

map_tab_info map_layer::tab(map_filter view, sim_runner const& sim, hazard_feed const& hz) const {
    map_tab_info t;
    t.title = view_title(view);
    switch(view) {
    case map_filter::climate: {
        for(hazard k : {quake, storm, flood, drought}) {
            t.legend.push_back({colour_of(hazard_colours[k].now), std::string(hazard_colours[k].label) + ": now"});
            t.legend.push_back({colour_of(hazard_colours[k].potential), std::string(hazard_colours[k].label) + ": potential"});
        }
        t.note = "Clouds and cyclones are shown in this view only.";
        int active = 0;
        for(auto const& n : hz.disasters()) {
            if(n.status == 2) continue;
            ++active;
            if(sim.controls(n.nation) && t.summary.size() < 8) t.summary.push_back(n.title + " · " + n.place);
        }
        t.summary.insert(t.summary.begin(), fmt("%.0f active disaster records worldwide", active)
                                                + fmt(", %.0f cyclones", static_cast<double>(hz.storms().size())));
        break;
    }
    case map_filter::economy:
        t.legend = {{colour_of(0xD0473A), "GDP below 2019"}, {colour_of(0x2E9E58), "GDP above 2019"}};
        t.note = "Rest the pointer on a region for details.";
        break;
    case map_filter::trade:
        t.legend = {{colour_of(0xD0473A), "Exports share falling"}, {colour_of(0x2E9E58), "Exports share rising"}};
        t.note = "Exports (% of GDP): change since the start.";
        break;
    case map_filter::energy:
        t.legend = {{colour_of(0x1F7A6E), "Net oil exporter"}, {colour_of(0xE08A2C), "Oil importer"}};
        t.note = "Oil production relative to demand.";
        break;
    case map_filter::epidemic:
        t.legend = {{colour_of(0x7A3FB0), "New infections per 100k (darker = more)"}};
        break;
    case map_filter::unrest: {
        t.legend = {{colour_of(0xB8201A), "Active insurgency"}, {colour_of(0xF0A08E), "Potential: high unrest, protest or grievance"}};
        int active = 0, potential = 0;
        for(std::size_t i = 0; have_snap_ && i < regions_.size(); ++i) {
            const int sr = sim_region(static_cast<int>(i));
            if(sr < 0 || static_cast<std::size_t>(sr) >= snap_.regions.size()) continue;
            auto const& r = snap_.regions[static_cast<std::size_t>(sr)];
            if(r.insurgency > 0.0f) {
                ++active;
                const int owner = owner_of(static_cast<int>(i));
                if(owner >= 0 && sim.controls(iso_of(owner)) && t.summary.size() < 8) {
                    t.summary.push_back(names_[i] + fmt(": insurgency %.2f", r.insurgency));
                }
            } else if(r.unrest > 1.5f || r.protest > 0.2f || r.grievance >= grievance_q90_) {
                ++potential;
            }
        }
        t.summary.insert(t.summary.begin(), fmt("%.0f regions with an insurgency, ", active) + fmt("%.0f at risk", potential));
        break;
    }
    default: break;
    }
    // The player's nations: the view's headline.
    if(have_snap_ && view != map_filter::climate && view != map_filter::unrest && view != map_filter::epidemic) {
        for(std::string const& iso : sim.nations()) {
            for(std::size_t n = 0; n < snap_.nations.size(); ++n) {
                if(iso_of(static_cast<int>(n)) != iso) continue;
                auto const& r = snap_.nations[n];
                if(view == map_filter::economy) t.summary.push_back(sim.nation_name(static_cast<uint32_t>(n)) + fmt(": GDP %+.1f%%", r.growth) + fmt(", unemployment %.1f%%", r.unemployment));
                if(view == map_filter::trade) t.summary.push_back(sim.nation_name(static_cast<uint32_t>(n)) + fmt(": exports %.1f%% of GDP", r.exports_pct));
                if(view == map_filter::energy && r.oil_demand > 0.0f) t.summary.push_back(sim.nation_name(static_cast<uint32_t>(n)) + fmt(": oil production %.0f%% of demand", r.oil_production / r.oil_demand * 100.0));
            }
        }
    }
    return t;
}

} // namespace geosim::client

// --- electoral map ----------------------------------------------------------

namespace geosim::client {

namespace {

// Party colours by family (sim::party_family): stable across nations.
constexpr uint32_t family_colour[] = {
    0x9AA0A6, // oth
    0xD6453D, // soc: red
    0x2F64B5, // con: blue
    0xE8B723, // lib: yellow
    0xE07B24, // chr: orange
    0x3A9A48, // grn: green
    0x2B3A67, // rad_right: navy
    0x8E1B2E, // rad_left: dark red
    0xB0162B, // com: crimson
    0x7E57A8, // reg: purple
    0x8AA834, // agr: lime
    0xC2408E, // challenger: magenta
};

// Distinct colours when the families say nothing (the generic L / C / R parties of
// nations without curated party systems are all "other").
constexpr uint32_t index_colour[] = {0xD6453D, 0xE8B723, 0x2F64B5, 0x3A9A48, 0x7E57A8, 0xE07B24, 0x2B3A67, 0xC2408E};

uint32_t party_colour(std::vector<party_info> const& parties, std::size_t j, std::string const& iso) {
    party_info const& p = parties[j];
    // US convention: Democrats blue, Republicans red (the families would give yellow / blue).
    if(iso == "USA" && p.family == 3) return 0x2F64B5;
    if(iso == "USA" && p.family == 2) return 0xD6453D;
    bool shared = false;
    for(std::size_t k = 0; k < parties.size(); ++k) shared = shared || (k != j && parties[k].family == p.family);
    if(p.family == 0 && shared) {
        if(p.key == "L") return 0xD6453D;
        if(p.key == "C") return 0xE8B723;
        if(p.key == "R") return 0x2F64B5;
        return index_colour[j % std::size(index_colour)];
    }
    return family_colour[p.family < std::size(family_colour) ? p.family : 0];
}

rgba blend_white(uint32_t hex_rgb, float t) {
    const rgba c = client::hex(hex_rgb);
    t = std::clamp(t, 0.0f, 1.0f);
    return {1.0f + (c.r - 1.0f) * t, 1.0f + (c.g - 1.0f) * t, 1.0f + (c.b - 1.0f) * t, 1.0f};
}

double mercator_y(double lat_deg) {
    const double phi = std::clamp(lat_deg, -85.0, 85.0) * 3.14159265358979 / 180.0;
    return std::log(std::tan(3.14159265358979 / 4.0 + phi / 2.0));
}

rgba red_green(float t) { // 0 red .. 1 green
    t = std::clamp(t, 0.0f, 1.0f);
    const rgba a = client::hex(0xC9443A), m = client::hex(0xF2EBD9), b = client::hex(0x2E9E58);
    auto mix = [](rgba x, rgba y, float u) { return rgba{x.r + (y.r - x.r) * u, x.g + (y.g - x.g) * u, x.b + (y.b - x.b) * u, 1.0f}; };
    return t < 0.5f ? mix(a, m, t * 2.0f) : mix(m, b, (t - 0.5f) * 2.0f);
}

float regime_support(politics_region const& r) {
    return std::clamp(0.5f * (1.0f - r.grievance) + 0.5f * r.trust - 0.15f * r.unrest - r.insurgency, 0.0f, 1.0f);
}

std::string pct(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", v * 100.0);
    return buf;
}

} // namespace

int map_layer::resolve_nation(int nation, int step, sim_runner const& sim) const {
    const auto& player = sim.nations();
    int n = nation;
    if(n < 0) {
        for(std::size_t k = 0; k < nation_iso3_.size() && n < 0; ++k) {
            if(!player.empty() && nation_iso3_[k] == player.front()) n = static_cast<int>(k);
        }
        if(n < 0 && !nation_iso3_.empty()) n = 0;
    }
    if(step != 0 && !nation_iso3_.empty()) {
        // Step through the player's nations first, then all nations.
        const int count = static_cast<int>(nation_iso3_.size());
        n = ((n + step) % count + count) % count;
    }
    return n;
}

void map_layer::update_politics(sim_runner& sim, int nation) {
    if(sim.status() != sim_status::running || nation < 0) return;
    if(pending_politics_.valid() && pending_politics_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        politics_ = pending_politics_.get();
        if(politics_.nation < nation_iso3_.size()) {
            if(start_votes_.size() < nation_iso3_.size()) start_votes_.resize(nation_iso3_.size());
            auto& sv = start_votes_[politics_.nation];
            if(sv.empty()) {
                for(auto const& p : politics_.parties) sv.push_back(p.vote_share);
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if(!pending_politics_.valid() && (nation != politics_for_ || now - last_politics_ > std::chrono::seconds(1))) {
        pending_politics_ = sim.request_politics(static_cast<uint32_t>(nation));
        politics_for_ = nation;
        last_politics_ = now;
    }
}

politics_region const* map_layer::politics_of(int raster) const noexcept {
    const int sr = sim_region(raster);
    if(sr < 0) return nullptr;
    for(auto const& r : politics_.regions) {
        if(r.region == static_cast<uint32_t>(sr)) return &r;
    }
    return nullptr;
}

bool map_layer::democracy() const noexcept {
    return politics_.electoral >= 0.3f && politics_.competitive && politics_.parties.size() >= 2;
}

// Landmass groups of a nation and their inset packing, rasterised (cached per nation).
map_layer::inset_cache const& map_layer::build_insets(int nation) const {
    for(auto const& c : insets_) {
        if(c.nation == nation) return c;
    }
    inset_cache cache;
    cache.nation = nation;
    auto raster = std::make_shared<electoral_raster>();
    std::vector<int> local(regions_.size(), -1);
    for(std::size_t i = 0; i < regions_.size(); ++i) {
        if(owner_of(static_cast<int>(i)) != nation) continue;
        local[i] = static_cast<int>(raster->regions.size());
        raster->regions.push_back(static_cast<int>(i));
    }
    if(raster->regions.empty() || ids_.empty()) {
        cache.raster = raster;
        insets_.push_back(cache);
        return insets_.back();
    }
    // 1. Connected components of each region on a coarse grid (every 4th raster cell,
    //    ~20 km), x wrapping around the globe.
    struct component {
        double lon0 = 1e9, lon1 = -1e9, lat0 = 1e9, lat1 = -1e9, weight = 0;
        std::vector<std::pair<double, double>> cells; // lon (unwrapped), lat
    };
    std::vector<component> comps;
    constexpr int step = 4;
    const double cell_deg = 360.0 / static_cast<double>(ids_w_);
    for(int r : raster->regions) {
        map_box const& b = boxes_[static_cast<std::size_t>(r)];
        const auto id = static_cast<uint16_t>(r + 1);
        const int x0 = static_cast<int>(std::floor((b.lon0 + 180.0) / cell_deg)), x1 = static_cast<int>(std::ceil((b.lon1 + 180.0) / cell_deg));
        const int y0 = std::max(0, static_cast<int>(std::floor((90.0 - b.lat1) / cell_deg))), y1 = std::min(static_cast<int>(ids_h_) - 1, static_cast<int>(std::ceil((90.0 - b.lat0) / cell_deg)));
        const int gw = std::max(1, (x1 - x0) / step + 1), gh = std::max(1, (y1 - y0) / step + 1);
        std::vector<uint8_t> in(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh), 0);
        for(int j = 0; j < gh; ++j) {
            const int y = std::min(static_cast<int>(ids_h_) - 1, y0 + j * step);
            for(int i = 0; i < gw; ++i) {
                const int x = ((x0 + i * step) % static_cast<int>(ids_w_) + static_cast<int>(ids_w_)) % static_cast<int>(ids_w_);
                in[static_cast<std::size_t>(j) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(i)] = ids_[static_cast<std::size_t>(y) * ids_w_ + static_cast<std::size_t>(x)] == id;
            }
        }
        bool any = false;
        std::vector<int> stack;
        for(std::size_t start = 0; start < in.size(); ++start) {
            if(in[start] != 1) continue;
            component c;
            stack.assign(1, static_cast<int>(start));
            in[start] = 2;
            while(!stack.empty()) {
                const int k = stack.back();
                stack.pop_back();
                const int i = k % gw, j = k / gw;
                const double lon = -180.0 + (x0 + i * step + 0.5) * cell_deg, lat = 90.0 - (y0 + j * step + 0.5) * cell_deg;
                c.cells.emplace_back(lon, lat);
                c.lon0 = std::min(c.lon0, lon - step * cell_deg * 0.5);
                c.lon1 = std::max(c.lon1, lon + step * cell_deg * 0.5);
                c.lat0 = std::min(c.lat0, lat - step * cell_deg * 0.5);
                c.lat1 = std::max(c.lat1, lat + step * cell_deg * 0.5);
                c.weight += std::cos(lat * deg);
                const int nb[4][2] = {{i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1}};
                for(auto const& q : nb) {
                    if(q[0] < 0 || q[0] >= gw || q[1] < 0 || q[1] >= gh) continue;
                    const std::size_t qk = static_cast<std::size_t>(q[1]) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(q[0]);
                    if(in[qk] == 1) {
                        in[qk] = 2;
                        stack.push_back(static_cast<int>(qk));
                    }
                }
            }
            comps.push_back(std::move(c));
            any = true;
        }
        if(!any && b.cells > 0) { // on the raster but smaller than a coarse cell: its centre (regions overdrawn by another nation have no cells)
            component c;
            const double lon = (b.lon0 + b.lon1) * 0.5, lat = (b.lat0 + b.lat1) * 0.5;
            c.cells.emplace_back(lon, lat);
            c = {b.lon0, b.lon1, b.lat0, b.lat1, std::cos(lat * deg) * 0.5, c.cells};
            comps.push_back(std::move(c));
        }
    }
    // 2. Unwrap longitudes around the largest component.
    std::size_t biggest = 0;
    for(std::size_t k = 0; k < comps.size(); ++k) {
        if(comps[k].weight > comps[biggest].weight) biggest = k;
    }
    const double ref = (comps[biggest].lon0 + comps[biggest].lon1) * 0.5;
    for(auto& c : comps) {
        const double shift = std::remainder((c.lon0 + c.lon1) * 0.5 - ref, 360.0) + ref - (c.lon0 + c.lon1) * 0.5;
        c.lon0 += shift;
        c.lon1 += shift;
        for(auto& p : c.cells) p.first += shift;
    }
    // 3. Group components closer than ~250 km (union-find on box gaps).
    std::vector<std::size_t> parent(comps.size());
    for(std::size_t k = 0; k < parent.size(); ++k) parent[k] = k;
    auto find = [&](std::size_t k) {
        while(parent[k] != k) k = parent[k] = parent[parent[k]];
        return k;
    };
    for(std::size_t a = 0; a < comps.size(); ++a) {
        for(std::size_t b2 = a + 1; b2 < comps.size(); ++b2) {
            component const& p = comps[a];
            component const& q = comps[b2];
            const double mid = std::cos(((p.lat0 + p.lat1 + q.lat0 + q.lat1) * 0.25) * deg);
            const double gx = std::max(0.0, std::max(p.lon0, q.lon0) - std::min(p.lon1, q.lon1)) * 111.0 * mid;
            const double gy = std::max(0.0, std::max(p.lat0, q.lat0) - std::min(p.lat1, q.lat1)) * 111.0;
            if(std::max(gx, gy) < 250.0) parent[find(a)] = find(b2);
        }
    }
    std::map<std::size_t, component> groups_by_root;
    for(std::size_t k = 0; k < comps.size(); ++k) {
        component& g = groups_by_root[find(k)];
        component const& c = comps[k];
        g.lon0 = std::min(g.lon0, c.lon0);
        g.lon1 = std::max(g.lon1, c.lon1);
        g.lat0 = std::min(g.lat0, c.lat0);
        g.lat1 = std::max(g.lat1, c.lat1);
        g.weight += c.weight;
        g.cells.insert(g.cells.end(), c.cells.begin(), c.cells.end());
    }
    std::vector<component> groups;
    double total = 0.0;
    for(auto& [root, g] : groups_by_root) total += g.weight;
    for(auto& [root, g] : groups_by_root) {
        // Drop specks (< 0.03 % of the nation's land; Puerto Rico-sized islands stay).
        if(g.weight >= 0.0003 * total) groups.push_back(std::move(g));
    }
    std::stable_sort(groups.begin(), groups.end(), [](auto const& a, auto const& b) { return a.weight > b.weight; });
    if(groups.size() > 9) groups.resize(9); // the main group and at most eight insets
    // 4. Pack in Mercator.
    std::vector<merc_rect> rects;
    std::vector<double> weight;
    for(auto const& g : groups) {
        rects.push_back({g.lon0 * deg, mercator_y(g.lat0), g.lon1 * deg, mercator_y(g.lat1)});
        weight.push_back(g.weight);
    }
    occupancy land = make_occupancy(rects[0]);
    for(auto const& [lon, lat] : groups[0].cells) land.mark(lon * deg, mercator_y(lat));
    const std::vector<inset_placement> placed = pack_insets(rects, weight, 0, land);
    // 5. Rasterise the layout: each cell maps back through its placement to lon/lat.
    const merc_rect u = union_of(placed);
    const double aspect = u.w() / std::max(1e-9, u.h());
    constexpr int long_side = 960; // cells on the long side: about one per pixel in a full-HD window
    raster->w = aspect >= 1.0 ? long_side : std::max(16, static_cast<int>(long_side * aspect));
    raster->h = aspect >= 1.0 ? std::max(16, static_cast<int>(long_side / aspect)) : long_side;
    raster->cell.assign(static_cast<std::size_t>(raster->w) * static_cast<std::size_t>(raster->h), electoral_raster::empty);
    for(int j = 0; j < raster->h; ++j) {
        const double y = u.y1 - (j + 0.5) / raster->h * u.h();
        for(int i = 0; i < raster->w; ++i) {
            const double x = u.x0 + (i + 0.5) / raster->w * u.w();
            inset_placement const* where = nullptr;
            for(std::size_t k = placed.size(); k-- > 0;) { // insets over the main group
                merc_rect const& r = placed[k].placed;
                if(x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
                    where = &placed[k];
                    break;
                }
            }
            if(!where) continue;
            int32_t& out = raster->cell[static_cast<std::size_t>(j) * static_cast<std::size_t>(raster->w) + static_cast<std::size_t>(i)];
            const double ox = where->source.x0 + (x - where->placed.x0) / where->scale;
            const double oy = where->source.y0 + (y - where->placed.y0) / where->scale;
            const double lat = (2.0 * std::atan(std::exp(oy)) - 3.14159265358979 / 2.0) / deg;
            const int r = pick(ox / deg, lat);
            if(r >= 0 && local[static_cast<std::size_t>(r)] >= 0) out = local[static_cast<std::size_t>(r)];
            if(where->inset) { // a thin frame around each inset
                const double fx = std::min(x - where->placed.x0, where->placed.x1 - x) / u.w() * raster->w;
                const double fy = std::min(y - where->placed.y0, where->placed.y1 - y) / u.h() * raster->h;
                if(fx < 1.0 || fy < 1.0) out = electoral_raster::frame;
            }
        }
    }
    // White borders between regions.
    std::vector<int32_t> borders = raster->cell;
    for(int j = 0; j < raster->h; ++j) {
        for(int i = 0; i < raster->w; ++i) {
            const std::size_t k = static_cast<std::size_t>(j) * static_cast<std::size_t>(raster->w) + static_cast<std::size_t>(i);
            const int32_t v = raster->cell[k];
            if(v < 0) continue;
            const int32_t left = i > 0 ? raster->cell[k - 1] : v, up = j > 0 ? raster->cell[k - static_cast<std::size_t>(raster->w)] : v;
            if((left >= 0 && left != v) || (up >= 0 && up != v)) borders[k] = electoral_raster::border;
        }
    }
    raster->cell.swap(borders);
    for(int j = 0; j < raster->h; ++j) {
        int i = 0;
        while(i < raster->w) {
            const int32_t v = raster->cell[static_cast<std::size_t>(j) * static_cast<std::size_t>(raster->w) + static_cast<std::size_t>(i)];
            int e = i + 1;
            while(e < raster->w && raster->cell[static_cast<std::size_t>(j) * static_cast<std::size_t>(raster->w) + static_cast<std::size_t>(e)] == v) ++e;
            if(v != electoral_raster::empty) raster->runs.push_back({static_cast<uint16_t>(j), static_cast<uint16_t>(i), static_cast<uint16_t>(e - i), v});
            i = e;
        }
    }
    cache.raster = raster;
    cache.aspect = static_cast<float>(aspect);
    SDL_Log("electoral map: %s, %zu regions, %zu groups, %zu runs", iso_of(nation).c_str(), raster->regions.size(), groups.size(), raster->runs.size());
    insets_.push_back(cache);
    return insets_.back();
}

electoral_view_info map_layer::electoral(int nation, int mode, sim_runner const& sim) const {
    electoral_view_info out;
    if(nation < 0) return out;
    out.nation = sim.nation_name(static_cast<uint32_t>(nation));
    if(politics_.nation != static_cast<uint32_t>(nation)) return out; // loading
    out.valid = true;
    out.democracy = democracy();
    const std::string iso = iso_of(nation);
    auto const& parties = politics_.parties;
    if(out.democracy) {
        out.modes.push_back("Winner");
        for(auto const& p : parties) out.modes.push_back(p.key);
        out.modes.push_back("Population");
        out.modes.push_back("Turnout");
    } else {
        out.modes = {"Regime support", "Unrest", "Insurgency", "Population"};
    }
    out.mode = std::clamp(mode, 0, static_cast<int>(out.modes.size()) - 1);
    inset_cache const& insets = build_insets(nation);
    out.aspect = insets.aspect;
    out.raster = insets.raster;
    auto const& members = insets.raster->regions;
    const int party_mode = out.democracy && out.mode >= 1 && out.mode <= static_cast<int>(parties.size()) ? out.mode - 1 : -1;
    const bool population = out.modes[static_cast<std::size_t>(out.mode)] == "Population";
    const bool turnout = out.modes[static_cast<std::size_t>(out.mode)] == "Turnout";
    // Population density range for the shading.
    float dmax = 0.0f;
    // A region's share of its sim region's electorate (in the slices one sim region
    // spans all of a nation's map regions): by raster cells.
    std::map<int, double> cells_of_sim;
    for(int r : members) cells_of_sim[sim_region(r)] += std::max(1u, boxes_[static_cast<std::size_t>(r)].cells);
    auto density = [&](int r, politics_region const& pr) {
        const double cells = std::max(1u, boxes_[static_cast<std::size_t>(r)].cells);
        const double share = cells / std::max(1.0, cells_of_sim[sim_region(r)]);
        return static_cast<float>(std::log10(1.0 + pr.electorate * share / cells));
    };
    for(int r : members) {
        if(politics_region const* pr = politics_of(r)) dmax = std::max(dmax, density(r, *pr));
    }
    out.colours.resize(members.size());
    for(std::size_t li = 0; li < members.size(); ++li) {
        const int region = members[li];
        politics_region const* pr = politics_of(region);
        rgba& colour = out.colours[li];
        colour = client::hex(0xD5DCE5);
        if(!pr) continue;
        if(population) {
            colour = blend_white(0x5B3F9E, 0.15f + 0.85f * (dmax > 0.0f ? density(region, *pr) / dmax : 0.0f));
        } else if(out.democracy && turnout) {
            const float to = pr->electorate > 0.0f ? pr->voters / pr->electorate : 0.0f;
            colour = blend_white(0x1F7A6E, 0.15f + 0.85f * std::clamp((to - 0.5f) / 0.25f, 0.0f, 1.0f));
        } else if(party_mode >= 0) {
            colour = blend_white(party_colour(parties, static_cast<std::size_t>(party_mode), iso),
                                   0.1f + 0.9f * std::clamp(pr->share[party_mode] / 0.6f, 0.0f, 1.0f));
        } else if(out.democracy) { // winner, saturation by margin
            std::size_t best = 0, second = parties.size() > 1 ? 1 : 0;
            for(std::size_t j = 0; j < parties.size(); ++j) {
                if(pr->share[j] > pr->share[best]) {
                    second = best;
                    best = j;
                } else if(j != best && pr->share[j] > pr->share[second]) {
                    second = j;
                }
            }
            const float margin = best == second ? 1.0f : pr->share[best] - pr->share[second];
            colour = blend_white(party_colour(parties, best, iso), 0.3f + 0.7f * std::clamp(margin / 0.3f, 0.0f, 1.0f));
        } else if(out.mode == 0) {
            colour = red_green(regime_support(*pr));
        } else if(out.mode == 1) {
            colour = blend_white(0xC23A30, std::clamp(pr->unrest / 3.0f, 0.05f, 1.0f));
        } else if(out.mode == 2) {
            colour = pr->insurgency > 0.0f ? client::hex(0xB8201A) : (pr->unrest > 1.5f || pr->protest > 0.2f ? client::hex(0xF0A08E) : client::hex(0xF2EBD9));
        }
    }
    // Legend and the national strip.
    if(out.democracy) {
        if(party_mode < 0 && !population && !turnout) {
            for(std::size_t j = 0; j < parties.size(); ++j) out.legend.push_back({client::hex(party_colour(parties, j, iso)), parties[j].key});
        }
        std::string votes = "Vote intention:", seats = "Seats:", gov = "Government:";
        auto const* sv = politics_.nation < start_votes_.size() ? &start_votes_[politics_.nation] : nullptr;
        std::string swing = "Swing since the start:";
        for(std::size_t j = 0; j < parties.size(); ++j) {
            votes += " " + parties[j].key + " " + pct(parties[j].vote_share);
            if(parties[j].seat_share > 0.0f) seats += " " + parties[j].key + " " + pct(parties[j].seat_share);
            if(parties[j].in_government) gov += " " + parties[j].key + (parties[j].incumbent ? " (head)" : "");
            if(sv && j < sv->size()) swing += " " + parties[j].key + fmt(" %+.1f", (parties[j].vote_share - (*sv)[j]) * 100.0);
        }
        out.strip = {votes};
        if(seats != "Seats:") out.strip.push_back(seats);
        out.strip.push_back(gov + fmt("  (electoral index %.2f)", politics_.electoral));
        out.strip.push_back(swing);
        if(politics_.next_election > politics_.tick && sim.status() == sim_status::running) {
            out.strip.push_back("Next election: " + format_long_date(core::date_from_tick(politics_.next_election, sim.epoch_days())));
        }
    } else {
        if(out.mode == 0) out.legend = {{red_green(0.0f), "Low support"}, {red_green(0.5f), "Mixed"}, {red_green(1.0f), "High support"}};
        out.strip = {"Not an election: regime support = (1 - grievance) and regional trust, less unrest and insurgency",
                     fmt("Legitimacy %.2f", politics_.legitimacy) + fmt(" · military loyalty %.2f", politics_.military_loyalty)
                         + fmt(" · repression %.2f", politics_.repression) + fmt(" · liberal index %.2f", politics_.liberal)};
    }
    return out;
}

map_tooltip_info map_layer::describe_tile(int region, int mode, sim_runner const& sim) const {
    map_tooltip_info out;
    if(region < 0 || static_cast<std::size_t>(region) >= regions_.size()) return out;
    politics_region const* pr = politics_of(region);
    out.valid = true;
    out.title = names_[static_cast<std::size_t>(region)];
    out.subtitle = sim.nation_name(politics_.nation);
    auto const& parties = politics_.parties;
    if(!pr) {
        out.label = "No data";
        return out;
    }
    (void)mode;
    if(democracy()) {
        std::size_t best = 0;
        for(std::size_t j = 0; j < parties.size(); ++j) {
            if(pr->share[j] > pr->share[best]) best = j;
        }
        out.label = "Leading";
        out.value = parties.empty() ? "-" : parties[best].key + " " + pct(pr->share[best]);
        out.details.emplace_back("Region", "");
        for(std::size_t j = 0; j < parties.size(); ++j) out.details.emplace_back(parties[j].key, pct(pr->share[j]));
        out.details.emplace_back("Turnout", pct(pr->electorate > 0.0f ? pr->voters / pr->electorate : 0.0f));
        out.details.emplace_back("Electorate", people(pr->electorate));
        out.details.emplace_back("National", "");
        for(auto const& p : parties) {
            out.details.emplace_back(p.key + (p.in_government ? (p.incumbent ? " (gov, head)" : " (gov)") : ""),
                                     pct(p.vote_share) + " · seats " + pct(p.seat_share));
        }
        if(politics_.next_election > politics_.tick && sim.status() == sim_status::running) {
            out.details.emplace_back("Next election", format_long_date(core::date_from_tick(politics_.next_election, sim.epoch_days())));
        }
    } else {
        out.label = "Regime support";
        out.value = fmt("%.2f", regime_support(*pr));
        out.t = regime_support(*pr) < 0.4f ? tone::bad : tone::neutral;
        out.details.emplace_back("Region (not an election)", "");
        out.details.emplace_back("Grievance", fmt("%.2f", pr->grievance));
        out.details.emplace_back("Trust", fmt("%.2f", pr->trust));
        out.details.emplace_back("Unrest", fmt("%.2f", pr->unrest));
        out.details.emplace_back("Protest", fmt("%.2f", pr->protest));
        out.details.emplace_back("Insurgency", fmt("%.2f", pr->insurgency));
        out.details.emplace_back("Population", people(pr->electorate));
        out.details.emplace_back("Nation", "");
        out.details.emplace_back("Legitimacy", fmt("%.2f", politics_.legitimacy));
        out.details.emplace_back("Military loyalty", fmt("%.2f", politics_.military_loyalty));
        out.details.emplace_back("Repression", fmt("%.2f", politics_.repression));
        out.details.emplace_back("Liberal index", fmt("%.2f", politics_.liberal));
    }
    return out;
}

} // namespace geosim::client
