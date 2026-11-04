// Simultaneous epidemics (assets/defs/pathogens.txt):
//  1. Seasonal influenza nearly vanishes under the 2020 COVID measures, compared
//     with the same year without measures (as WHO FluNet surveillance showed).
//  2. A second, severe pandemic at the same time raises COVID mortality through
//     shared hospital capacity (policy and behaviour switched off to isolate the
//     channel: with them on, the extra threat makes everyone more careful and
//     COVID deaths can fall instead - an emergent interaction).
//  3. Cross-immunity: with full protection from flu recovery, a pathogen cannot
//     spread among people who recovered from flu.

#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"

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

static uint8_t pathogen(game_state const& s, char const* key) {
    for(auto p : s.world.in_pathogen) {
        if(p.get_key() == core::pack_key(key)) return static_cast<uint8_t>(p.id.index());
    }
    return 0xFF;
}

// World infections of pathogen p summed over days [from, to).
static double run_infections(game_state& s, uint8_t p, int from, int to) {
    double sum = 0.0;
    for(int d = 0; d < to; ++d) {
        sim::advance_day(s);
        if(d >= from) {
            for(auto r : s.world.in_region) sum += r.get_infections_p(p);
        }
    }
    return sum;
}

static void seed(game_state& s, char const* iso, uint8_t p, float infections, int tick) {
    auto e = s.world.create_event();
    s.world.event_set_kind(e, static_cast<uint8_t>(sim::event_kind::epidemic_seed));
    s.world.event_set_target(e, sim::find_nation(s, iso));
    s.world.event_set_param(e, p);
    s.world.event_set_value(e, infections);
    s.world.event_set_tick(e, tick);
}

int main() {
    // 1. Influenza under COVID measures vs no measures (no COVID, no anchors).
    {
        auto with = std::make_unique<game_state>();
        auto without = std::make_unique<game_state>();
        (void)sim::build_scenario(*with, GEOSIM_ASSETS_DIR, "2020");
        (void)sim::build_scenario(*without, GEOSIM_ASSETS_DIR, "2020");
        const uint8_t cov = pathogen(*without, "cov");
        for(auto e : without->world.in_event) {
            if(e.get_kind() == static_cast<uint8_t>(sim::event_kind::epidemic_seed) && e.get_param() == cov) e.set_done(1);
        }
        without->params.historical_anchors = 0.0; // and no epidemic -> no measures
        const uint8_t flu = pathogen(*with, "flu");
        CHECK(flu != 0xFF);
        const double f_with = run_infections(*with, flu, 91, 366);
        const double f_without = run_infections(*without, flu, 91, 366);
        std::printf("flu infections Apr-Dec: with measures %.3g, without %.3g (ratio %.3f)\n", f_with, f_without, f_with / f_without);
        CHECK(f_with < 0.5 * f_without);
    }

    // 2. A severe pandemic at the same time raises COVID deaths (shared hospitals).
    {
        auto alone = std::make_unique<game_state>();
        auto both = std::make_unique<game_state>();
        (void)sim::build_scenario(*alone, GEOSIM_ASSETS_DIR, "2020");
        (void)sim::build_scenario(*both, GEOSIM_ASSETS_DIR, "2020");
        for(auto* g : {alone.get(), both.get()}) {
            g->params.historical_anchors = 0.0;
            g->params.ai_case_tolerance = 1e12; // no NPIs
            g->params.voluntary_k = 0.0;        // no behavioural distancing
            g->params.voluntary_demand_k = 0.0;
        }
        const uint8_t pan = pathogen(*both, "pan");
        const uint8_t cov = pathogen(*both, "cov");
        CHECK(pan != 0xFF);
        seed(*both, "ITA", pan, 1000.0f, 20);
        double cov_alone = 0.0, cov_both = 0.0, pan_deaths = 0.0;
        for(int d = 0; d < 200; ++d) {
            sim::advance_day(*alone);
            sim::advance_day(*both);
        }
        for(auto r : alone->world.in_region) cov_alone += r.get_deaths_reported_p(cov);
        for(auto r : both->world.in_region) {
            cov_both += r.get_deaths_reported_p(cov);
            pan_deaths += r.get_deaths_reported_p(pan);
        }
        std::printf("COVID deaths alone %.3g, with severe pandemic %.3g; pandemic deaths %.3g\n", cov_alone, cov_both, pan_deaths);
        CHECK(pan_deaths > 0.0);
        CHECK(cov_both > cov_alone);
        bool finite = true;
        for(auto r : both->world.in_region) finite = finite && std::isfinite(r.get_ep_s(0)) && r.get_ep_s(0) >= 0.0f;
        CHECK(finite);
    }

    // 3. Full cross-immunity from flu recovery: a pathogen seeded into a population
    //    that is entirely flu-recovered cannot grow.
    {
        auto s = std::make_unique<game_state>();
        (void)sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
        s->params.historical_anchors = 0.0;
        const uint8_t flu = pathogen(*s, "flu");
        const uint8_t h1 = pathogen(*s, "h1n1");
        for(auto e : s->world.in_event) e.set_done(1);
        s->world.pathogen_set_cross_immunity(dcon::pathogen_id{h1}, flu, 1.0f);
        for(auto r : s->world.in_region) {
            for(uint8_t a = 0; a < sim::age_groups; ++a) {
                const uint8_t i = sim::epi_index(flu, a);
                r.set_ep_r(i, r.get_ep_r(i) + r.get_ep_s(i));
                r.set_ep_s(i, 0.0f);
            }
        }
        seed(*s, "USA", h1, 10000.0f, 0);
        const double later = run_infections(*s, h1, 30, 60);
        std::printf("h1n1 infections days 30-60 under full cross-immunity: %.3g\n", later);
        CHECK(later < 10000.0);
    }

    if(failures == 0) {
        std::puts("test_epidemics: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_epidemics: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
