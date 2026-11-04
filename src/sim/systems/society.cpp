// Society (docs/04-society.md): demography, migration, values and opinion, unrest.
//
// All four run on 28-day cadences (regions or nations sharded by index). They
// move stocks between pops and regions along irregular graphs, so they are
// scalar passes (the SIMD carve-out); every money movement still goes through
// sim::transfer, so the ledger stays exact.

#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

struct region_pops {
    std::array<dcon::pop_id, 4> by_kind{}; // indexed by pop_kind
};

region_pops pops_in(game_state const& state, dcon::region_id r) {
    region_pops out;
    for(auto loc : state.world.region_get_pop_location(r)) {
        auto p = loc.get_pop();
        if(p.get_kind() < 4) {
            out.by_kind[p.get_kind()] = p.id;
        }
    }
    return out;
}

float workforce(game_state const& state, dcon::pop_id p) {
    auto const& w = state.world;
    float e = 0.0f;
    for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
        e += w.pop_get_employed(p, s);
    }
    return e + w.pop_get_unemployed(p) + w.pop_get_inactive(p);
}

// Scales a worker pop by `keep` (0..1): employment, unemployment, inactivity,
// baseline jobs (when `jobs`), and size.
void scale_workers(game_state& state, dcon::pop_id p, float keep, bool jobs) {
    auto& w = state.world;
    for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
        w.pop_set_employed(p, s, w.pop_get_employed(p, s) * keep);
        w.pop_set_need_slow(p, s, w.pop_get_need_slow(p, s) * keep);
        if(jobs) {
            w.pop_set_emp0(p, s, w.pop_get_emp0(p, s) * keep);
        }
    }
    w.pop_set_unemployed(p, w.pop_get_unemployed(p) * keep);
    w.pop_set_inactive(p, w.pop_get_inactive(p) * keep);
    w.pop_set_size(p, w.pop_get_size(p) * keep);
}

// Adds `people` job seekers to a worker pop; baseline jobs grow so that the
// economy absorbs them at the natural rate (labour demand follows supply).
void add_seekers(game_state& state, dcon::pop_id p, float people, float u0) {
    auto& w = state.world;
    float emp0 = 0.0f;
    for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
        emp0 += w.pop_get_emp0(p, s);
    }
    if(emp0 > 0.0f) {
        const float grow = 1.0f + people * (1.0f - u0) / emp0;
        for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
            w.pop_set_emp0(p, s, w.pop_get_emp0(p, s) * grow);
        }
    }
    w.pop_set_unemployed(p, w.pop_get_unemployed(p) + people);
    w.pop_set_size(p, w.pop_get_size(p) + people);
}

// Moves a fraction of the young epidemic compartments between regions (migration)
// or into the old group (ageing).
// Moves a fraction of the young epidemic compartments (every pathogen) between
// regions (migration) or into the old group (ageing).
void move_young(game_state& state, dcon::region_id from, dcon::region_id to, uint8_t to_group, float fraction) {
    auto& w = state.world;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    for(uint8_t p = 0; p < state.derived.pathogen_count; ++p) {
        const uint8_t src = epi_index(p, age_young);
        const uint8_t dst = epi_index(p, to_group);
        const float s = w.region_get_ep_s(from, src) * fraction;
        const float e = w.region_get_ep_e(from, src) * fraction;
        const float i = w.region_get_ep_i(from, src) * fraction;
        const float r = w.region_get_ep_r(from, src) * fraction;
        w.region_set_ep_s(from, src, w.region_get_ep_s(from, src) - s);
        w.region_set_ep_e(from, src, w.region_get_ep_e(from, src) - e);
        w.region_set_ep_i(from, src, w.region_get_ep_i(from, src) - i);
        w.region_set_ep_r(from, src, w.region_get_ep_r(from, src) - r);
        w.region_set_ep_s(to, dst, w.region_get_ep_s(to, dst) + s);
        w.region_set_ep_e(to, dst, w.region_get_ep_e(to, dst) + e);
        w.region_set_ep_i(to, dst, w.region_get_ep_i(to, dst) + i);
        w.region_set_ep_r(to, dst, w.region_get_ep_r(to, dst) + r);
    }
}

