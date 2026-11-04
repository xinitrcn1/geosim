#include "map_layer.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace geosim::client {

namespace {

constexpr uint32_t key_unsimulated = 0x10000u; // + asset nation index: borders between nations the scenario lacks

uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) { return r | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16); }

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

} // namespace

void map_layer::init(map_data const& data) {
    nations_ = data.nations;
    regions_ = data.regions;
    sim_region_of_.assign(regions_.size(), -1);
    rebuild();
}

bool map_layer::update(sim_runner& sim) {
    if(sim.status() != sim_status::running) return false;
    if(!requested_) {
        pending_ = sim.request_map_world();
        requested_ = true;
        return false;
    }
    if(!bound_) {
        if(pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        const sim_map_world world = pending_.get();
        // Sim regions by owner ISO3.
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
            const double clat = std::cos(regions_[i].lat * 3.14159265358979 / 180.0);
            for(uint32_t r : it->second) {
                double dlon = std::remainder(static_cast<double>(world.lon[r]) - regions_[i].lon, 360.0) * clat;
                double dlat = static_cast<double>(world.lat[r]) - regions_[i].lat;
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
        bound_ = true;
        SDL_Log("map: %zu of %zu map regions bound to %zu scenario regions", bound, regions_.size(), world.owner.size());
        rebuild();
        last_read_ = std::chrono::steady_clock::now();
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if(now - last_read_ < std::chrono::milliseconds(250)) return false;
    last_read_ = now;
    const std::vector<uint16_t> before = owners_;
    sim.read_region_owners(owners_);
    if(owners_ == before) return false;
    rebuild();
    return true;
}

void map_layer::rebuild() {
    table_.assign((regions_.size() + 1) * 2, 0u);
    for(std::size_t i = 0; i < regions_.size(); ++i) {
        uint32_t key = 0, rgb = 0;
        const float alpha = fill_strength;
        if(!bound_) { // the asset's world2020 owners until the scenario is bound
            auto const& n = nations_[regions_[i].nation];
            key = 1u + regions_[i].nation;
            rgb = pack_rgb(n.r, n.g, n.b);
        } else if(sim_region_of_[i] >= 0 && owners_[static_cast<std::size_t>(sim_region_of_[i])] < sim_nation_rgb_.size()) {
            const uint16_t owner = owners_[static_cast<std::size_t>(sim_region_of_[i])];
            key = 1u + owner;
            rgb = sim_nation_rgb_[owner];
        } else { // a nation the scenario does not simulate (folded into an aggregate): its world2020 owner
            auto const& n = nations_[regions_[i].nation];
            key = key_unsimulated + regions_[i].nation;
            rgb = pack_rgb(n.r, n.g, n.b);
        }
        table_[2 * (i + 1)] = key;
        table_[2 * (i + 1) + 1] = with_alpha(rgb, alpha);
    }
}

} // namespace geosim::client
