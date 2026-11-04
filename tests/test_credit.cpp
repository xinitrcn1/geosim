// Credit, central banks and bonds on the 2020 slice:
//  1. with the historical rate decisions as priors, Turkey's dependent central bank
//     and large state banks produce a credit boom (directed lending, credit growth
//     well above its nominal path) - no Turkey-specific rule;
//  2. the same shock with the AI alone: a dependent central bank sets lower rates
//     than an independent one (Turkey's independence index swapped);
//  3. reserve-currency yields fall in 2020 (flight to quality, QE);
//  4. money is conserved through credit flows, guarantees and defaults.

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
    return s;
}

} // namespace

int main() {
    // 1, 3, 4: the historical 2020 run.
    {
        auto s = make();
        sim::begin_measurement(*s);
        const double money0 = sim::world_money(*s);
        float directed_peak = 0.0f;
        const auto tur = sim::find_nation(*s, "TUR");
        for(int d = 0; d < 366; ++d) {
            sim::advance_day(*s);
            directed_peak = std::max(directed_peak, s->world.nation_get_directed_credit(tur));
        }
        const auto sum = sim::summarize(*s);
        double tur_credit = 0, deu_credit = 0, usa_y0 = 0, usa_y1 = 0;
        for(auto const& n : sum.nations) {
            if(n.iso3 == "TUR") tur_credit = n.credit_growth_pct;
            if(n.iso3 == "DEU") deu_credit = n.credit_growth_pct;
            if(n.iso3 == "USA") {
                usa_y0 = n.yield10_start_pct;
                usa_y1 = n.yield10_end_pct;
            }
        }
        std::printf("2020: TUR credit +%.1f%% (real +35%%; directed lending peak %.1f%% GDP/yr), DEU +%.1f%%; US 10y %.2f -> %.2f (real 1.86 -> 0.93)\n",
                    tur_credit, directed_peak, deu_credit, usa_y0, usa_y1);
        CHECK(tur_credit > 20.0);
        CHECK(tur_credit > 2.0 * deu_credit);
        CHECK(directed_peak > 2.0f);
        CHECK(usa_y1 < usa_y0 - 0.5);
        CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);
    }

    // 2. Central-bank independence, AI alone.
    {
        auto run = [](float cbi) {
            auto s = make();
            s->params.historical_anchors = 0.0;
            const auto tur = sim::find_nation(*s, "TUR");
            s->world.nation_set_cb_independence(tur, cbi);
            double rate = 0.0;
            for(int d = 0; d < 240; ++d) {
                sim::advance_day(*s);
                if(d >= 60) rate += s->world.nation_get_policy_rate(tur);
            }
            return rate / 180.0;
        };
        const double dependent = run(0.2f);
        const double independent = run(0.9f);
        std::printf("TUR mean policy rate (AI alone): dependent central bank %.2f%%, independent %.2f%%\n", dependent, independent);
        CHECK(dependent < independent - 0.5);
    }

    if(failures == 0) {
        std::puts("test_credit: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_credit: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
