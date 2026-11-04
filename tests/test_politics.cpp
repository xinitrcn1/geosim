// Politics and geopolitics on the 2020 slice:
//  - elections are held on their 2020 dates (USA 2020-11-03, KOR 2020-04-15) and logged;
//  - no war starts in 2020; status-quo sanctions persist (anchored);
//  - forced overrides cost legitimacy and raise the capture index;
//  - sanctions cut bilateral trade access.

#include "core/calendar.hpp"
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
    (void)sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    auto& w = s->world;
    const auto usa = sim::find_nation(*s, "USA");
    const auto chn = sim::find_nation(*s, "CHN");
    const auto kor = sim::find_nation(*s, "KOR");
    const float sanction0 = w.nation_get_sanction(usa, static_cast<uint8_t>(chn.index()));
    for(int d = 0; d < 366; ++d) sim::advance_day(*s);

    int us_elections = 0, kor_elections = 0, wars = 0;
    for(auto dec : w.in_decision) {
        const auto lever = dec.get_lever();
        if(lever == sim::journal_election) {
            const auto date = core::date_from_tick(dec.get_tick());
            if(dec.get_target() == usa) {
                ++us_elections;
                std::printf("USA election %s: governing side %d -> %d (0 L, 1 C, 2 R)\n", core::format_date(date).c_str(),
                            static_cast<int>(dec.get_before()), static_cast<int>(dec.get_after()));
                CHECK(date.month == 11);
            }
            if(dec.get_target() == kor) {
                ++kor_elections;
                std::printf("KOR election %s: governing side %d -> %d\n", core::format_date(date).c_str(),
                            static_cast<int>(dec.get_before()), static_cast<int>(dec.get_after()));
            }
        }
        if(lever == sim::journal_war) ++wars;
    }
    CHECK(us_elections == 1);
    CHECK(kor_elections == 1);
    CHECK(wars == 0);
    CHECK(std::abs(w.nation_get_sanction(usa, static_cast<uint8_t>(chn.index())) - sanction0) < 0.1f);
    for(auto n : w.in_nation) {
        CHECK(std::isfinite(n.get_legitimacy()) && n.get_legitimacy() >= 0.0f && n.get_legitimacy() <= 1.0f);
        CHECK(std::isfinite(n.get_approval()));
    }

    // Forced overrides cost legitimacy.
    {
        auto a = std::make_unique<game_state>();
        auto b = std::make_unique<game_state>();
        (void)sim::build_scenario(*a, GEOSIM_ASSETS_DIR, "2020");
        (void)sim::build_scenario(*b, GEOSIM_ASSETS_DIR, "2020");
        const auto fra = sim::find_nation(*b, "FRA");
        const auto exec = b->world.nation_get_agent_from_executive_seat(fra);
        for(uint8_t k = 0; k < b->derived.npi_count; ++k) {
            b->world.agent_set_override_mode(exec, k, static_cast<uint8_t>(sim::override_mode::force));
            b->world.agent_set_override_value(exec, k, b->world.npi_indicator_get_max_level(dcon::npi_indicator_id{k}));
        }
        for(int d = 0; d < 180; ++d) {
            sim::advance_day(*a);
            sim::advance_day(*b);
        }
        std::printf("FRA legitimacy: normal %.3f, forced %.3f; capture %.3f\n", a->world.nation_get_legitimacy(fra),
                    b->world.nation_get_legitimacy(fra), b->world.nation_get_capture_index(fra));
        CHECK(b->world.nation_get_legitimacy(fra) < a->world.nation_get_legitimacy(fra) - 0.05f);
        CHECK(b->world.nation_get_capture_index(fra) > 0.0f);
    }

    // Sanctions cut trade access.
    {
        auto c = std::make_unique<game_state>();
        (void)sim::build_scenario(*c, GEOSIM_ASSETS_DIR, "2020");
        const auto deu = sim::find_nation(*c, "DEU");
        const float before = sim::trade_access(*c, deu);
        c->world.nation_set_sanction(deu, static_cast<uint8_t>(sim::find_nation(*c, "CHN").index()), 1.0f);
        CHECK(sim::trade_access(*c, deu) < before);
    }

    if(failures == 0) {
        std::puts("test_politics: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_politics: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
