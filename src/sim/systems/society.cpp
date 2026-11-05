// Society (docs/04-society.md): demography, migration, values and opinion, unrest.
//
// All four run on 28-day cadences (regions or nations sharded by index). They
// move stocks between pops and regions along irregular graphs, so they are
// scalar passes (the SIMD carve-out); every money movement still goes through
// sim::transfer, so the ledger stays exact.

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

// Pops of a region by kind (one per identity group).
struct region_pops {
    std::array<std::vector<dcon::pop_id>, 4> by_kind; // indexed by pop_kind
};

region_pops pops_in(game_state const& state, dcon::region_id r) {
    region_pops out;
    for(auto loc : state.world.region_get_pop_location(r)) {
        auto p = loc.get_pop();
        if(p.get_kind() < 4) {
            out.by_kind[p.get_kind()].push_back(p.id);
        }
    }
    return out;
}

// The pop of `kind` and `group` in a region (empty if none).
dcon::pop_id find_pop(game_state const& state, region_pops const& rp, uint8_t kind, dcon::social_group_id group) {
    for(auto p : rp.by_kind[kind]) {
        if(state.world.pop_get_group(p) == group) return p;
    }
    return {};
}

// International migrants join the destination's immigrant group (or its largest).
dcon::social_group_id arrival_group(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    dcon::social_group_id best, largest;
    float share = -1.0f;
    for(auto g : w.in_social_group) {
        if(g.get_nation() != n) continue;
        if(g.get_immigrant() && !best) best = g.id;
        if(g.get_pop_share() > share) {
            share = g.get_pop_share();
            largest = g.id;
        }
    }
    return best ? best : largest;
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
    const auto retired_kind = static_cast<uint8_t>(pop_kind::retired);
    for(auto r : w.in_region) {
        if(!is_due(static_cast<uint32_t>(r.id.index()), tick, monthly_cadence)) {
            continue;
        }
        const auto n = r.get_owner();
        const auto pops = pops_in(state, r.id);
        float population = r.get_population();
        float retired_size = 0.0f;
        for(auto p : pops.by_kind[retired_kind]) retired_size += w.pop_get_size(p);

        // Births and childhood (children are part of the young epidemic group).
        // Births fall in recessions (Kearney & Levine 2021: the COVID baby bust).
        const float bust = std::max(0.2f, 1.0f - static_cast<float>(P.baby_bust) * std::max(0.0f, n.get_unemployment() - n.get_unemployment0()));
        const float births = n.get_birth_rate() * population * years * bust;
        r.set_children(r.get_children() + births);
        for(uint8_t p = 0; p < state.derived.pathogen_count; ++p) {
            r.set_ep_s(epi_index(p, age_young), r.get_ep_s(epi_index(p, age_young)) + births);
        }
        population += births;
        // Labour-market entrants by identity group (its population share) and by the
        // group's own skill mix (education gaps persist across cohorts).
        const float entrants = r.get_children() * years / static_cast<float>(P.childhood_years);
        r.set_children(r.get_children() - entrants);
        const float high = std::clamp(n.get_tertiary_share(), 0.05f, 0.7f);
        const std::array<float, 3> split = {std::max(0.05f, 1.0f - high - 0.45f), 0.45f, high};
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            for(auto p : pops.by_kind[k]) {
                if(w.pop_get_stratum(p) != static_cast<uint8_t>(stratum::base)) continue; // entrants join the base stratum
                const auto g = w.pop_get_group(p);
                const float gshare = g ? w.social_group_get_pop_share(g) : 1.0f / static_cast<float>(pops.by_kind[k].size());
                const float sk = g && w.social_group_get_skill(g, k) > 0.0f ? w.social_group_get_skill(g, k) : split[k];
                // Cohorts entering a slack labour market carry a lasting wage penalty (Kahn 2010).
                const float added = entrants * gshare * sk;
                const float size = std::max(1.0f, workforce(state, p));
                const float scar = static_cast<float>(P.scarring) * std::max(0.0f, n.get_unemployment() - n.get_unemployment0()) * added / (size + added);
                if(scar > 0.0f) {
                    for(uint8_t s = 0; s < state.derived.sector_count; ++s) w.pop_set_wage0(p, s, w.pop_get_wage0(p, s) * (1.0f - scar));
                }
                // Formative years: cohorts entering in insecure times hold survival values for life
                // (Inglehart 1997); entrants pull their pop's values by their weight.
                const float insecurity = std::clamp(5.0f * std::max(0.0f, n.get_unemployment() - n.get_unemployment0())
                                                        + 0.05f * reported_deaths_per_million_daily(state, n.id), 0.0f, 1.0f);
                if(insecurity > 0.0f) {
                    const auto se = static_cast<uint8_t>(value_axis::self_expression);
                    w.pop_set_values(p, se, std::clamp(w.pop_get_values(p, se) - static_cast<float>(P.formative_values) * insecurity * added / (size + added), -1.0f, 1.0f));
                }
                add_seekers(state, p, added, n.get_unemployment0());
            }
        }

        // Retirement: a share of each worker pop moves to the retired pop of its group;
        // a later statutory age (policy.cpp) lengthens working lives.
        const float retire = years / std::max(20.0f, static_cast<float>(P.working_years) + n.get_retirement_age_shift());
        float retiring = 0.0f;
        float young_alive = 0.0f;
        {
            const uint8_t y0 = epi_index(0, age_young);
            young_alive = r.get_ep_s(y0) + r.get_ep_e(y0) + r.get_ep_i(y0) + r.get_ep_r(y0);
        }
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            for(auto p : pops.by_kind[k]) {
                const auto retired = find_pop(state, pops, retired_kind, w.pop_get_group(p));
                if(!retired) continue;
                const float moving = workforce(state, p) * retire;
                retiring += moving;
                move_money(state, p, retired, retire);
                scale_workers(state, p, 1.0f - retire, true);
                w.pop_set_size(retired, w.pop_get_size(retired) + moving);
            }
        }
        if(young_alive > 0.0f) {
            move_young(state, r.id, r.id, age_old, retiring / young_alive);
        }

        // Baseline (non-COVID) mortality, then epidemic deaths by age.
        retired_size = 0.0f;
        for(auto p : pops.by_kind[retired_kind]) retired_size += w.pop_get_size(p);
        // Mortality follows income and health spending relative to 2019 (Preston 1975;
        // Cutler, Deaton & Lleras-Muney 2006).
        const float income_rel = std::max(0.3f, n.get_gdp_real() / std::max(1.0f, n.get_gdp0_daily() * n.get_gf()));
        const float mortality = std::clamp(std::pow(income_rel, -static_cast<float>(P.mortality_income))
                                               * (1.0f - static_cast<float>(P.mortality_health) * n.get_health_spending()), 0.5f, 2.0f);
        const float old_deaths = retired_size * n.get_death_rate_old() * years * mortality;
        // Non-negative even if the region's head-count and its pops disagree (a negative
        // death toll would scale workers up; docs/13).
        const float young_deaths = std::max(0.0f, population - retired_size) * static_cast<float>(P.young_mortality) * years * mortality;
        remove_alive(state, r.id, age_old, old_deaths);
        remove_alive(state, r.id, age_young, young_deaths);
        const float epi_old = r.get_epi_deaths_old();
        const float epi_young = r.get_epi_deaths_young() + r.get_war_deaths(); // war deaths: working age
        if(r.get_war_deaths() > 0.0f) remove_alive(state, r.id, age_young, r.get_war_deaths());
        r.set_war_deaths(0.0f);
        r.set_epi_deaths_old(0.0f);
        r.set_epi_deaths_young(0.0f);
        if(retired_size > 0.0f) {
            const float keep = std::max(0.0f, 1.0f - (old_deaths + epi_old) / retired_size);
            for(auto p : pops.by_kind[retired_kind]) w.pop_set_size(p, w.pop_get_size(p) * keep);
        }
        float workers = 0.0f;
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            for(auto p : pops.by_kind[k]) workers += workforce(state, p);
        }
        const float worker_deaths = workers > 0.0f ? (epi_young + young_deaths * workers / std::max(1.0f, population)) : 0.0f;
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            for(auto p : pops.by_kind[k]) {
                if(workers > 0.0f) scale_workers(state, p, std::max(0.0f, 1.0f - worker_deaths / workers), false);
            }
        }
        r.set_population(std::max(0.0f, population - old_deaths - young_deaths - epi_old - epi_young));

        // Age bands (0-14, 15-29, 30-49, 50-64, 65+): a cohort-component account fed by
        // the flows above (UN WPP method, coarse): births enter band 0, each band ages
        // into the next at 1/width per year, deaths fall on the bands of their group.
        {
            static constexpr float width[age_bands] = {15.0f, 15.0f, 20.0f, 15.0f, 1e9f};
            std::array<float, age_bands> band{};
            float total = 0.0f;
            for(uint8_t b = 0; b < age_bands; ++b) total += band[b] = r.get_age_band(b);
            if(total > 0.0f) {
                std::array<float, age_bands> next = band;
                next[0] += births;
                for(uint8_t b = 0; b + 1 < age_bands; ++b) {
                    const float up = band[b] * years / width[b];
                    next[b] -= up;
                    next[b + 1] += up;
                }
                const float working = next[1] + next[2] + next[3];
                const float young_d = young_deaths + epi_young;
                for(uint8_t b = 1; b <= 3 && working > 0.0f; ++b) next[b] = std::max(0.0f, next[b] - young_d * next[b] / working);
                next[4] = std::max(0.0f, next[4] - old_deaths - epi_old);
                // Keep the account consistent with the population total.
                float sum = 0.0f;
                for(float v : next) sum += v;
                const float scale = sum > 0.0f ? r.get_population() / sum : 1.0f;
                for(uint8_t b = 0; b < age_bands; ++b) r.set_age_band(b, next[b] * scale);
            }
            r.set_ifr_age(ifr_age_factor(state, r.id));
        }

        // Intergenerational mobility between class tiers (low -> mid -> high skill),
        // lower where inequality is high: the Great Gatsby curve (Corak 2013; Chetty et
        // al. 2014). The intergenerational elasticity IGE = gatsby_slope x Gini - 0.2.
        {
            const float ige = std::clamp(static_cast<float>(P.gatsby_slope) * n.get_gini() - 0.2f, 0.1f, 0.9f);
            n.set_ige(ige);
            const float up = static_cast<float>(P.mobility_rate) * (1.0f - ige) * years;
            float moved = 0.0f;
            for(uint8_t k = 0; k + 1 < worker_kinds; ++k) {
                for(auto p : pops.by_kind[k]) {
                    if(w.pop_get_stratum(p) != static_cast<uint8_t>(stratum::base)) continue;
                    const auto q = find_pop(state, pops, static_cast<uint8_t>(k + 1), w.pop_get_group(p));
                    if(!q) continue;
                    // Movers are job seekers (retraining while unemployed): they re-enter the higher tier as seekers.
                    const float people = std::min(w.pop_get_unemployed(p), workforce(state, p) * up);
                    if(people <= 0.0f) continue;
                    const float frac = people / std::max(1.0f, workforce(state, p));
                    move_money(state, p, q, frac);
                    w.pop_set_unemployed(p, w.pop_get_unemployed(p) - people);
                    w.pop_set_size(p, std::max(0.0f, w.pop_get_size(p) - people));
                    add_seekers(state, q, people, n.get_unemployment0());
                    moved += people;
                }
            }
            n.set_mobility_up(moved);
        }
    }
}