// Removes people of one age group from every pathogen's compartments.
void remove_alive(game_state& state, dcon::region_id r, uint8_t group, float people) {
    auto& w = state.world;
    for(uint8_t p = 0; p < state.derived.pathogen_count; ++p) {
        const uint8_t i = epi_index(p, group);
        const float alive = w.region_get_ep_s(r, i) + w.region_get_ep_e(r, i) + w.region_get_ep_i(r, i) + w.region_get_ep_r(r, i);
        if(alive <= 0.0f) {
            continue;
        }
        const float keep = std::max(0.0f, 1.0f - people / alive);
        w.region_set_ep_s(r, i, w.region_get_ep_s(r, i) * keep);
        w.region_set_ep_e(r, i, w.region_get_ep_e(r, i) * keep);
        w.region_set_ep_i(r, i, w.region_get_ep_i(r, i) * keep);
        w.region_set_ep_r(r, i, w.region_get_ep_r(r, i) * keep);
    }
}

void move_money(game_state& state, dcon::pop_id from, dcon::pop_id to, double fraction) {
    auto& w = state.world;
    const double amount = w.pop_get_balance(from) * std::clamp(fraction, 0.0, 1.0);
    transfer(state, account::of_pop(w.pop_get_nation(from), from), account::of_pop(w.pop_get_nation(to), to), amount,
             flow::household_transfers);
    const double trend = w.pop_get_balance_trend(from) * std::clamp(fraction, 0.0, 1.0);
    w.pop_set_balance_trend(from, w.pop_get_balance_trend(from) - trend);
    w.pop_set_balance_trend(to, w.pop_get_balance_trend(to) + trend);
}

} // namespace

// ---------------------------------------------------------------- demography
// Cohort flows per region every 28 days: births, children entering the labour
// force by skill, retirement, baseline mortality, and epidemic deaths by age
// (docs/04-society.md#demography).
void update_demography(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto r : w.in_region) {
        if(!is_due(static_cast<uint32_t>(r.id.index()), tick, monthly_cadence)) {
            continue;
        }
        const auto n = r.get_owner();
        const auto pops = pops_in(state, r.id);
        const auto retired = pops.by_kind[static_cast<uint8_t>(pop_kind::retired)];
        float population = r.get_population();

        // Births and childhood (children are part of the young epidemic group).
        const float births = n.get_birth_rate() * population * years;
        r.set_children(r.get_children() + births);
        for(uint8_t p = 0; p < state.derived.pathogen_count; ++p) {
            r.set_ep_s(epi_index(p, age_young), r.get_ep_s(epi_index(p, age_young)) + births);
        }
        population += births;
        const float entrants = r.get_children() * years / static_cast<float>(P.childhood_years);
        r.set_children(r.get_children() - entrants);
        const float high = std::clamp(n.get_tertiary_share(), 0.05f, 0.7f);
        const std::array<float, 3> split = {std::max(0.05f, 1.0f - high - 0.45f), 0.45f, high};
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            if(pops.by_kind[k]) {
                add_seekers(state, pops.by_kind[k], entrants * split[k], n.get_unemployment0());
            }
        }

        // Retirement: a share of each worker pop moves to the retired pop.
        const float retire = years / static_cast<float>(P.working_years);
        float retiring = 0.0f;
        float young_alive = 0.0f;
        {
            const uint8_t y0 = epi_index(0, age_young);
            young_alive = r.get_ep_s(y0) + r.get_ep_e(y0) + r.get_ep_i(y0) + r.get_ep_r(y0);
        }
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            const auto p = pops.by_kind[k];
            if(!p || !retired) {
                continue;
            }
            const float moving = workforce(state, p) * retire;
            retiring += moving;
            move_money(state, p, retired, retire);
            scale_workers(state, p, 1.0f - retire, true);
        }
        if(retired) {
            w.pop_set_size(retired, w.pop_get_size(retired) + retiring);
        }
        if(young_alive > 0.0f) {
            move_young(state, r.id, r.id, age_old, retiring / young_alive);
        }

        // Baseline (non-COVID) mortality, then epidemic deaths by age.
        const float old_deaths = retired ? w.pop_get_size(retired) * n.get_death_rate_old() * years : 0.0f;
        const float young_deaths = (population - (retired ? w.pop_get_size(retired) : 0.0f))
                                   * static_cast<float>(P.young_mortality) * years;
        remove_alive(state, r.id, age_old, old_deaths);
        remove_alive(state, r.id, age_young, young_deaths);
        const float epi_old = r.get_epi_deaths_old();
        const float epi_young = r.get_epi_deaths_young() + r.get_war_deaths(); // war deaths: working age
        if(r.get_war_deaths() > 0.0f) remove_alive(state, r.id, age_young, r.get_war_deaths());
        r.set_war_deaths(0.0f);
        r.set_epi_deaths_old(0.0f);
        r.set_epi_deaths_young(0.0f);
        if(retired) {
            w.pop_set_size(retired, std::max(0.0f, w.pop_get_size(retired) - old_deaths - epi_old));
        }
        float workers = 0.0f;
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            if(pops.by_kind[k]) workers += workforce(state, pops.by_kind[k]);
        }
        const float worker_deaths = workers > 0.0f ? (epi_young + young_deaths * workers / std::max(1.0f, population)) : 0.0f;
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            const auto p = pops.by_kind[k];
            if(p && workers > 0.0f) {
                scale_workers(state, p, std::max(0.0f, 1.0f - worker_deaths / workers), false);
            }
        }
        r.set_population(std::max(0.0f, population - old_deaths - young_deaths - epi_old - epi_young));
    }
}

