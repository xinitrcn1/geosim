// Identity groups and collective action on the 2020 slice:
//  1. India's groups start with the observed ordering of income (forward castes
//     above OBC above SC/ST), and population totals match the nation;
//  2. in the 2020 lockdown, groups with more informal work lose more income
//     relative to the national mean (Deshpande & Ramachandran 2020);
//  3. a no-shock year stays calm everywhere (no spontaneous revolutions);
//  4. a sudden, total restriction of online media in a young, connected nation
//     under strain backfires (protests surge) far more than the same restriction
//     phased in slowly (Hobbs & Roberts 2018) - forced through the player override.

#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/report.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/systems.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

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

std::map<std::string, dcon::social_group_id> groups_of(game_state const& s, dcon::nation_id n) {
    std::map<std::string, dcon::social_group_id> out;
    for(auto g : s.world.in_social_group) {
        if(g.get_nation() == n) out[core::unpack_key(g.get_key())] = g.id;
    }
    return out;
}

float mean_unrest(game_state const& s, dcon::nation_id n) {
    float u = 0.0f, p = 0.0f;
    for(auto own : s.world.nation_get_region_ownership(n)) {
        u += own.get_region().get_unrest() * own.get_region().get_population();
        p += own.get_region().get_population();
    }
    return p > 0.0f ? u / p : 0.0f;
}

} // namespace

int main() {
    // 1. India's groups at the start.
    {
        auto s = make(false);
        const auto ind = sim::find_nation(*s, "IND");
        auto g = groups_of(*s, ind);
        CHECK(g.size() == 5);
        if(g.size() == 5) {
            const float fwd = s->world.social_group_get_income_rel(g["FWD"]);
            const float obc = s->world.social_group_get_income_rel(g["OBC"]);
            const float sc = s->world.social_group_get_income_rel(g["SC"]);
            const float st = s->world.social_group_get_income_rel(g["ST"]);
            std::printf("IND income per head vs national mean: FWD %.2f OBC %.2f SC %.2f ST %.2f\n", fwd, obc, sc, st);
            CHECK(fwd > obc && obc > sc && sc > 0.0f && st < obc);
        }
        // The skill split (RAS) keeps each group's share of the labour force and retirees.
        double total = 0.0, sc_size = 0.0;
        for(auto p : s->world.in_pop) {
            if(p.get_nation() != ind) continue;
            total += p.get_size();
            if(g.size() == 5 && p.get_group() == g["SC"]) sc_size += p.get_size();
        }
        std::printf("IND Scheduled Castes: %.3f of pops (data 0.17)\n", sc_size / total);
        CHECK(std::abs(sc_size / total - 0.17) < 0.01);
    }

    // 2. The 2020 lockdown hits informal groups harder.
    {
        auto s = make(true);
        const auto ind = sim::find_nation(*s, "IND");
        auto g = groups_of(*s, ind);
        const float fwd0 = s->world.social_group_get_income_rel(g["FWD"]);
        const float sc0 = s->world.social_group_get_income_rel(g["SC"]);
        float worst_gap = 0.0f;
        for(int d = 0; d < 200; ++d) {
            sim::advance_day(*s);
            const float ratio = (s->world.social_group_get_income_rel(g["SC"]) / s->world.social_group_get_income_rel(g["FWD"])) / (sc0 / fwd0);
            worst_gap = std::max(worst_gap, 1.0f - ratio);
        }
        std::printf("IND 2020: SC income relative to forward castes fell by up to %.1f%%\n", worst_gap * 100.0f);
        CHECK(worst_gap > 0.01f);
    }

    // 3. A calm no-shock year.
    {
        auto s = make(false);
        float peak = 0.0f;
        for(int d = 0; d < 366; ++d) {
            sim::advance_day(*s);
            for(auto n : s->world.in_nation) peak = std::max(peak, mean_unrest(*s, n.id));
        }
        std::printf("no-shock year: highest national unrest %.2f (ladder 0-4)\n", peak);
        CHECK(peak < 3.0f);
    }

    // 4. Sudden vs gradual information control in a young, connected nation under strain.
    {
        auto run = [](bool sudden) {
            auto s = make(false);
            const auto idn = sim::find_nation(*s, "IDN");
            s->world.nation_set_youth_share(idn, 0.45f);
            s->world.nation_set_connectivity(idn, 0.9f);
            s->params.spark_rate = 0.0;
            const auto exec = s->world.nation_get_agent_from_executive_seat(idn);
            const auto lev = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::info_control));
            float peak = 0.0f;
            for(int d = 0; d < 120; ++d) {
                // Strain: a standing grievance drive (as after a youth-unemployment shock).
                for(auto own : s->world.nation_get_region_ownership(idn)) own.get_region().set_drive(0.3f);
                if(d >= 30) {
                    const float level = sudden ? 1.0f : std::min(1.0f, static_cast<float>(d - 30) / 90.0f);
                    s->world.agent_set_override_mode(exec, lev, static_cast<uint8_t>(sim::override_mode::force));
                    s->world.agent_set_override_value(exec, lev, level);
                }
                sim::advance_day(*s);
                if(d >= 30) peak = std::max(peak, mean_unrest(*s, idn));
            }
            return peak;
        };
        const float sudden = run(true);
        const float gradual = run(false);
        std::printf("information control in a young connected nation: peak unrest sudden %.2f, gradual %.2f\n", sudden, gradual);
        CHECK(sudden > gradual + 0.5f);
        CHECK(sudden >= 2.5f);
    }

    if(failures == 0) {
        std::puts("test_society_groups: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_society_groups: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
