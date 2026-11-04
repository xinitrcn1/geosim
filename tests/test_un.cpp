// International organisations on the 2020 slice:
//  1. a permanent member occupying a neighbour vetoes Security Council sanctions on itself;
//  2. WTO: a tariff hike draws complaints; with the Appellate Body paralysed the loser
//     appeals into the void, with it working the loser complies or faces retaliation.

#include "sim/model.hpp"
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
    s->params.incident_rate_scale = 0.0;
    s->params.pandemic_rate = 0.0;
    s->params.war_base_rate = 0.0;
    return s;
}
uint16_t ix(dcon::nation_id n) { return static_cast<uint16_t>(n.index()); }
int count(game_state const& s, uint8_t code, float after) {
    int c = 0;
    for(auto d : s.world.in_decision) c += d.get_lever() == code && (std::isnan(after) || d.get_after() == after) ? 1 : 0;
    return c;
}
} // namespace

int main() {
    // 1. Veto.
    {
        auto s = make();
        auto& w = s->world;
        const auto chn = sim::find_nation(*s, "CHN"), ind = sim::find_nation(*s, "IND");
        CHECK(w.nation_get_unsc_seat(chn) == 2);
        for(int d = 0; d < 60; ++d) {
            w.nation_set_control(ind, ix(chn), 0.2f);
            sim::advance_day(*s);
        }
        CHECK(count(*s, sim::journal_unsc, 0.0f) > 0); // vetoed
        CHECK(w.nation_get_un_sanctioned(chn) == 0.0f);
    }
    // 2. WTO.
    for(int appellate = 0; appellate < 2; ++appellate) {
        auto s = make();
        auto& w = s->world;
        s->params.wto_panel_days = 60.0;
        s->params.wto_win = 1.0;
        s->params.wto_appellate = appellate;
        const auto usa = sim::find_nation(*s, "USA");
        for(int d = 0; d < 150; ++d) {
            if(d < 40) w.nation_set_tariff(usa, w.nation_get_tariff0(usa) + 0.10f);
            sim::advance_day(*s);
        }
        CHECK(count(*s, sim::journal_wto, NAN) > 0);
        if(appellate == 0) {
            CHECK(count(*s, sim::journal_wto, -1.0f) > 0);
        } else {
            CHECK(count(*s, sim::journal_wto, 1.0f) + count(*s, sim::journal_wto, 0.0f) > 0);
        }
    }
    if(failures == 0) std::printf("test_un: all checks passed\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
