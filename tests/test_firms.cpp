// Firms, bankruptcies and trade policy on the 2020 slice:
//  1. no shock: firm exit rates stay at their normal levels;
//  2. 2020 with and without government support for firms: without support small
//     firms fail more (Gourinchas et al. 2020);
//  3. a forced US tariff rise passes into US prices, cuts US imports and invites
//     Chinese retaliation (sanctions); money is conserved throughout.

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
    return s;
}

double sme_exits(game_state const& s) {
    double x = 0.0;
    for(auto f : s.world.in_firm) {
        if(!f.get_large()) x += f.get_exits() * f.get_share();
    }
    return x;
}

} // namespace

int main() {
    // 1. Baseline.
    {
        auto s = make(false);
        for(int d = 0; d < 365; ++d) sim::advance_day(*s);
        float worst = 0.0f;
        for(auto f : s->world.in_firm) {
            const float base = static_cast<float>(f.get_large() ? s->params.firm_exit_base_large : s->params.firm_exit_base_sme);
            worst = std::max(worst, f.get_exit_rate() / base);
        }
        std::printf("no-shock year: highest exit rate %.2fx its normal level\n", worst);
        CHECK(worst < 1.5f);
    }

    // 2. Support matters.
    double with = 0.0, without = 0.0;
    {
        auto s = make(true);
        const double money0 = sim::world_money(*s);
        for(int d = 0; d < 300; ++d) sim::advance_day(*s);
        with = sme_exits(*s);
        CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);
    }
    {
        auto s = make(true);
        s->params.firm_support_cover = 0.0;
        for(int d = 0; d < 300; ++d) sim::advance_day(*s);
        without = sme_exits(*s);
    }
    std::printf("2020 SME exits (share-weighted, 300 days): with support %.4f, without %.4f\n", with, without);
    CHECK(without > with);

    // 3. A US tariff rise.
    {
        auto run = [](bool tariff) {
            auto s = make(false);
            const auto usa = sim::find_nation(*s, "USA");
            const auto chn = sim::find_nation(*s, "CHN");
            const auto exec = s->world.nation_get_agent_from_executive_seat(usa);
            const auto lev = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::tariff));
            if(tariff) {
                s->world.agent_set_override_mode(exec, lev, static_cast<uint8_t>(sim::override_mode::force));
                s->world.agent_set_override_value(exec, lev, s->world.nation_get_tariff0(usa) + 0.2f);
            }
            for(int d = 0; d < 180; ++d) sim::advance_day(*s);
            struct out {
                float cpi, imports, sanction;
            };
            return out{s->world.nation_get_cpi(usa), s->world.nation_get_imports(usa),
                       s->world.nation_get_sanction(chn, static_cast<uint8_t>(usa.index()))};
        };
        const auto base = run(false);
        const auto hit = run(true);
        std::printf("US tariff +20pp: CPI %.4f -> %.4f, imports %.4g -> %.4g, China's sanction on the US %.2f -> %.2f\n", base.cpi, hit.cpi,
                    base.imports, hit.imports, base.sanction, hit.sanction);
        CHECK(hit.cpi > base.cpi);
        CHECK(hit.imports < 0.9f * base.imports);
        CHECK(hit.sanction > base.sanction);
    }

    if(failures == 0) {
        std::puts("test_firms: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_firms: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
