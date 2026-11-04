// Finance and long run on the 2020 slice:
//  - no-shock exchange rates stay near 1 and nobody but Saudi Arabia is pegged;
//  - 2020 emissions fall (real: -5.4%, Global Carbon Project) and warming continues;
//  - Mexico's remittance receipts are close to 3% of GDP;
//  - a debt blowout raises the spread and ends in a default, with money conserved.

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

int main() {
    // Baseline: exchange rates and pegs.
    {
        auto s = std::make_unique<game_state>();
        (void)sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
        int pegged = 0;
        for(auto n : s->world.in_nation) pegged += sim::peg_anchor(*s, n.id) ? 1 : 0;
        CHECK(pegged == 1);
        for(auto e : s->world.in_event) e.set_done(1);
        for(int d = 0; d < 180; ++d) sim::advance_day(*s);
        for(auto n : s->world.in_nation) {
            CHECK(std::isfinite(n.get_exchange_rate()) && std::abs(n.get_exchange_rate() - 1.0f) < 0.1f);
        }
    }

    // 2020: emissions, warming, remittances.
    {
        auto s = std::make_unique<game_state>();
        (void)sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
        sim::begin_measurement(*s);
        double emissions = 0.0;
        int samples = 0;
        float t0 = 0.0f;
        for(auto c : s->world.in_climate) t0 = c.get_temperature();
        for(int d = 0; d < 366; ++d) {
            sim::advance_day(*s);
            if(d % 28 == 0) {
                for(auto c : s->world.in_climate) emissions += c.get_emissions();
                ++samples;
            }
        }
        const double mean = emissions / samples;
        float t1 = 0.0f;
        for(auto c : s->world.in_climate) t1 = c.get_temperature();
        std::printf("2020 emissions %.1f Gt (2019: 36.4), warming %.3f -> %.3f C\n", mean, t0, t1);
        CHECK(mean < 36.4 && mean > 30.0);
        CHECK(t1 > t0);
        const auto mex = sim::find_nation(*s, "MEX");
        const double gdp = s->world.nation_get_acc_gdp_nominal(mex);
        const double remit = s->world.nation_get_flows(mex, static_cast<uint8_t>(sim::flow::remittances_in));
        // Real: 3.05% in 2019, about 3.8% in 2020 (receipts held up while GDP fell).
        std::printf("MEX remittances %.2f%% of GDP (2019 data 3.05%%, 2020 about 3.8%%)\n", remit / gdp * 100.0);
        CHECK(remit / gdp > 0.02 && remit / gdp < 0.05);
    }

    // Debt blowout -> spread -> default; the haircut moves money, it does not create it.
    {
        auto s = std::make_unique<game_state>();
        (void)sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
        const auto bra = sim::find_nation(*s, "BRA");
        const double money0 = sim::world_money(*s);
        const double gdp = s->world.nation_get_gdp0(bra);
        sim::transfer(*s, sim::account::government(bra), sim::account::finance(bra), 4.0 * gdp, sim::flow::fiscal_households);
        int defaults = 0;
        for(int d = 0; d < 120; ++d) sim::advance_day(*s);
        for(auto dec : s->world.in_decision) defaults += dec.get_lever() == sim::journal_default && dec.get_target() == bra ? 1 : 0;
        std::printf("BRA spread %.1f pp (start %.1f), defaults %d\n", s->world.nation_get_spread(bra), s->world.nation_get_spread0(bra), defaults);
        CHECK(defaults == 1);
        CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);
    }

    if(failures == 0) {
        std::puts("test_finance: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_finance: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
