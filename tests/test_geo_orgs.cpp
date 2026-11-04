// Organisations with geopolitical weight on the 2020 slice (geo_orgs.cpp, megaevents.cpp):
//  1. NATO members are allied; a rising threat pushes members below the guideline to spend;
//  2. clubs: G7 members align their sanctions, BRICS/SCO members shield each other;
//  3. OPEC+ cuts its quota when the price falls below the band;
//  4. a coup suspends a member of a democracy-clause organisation (and lifts its deterrence);
//  5. Tokyo 2020 is postponed under a grave epidemic threat, and otherwise held: visitors'
//     spending, the end of construction, and trust in the host afterwards.

#include "core/calendar.hpp"
#include "core/keys.hpp"
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
dcon::organisation_id org(game_state const& s, char const* key) {
    for(auto o : s.world.in_organisation) {
        if(o.get_key() == core::pack_key(key)) return o.id;
    }
    return {};
}
dcon::mega_event_id tokyo(game_state const& s) {
    const auto jpn = sim::find_nation(s, "JPN");
    for(auto e : s.world.in_mega_event) {
        if(e.get_host() == jpn) return e.id;
    }
    return {};
}
} // namespace

int main() {
    // 1. Alliances.
    {
        auto s = make();
        auto& w = s->world;
        const auto usa = sim::find_nation(*s, "USA"), deu = sim::find_nation(*s, "DEU"), rus = sim::find_nation(*s, "RUS");
        CHECK(w.nation_get_alliance(usa, ix(deu)) == 1.0f);
        CHECK(w.nation_get_alliance(deu, ix(usa)) == 1.0f);
        CHECK(sim::alliance_burden_push(*s, deu) == 0.0f); // no rise in threat at the start
        w.nation_set_threat_to(deu, ix(rus), w.nation_get_threat_max0(deu) + 0.3f);
        const float push = sim::alliance_burden_push(*s, deu);
        std::printf("burden push DEU (defence %.2f%% of GDP): %.3f pp\n", w.nation_get_defense_share(deu), push);
        CHECK(push > 0.0f);
    }
    // 2. Clubs.
    {
        auto s = make();
        auto& w = s->world;
        const auto chn = sim::find_nation(*s, "CHN"), rus = sim::find_nation(*s, "RUS"), jpn = sim::find_nation(*s, "JPN");
        CHECK(sim::club_sanction_shift(*s, chn, rus, 0.8f) < 0.8f);
        for(char const* iso : {"USA", "GBR", "FRA", "DEU", "ITA", "CAN"}) w.nation_set_sanction(sim::find_nation(*s, iso), ix(rus), 1.0f);
        const float aligned = sim::club_sanction_shift(*s, jpn, rus, 0.0f);
        std::printf("JPN sanction on RUS with G7 partners at 1: %.2f\n", aligned);
        CHECK(aligned > 0.3f);
    }
    // 3. Cartel.
    {
        auto s = make();
        auto& w = s->world;
        const auto oil = s->derived.oil;
        const auto sau = sim::find_nation(*s, "SAU"), mex = sim::find_nation(*s, "MEX");
        CHECK(w.nation_get_cartel_quota(sau) == 1.0f);
        for(int d = 0; d < 60; ++d) {
            w.commodity_set_price(oil, 0.6f * w.commodity_get_price0(oil));
            sim::advance_day(*s);
        }
        std::printf("quotas after 60 days at 60%% of the 2019 price: SAU %.3f, MEX %.3f, OPEC+ %.3f\n", w.nation_get_cartel_quota(sau),
                    w.nation_get_cartel_quota(mex), w.organisation_get_quota(org(*s, "opec")));
        CHECK(w.organisation_get_quota(org(*s, "opec")) < 1.0f);
        CHECK(w.nation_get_cartel_quota(sau) < 1.0f || w.organisation_get_price_war_days(org(*s, "opec")) != 0.0f);
    }
    // 4. Coup in a democracy-clause organisation.
    {
        auto s = make();
        const auto bra = sim::find_nation(*s, "BRA");
        CHECK(sim::coup_deterrence(*s, bra) > 0.0f);
        CHECK(sim::is_member(*s, sim::org_kind::regional, bra));
        sim::on_coup(*s, bra);
        CHECK(!sim::is_member(*s, sim::org_kind::regional, bra));
        CHECK(sim::coup_deterrence(*s, bra) == 0.0f);
        CHECK(sim::is_member(*s, sim::org_kind::club, bra)); // BRICS has no democracy clause
    }
    // 5. Tokyo 2020.
    {
        auto s = make();
        auto& w = s->world;
        const auto jpn = sim::find_nation(*s, "JPN");
        const auto e = tokyo(*s);
        CHECK(bool(e));
        const int32_t start = w.mega_event_get_start(e);
        while(s->current_tick < start - 80) sim::advance_day(*s);
        w.nation_set_threat(jpn, 1.0f); // as the epidemic system would set it
        sim::update_mega_events(*s);
        CHECK(w.mega_event_get_start(e) == start + 365);
    }
    {
        auto s = make();
        auto& w = s->world;
        const auto jpn = sim::find_nation(*s, "JPN"), usa = sim::find_nation(*s, "USA");
        const auto e = tokyo(*s);
        const int32_t start = w.mega_event_get_start(e), finish = w.mega_event_get_finish(e);
        CHECK(w.nation_get_mega_build_share0(jpn) > 0.0f);
        float exports = 0.0f, glow = 0.0f;
        while(s->current_tick <= finish + 2) {
            w.nation_set_threat(jpn, 0.0f);
            sim::advance_day(*s);
            if(s->current_tick == start + 3) {
                exports = w.nation_get_mega_exports(jpn);
                glow = w.nation_get_mega_glow(jpn);
            }
        }
        const float trust0 = w.nation_get_trust(usa, ix(jpn));
        std::printf("Tokyo: visitor exports %.3f%% of GDP/yr, build-up end %.3f%% of GDP, trust USA->JPN %.3f\n", exports,
                    w.nation_get_mega_investment(jpn), trust0);
        CHECK(w.mega_event_get_start(e) == start);
        CHECK(exports > 0.0f);
        CHECK(glow > 0.0f);
        CHECK(w.nation_get_mega_investment(jpn) < 0.0f); // the 2019 build-up has ended
        CHECK((w.mega_event_get_status(e) & 0x7F) == 2);
    }
    if(failures == 0) std::printf("test_geo_orgs: ok\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