// ---------------------------------------------------------------- migration
// Harris & Todaro (1970): people move toward higher *expected* income (income
// per head of their skill group, which includes unemployment risk) net of a
// moving cost. Internal moves follow region adjacency and are cut by internal
// movement restrictions (C7); international moves follow a gravity term
// (destination size) and are cut by travel controls at both ends (C8).
void update_migration(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;

    auto expected = [&](dcon::pop_id p) {
        const float size = w.pop_get_size(p);
        return size > 0.0f ? std::max(1e-3f, w.pop_get_income(p) / size / std::max(0.01f, w.nation_get_cpi(w.pop_get_nation(p)))) : 1e-3f;
    };
    auto restriction = [&](dcon::nation_id n, uint8_t k) { return k != 0xFF ? npi_fraction(state, n, k) : 0.0f; };

    struct move {
        dcon::pop_id from, to;
        float people;
    };
    std::vector<move> moves;

    // Internal (and cross-border) moves along adjacency, both directions.
    for(auto edge : w.in_region_adjacency) {
        const auto a = edge.get_a().id;
        const auto b = edge.get_b().id;
        const dcon::nation_id na = w.region_get_owner(a).id;
        if(!is_due(static_cast<uint32_t>(na.index()), tick, monthly_cadence)) {
            continue;
        }
        const dcon::nation_id nb = w.region_get_owner(b).id;
        const bool domestic = na == nb;
        const auto pa = pops_in(state, a);
        const auto pb = pops_in(state, b);
        const float cost = static_cast<float>(domestic ? P.migration_cost_internal : P.migration_cost_international);
        const float rate = static_cast<float>(domestic ? P.migration_rate_internal : P.migration_rate_international);
        const float block_a = domestic ? restriction(na, D.npi_movement) : restriction(na, D.npi_travel);
        const float block_b = domestic ? restriction(nb, D.npi_movement) : restriction(nb, D.npi_travel);
        const float open = (1.0f - 0.8f * block_a) * (1.0f - 0.8f * block_b);
        float wsum_a = 0.0f, wsum_b = 0.0f;
        for(auto e2 : w.region_get_region_adjacency_as_a(a)) wsum_a += e2.get_weight();
        for(auto e2 : w.region_get_region_adjacency_as_b(a)) wsum_a += e2.get_weight();
        for(auto e2 : w.region_get_region_adjacency_as_a(b)) wsum_b += e2.get_weight();
        for(auto e2 : w.region_get_region_adjacency_as_b(b)) wsum_b += e2.get_weight();
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            const auto x = pa.by_kind[k];
            const auto y = pb.by_kind[k];
            if(!x || !y) continue;
            const float gap = std::log(expected(y) / expected(x));
            if(gap > cost) {
                moves.push_back({x, y, rate * years * (gap - cost) * workforce(state, x) * open * edge.get_weight() / std::max(1e-6f, wsum_a)});
            } else if(-gap > cost) {
                moves.push_back({y, x, rate * years * (-gap - cost) * workforce(state, y) * open * edge.get_weight() / std::max(1e-6f, wsum_b)});
            }
        }
    }

    // International moves between nations (gravity: toward larger economies).
    double world_pop = 0.0;
    for(auto n : w.in_nation) world_pop += n.get_population();
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) {
            continue;
        }
        const auto origin_region = capital_region(state, n.id);
        if(!origin_region) continue;
        const auto po = pops_in(state, origin_region);
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const auto dest_region = capital_region(state, m.id);
            if(!dest_region) continue;
            const auto pd = pops_in(state, dest_region);
            const float open = (1.0f - 0.9f * restriction(n.id, D.npi_travel)) * (1.0f - 0.9f * restriction(m.id, D.npi_travel));
            const float gravity = static_cast<float>(m.get_population() / world_pop);
            for(uint8_t k = 0; k < worker_kinds; ++k) {
                const auto x = po.by_kind[k];
                const auto y = pd.by_kind[k];
                if(!x || !y) continue;
                // War at home pushes refugees out regardless of the income gap.
                float at_war = 0.0f;
                for(auto o : w.in_nation) at_war = std::max(at_war, n.get_at_war(static_cast<uint8_t>(o.id.index())));
                const float gap = std::log(expected(y) / expected(x)) - static_cast<float>(P.migration_cost_international)
                                  + static_cast<float>(P.refugee_push) * at_war;
                if(gap > 0.0f) {
                    moves.push_back({x, y, static_cast<float>(P.migration_rate_international) * years * gap * workforce(state, x) * open * gravity});
                }
            }
        }
    }

    // Apply moves: migrants leave jobs or job search and arrive as job seekers.
    for(auto const& mv : moves) {
        const float avail = workforce(state, mv.from);
        const float people = std::min(mv.people, static_cast<float>(P.migration_cap) * avail);
        if(people <= 0.0f || avail <= 0.0f) continue;
        const float frac = people / avail;
        move_money(state, mv.from, mv.to, frac);
        scale_workers(state, mv.from, 1.0f - frac, true);
        const auto to_nation = w.pop_get_nation(mv.to);
        add_seekers(state, mv.to, people, w.nation_get_unemployment0(to_nation));
        const auto ra = w.pop_get_home(mv.from);
        const auto rb = w.pop_get_home(mv.to);
        const uint8_t y0 = epi_index(0, age_young);
        const float young = w.region_get_ep_s(ra, y0) + w.region_get_ep_e(ra, y0) + w.region_get_ep_i(ra, y0) + w.region_get_ep_r(ra, y0);
        if(young > 0.0f) {
            move_young(state, ra, rb, age_young, people / young); // migrants carry infection
        }
        w.region_set_population(ra, w.region_get_population(ra) - people);
        w.region_set_population(rb, w.region_get_population(rb) + people);
        w.region_set_migration_net(ra, w.region_get_migration_net(ra) - people);
        w.region_set_migration_net(rb, w.region_get_migration_net(rb) + people);
        const auto from_nation = w.pop_get_nation(mv.from);
        if(from_nation != to_nation) {
            w.nation_set_migration_net(from_nation, w.nation_get_migration_net(from_nation) - people);
            w.nation_set_migration_net(to_nation, w.nation_get_migration_net(to_nation) + people);
            w.nation_set_population(from_nation, w.nation_get_population(from_nation) - people);
            w.nation_set_population(to_nation, w.nation_get_population(to_nation) + people);
        }
    }
}

