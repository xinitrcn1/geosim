// Policy catalogue and electoral systems on the 2020 slice:
//  1. list PR with a legal threshold (Turkey, 10%): parties below it win no seats;
//  2. single-member districts (UK): the largest party wins a seat bonus (cube law);
//  3. coalition cabinets (Germany): portfolios go to more than one governing party;
//  4. repression forced on: it is applied and leaves a memory that feeds backlash;
//  5. later retirement lowers the pension transfer rate.

#include "sim/model.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/systems.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>

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
    return s;
}

void election_now(game_state& s, dcon::nation_id n) {
    sim::advance_day(s); // elections need a positive date
    s.world.nation_set_next_election(n, static_cast<int32_t>(s.current_tick));
    sim::advance_day(s);
}
} // namespace

int main() {
    // 1. Threshold PR.
    {
        auto s = make();
        auto& w = s->world;
        const auto tur = sim::find_nation(*s, "TUR");
        CHECK(w.nation_get_electoral_system(tur) == static_cast<uint8_t>(sim::electoral_system::pr));
        CHECK(std::abs(w.nation_get_pr_threshold(tur) - 0.10f) < 1e-4f);
        election_now(*s, tur);
        float total = 0.0f;
        int below = 0;
        for(auto m : w.nation_get_party_membership(tur)) {
            auto p = m.get_party();
            total += p.get_seat_share();
            if(p.get_vote_share() < 0.10f) {
                ++below;
                CHECK(p.get_seat_share() == 0.0f);
            }
        }
        CHECK(below > 0);
        CHECK(std::abs(total - 1.0f) < 1e-3f);
    }
    // 2. Single-member districts.
    {
        auto s = make();
        auto& w = s->world;
        const auto gbr = sim::find_nation(*s, "GBR");
        CHECK(w.nation_get_electoral_system(gbr) == static_cast<uint8_t>(sim::electoral_system::fptp));
        election_now(*s, gbr);
        dcon::party_id largest;
        for(auto m : w.nation_get_party_membership(gbr)) {
            if(!largest || m.get_party().get_vote_share() > w.party_get_vote_share(largest)) largest = m.get_party().id;
        }
        CHECK(w.party_get_seat_share(largest) > w.party_get_vote_share(largest));
    }
    // 3. Coalition cabinet.
    {
        auto s = make();
        auto& w = s->world;
        const auto deu = sim::find_nation(*s, "DEU");
        std::set<uint32_t> parties;
        for(uint8_t k = 0; k < static_cast<uint8_t>(sim::portfolio::count); ++k) {
            const auto p = w.nation_get_portfolio_party(deu, k);
            CHECK(bool(p));
            if(p) {
                CHECK(w.party_get_in_government(p) != 0);
                parties.insert(p.id.index());
            }
        }
        CHECK(parties.size() >= 2);
    }
    // 4. Repression.
    {
        auto s = make();
        auto& w = s->world;
        const auto rus = sim::find_nation(*s, "RUS");
        const auto exec = w.nation_get_agent_from_executive_seat(rus);
        const uint8_t lev = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::repression));
        w.agent_set_override_mode(exec, lev, static_cast<uint8_t>(sim::override_mode::force));
        w.agent_set_override_value(exec, lev, 1.0f);
        for(int d = 0; d < 60; ++d) sim::advance_day(*s);
        CHECK(w.nation_get_repression(rus) > 0.5f);
        CHECK(w.nation_get_repression_memory(rus) > 0.0f);
    }
    // 5. Retirement age.
    {
        auto base = make();
        auto later = make();
        const auto jpn = sim::find_nation(*base, "JPN");
        later->world.nation_set_retirement_age_shift(jpn, 5.0f);
        for(int d = 0; d < 7; ++d) {
            later->world.nation_set_retirement_age_shift(jpn, 5.0f);
            sim::advance_day(*base);
            sim::advance_day(*later);
        }
        CHECK(later->world.nation_get_rate_transfer(jpn) < base->world.nation_get_rate_transfer(jpn));
    }
    if(failures == 0) std::printf("test_policy: all checks passed\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
