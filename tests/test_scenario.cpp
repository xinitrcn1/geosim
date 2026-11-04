// Scenario-level invariants on the real 2020 assets:
//  1. the scenario builds without errors;
//  2. a no-shock baseline stays near steady state (calibration is consistent);
//  3. money is conserved: the sum of all ledger balances never changes (SFC);
//  4. runs are deterministic;
//  5. save -> load -> continue gives exactly the same state as continuing.

#include "sim/report.hpp"
#include "sim/save.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/ledger.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

namespace {

std::unique_ptr<geosim::game_state> make(bool anchors) {
    auto s = std::make_unique<geosim::game_state>();
    const auto report = geosim::sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    if(!report.ok()) {
        std::fprintf(stderr, "scenario build failed: %s\n", report.error.c_str());
        std::exit(EXIT_FAILURE);
    }
    if(!anchors) {
        // Baseline: no events at all (no virus, no policy, no oil shock).
        for(auto e : s->world.in_event) {
            e.set_done(1);
        }
    }
    geosim::sim::begin_measurement(*s);
    return s;
}

double fingerprint(geosim::game_state const& s) {
    double h = 0.0;
    for(auto n : s.world.in_nation) {
        h = h * 1.000001 + n.get_bal_gov() + n.get_gdp_real() + n.get_unemployment() + n.get_cpi();
    }
    for(auto r : s.world.in_region) {
        h = h * 1.000001 + r.get_ep_i(0) + r.get_ep_d(1);
    }
    return h;
}

} // namespace

int main() {
    // 2. Baseline steady state.
    {
        auto s = make(false);
        const double money0 = geosim::sim::world_money(*s);
        for(int d = 0; d < 120; ++d) {
            geosim::sim::advance_day(*s);
        }
        const auto sum = geosim::sim::summarize(*s);
        for(auto const& n : sum.nations) {
            auto nid = geosim::sim::find_nation(*s, n.iso3);
            const double u0 = s->world.nation_get_unemployment0(nid) * 100.0;
            const double gdp_index = s->world.nation_get_gdp_real(nid)
                                     / (s->world.nation_get_gdp0_daily(nid) * std::exp(std::log1p(s->world.nation_get_trend_growth(nid)) * 120.0f / 365.0f));
            if(std::abs(n.unemployment_pct - u0) > 0.5 || std::abs(gdp_index - 1.0) > 0.02) {
                std::fprintf(stderr, "baseline drift %s: unemployment %.2f vs %.2f, gdp index %.4f\n", n.iso3.c_str(),
                             n.unemployment_pct, u0, gdp_index);
                ++failures;
            }
        }
        // 3. Conservation (relative to the world money stock).
        const double money1 = geosim::sim::world_money(*s);
        CHECK(std::abs(money1 - money0) <= 1e-9 * std::abs(money0) + 1.0);
    }

    // 3 + 4 with the full historical scenario.
    auto a = make(true);
    auto b = make(true);
    const double money0 = geosim::sim::world_money(*a);
    for(int d = 0; d < 90; ++d) {
        geosim::sim::advance_day(*a);
        geosim::sim::advance_day(*b);
    }
    CHECK(fingerprint(*a) == fingerprint(*b));
    CHECK(std::abs(geosim::sim::world_money(*a) - money0) <= 1e-9 * std::abs(money0) + 1.0);

    // 5. Save/load round trip mid-run.
    std::error_code ec;
    const auto path = (std::filesystem::temp_directory_path(ec) / "geosim_test_roundtrip.bin").string();
    geosim::core::diagnostics diag;
    CHECK(geosim::sim::save_game(*a, path, diag));
    auto c = std::make_unique<geosim::game_state>();
    CHECK(geosim::sim::load_game(*c, path, diag));
    CHECK(!diag.failed());
    std::filesystem::remove(path, ec);
    CHECK(c->current_tick == a->current_tick);
    for(int d = 0; d < 60; ++d) {
        geosim::sim::advance_day(*a);
        geosim::sim::advance_day(*c);
    }
    CHECK(fingerprint(*a) == fingerprint(*c));

    if(failures == 0) {
        std::puts("test_scenario: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_scenario: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
