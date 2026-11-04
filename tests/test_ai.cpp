// AI agents: anchors as priors, sovereignty blending, currency pegs, overrides,
// and the decision journal. Uses the 2020 slice assets.

#include "sim/model.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"

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

static std::unique_ptr<game_state> make() {
    auto s = std::make_unique<game_state>();
    (void)sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    return s;
}

static void run(game_state& s, int days) {
    for(int d = 0; d < days; ++d) {
        sim::advance_day(s);
    }
}

int main() {
    const auto c2 = [](game_state const& s) {
        for(auto k : s.world.in_npi_indicator) {
            if(k.get_key() == ('C' | ('2' << 8))) return static_cast<uint8_t>(k.id.index());
        }
        return uint8_t(0);
    };

    // 1. Strong prior: on 2020-04-15 the USA's workplace closing is close to the anchor.
    {
        auto s = make();
        s->params.anchor_prior_weight = 1000.0;
        run(*s, 106);
        const auto usa = sim::find_nation(*s, "USA");
        const auto k = c2(*s);
        CHECK(std::abs(s->world.nation_get_npi(usa, k) - s->world.nation_get_prior_npi(usa, k)) < 0.01f);
        // The journal recorded decisions with their reasons.
        int decisions = 0;
        for(auto d : s->world.in_decision) {
            decisions += d.get_target() == usa ? 1 : 0;
        }
        CHECK(decisions > 0);
    }

    // 2. Sovereignty with zero autonomy: the subject copies the sovereign's measures.
    {
        auto s = make();
        const auto usa = sim::find_nation(*s, "USA");
        const auto can = sim::find_nation(*s, "CAN");
        const auto rel = s->world.force_create_sovereignty(can, usa);
        s->world.sovereignty_set_autonomy(rel, 0.0f);
        run(*s, 100);
        for(uint8_t k = 0; k < s->derived.npi_count; ++k) {
            CHECK(std::abs(s->world.nation_get_npi(can, k) - s->world.nation_get_npi(usa, k)) < 1e-5f);
        }
    }

    // 3. Peg without anchors: Saudi Arabia's rate follows the US rate.
    {
        auto s = make();
        s->params.historical_anchors = 0.0;
        run(*s, 120);
        const auto usa = sim::find_nation(*s, "USA");
        const auto sau = sim::find_nation(*s, "SAU");
        CHECK(std::abs(s->world.nation_get_policy_rate(sau) - s->world.nation_get_policy_rate(usa)) < 0.5f);
    }

    // 4. Direct override: the player sets the lever, the AI's own choice is still logged.
    {
        auto s = make();
        const auto usa = sim::find_nation(*s, "USA");
        const auto exec = s->world.nation_get_agent_from_executive_seat(usa);
        const auto k = c2(*s);
        s->world.agent_set_override_mode(exec, k, static_cast<uint8_t>(sim::override_mode::direct));
        s->world.agent_set_override_value(exec, k, 3.0f);
        run(*s, 30);
        CHECK(s->world.nation_get_npi(usa, k) == 3.0f);
        CHECK(s->world.agent_get_last_ai_choice(exec, k) < 3.0f);
    }

    if(failures == 0) {
        std::puts("test_ai: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_ai: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