// Fatality of the under-65 group relative to a reference age mix: infection fatality
// roughly doubles every eight years of age (Levin et al. 2020, SARS-CoV-2; similar
// gradients for influenza), so a young population under 65 dies far less from the
// same infections. Band weights ~ IFR at 0-14, 15-29, 30-49, 50-64; the reference is
// a high-income under-65 mix (22/24/31/23%) against which pathogens are calibrated.
float ifr_age_factor(game_state const& state, dcon::region_id r) {
    auto const& w = state.world;
    static constexpr float weight[4] = {0.0001f, 0.001f, 0.01f, 0.06f};
    constexpr float reference = 0.22f * 0.0001f + 0.24f * 0.001f + 0.31f * 0.01f + 0.23f * 0.06f;
    float under = 0.0f, sum = 0.0f;
    for(uint8_t b = 0; b < 4; ++b) {
        under += w.region_get_age_band(r, b);
        sum += w.region_get_age_band(r, b) * weight[b];
    }
    return under > 0.0f ? std::clamp(sum / under / reference, 0.1f, 2.0f) : 1.0f;
}

// Pension outlays scale with the old-age share relative to 2019 (ageing).
float pension_ageing_factor(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const float s0 = w.nation_get_old_share0(n);
    if(s0 <= 0.0f) return 1.0f;
    float old = 0.0f, total = 0.0f;
    for(auto own : w.nation_get_region_ownership(n)) {
        auto r = own.get_region();
        old += r.get_age_band(age_bands - 1);
        for(uint8_t b = 0; b < age_bands; ++b) total += r.get_age_band(b);
    }
    if(total <= 0.0f) return 1.0f;
    return std::pow(std::max(0.2f, old / total / s0), static_cast<float>(state.params.pension_ageing));
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
        return size > 0.0f ? std::max(1e-3f, w.pop_get_income(p) / size / std::max(0.01f, w.pop_get_cpi_p(p))) : 1e-3f;
    };
    auto restriction = [&](dcon::nation_id n, uint8_t k) { return k != 0xFF ? npi_fraction(state, n, k) : 0.0f; };

    struct move {
        dcon::pop_id from, to;
        float people;
    };
    std::vector<move> moves;

    // Per-nation lookups shared by every move this call (built only when a nation is
    // due; they were recomputed per region pair and per pop: quadratic and worse at
    // world scale).
    bool any_due = false;
    for(auto n : w.in_nation) any_due = any_due || is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence);
    if(!any_due) return;
    const std::size_t N = w.nation_size();
    std::vector<dcon::social_group_id> arrival(N);
    for(auto n : w.in_nation) arrival[n.id.index()] = arrival_group(state, n.id);

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
            for(auto x : pa.by_kind[k]) {
                // Same group on the other side (domestic); immigrants abroad.
                const auto y = domestic ? find_pop(state, pb, k, w.pop_get_group(x)) : find_pop(state, pb, k, arrival[nb.index()]);
                if(!y) continue;
                // Housing costs deter moves to booming regions (Ganong & Shoag 2017).
                const float housing = 0.3f * std::log(std::max(0.05f, w.region_get_house_price_r(b)) / std::max(0.05f, w.region_get_house_price_r(a)));
                const float gap = std::log(expected(y) / expected(x)) - housing;
                if(gap > cost) {
                    moves.push_back({x, y, rate * years * (gap - cost) * workforce(state, x) * open * edge.get_weight() / std::max(1e-6f, wsum_a)});
                } else if(-gap > cost && domestic) {
                    moves.push_back({y, x, rate * years * (-gap - cost) * workforce(state, y) * open * edge.get_weight() / std::max(1e-6f, wsum_b)});
                }
            }
        }
    }

    // International moves between nations (gravity: toward larger economies).
    double world_pop = 0.0;
    for(auto n : w.in_nation) world_pop += n.get_population();
    std::vector<dcon::region_id> capital(N);
    std::vector<region_pops> capital_pops(N);
    for(auto n : w.in_nation) {
        capital[n.id.index()] = capital_region(state, n.id);
        if(capital[n.id.index()]) capital_pops[n.id.index()] = pops_in(state, capital[n.id.index()]);
    }
    std::vector<uint8_t> deal(N);
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) {
            continue;
        }
        const auto origin_region = capital[n.id.index()];
        if(!origin_region) continue;
        region_pops const& po = capital_pops[n.id.index()];
        // War at home pushes refugees out regardless of the income gap.
        float at_war = 0.0f;
        for(auto o : w.in_nation) at_war = std::max(at_war, n.get_at_war(static_cast<uint8_t>(o.id.index())));
        // Destinations holding a migration deal with n.
        std::fill(deal.begin(), deal.end(), uint8_t(0));
        for(auto t : w.in_treaty) {
            if(!t.get_active() || t.get_kind() != static_cast<uint8_t>(treaty_kind::migration)) continue;
            if(t.get_a() == n.id && t.get_b()) deal[t.get_b().id.index()] = 1;
            if(t.get_b() == n.id && t.get_a()) deal[t.get_a().id.index()] = 1;
        }
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const auto dest_region = capital[m.id.index()];
            if(!dest_region) continue;
            region_pops const& pd = capital_pops[m.id.index()];
            const float open = (1.0f - 0.9f * restriction(n.id, D.npi_travel)) * (1.0f - 0.9f * restriction(m.id, D.npi_travel));
            // Migrant networks lower the cost of following (Massey et al. 1993; Beine,
            // Docquier & Ozden 2011): a diaspora from n already in m pulls more.
            const auto ni = static_cast<uint16_t>(n.id.index());
            const float network = 1.0f + static_cast<float>(P.diaspora_pull) * 100.0f * m.get_diaspora(ni) / std::max(1.0f, static_cast<float>(m.get_population()));
            // A migration deal (agreements.cpp) has the origin hold departures to that destination.
            const float held = deal[m.id.index()] ? 0.3f : 1.0f;
            const float gravity = static_cast<float>(m.get_population() / world_pop) * network * m.get_immigration_openness() * held;
            const auto dest_group = arrival[m.id.index()];
            for(uint8_t k = 0; k < worker_kinds; ++k) {
              for(auto x : po.by_kind[k]) {
                const auto y = find_pop(state, pd, k, dest_group);
                if(!y) continue;
                const float gap = std::log(expected(y) / expected(x)) - static_cast<float>(P.migration_cost_international)
                                  + static_cast<float>(P.refugee_push) * at_war * (1.0f + static_cast<float>(P.refugee_occupation) * n.get_occupied())
                                        * static_cast<float>(P.asylum_refugee) * m.get_asylum_openness();
                if(gap > 0.0f) {
                    moves.push_back({x, y, static_cast<float>(P.migration_rate_international) * years * gap * workforce(state, x) * open * gravity});
                }
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
            const auto fi = static_cast<uint16_t>(from_nation.id.index());
            w.nation_set_diaspora(to_nation, fi, w.nation_get_diaspora(to_nation, fi) + people);
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
        float warming = 0.0f;
        for(auto c : w.in_climate) warming = c.get_temperature() - static_cast<float>(P.temperature0);
        for(auto p : pops) {
            const float size = std::max(1.0f, w.pop_get_size(p));
            const float loss = std::max(0.0f, 1.0f - w.pop_get_income(p) / std::max(1e-3f, w.pop_get_income_expected(p)));
            const float jobless = w.pop_get_unemployed(p) / size;
            // Insecurity is joblessness above the normal rate: the baseline itself must not
            // move values (it made every public drift toward survival values in steady state).
            const float insecurity = std::clamp(2.0f * loss + 2.0f * std::max(0.0f, jobless - n.get_unemployment0()) + 0.05f * deaths_pm, 0.0f, 1.0f);
            const float survival = -static_cast<float>(P.values_insecurity_pull) * insecurity * years;
            w.pop_set_values(p, static_cast<uint8_t>(value_axis::self_expression),
                             std::clamp(w.pop_get_values(p, static_cast<uint8_t>(value_axis::self_expression)) + survival, -1.0f, 1.0f));
            // Losers move left (demand for redistribution). The thermostatic public (Wlezien
            // 1995) reacts to policy in vote choice (politics.cpp), not by moving its values:
            // integrating it here fed back through the window centre and drove voters
            // steadily away from whichever party governed.
            const uint8_t er = static_cast<uint8_t>(value_axis::economic_right);
            w.pop_set_values(p, er,
                             std::clamp(w.pop_get_values(p, er) - static_cast<float>(P.values_loss_left) * w.pop_get_grievance(p) * years, -1.0f, 1.0f));
            // Salience of issues.
            const auto home = w.pop_get_home(p);
            const float mig = 10.0f * std::abs(w.region_get_migration_net(home)) / std::max(1.0f, w.region_get_population(home));
            std::array<float, static_cast<std::size_t>(issue::count)> sal = {
                0.4f + 3.0f * jobless + 2.0f * loss,                                                   // economy
                0.1f + 0.2f * deaths_pm,                                                               // health
                0.1f + 0.2f * w.region_get_unrest(home),                                               // security
                0.1f + 2.0f * (w.pop_get_group(p) ? w.social_group_get_mobilisation(w.pop_get_group(p)) : 0.0f), // identity
                0.1f + mig,                                                                            // immigration
                0.05f + 0.1f * std::max(0.0f, warming) * std::max(0.0f, 0.5f + w.pop_get_values(p, static_cast<uint8_t>(value_axis::self_expression))), // environment
                0.05f + 0.3f * n.get_corruption() * n.get_connectivity() * w.pop_get_grievance(p)};      // corruption
            // Salient immigration pulls toward traditional values (Hainmueller & Hopkins 2014).
            w.pop_set_values(p, static_cast<uint8_t>(value_axis::self_expression),
                             std::clamp(w.pop_get_values(p, static_cast<uint8_t>(value_axis::self_expression))
                                            - static_cast<float>(P.immigration_tan) * mig * years, -1.0f, 1.0f));
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
// unemployment, price shocks, plus the mobilisation of the pop's identity group
// (horizontal inequality; update_groups). Political Stress Index (Goldstone
// 1991; Turchin 2016) per nation = mass mobilisation (wages, youth bulge,
// urbanisation; Urdal 2006) x elite mobilisation (educated unemployment) x state
// fiscal distress. Every 28 days this sets each region's *drive*; collective
// action itself is a daily threshold cascade (update_protest).
void update_unrest(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
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
        // Youth bulges and cities amplify mass mobilisation (Goldstone 1991; Urdal 2006).
        const float youth = n.get_youth_share() > 0.0f ? n.get_youth_share() / static_cast<float>(P.youth_bulge_ref) : 1.0f;
        const float mmp = std::max(0.0f, 1.0f - mass_wage / std::max(1e-3f, n.get_mass_wage0())) * youth * (0.5f + 0.5f * n.get_urban_share());
        // Educated youth without jobs: elite overproduction (Turchin 2016).
        const float emp = elite_n > 0 ? std::max(0.0f, static_cast<float>(elite_unemp / elite_n) - n.get_elite_unemp0())
                                            * std::max(1.0f, 0.5f * n.get_youth_unemp_rel())
                                      : 0.0f;
        const double gdp_nominal = n.get_gdp0() * n.get_core_price();
        const float sfd = static_cast<float>(std::max(0.0, -n.get_bal_gov()) / gdp_nominal);
        n.set_psi_mmp(mmp);
        n.set_psi_emp(emp);
        n.set_psi_sfd(sfd);
        // Zero in the calibrated baseline; fiscal distress amplifies mass and elite stress.
        n.set_psi(((1.0f + 3.0f * mmp) * (1.0f + 5.0f * emp) - 1.0f) * (1.0f + 0.5f * sfd));
    }

    // Last month's national grievance (population-weighted), the reference for regional PSI.
    std::vector<float> nation_grievance(w.nation_size(), 0.0f);
    {
        std::vector<float> pop(w.nation_size(), 0.0f);
        for(auto r : w.in_region) {
            const auto ni = static_cast<std::size_t>(r.get_owner().id.index());
            nation_grievance[ni] += r.get_grievance() * r.get_population();
            pop[ni] += r.get_population();
        }
        for(std::size_t i = 0; i < pop.size(); ++i) nation_grievance[i] = pop[i] > 0.0f ? nation_grievance[i] / pop[i] : 0.0f;
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
            const float mobil = p.get_group() ? p.get_group().get_mobilisation() : 0.0f;
            const float grievance = std::clamp(2.4f * shortfall + 1.2f * jobless_rise * 3.0f + mobil, 0.0f, 1.0f);
            p.set_grievance(grievance);
            g += grievance * s;
            size += s;
        }
        const float grievance = size > 0 ? static_cast<float>(g / size) : 0.0f;
        r.set_grievance(grievance);
        // Regional PSI: the national index concentrated where grievance is (its
        // population-weighted mean stays the national value; Turchin 2016 applied
        // to regions, cf. Goldstone et al. 2010 on local instability).
        const float ng = std::max(0.02f, nation_grievance[static_cast<std::size_t>(n.id.index())]);
        r.set_psi_region(n.get_psi() * std::clamp((grievance + 0.02f) / (ng + 0.02f), 0.5f, 2.0f));
        // Regional trust: national trust eroded locally by unrest (Putnam 1993).
        r.set_trust_r(std::clamp(n.get_social_trust() - static_cast<float>(P.trust_region_unrest) * r.get_unrest(), 0.0f, 1.0f));
        // Corruption is more visible, and more resented, where people are online.
        const float amplifier = 1.0f + static_cast<float>(P.corruption_amplifier) * n.get_corruption() * n.get_connectivity();
        // Blocked emigration bottles up voice (Hirschman 1970): travel bans x emigrant networks.
        const float travel = D.npi_travel != 0xFF ? npi_fraction(state, n.id, D.npi_travel) : 0.0f;
        const float exit_blocked = static_cast<float>(P.exit_blocked) * n.get_emigrant_stock() * 10.0f * travel * grievance;
        // Liberty-restricting policy beyond the Overton window (e.g. lockdowns where they
        // are unacceptable) adds a backlash.
        const float backlash = static_cast<float>(P.overton_backlash) * n.get_policy_outside();
        // Strikes: real-wage losses where unions are strong (labour.cpp).
        const float wage_loss = std::max(0.0f, 1.0f - n.get_wage_real() * n.get_core_price() / std::max(0.01f, n.get_cpi()));
        const float strikes = static_cast<float>(P.strike_drive) * n.get_union_density() * wage_loss * 10.0f;
        const float drive = (static_cast<float>(P.unrest_grievance) * grievance * (1.0f - n.get_legitimacy())
                             + static_cast<float>(P.unrest_psi) * r.get_psi_region()) * amplifier
                            + exit_blocked + backlash + strikes
                            // Mega-event spending amid grievance (Brazil 2013-14).
                            + static_cast<float>(P.mega_protest) * 100.0f * n.get_mega_build_share() * grievance
                            // Fiscally starved regions resent the centre (Boadway & Shah 2009).
                            + 0.2f * std::max(0.0f, r.get_fiscal_capacity0() - r.get_fiscal_capacity());
        r.set_drive(std::clamp(drive, 0.0f, 2.0f));
    }
}

// ---------------------------------------------------------------- social capital and demography
// Every 28 days per nation:
//  - social trust (a slow stock; Putnam 1993; Knack & Keefer 1997): falls with
//    rising inequality, unrest and corruption, recovers slowly; it raises tax and
//    NPI compliance (Bargain & Aminjonov 2020);
//  - crime (Becker 1968): property crime rises with unemployment and poverty
//    (Raphael & Winter-Ebmer 2001) and deters investment;
//  - the demographic transition: the birth rate drifts with income per head and
//    education (Bongaarts 1978; Lee 2003); the tertiary share rises with
//    educational expansion (mobility between skill tiers);
//  - CPI per pop: food weighs more in poorer budgets (Engel's law), so food
//    inflation hits the poor harder.
void update_social_capital(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        float unrest = 0.0f, pop = 0.0f;
        for(auto own : n.get_region_ownership()) {
            unrest += own.get_region().get_unrest() * own.get_region().get_population();
            pop += own.get_region().get_population();
        }
        unrest = pop > 0.0f ? unrest / pop : 0.0f;
        const float trust_target = std::clamp(n.get_social_trust0() - static_cast<float>(P.trust_inequality) * (n.get_gini() - n.get_gini0())
                                                  - static_cast<float>(P.trust_unrest) * unrest - 0.1f * std::max(0.0f, n.get_crime() - 1.0f),
                                              0.01f, 0.95f);
        n.set_social_trust(n.get_social_trust() + (trust_target - n.get_social_trust()) * std::min(1.0f, years / static_cast<float>(P.social_trust_years)));
        const float poverty_rise = std::max(0.0f, n.get_poverty() - n.get_poverty0());
        // Police spending above baseline (reforms.cpp) deters crime (Chalfin & McCrary 2017).
        n.set_crime(std::max(0.3f, (1.0f + static_cast<float>(P.crime_unemployment) * std::max(0.0f, n.get_unemployment() - n.get_unemployment0()) + 2.0f * poverty_rise)
                                       * (1.0f - static_cast<float>(P.police_crime) * n.get_police())));
        // Demographic transition.
        const float gdp_pc = static_cast<float>(n.get_gdp_real() * 365.0 / std::max(1.0f, n.get_population()));
        const float inc_rel = n.get_gdp_pc0() > 0.0f ? std::max(0.1f, gdp_pc / n.get_gdp_pc0()) : 1.0f;
        const float edu_rel = n.get_tertiary0() > 0.0f ? std::max(0.1f, n.get_tertiary_share() / n.get_tertiary0()) : 1.0f;
        const float birth_target = n.get_birth_rate0() * std::pow(inc_rel, -static_cast<float>(P.fertility_income)) * std::pow(edu_rel, -static_cast<float>(P.fertility_education));
        n.set_birth_rate(n.get_birth_rate() + (birth_target - n.get_birth_rate()) * std::min(1.0f, years / static_cast<float>(P.fertility_years)));
        n.set_tertiary_share(std::min(0.8f, n.get_tertiary_share() + static_cast<float>(P.tertiary_drift) * years * std::min(1.0f, inc_rel)));
        // CPI per pop (Engel).
        double inc = 0.0, size = 0.0;
        for(auto own : n.get_region_ownership()) {
            for(auto loc : own.get_region().get_pop_location()) {
                inc += std::max(0.0f, loc.get_pop().get_income());
                size += loc.get_pop().get_size();
            }
        }
        const double mean = size > 0.0 ? inc / size : 1.0;
        for(auto own : n.get_region_ownership()) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                const double pc = p.get_size() > 0.0f ? std::max(0.0f, p.get_income()) / p.get_size() : mean;
                const float share = std::clamp(n.get_food_cpi_share() * static_cast<float>(std::pow(std::max(1e-3, pc / mean), -P.engel_elasticity)), 0.0f, 0.8f);
                const float f = n.get_food_price() - 1.0f;
                p.set_cpi_p(n.get_cpi() * (1.0f + share * f) / std::max(0.1f, 1.0f + n.get_food_cpi_share() * f));
            }
        }
    }
}