// ---------------------------------------------------------------- values and opinion
// Inglehart-Welzel axes plus economic left-right (docs/04-society.md#values-and-opinion).
// Existential insecurity pulls values toward survival; income losers move left
// (demand for redistribution); bounded confidence (Hegselmann & Krause 2002)
// within each nation, with a confidence radius widened by media freedom.
// Issue salience follows the crisis of the moment.
void update_opinion(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    constexpr uint8_t A = static_cast<uint8_t>(value_axis::count);
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) {
            continue;
        }
        std::vector<dcon::pop_id> pops;
        for(auto own : n.get_region_ownership()) {
            for(auto loc : own.get_region().get_pop_location()) pops.push_back(loc.get_pop().id);
        }
        // Drift.
        const float deaths_pm = reported_deaths_per_million_daily(state, n.id);
        for(auto p : pops) {
            const float size = std::max(1.0f, w.pop_get_size(p));
            const float loss = std::max(0.0f, 1.0f - w.pop_get_income(p) / std::max(1e-3f, w.pop_get_income_expected(p)));
            const float jobless = w.pop_get_unemployed(p) / size;
            const float insecurity = std::clamp(2.0f * loss + 2.0f * jobless + 0.05f * deaths_pm, 0.0f, 1.0f);
            const float survival = -static_cast<float>(P.values_insecurity_pull) * insecurity * years;
            w.pop_set_values(p, static_cast<uint8_t>(value_axis::self_expression),
                             std::clamp(w.pop_get_values(p, static_cast<uint8_t>(value_axis::self_expression)) + survival, -1.0f, 1.0f));
            w.pop_set_values(p, static_cast<uint8_t>(value_axis::economic_right),
                             std::clamp(w.pop_get_values(p, static_cast<uint8_t>(value_axis::economic_right))
                                            - static_cast<float>(P.values_loss_left) * w.pop_get_grievance(p) * years, -1.0f, 1.0f));
            // Salience of issues.
            std::array<float, static_cast<std::size_t>(issue::count)> sal = {
                0.4f + 3.0f * jobless + 2.0f * loss, 0.1f + 0.2f * deaths_pm, 0.1f + 0.2f * w.region_get_unrest(w.pop_get_home(p)),
                0.2f + 10.0f * std::abs(w.region_get_migration_net(w.pop_get_home(p))) / std::max(1.0f, w.region_get_population(w.pop_get_home(p)))};
            float total = 0.0f;
            for(float v : sal) total += v;
            for(uint8_t i = 0; i < sal.size(); ++i) w.pop_set_salience(p, i, sal[i] / total);
        }
        // Bounded confidence: move toward the size-weighted mean of pops within eps.
        const float eps = static_cast<float>(P.opinion_eps_base + P.opinion_eps_media * n.get_media_freedom());
        const float mu = std::min(1.0f, static_cast<float>(P.opinion_speed) * years);
        std::vector<std::array<float, A>> next(pops.size());
        for(std::size_t i = 0; i < pops.size(); ++i) {
            std::array<double, A> sum{};
            double weight = 0.0;
            for(std::size_t j = 0; j < pops.size(); ++j) {
                float d2 = 0.0f;
                for(uint8_t a = 0; a < A; ++a) {
                    const float d = w.pop_get_values(pops[i], a) - w.pop_get_values(pops[j], a);
                    d2 += d * d;
                }
                if(d2 <= eps * eps) {
                    const double wt = w.pop_get_size(pops[j]);
                    for(uint8_t a = 0; a < A; ++a) sum[a] += wt * w.pop_get_values(pops[j], a);
                    weight += wt;
                }
            }
            for(uint8_t a = 0; a < A; ++a) {
                const float x = w.pop_get_values(pops[i], a);
                next[i][a] = weight > 0.0 ? x + mu * (static_cast<float>(sum[a] / weight) - x) : x;
            }
        }
        for(std::size_t i = 0; i < pops.size(); ++i) {
            for(uint8_t a = 0; a < A; ++a) w.pop_set_values(pops[i], a, next[i][a]);
        }
    }
}

