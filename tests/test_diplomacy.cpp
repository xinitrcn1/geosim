// Disputes and diplomacy on the 2020 slice:
//  1. with escalation forced up and no exits, a non-nuclear pair reaches war while a
//     nuclear pair (stability-instability paradox) almost never takes the last step;
//  2. a democracy that backs down after escalating pays an audience cost;
//  3. a sanctioned dependent target concedes and the sender lifts its sanctions;
//  4. procurement above baseline buys arms abroad (supplier arms exports).

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
    s->params.war_base_rate = 0.0; // no spontaneous disputes
    return s;
}
uint16_t ix(dcon::nation_id n) { return static_cast<uint16_t>(n.index()); }
} // namespace

int main() {
    // 1. Escalation and nuclear deterrence.
    {
        auto s = make();
        auto& w = s->world;
        s->params.escalate_rate = 60.0;
        s->params.backdown_rate = 0.0;
        s->params.dispute_settle_rate = 0.0;
        const auto bra = sim::find_nation(*s, "BRA"), arg = sim::find_nation(*s, "ARG");
        const auto usa = sim::find_nation(*s, "USA"), rus = sim::find_nation(*s, "RUS");
        sim::start_dispute(*s, bra, arg);
        sim::start_dispute(*s, usa, rus);
        bool war = false;
        for(int d = 0; d < 365; ++d) {
            sim::advance_day(*s);
            war = war || w.nation_get_at_war(bra, ix(arg)) > 0.0f;
        }
        CHECK(war);
        CHECK(w.nation_get_at_war(usa, ix(rus)) == 0.0f);
        CHECK(w.nation_get_hostility(usa, ix(rus)) >= 4.0f);
        CHECK(w.nation_get_rivalry(bra, ix(arg)) > 0.0f);
    }
    // 2. Audience costs.
    {
        auto s = make();
        auto& w = s->world;
        s->params.escalate_rate = 0.0;
        s->params.backdown_rate = 500.0;
        s->params.dispute_settle_rate = 0.0;
        const auto deu = sim::find_nation(*s, "DEU"), chn = sim::find_nation(*s, "CHN");
        sim::start_dispute(*s, deu, chn);
        w.nation_set_hostility(deu, ix(chn), 4.0f);
        w.nation_set_hostility(chn, ix(deu), 4.0f);
        const float legit0 = w.nation_get_legitimacy(deu);
        for(int d = 0; d < 28; ++d) sim::advance_day(*s);
        CHECK(w.nation_get_hostility(deu, ix(chn)) == 0.0f);
        // Germany (weaker by CINC) backs down and loses legitimacy beyond its monthly drift.
        CHECK(w.nation_get_legitimacy(deu) < legit0 - 0.01f);
    }
    // 3. Sanction concession.
    {
        auto s = make();
        auto& w = s->world;
        s->params.concede_rate = 1000.0;
        s->params.historical_anchors = 1.0;
        s->params.anchor_prior_weight = 10.0;
        const auto usa = sim::find_nation(*s, "USA"), mex = sim::find_nation(*s, "MEX");
        int conceded = 0;
        for(int d = 0; d < 200; ++d) { // a divided legislature ratifies one step a month
            w.nation_set_sanction_prior(usa, ix(mex), 1.0f); // the sender's standing policy
            sim::advance_day(*s);
        }
        for(auto dd : w.in_decision) conceded += dd.get_lever() == sim::journal_concession ? 1 : 0;
        CHECK(conceded > 0);
    }
    // 4. Arms imports.
    {
        auto s = make();
        auto& w = s->world;
        const auto pol = sim::find_nation(*s, "SAU");
        for(int d = 0; d < 56; ++d) {
            w.nation_set_defense_share(pol, w.nation_get_defense_share0(pol) + 3.0f);
            sim::advance_day(*s);
        }
        float exports = 0.0f;
        for(auto n : w.in_nation) exports += n.get_arms_exports();
        CHECK(exports > 0.0f);
    }
    if(failures == 0) std::printf("test_diplomacy: all checks passed\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