// ---------------------------------------------------------------- identity groups
// Every 28 days per nation: group income relative to the national mean
// (horizontal inequality; Stewart 2008), its change since the start (relative
// decline), political exclusion (Ethnic Power Relations; Cederman, Wimmer & Min
// 2010; Cederman, Weidmann & Gleditsch 2011) -> group grievance; mobilisation =
// grievance x network cohesion (organisational capacity). Networks also insure:
// members whose income falls below expectation receive transfers from better-off
// members of the same group in the same region (Munshi & Rosenzweig 2016).
void update_groups(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float days = static_cast<float>(monthly_cadence.period_days);
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        double nat_inc = 0.0, nat_size = 0.0;
        for(auto own : n.get_region_ownership()) {
            for(auto loc : own.get_region().get_pop_location()) {
                nat_inc += std::max(0.0f, loc.get_pop().get_income());
                nat_size += loc.get_pop().get_size();
            }
        }
        const double mean = nat_size > 0.0 ? nat_inc / nat_size : 0.0;
        for(auto g : w.in_social_group) {
            if(g.get_nation() != n.id) continue;
            double inc = 0.0, size = 0.0, unemp = 0.0, lf = 0.0;
            for(auto own : n.get_region_ownership()) {
                for(auto loc : own.get_region().get_pop_location()) {
                    auto p = loc.get_pop();
                    if(p.get_group() != g.id) continue;
                    inc += std::max(0.0f, p.get_income());
                    size += p.get_size();
                    if(p.get_kind() < worker_kinds) {
                        unemp += p.get_unemployed();
                        lf += workforce(state, p.id) - p.get_inactive();
                    }
                }
            }
            const float rel = mean > 0.0 && size > 0.0 ? static_cast<float>(inc / size / mean) : 1.0f;
            g.set_income_rel(rel);
            g.set_unemployment(lf > 0.0 ? static_cast<float>(unemp / lf) : 0.0f);
            const float r0 = g.get_income_rel0() > 0.0f ? g.get_income_rel0() : rel;
            const float hw = static_cast<float>(P.horizontal_weight);
            const float grievance = hw * std::max(0.0f, -std::log(std::max(0.05f, rel)))        // standing disadvantage
                                    + 2.0f * hw * std::max(0.0f, std::log(r0 / std::max(0.05f, rel))) // relative decline
                                    + static_cast<float>(P.exclusion_weight) * g.get_excluded();
            g.set_grievance(grievance);
            g.set_mobilisation(static_cast<float>(P.group_mobilisation) * grievance * g.get_network() * (1.0f + g.get_excluded()));
        }
        // Network insurance within (region, group).
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            std::vector<dcon::pop_id> pops;
            for(auto loc : r.get_pop_location()) pops.push_back(loc.get_pop().id);
            for(std::size_t i = 0; i < pops.size(); ++i) {
                const auto pi = pops[i];
                const auto g = w.pop_get_group(pi);
                if(!g || w.social_group_get_network(g) <= 0.0f) continue;
                const double need = static_cast<double>(std::max(0.0f, w.pop_get_income_expected(pi) - w.pop_get_income(pi))) * days
                                    * P.network_insurance * w.social_group_get_network(g);
                if(need <= 0.0) continue;
                double capacity = 0.0;
                for(auto pj : pops) {
                    if(pj != pi && w.pop_get_group(pj) == g && w.pop_get_income(pj) >= w.pop_get_income_expected(pj)) {
                        capacity += std::max(0.0, w.pop_get_balance(pj)) * 0.05;
                    }
                }
                if(capacity <= 0.0) continue;
                const double paid = std::min(need, capacity);
                for(auto pj : pops) {
                    if(pj != pi && w.pop_get_group(pj) == g && w.pop_get_income(pj) >= w.pop_get_income_expected(pj)) {
                        const double part = paid * std::max(0.0, w.pop_get_balance(pj)) * 0.05 / capacity;
                        transfer(state, account::of_pop(n.id, pj), account::of_pop(n.id, pi), part, flow::network_transfers);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------- protest cascades
// Daily per region. Collective action is a threshold process (Granovetter 1978):
// people join when the share already protesting exceeds their personal
// threshold, so participation has tipping points and hysteresis, and the timing
// of an uprising is hard to predict even when its conditions are not (Kuran
// 1991). Participation x solves x = x_max * Phi((drive + s * x - mu) / sigma):
//  - drive: grievance, PSI, group mobilisation (monthly), plus random triggering
//    incidents (a killing on video, a scandal) and censorship backlash;
//  - s: coordination through social media (Steinert-Threlkeld 2017; Enikolopov,
//    Makarin & Petrova 2020), cut by information control (update_ai);
//  - mu: the cost of joining, raised by repression capacity (Davenport 2007);
//  - a sudden rise of information control angers and alerts the connected young
//    (Hobbs & Roberts 2018 "gateway effect"; Hassanpour 2014 on the 2011 Egyptian
//    shutdown), so it can trigger the cascade it meant to stop; gradual, early
//    control builds domestic substitutes and backfires little.
// Participation maps onto the ladder calm (0) .. protests (1) .. strikes (2) ..
// riots (3) .. insurgency / regime crisis (4) (~3.5% of the population active;
// Chenoweth & Stephan 2011). Strikes cut capacity; riots destroy capital.
static float ladder(float x) {
    static constexpr float px[] = {1e-4f, 1e-3f, 5e-3f, 1.5e-2f, 3.5e-2f};
    if(x <= px[0]) return 0.0f;
    for(int i = 1; i < 5; ++i) {
        if(x <= px[i]) {
            const float t = std::log(x / px[i - 1]) / std::log(px[i] / px[i - 1]);
            return static_cast<float>(i - 1) + t;
        }
    }
    return 4.0f;
}

void update_protest(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<uint32_t>(state.current_tick);
    const float spark_decay = std::exp(-1.0f / static_cast<float>(P.spark_days));
    for(auto r : w.in_region) {
        const auto n = r.get_owner();
        const auto ri = static_cast<uint32_t>(r.id.index());
        // Triggering incidents (Poisson), exponentially sized.
        float spark = r.get_spark() * spark_decay;
        if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::unrest), tick, ri, 0) < static_cast<float>(P.spark_rate) / 365.0f) {
            const float u = std::max(1e-6f, core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::unrest), tick, ri, 1));
            spark += static_cast<float>(P.spark_size) * -std::log(u);
        }
        r.set_spark(spark);
        // Censorship backlash: sudden rises only.
        const float jump = state.current_tick > 0 ? std::max(0.0f, n.get_info_control() - n.get_info_control_prev()) : 0.0f;
        const float youth = n.get_youth_share() > 0.0f ? n.get_youth_share() / static_cast<float>(P.youth_bulge_ref) : 1.0f;
        float backfire = r.get_backfire() * spark_decay + static_cast<float>(P.censor_backfire) * jump * n.get_connectivity() * youth;
        r.set_backfire(backfire);

        const float conn = n.get_connectivity() * (1.0f - static_cast<float>(P.censor_effect) * n.get_info_control());
        const float repression = n.get_state_capacity() * (1.0f - std::clamp((n.get_voice() + 2.5f) / 5.0f, 0.0f, 1.0f));
        // Cycles of contention (Tarrow 1998): mobilisation exhausts itself, so a long
        // wave raises the cost of joining until it subsides.
        const float fatigue = r.get_protest_fatigue();
        // Active repression (policy.cpp) deters joining at once; its memory angers
        // later (Davenport 2007; Francisco 1995), more so where people are online.
        const float mu = static_cast<float>(P.cascade_mu) + 0.3f * repression + fatigue
                         + static_cast<float>(P.repression_deterrence) * n.get_repression();
        const float repression_anger = static_cast<float>(P.repression_backlash) * n.get_repression_memory() * (0.5f + n.get_connectivity());
        const float sigma = static_cast<float>(P.cascade_sigma);
        const float social = static_cast<float>(P.cascade_social) * (0.5f + conn);
        // Foreign protest through the internet, and foreign influence operations (internet.cpp).
        const float drive = r.get_drive() + spark + backfire + n.get_net_contagion() + n.get_influence_ops() + repression_anger;
        float x = r.get_protest();
        for(int it = 0; it < 4; ++it) {
            const float z = (drive + social * x - mu) / sigma;
            x = static_cast<float>(P.cascade_max) * 0.5f * std::erfc(-z / std::numbers::sqrt2_v<float>);
        }
        float p = r.get_protest();
        p += (x - p) * std::min(1.0f, 1.0f / static_cast<float>(x > p ? P.unrest_rise_days : P.unrest_decay_days));
        r.set_protest(p);
        r.set_protest_fatigue(fatigue + static_cast<float>(P.protest_fatigue_gain) * p / static_cast<float>(P.cascade_max)
                              - fatigue / static_cast<float>(P.protest_fatigue_days));
        const float u = ladder(p);
        r.set_unrest(u);
        // General strikes cut capacity; riots destroy capital.
        r.set_unrest_capacity(1.0f - static_cast<float>(P.strike_capacity) * std::max(0.0f, u - 2.0f));
        if(u > 3.0f) {
            const float loss = static_cast<float>(P.riot_capital_loss) / 28.0f * (u - 3.0f);
            for(uint8_t s = 0; s < state.derived.sector_count; ++s) r.set_va0(s, r.get_va0(s) * (1.0f - loss));
        }
    }
    for(auto n : w.in_nation) n.set_info_control_prev(n.get_info_control());
}

} // namespace geosim::sim
