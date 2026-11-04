// Strategy, reputation and war on the 2020 slice:
//  1. CINC shares sum to one;
//  2. full media freedom and state capacity mean no misperception;
//  3. an insecure executive adopts the regime-security goal;
//  4. an ally that declines a call to war loses credibility and the victim's trust;
//  5. Lanchester fronts: a much stronger neighbour advances into a weaker one, and
//     occupation cuts the defender's capacity.

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
    return s;
}
} // namespace

int main() {
    // 1, 2.
    {
        auto s = make();
        auto& w = s->world;
        double total = 0.0;
        for(auto n : w.in_nation) total += n.get_cinc();
        CHECK(std::abs(total - 1.0) < 1e-3);
        const auto deu = sim::find_nation(*s, "DEU");
        w.nation_set_media_freedom(deu, 1.0f);
        w.nation_set_state_capacity(deu, 1.0f);
        CHECK(std::abs(sim::perception_noise(*s, deu, 0) - 1.0f) < 1e-6f);
        const auto chn = sim::find_nation(*s, "CHN");
        w.nation_set_media_freedom(chn, 0.0f);
        CHECK(sim::perception_noise(*s, chn, 0) != 1.0f);
    }
    // 3. Regime security.
    {
        auto s = make();
        auto& w = s->world;
        const auto jpn = sim::find_nation(*s, "JPN");
        for(int d = 0; d < 30; ++d) {
            w.nation_set_legitimacy(jpn, 0.01f);
            sim::advance_day(*s);
        }
        const auto exec = w.nation_get_agent_from_executive_seat(jpn);
        CHECK(w.agent_get_goal(exec) == static_cast<uint8_t>(sim::goal::regime_security));
        CHECK(sim::rigidity(*s, exec, jpn, 0.0f) < w.agent_get_rho_base(exec));
    }
    // 4. Alliance call declined.
    {
        auto s = make();
        auto& w = s->world;
        s->params.alliance_honor = 0.0;
        const auto usa = sim::find_nation(*s, "USA"), chn = sim::find_nation(*s, "CHN");
        dcon::nation_id ally;
        for(auto a : w.in_nation) {
            if(a.id != usa && a.get_alliance(static_cast<uint16_t>(usa.index())) > 0.0f) ally = a.id;
        }
        CHECK(bool(ally));
        if(ally) {
            const float trust0 = w.nation_get_trust(usa, static_cast<uint16_t>(ally.index()));
            sim::call_allies(*s, usa, chn);
            CHECK(w.nation_get_credibility(ally) < 1.0f);
            CHECK(w.nation_get_at_war(ally, static_cast<uint16_t>(chn.index())) == 0.0f);
            CHECK(w.nation_get_trust(usa, static_cast<uint16_t>(ally.index())) < trust0);
        }
    }
    // 5. Fronts.
    {
        auto s = make();
        auto& w = s->world;
        const auto usa = sim::find_nation(*s, "USA"), mex = sim::find_nation(*s, "MEX");
        const auto ku = static_cast<uint16_t>(usa.index()), km = static_cast<uint16_t>(mex.index());
        w.nation_set_at_war(usa, km, 1.0f);
        w.nation_set_at_war(mex, ku, 1.0f);
        w.nation_set_war_effort(usa, 1.0f);
        w.nation_set_war_effort(mex, 1.0f);
        for(int i = 0; i < 6; ++i) sim::update_front(*s, usa, mex, 28.0f / 365.0f);
        CHECK(w.nation_get_control(mex, ku) > 0.0f);
        CHECK(w.nation_get_control(usa, km) == 0.0f);
        CHECK(w.nation_get_occupied(mex) > 0.0f);
    }
    if(failures == 0) std::printf("test_strategy: all checks passed\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
