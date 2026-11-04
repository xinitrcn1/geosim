// Long-campaign detail on the 2020 slice (no events):
//  1. technology diffusion: by 2030 EVs cut US oil use per unit of activity more
//     than India's (adoption lags with the income and skills gap), and a nation
//     that filters the internet adopts later;
//  2. strata: a jobless shock splits a precarious stratum off a worker pop; once
//     its members are back in work it merges again; money is conserved;
//  3. spatial LOD: forced low detail keeps the no-shock baseline steady, and a
//     player override promotes the nation back to full detail;
//  4. forks: a reduced-detail fork and a threaded fork both run.

#include "sim/fork.hpp"
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
} // namespace

int main() {
    // 1. Technology diffusion (technology system alone, 10 years).
    {
        auto s = make();
        const auto usa = sim::find_nation(*s, "USA"), ind = sim::find_nation(*s, "IND"), chn = sim::find_nation(*s, "CHN");
        auto filtered = make();
        const auto chn_f = sim::find_nation(*filtered, "CHN");
        filtered->world.nation_set_net_filtering(chn_f, 1.0f);
        for(int d = 0; d < 3650; ++d) {
            sim::update_technology(*s);
            sim::update_technology(*filtered);
            ++s->current_tick;
            ++filtered->current_tick;
        }
        const float us = sim::tech_oil_factor(*s, usa), in = sim::tech_oil_factor(*s, ind);
        const float cn = sim::tech_oil_factor(*s, chn), cn_f = sim::tech_oil_factor(*filtered, chn_f);
        std::printf("2030 oil use per unit of activity from EVs: USA %.3f, IND %.3f; CHN %.3f open vs %.3f fully filtered\n", us, in, cn, cn_f);
        CHECK(us < 1.0f && us < in);
        CHECK(cn < cn_f);
    }

    // 2. Strata split and merge.
    {
        auto s = make();
        auto& w = s->world;
        const auto usa = sim::find_nation(*s, "USA");
        dcon::pop_id low;
        for(auto p : w.in_pop) {
            if(p.get_nation() == usa && p.get_kind() == 0 && !low) low = p.id;
        }
        const double money0 = sim::world_money(*s);
        double size0 = 0.0;
        for(auto p : w.in_pop) size0 += p.get_size();
        // Shock: 40% of the pop's workers lose their jobs (and their job slots stay).
        for(uint8_t sct = 0; sct < s->derived.sector_count; ++sct) {
            const float lost = w.pop_get_employed(low, sct) * 0.4f;
            w.pop_set_employed(low, sct, w.pop_get_employed(low, sct) - lost);
            w.pop_set_unemployed(low, w.pop_get_unemployed(low) + lost);
            w.pop_set_emp0(low, sct, w.pop_get_emp0(low, sct) * 0.6f); // the slots are gone too
        }
        int strata = 0;
        for(int d = 0; d < 28; ++d) {
            sim::update_strata(*s);
            ++s->current_tick;
        }
        for(auto p : w.in_pop) strata += p.get_stratum() == static_cast<uint8_t>(sim::stratum::precarious) ? 1 : 0;
        double size1 = 0.0;
        for(auto p : w.in_pop) size1 += p.get_size();
        std::printf("after a 40%% job loss: %d precarious stratum(s); people %.0f -> %.0f\n", strata, size0, size1);
        CHECK(strata == 1);
        CHECK(std::abs(size1 - size0) <= 1e-6 * size0);
        // Recovery: the stratum's members find work again -> it merges back.
        for(auto p : w.in_pop) {
            if(p.get_stratum() != static_cast<uint8_t>(sim::stratum::precarious)) continue;
            const float jobless = p.get_unemployed() + p.get_inactive();
            p.set_unemployed(0.0f);
            p.set_inactive(0.0f);
            p.set_employed(0, p.get_employed(0) + jobless);
        }
        w.pop_set_unemployed(low, 0.0f);
        w.pop_set_inactive(low, 0.0f);
        for(int d = 0; d < 28; ++d) {
            sim::update_strata(*s);
            ++s->current_tick;
        }
        strata = 0;
        for(auto p : w.in_pop) strata += p.get_stratum() == static_cast<uint8_t>(sim::stratum::precarious) ? 1 : 0;
        std::printf("after recovery: %d precarious stratum(s)\n", strata);
        CHECK(strata == 0);
        CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);
    }

    // 3. Spatial LOD.
    {
        auto s = make();
        s->params.lod_force = 1.0;
        const double money0 = sim::world_money(*s);
        for(int d = 0; d < 180; ++d) sim::advance_day(*s);
        int worst = 0;
        float worst_gdp = 1.0f;
        for(auto n : s->world.in_nation) {
            const float g = n.get_gdp_real() / (n.get_gdp0_daily() * n.get_gf());
            if(std::abs(g - 1.0f) > std::abs(worst_gdp - 1.0f)) worst_gdp = g;
            worst += n.get_lod() ? 1 : 0;
        }
        std::printf("low detail: %d nations at monthly economy steps, worst GDP index after 180 days %.4f\n", worst, worst_gdp);
        CHECK(std::abs(worst_gdp - 1.0f) < 0.05f);
        CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);
        // Natural LOD with a promotion on an override.
        s->params.lod_force = 0.0;
        s->params.lod_min_nations = 0.0;
        s->params.lod_calm_days = 0.0;
        sim::update_lod(*s);
        const auto usa = sim::find_nation(*s, "USA");
        const auto exec = s->world.nation_get_agent_from_executive_seat(usa);
        s->world.agent_set_override_mode(exec, 0, static_cast<uint8_t>(sim::override_mode::advise));
        sim::update_lod(*s);
        CHECK(s->world.nation_get_lod(usa) == 0);
    }

    // 4. Forks.
    {
        auto s = make();
        const auto reduced = sim::run_fork(*s, 90, {}, sim::fork_options{true});
        sim::fork_job job(*s, 90, {});
        for(int d = 0; d < 10; ++d) sim::advance_day(*s); // the live state keeps running
        const auto& threaded = job.wait();
        CHECK(reduced.error.empty() && threaded.error.empty());
        CHECK(reduced.nations.size() == s->world.nation_size() && threaded.nations[0].gdp_index.size() == 90u);
        std::printf("forks: reduced-detail US GDP index after 90 days %.4f, full-detail threaded %.4f\n", reduced.nations[0].gdp_index.back(),
                    threaded.nations[0].gdp_index.back());
    }

    if(failures == 0) {
        std::puts("test_longcampaign: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_longcampaign: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