// ---------------------------------------------------------------- unrest
// Grievance = relative deprivation (Gurr 1970): income below expectation,
// unemployment, price shocks. Political Stress Index (Goldstone 1991; Turchin
// 2016) per nation = mass mobilisation x elite mobilisation x state fiscal
// distress. Regional unrest climbs a ladder (calm 0, protests 1, strikes 2,
// riots 3, insurgency 4) toward a target set by grievance, low legitimacy and
// PSI; repression lowers expression but legitimacy pays (Davenport 2007).
// Strikes cut regional capacity; riots destroy capital.
void update_unrest(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float days = static_cast<float>(monthly_cadence.period_days);

    // Nation PSI (on the nation's shard).
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        double mass_inc = 0, mass_n = 0, elite_unemp = 0, elite_n = 0;
        for(auto own : n.get_region_ownership()) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                if(p.get_kind() <= static_cast<uint8_t>(pop_kind::mid_skill)) {
                    mass_inc += p.get_income();
                    mass_n += p.get_size();
                } else if(p.get_kind() == static_cast<uint8_t>(pop_kind::high_skill)) {
                    elite_unemp += p.get_unemployed();
                    elite_n += p.get_size();
                }
            }
        }
        const float cpi = std::max(0.01f, n.get_cpi());
        const float mass_wage = mass_n > 0 ? static_cast<float>(mass_inc / mass_n) / cpi : 0.0f;
        const float mmp = std::max(0.0f, 1.0f - mass_wage / std::max(1e-3f, n.get_mass_wage0()));
        const float emp = elite_n > 0 ? std::max(0.0f, static_cast<float>(elite_unemp / elite_n) - n.get_elite_unemp0()) : 0.0f;
        const double gdp_nominal = n.get_gdp0() * n.get_core_price();
        const float sfd = static_cast<float>(std::max(0.0, -n.get_bal_gov()) / gdp_nominal);
        n.set_psi_mmp(mmp);
        n.set_psi_emp(emp);
        n.set_psi_sfd(sfd);
        // Zero in the calibrated baseline; fiscal distress amplifies mass and elite stress.
        n.set_psi(((1.0f + 3.0f * mmp) * (1.0f + 5.0f * emp) - 1.0f) * (1.0f + 0.5f * sfd));
    }

    for(auto r : w.in_region) {
        if(!is_due(static_cast<uint32_t>(r.id.index()), tick, monthly_cadence)) continue;
        const auto n = r.get_owner();
        double g = 0.0, size = 0.0;
        for(auto loc : r.get_pop_location()) {
            auto p = loc.get_pop();
            const float s = std::max(1.0f, p.get_size());
            // Expectations adapt slowly (relative deprivation needs a reference).
            float expected = p.get_income_expected();
            expected += (p.get_income() - expected) * std::min(1.0f, days / static_cast<float>(P.expectations_income_days));
            p.set_income_expected(expected);
            const float shortfall = std::max(0.0f, 1.0f - p.get_income() / std::max(1e-3f, expected));
            const float jobless_rise = std::max(0.0f, p.get_unemployed() / s - n.get_unemployment0());
            const float grievance = std::clamp(2.4f * shortfall + 1.2f * jobless_rise * 3.0f, 0.0f, 1.0f);
            p.set_grievance(grievance);
            g += grievance * s;
            size += s;
        }
        const float grievance = size > 0 ? static_cast<float>(g / size) : 0.0f;
        r.set_grievance(grievance);
        const float repression = n.get_state_capacity() * (1.0f - std::clamp((n.get_voice() + 2.5f) / 5.0f, 0.0f, 1.0f));
        const float target = 4.0f * std::clamp(static_cast<float>(P.unrest_grievance) * grievance * (1.0f - n.get_legitimacy())
                                                   + static_cast<float>(P.unrest_psi) * n.get_psi(), 0.0f, 1.0f)
                             * (1.0f - 0.6f * repression);
        float u = r.get_unrest();
        u += (target - u) * std::min(1.0f, days / static_cast<float>(P.unrest_days));
        r.set_unrest(u);
        // Strikes reduce capacity; riots (above 3) destroy productive capital.
        r.set_unrest_capacity(1.0f - 0.05f * std::max(0.0f, u - 1.0f));
        if(u > 3.0f) {
            for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
                r.set_va0(s, r.get_va0(s) * (1.0f - static_cast<float>(P.riot_capital_loss) * (u - 3.0f)));
            }
        }
    }
}

} // namespace geosim::sim
