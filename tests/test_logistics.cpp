// Logistics on the 2020 slice (no other events):
//  1. a 6-day full blockage of the Suez Canal (forced, as a 2021-style grounding
//     would be drawn by the hazard) builds a queue of a few hundred ships,
//     lengthens Europe-Asia transit, raises freight rates, and the queue clears
//     within days of reopening; money is conserved;
//  2. a long closure makes carriers divert round the Cape (traffic falls);
//  3. the incident hazard gives a plausible number of blockages over decades;
//  4. a war of a littoral nation closes its strait (Turkish Straits).

#include "core/keys.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/report.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/systems.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

using namespace geosim;

namespace {

std::unique_ptr<game_state> make() {
    auto s = std::make_unique<game_state>();
    const auto report = sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    if(!report.ok()) {
        std::fprintf(stderr, "scenario: %s\n", report.error.c_str());
        std::exit(EXIT_FAILURE);
    }
    for(auto e : s->world.in_event) e.set_done(1);
    s->params.incident_rate_scale = 0.0; // no random incidents unless a test wants them
    return s;
}

dcon::chokepoint_id chokepoint(game_state const& s, char const* key) {
    for(auto c : s.world.in_chokepoint) {
        if(c.get_key() == core::pack_key(key)) return c.id;
    }
    return {};
}

void block(game_state& s, dcon::chokepoint_id c, float days) {
    s.world.chokepoint_set_blocked_left(c, days);
    s.world.chokepoint_set_blocked_elapsed(c, 0.0f);
    s.world.chokepoint_set_incident_open(c, 1.0f - s.world.chokepoint_get_incident_cut(c));
}

} // namespace

int main() {
    // 1. Six-day Suez blockage.
    {
        auto s = make();
        const auto suez = chokepoint(*s, "SUE");
        CHECK(bool(suez));
        const auto deu = sim::find_nation(*s, "DEU");
        const auto chn = sim::find_nation(*s, "CHN");
        const auto kc = static_cast<uint8_t>(chn.index());
        for(int d = 0; d < 60; ++d) sim::advance_day(*s);
        const double money0 = sim::world_money(*s);
        const float transit0 = s->world.nation_get_transit_days(deu, kc);
        const float rate0 = s->world.shipping_get_spot_rate(dcon::shipping_id{0});
        block(*s, suez, 6.0f);
        float peak_queue = 0.0f, peak_transit = 0.0f;
        int cleared_day = -1;
        for(int d = 0; d < 30; ++d) {
            sim::advance_day(*s);
            peak_queue = std::max(peak_queue, s->world.chokepoint_get_queue(suez));
            peak_transit = std::max(peak_transit, s->world.nation_get_transit_days(deu, kc));
            if(d > 6 && cleared_day < 0 && s->world.chokepoint_get_queue(suez) < 1.0f) cleared_day = d;
        }
        const float rate1 = s->world.shipping_get_spot_rate(dcon::shipping_id{0});
        std::printf("Suez 6-day blockage: queue peak %.0f ships, cleared on day %d, DEU-CHN transit %.1f -> %.1f days, spot %.3f -> %.3f\n",
                    peak_queue, cleared_day, transit0, peak_transit, rate0, rate1);
        CHECK(peak_queue > 200.0f && peak_queue < 600.0f); // Ever Given: ~420 ships
        CHECK(cleared_day > 6 && cleared_day < 20);
        CHECK(peak_transit > transit0 + 2.0f);
        CHECK(rate1 > rate0);
        CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);
    }

    // 2. A long closure: carriers divert once waiting costs more than the Cape detour.
    {
        auto s = make();
        const auto suez = chokepoint(*s, "SUE");
        for(int d = 0; d < 30; ++d) sim::advance_day(*s);
        block(*s, suez, 120.0f);
        for(int d = 0; d < 30; ++d) sim::advance_day(*s);
        const float traffic = s->world.chokepoint_get_traffic(suez);
        const float traffic0 = s->world.chokepoint_get_traffic0(suez);
        std::printf("Suez closed 30 of 120 days: traffic routed through it %.1f/day (normal %.1f)\n", traffic, traffic0);
        CHECK(traffic < 0.5f * traffic0);
    }

    // 3. Hazard: blockages over 40 simulated years (logistics only).
    {
        auto s = make();
        s->params.incident_rate_scale = 1.0;
        const auto suez = chokepoint(*s, "SUE");
        int incidents = 0;
        for(int d = 0; d < 365 * 40; ++d) {
            const float before = s->world.chokepoint_get_incidents(suez);
            sim::update_logistics(*s);
            if(s->world.chokepoint_get_incidents(suez) > before) ++incidents;
            ++s->current_tick;
        }
        std::printf("Suez blockages in 40 years: %d (calibration ~0.1/year, rising with vessel size)\n", incidents);
        CHECK(incidents >= 1 && incidents <= 20);
    }

    // 4. War in a littoral nation closes its strait.
    {
        auto s = make();
        const auto bos = chokepoint(*s, "BOS");
        const auto tur = sim::find_nation(*s, "TUR");
        const auto rus = sim::find_nation(*s, "RUS");
        s->world.nation_set_at_war(tur, static_cast<uint8_t>(rus.index()), 1.0f);
        s->world.nation_set_at_war(rus, static_cast<uint8_t>(tur.index()), 1.0f);
        for(int d = 0; d < 30; ++d) sim::update_logistics(*s), ++s->current_tick;
        std::printf("Turkish Straits open share with Turkey at war: %.2f\n", s->world.chokepoint_get_open(bos));
        CHECK(s->world.chokepoint_get_open(bos) < 0.5f);
    }

    if(failures == 0) {
        std::puts("test_logistics: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_logistics: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
