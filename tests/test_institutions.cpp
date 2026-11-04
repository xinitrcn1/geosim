// Institutions, labour market, forks and vaccines on the 2020 slice:
//  1. a governing majority passes its bill in full; facing an opposition
//     legislature it passes only part, and less with more veto players;
//  2. an independent judiciary strikes down a forced lever;
//  3. a disloyal military defects under mass protest (regime change);
//  4. wage rigidity: high bargaining coverage damps the real-wage response to
//     unemployment (wage curve);
//  5. a counterfactual fork runs a change without touching the live state;
//  6. COVID vaccination starts only after the vaccine's development time.

#include "sim/fork.hpp"
#include "sim/ledger.hpp"
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
std::unique_ptr<game_state> make(bool events) {
    auto s = std::make_unique<game_state>();
    const auto report = sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    if(!report.ok()) {
        std::fprintf(stderr, "scenario: %s\n", report.error.c_str());
        std::exit(EXIT_FAILURE);
    }
    if(!events) {
        for(auto e : s->world.in_event) e.set_done(1);
    }
    s->params.incident_rate_scale = 0.0;
    s->params.pandemic_rate = 0.0;
    return s;
}
} // namespace

int main() {
    // 1. Bills.
    {
        auto s = make(false);
        auto& w = s->world;
        const auto deu = sim::find_nation(*s, "DEU");
        const uint8_t lev = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::public_investment));
        const float majority = sim::enact(*s, deu, lev, 0.0f, 3.0f, sim::value_axis::economic_right, -0.03f, false);
        for(auto m : w.nation_get_party_membership(deu)) m.get_party().set_in_government(0); // opposition controls the chamber
        const float divided = sim::enact(*s, deu, lev, 0.0f, 3.0f, sim::value_axis::economic_right, -0.03f, false);
        w.nation_set_veto(deu, 0.9f);
        const float vetoed = sim::enact(*s, deu, lev, 0.0f, 3.0f, sim::value_axis::economic_right, -0.03f, false);
        std::printf("public-investment bill (3%% GDP): majority %.2f, divided %.2f, divided + strong veto players %.2f\n", majority, divided, vetoed);
        CHECK(std::abs(majority - 3.0f) < 1e-4f);
        CHECK(divided < majority);
        CHECK(vetoed <= divided);
    }

    // 2. Courts.
    {
        auto s = make(false);
        const auto gbr = sim::find_nation(*s, "GBR");
        const auto exec = s->world.nation_get_agent_from_executive_seat(gbr);
        const uint8_t lev = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::info_control));
        s->world.agent_set_override_mode(exec, lev, static_cast<uint8_t>(sim::override_mode::force));
        s->world.agent_set_override_value(exec, lev, 0.9f);
        int day = -1;
        for(int d = 0; d < 365 && day < 0; ++d) {
            sim::advance_day(*s);
            if(s->world.agent_get_override_mode(exec, lev) == 0) day = d;
        }
        std::printf("UK courts struck down forced media control after %d days\n", day);
        CHECK(day >= 0);
    }

    // 3. Military defection under mass protest.
    {
        auto s = make(false);
        const auto rus = sim::find_nation(*s, "RUS");
        const double liberal0 = s->world.nation_get_liberal(rus);
        bool changed = false;
        for(int d = 0; d < 60 && !changed; ++d) {
            s->world.nation_set_military_loyalty(rus, 0.2f);
            for(auto own : s->world.nation_get_region_ownership(rus)) {
                own.get_region().set_protest(0.05f);
                own.get_region().set_drive(1.0f);
            }
            sim::advance_day(*s);
            for(auto dec : s->world.in_decision) changed = changed || (dec.get_lever() == sim::journal_regime_change && dec.get_target() == rus);
        }
        std::printf("regime change under mass protest with a disloyal military: %s (liberal %.2f -> %.2f)\n", changed ? "yes" : "no", liberal0,
                    s->world.nation_get_liberal(rus));
        CHECK(changed);
    }

    // 4. Wage rigidity (a tight labour market: nominal floors bind on the way down).
    {
        auto run = [](float coverage) {
            auto s = make(false);
            const auto usa = sim::find_nation(*s, "USA");
            s->world.nation_set_bargaining_coverage(usa, coverage);
            for(int d = 0; d < 365; ++d) {
                s->world.nation_set_unemployment(usa, 0.015f);
                sim::update_labour(*s);
                ++s->current_tick;
            }
            return s->world.nation_get_wage_real(usa);
        };
        const float flexible = run(0.1f), rigid = run(0.95f);
        std::printf("real wage after a year at 1.5%% unemployment: coverage 10%% -> %.3f, 95%% -> %.3f\n", flexible, rigid);
        CHECK(flexible > rigid);
        CHECK(rigid > 1.0f);
    }

    // 5. Forks.
    {
        auto s = make(false);
        for(int d = 0; d < 30; ++d) sim::advance_day(*s);
        const auto usa = sim::find_nation(*s, "USA");
        const float u_before = s->world.nation_get_unemployment(usa);
        const int tick = s->current_tick;
        const auto base = sim::run_fork(*s, 60, {});
        const auto shocked = sim::run_fork(*s, 60, [&](game_state& f) {
            const auto exec = f.world.nation_get_agent_from_executive_seat(usa);
            const uint8_t lev = static_cast<uint8_t>(f.derived.lever_base + static_cast<uint8_t>(sim::lever::tariff));
            f.world.agent_set_override_mode(exec, lev, static_cast<uint8_t>(sim::override_mode::direct));
            f.world.agent_set_override_value(exec, lev, 0.3f);
        });
        CHECK(base.error.empty() && shocked.error.empty());
        CHECK(s->current_tick == tick && s->world.nation_get_unemployment(usa) == u_before);
        const auto& b = base.nations[usa.index()];
        const auto& h = shocked.nations[usa.index()];
        std::printf("fork: US CPI after 60 days %.4f (no change) vs %.4f (30%% tariff); live state untouched\n", b.cpi.back(), h.cpi.back());
        CHECK(h.cpi.back() > b.cpi.back());
    }

    // 6. Vaccines (historical 2020).
    {
        auto s = make(true);
        const auto usa = sim::find_nation(*s, "USA");
        float before = 0.0f;
        for(int d = 0; d < 366; ++d) {
            sim::advance_day(*s);
            if(d == 300) before = s->world.nation_get_vaccinated(usa);
        }
        const float after = s->world.nation_get_vaccinated(usa);
        std::printf("US vaccinations: by late Oct %.0f, by 31 Dec %.0f (real: first doses 14 Dec 2020)\n", before, after);
        CHECK(before == 0.0f);
        CHECK(after > 0.0f);
    }

    if(failures == 0) {
        std::puts("test_institutions: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_institutions: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
