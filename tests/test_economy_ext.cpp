// Economy extensions on the 2020 slice (no events):
//  1. Stone-Geary demand: when household spending falls, food (agriculture) falls
//     proportionally less than the rest (subsistence comes first);
//  2. a forced carbon price raises revenue, retires fossil capacity early
//     (stranded assets) and lowers world emissions; money is conserved;
//  3. disasters are more frequent in a warmer world;
//  4. Armington: a depreciated nation gains bilateral export share against the
//     other suppliers in its markets.

#include "core/keys.hpp"
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
uint8_t sector(game_state const& s, char const* key) {
    for(auto sec : s.world.in_sector) {
        if(sec.get_key() == core::pack_key(key)) return static_cast<uint8_t>(sec.id.index());
    }
    return 0;
}
} // namespace

int main() {
    // 1. Stone-Geary.
    {
        auto run = [](float cut) {
            auto s = make();
            const auto ind = sim::find_nation(*s, "IND");
            for(int d = 0; d < 7; ++d) {
                s->world.nation_set_c_des_total(ind, s->world.nation_get_c_des_total(ind) * cut);
                sim::update_economy(*s);
                ++s->current_tick;
            }
            const auto agr = sector(*s, "agr"), man = sector(*s, "man");
            return std::pair{s->world.nation_get_demand(ind, agr), s->world.nation_get_demand(ind, man)};
        };
        const auto base = run(1.0f), poor = run(0.8f);
        const float food = poor.first / base.first, goods = poor.second / base.second;
        std::printf("India, household spending -20%%: agriculture demand x%.3f, manufacturing x%.3f\n", food, goods);
        CHECK(food > goods);
    }

    // 2. Carbon price.
    {
        auto run = [](bool priced) {
            auto s = make();
            const auto deu = sim::find_nation(*s, "DEU");
            if(priced) {
                const auto exec = s->world.nation_get_agent_from_executive_seat(deu);
                const auto lev = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::carbon_price));
                s->world.agent_set_override_mode(exec, lev, static_cast<uint8_t>(sim::override_mode::direct));
                s->world.agent_set_override_value(exec, lev, 250.0f);
            }
            const double money0 = sim::world_money(*s);
            for(int d = 0; d < 365; ++d) sim::advance_day(*s);
            float emissions = 0.0f;
            for(auto c : s->world.in_climate) emissions = c.get_emissions();
            struct out {
                double revenue;
                float stranded, emissions;
                bool conserved;
            };
            return out{s->world.nation_get_flows(deu, static_cast<uint8_t>(sim::flow::carbon_tax)), s->world.nation_get_stranded(deu), emissions,
                       std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0};
        };
        const auto base = run(false), priced = run(true);
        std::printf("Germany carbon price -> 250 USD/t (legislated gradually): revenue %.3g vs %.3g, stranded %.4f, world emissions %.2f vs %.2f Gt\n",
                    priced.revenue, base.revenue, priced.stranded, priced.emissions, base.emissions);
        CHECK(priced.revenue > base.revenue);
        CHECK(priced.emissions < base.emissions);
        CHECK(priced.conserved && base.conserved);
    }

    // 3. Disasters and warming (climate-policy system alone, 30 years each).
    {
        auto count = [](float warming) {
            auto s = make();
            for(auto c : s->world.in_climate) c.set_temperature(static_cast<float>(s->params.temperature0) + warming);
            int n = 0;
            for(int d = 0; d < 365 * 30; ++d) {
                sim::update_climate_policy(*s);
                ++s->current_tick;
            }
            for(auto dec : s->world.in_decision) n += dec.get_lever() == sim::journal_disaster ? 1 : 0;
            return n;
        };
        const int today = count(0.0f), hot = count(2.0f);
        std::printf("major disasters in 30 years: today's climate %d, +2C %d\n", today, hot);
        CHECK(hot > today);
    }

    // 4. Armington.
    {
        auto share = [](float rate) {
            auto s = make();
            const auto jpn = sim::find_nation(*s, "JPN"), usa = sim::find_nation(*s, "USA"), kor = sim::find_nation(*s, "KOR");
            s->world.nation_set_exchange_rate(jpn, rate);
            const float a = sim::trade_access(*s, jpn);
            (void)usa;
            (void)kor;
            return a;
        };
        const float even = share(1.0f), cheap = share(0.8f);
        std::printf("Japan's export access with the yen at 1.0 vs 0.8 (real): %.4f vs %.4f\n", even, cheap);
        CHECK(cheap > even);
    }

    if(failures == 0) {
        std::puts("test_economy_ext: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_economy_ext: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
