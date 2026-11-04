// sim_runner::request_hazards: live cyclones, recent earthquakes and disaster
// records, copied at the simulation's sync point (storms and disasters are
// erasable objects, so they are never read racily from the UI thread).
#include "sim_runner.hpp"

#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <cmath>

namespace geosim::client {

namespace {

uint64_t mix_key(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

uint64_t hash_string(uint64_t h, std::string const& s) {
    for(char c : s) h = mix_key(h, static_cast<uint8_t>(c));
    return h;
}

constexpr int32_t quake_ring_days = 7;

} // namespace

std::future<hazard_snapshot> sim_runner::request_hazards() {
    auto promise = std::make_shared<std::promise<hazard_snapshot>>();
    auto future = promise->get_future();
    post([this, promise](game_state& s) {
        hazard_snapshot out;
        out.tick = s.current_tick;
        auto const& w = s.world;
        for(auto st : w.in_storm) {
            if(out.storms.size() >= map_max_storms) break;
            out.storms.push_back({st.get_lon(), st.get_lat(), st.get_wind(), st.get_radius_km()});
        }
        for(auto d : w.in_disaster) {
            const uint8_t kind = d.get_kind();
            const int32_t start = d.get_start();
            if(kind == static_cast<uint8_t>(sim::disaster_kind::earthquake) && s.current_tick - start <= quake_ring_days
               && out.quakes.size() < map_max_quakes) {
                map_quake q;
                q.key = mix_key(mix_key(0x51u, static_cast<uint64_t>(start)), static_cast<uint64_t>(std::lround(d.get_lat() * 100.0f + 1e5f)));
                q.key = mix_key(q.key, static_cast<uint64_t>(std::lround(d.get_lon() * 100.0f + 1e5f)));
                q.lon = d.get_lon();
                q.lat = d.get_lat();
                q.magnitude = d.get_magnitude();
                q.age_days = static_cast<float>(s.current_tick - start);
                out.quakes.push_back(q);
            }
            const sim::disaster_text text = sim::describe_disaster(s, d.id);
            disaster_note n;
            n.kind = kind;
            n.status = d.get_status();
            n.start = start;
            const auto nation = text.nation;
            if(nation) n.nation = core::unpack_key(w.nation_get_iso3(nation));
            n.title = text.title;
            n.place = place_name(nation ? static_cast<uint32_t>(nation.index()) : 0xFFFFFFFFu,
                                 text.region ? static_cast<uint32_t>(text.region.index()) : 0xFFFFFFFFu);
            n.date = text.date;
            n.deaths = text.deaths;
            n.affected = text.affected;
            n.damage_usd = text.damage_usd;
            n.gdp_loss_pct = text.gdp_loss_pct;
            n.potential = text.potential;
            n.id = static_cast<uint32_t>(d.id.index());
            n.key = hash_string(mix_key(mix_key(0xD15A57E2ull, kind), static_cast<uint64_t>(start)), n.title); // hazard_feed makes it per record
            out.disasters.push_back(std::move(n));
        }
        promise->set_value(std::move(out));
    });
    return future;
}

} // namespace geosim::client
