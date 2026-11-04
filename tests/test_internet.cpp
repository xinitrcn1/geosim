// The internet as political infrastructure, on the 2020 slice (no events):
//  1. starting online controls follow one rule from Freedom on the Net scores
//     (China filters most, Russia some, the US none) - no nation-specific code;
//  2. protest abroad spreads to coupled publics (US -> Canada) far more than to a
//     public behind a filter (US -> China);
//  3. over two years rivals launch some cyber operations; money is conserved.

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

int main() {
    auto s = std::make_unique<game_state>();
    const auto report = sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    if(!report.ok()) {
        std::fprintf(stderr, "scenario: %s\n", report.error.c_str());
        return EXIT_FAILURE;
    }
    for(auto e : s->world.in_event) e.set_done(1);
    auto& w = s->world;
    const auto usa = sim::find_nation(*s, "USA"), chn = sim::find_nation(*s, "CHN"), rus = sim::find_nation(*s, "RUS"),
               can = sim::find_nation(*s, "CAN");

    // 1. Starting controls.
    std::printf("cross-border filtering: CHN %.2f RUS %.2f USA %.2f; information control: CHN %.2f USA %.2f\n", w.nation_get_net_filtering(chn),
                w.nation_get_net_filtering(rus), w.nation_get_net_filtering(usa), w.nation_get_info_control(chn), w.nation_get_info_control(usa));
    CHECK(w.nation_get_net_filtering(chn) > w.nation_get_net_filtering(rus));
    CHECK(w.nation_get_net_filtering(rus) > w.nation_get_net_filtering(usa));
    CHECK(w.nation_get_net_filtering(usa) == 0.0f);

    // 2. Contagion: a mass protest in the US.
    for(auto own : w.nation_get_region_ownership(usa)) own.get_region().set_protest(static_cast<float>(s->params.cascade_max));
    sim::update_internet(*s);
    std::printf("contagion drive from a US uprising: CAN %.4f, CHN %.4f\n", w.nation_get_net_contagion(can), w.nation_get_net_contagion(chn));
    CHECK(w.nation_get_net_contagion(can) > 5.0f * w.nation_get_net_contagion(chn));
    for(auto own : w.nation_get_region_ownership(usa)) own.get_region().set_protest(0.0f);

    // 3. Two years: cyber operations between rivals; conservation.
    const double money0 = sim::world_money(*s);
    for(int d = 0; d < 730; ++d) sim::advance_day(*s);
    int ops = 0;
    for(auto d : w.in_decision) ops += d.get_lever() == sim::journal_cyber ? 1 : 0;
    std::printf("cyber operations in two years: %d\n", ops);
    CHECK(ops > 0);
    CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);

    if(failures == 0) {
        std::puts("test_internet: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_internet: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
