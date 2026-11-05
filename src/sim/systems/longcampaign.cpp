// Long-campaign detail (docs/02-simulation-core.md#long-campaign-detail-extension).
//
// Technology diffusion (every 28 days, nation shard). Technologies are data
// (assets/defs/technologies.txt). Adoption is logistic (Griliches 1957); each
// nation lags the frontier by a base lag plus gaps in income, skills and
// openness (Comin & Hobijn 2010). Trend growth already contains the diffusion
// a nation was on in 2019, so productivity effects use only the *deviation*
// from the adoption path implied by the starting lag (a nation that falls behind,
// filters the internet or loses skills adopts later and loses output); physical
// effects (oil per unit of activity, CO2 per unit of energy, connectivity,
// vaccine development time) follow adoption relative to the 2020 start.
//
// Strata split and merge (every 28 days, region shard). Pops are a fixed grid of
// skill kind x identity group until their members diverge: when the jobless share
// of a worker pop rises well above what its job slots imply (a long-term
// unemployed, precarious layer forms after a shock), the excess jobless split off
// into a precarious stratum with their share of savings, weights and job slots.
// When the stratum's jobless share has converged back to its parent's, it merges.
// Sizes, money and job slots are conserved in both directions.


#include "core/keys.hpp"
#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

float logistic(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

struct frontier_info {
    float gdp_pc = 1.0f;
    float tertiary = 0.0f;
};

frontier_info frontier(game_state const& state) {
    frontier_info f;
    for(auto n : state.world.in_nation) {
        const float pc = static_cast<float>(n.get_gdp0() * n.get_gf() / std::max(1.0f, n.get_population()));
        f.gdp_pc = std::max(f.gdp_pc, pc);
        f.tertiary = std::max(f.tertiary, n.get_tertiary_share());
    }
    return f;
}

float adoption_lag(game_state const& state, dcon::technology_id t, dcon::nation_id n, frontier_info const& f) {
    auto const& w = state.world;
    auto const& P = state.params;
    const float pc = static_cast<float>(w.nation_get_gdp_real(n) > 0.0f ? w.nation_get_gdp_real(n) * 365.0 / std::max(1.0f, w.nation_get_population(n))
                                                                         : w.nation_get_gdp0(n) / std::max(1.0f, w.nation_get_population(n)));
    return w.technology_get_lag_years(t) + static_cast<float>(P.tech_income_lag) * std::max(0.0f, std::log(f.gdp_pc / std::max(1.0f, pc)))
           + static_cast<float>(P.tech_skill_lag) * std::max(0.0f, f.tertiary - w.nation_get_tertiary_share(n))
           + static_cast<float>(P.tech_openness_lag) * w.nation_get_net_filtering(n)
           // A carbon price above the start pulls clean technology forward (induced innovation;
           // Acemoglu, Aghion, Bursztyn & Hemous 2012): years per 10 USD/t.
           - (w.technology_get_emission_cut(t) > 0.0f ? 0.5f * std::max(0.0f, w.nation_get_carbon_price(n) - w.nation_get_carbon_price0(n)) / 10.0f : 0.0f);
}

// Calendar year (fractional) of a tick; the scenario starts at its epoch.
float start_year(game_state const& state) {
    return 1970.0f + static_cast<float>(state.epoch_days) / 365.2425f;
}
float year_of(game_state const& state, int32_t tick) {
    return start_year(state) + static_cast<float>(tick) / 365.25f;
}

float adoption(game_state const& state, dcon::technology_id t, float year, float lag) {
    auto const& w = state.world;
    return logistic(w.technology_get_speed(t) * (year - (w.technology_get_frontier_year(t) + lag)));
}

} // namespace

void init_technologies(game_state& state, std::vector<observed_adoption> const& observed) {
    auto& w = state.world;
    const auto T = static_cast<uint32_t>(w.technology_size());
    w.nation_resize_tech_adoption(T);
    w.nation_resize_tech_lag(T);
    w.nation_resize_tech_offset(T);
    const auto f = frontier(state);
    for(auto n : w.in_nation) {
        for(auto t : w.in_technology) {
            const auto ti = static_cast<uint8_t>(t.id.index());
            // The rule's lag at the start; observed 2019 adoption (when given) fixes the
            // starting position instead, and the rule then only moves it.
            float lag = adoption_lag(state, t.id, n.id, f);
            for(auto const& o : observed) {
                if(o.nation == n.id && o.tech == t.id && o.share > 0.0f && o.share < 1.0f) {
                    // Observed adoption is for the year before the start.
                    const float t50 = start_year(state) - 1.0f - std::log(o.share / (1.0f - o.share)) / t.get_speed();
                    lag = t50 - t.get_frontier_year();
                }
            }
            n.set_tech_lag(ti, lag); // the 2019 path (trend growth already contains it)
            n.set_tech_offset(ti, lag - adoption_lag(state, t.id, n.id, f));
            n.set_tech_adoption(ti, adoption(state, t.id, year_of(state, 0), lag));
        }
    }
}

// Adoption of technology t in nation n relative to the start (physical effects).
float tech_progress(game_state const& state, dcon::nation_id n, uint8_t t) {
    auto const& w = state.world;
    const float a0 = adoption(state, dcon::technology_id{t}, start_year(state), w.nation_get_tech_lag(n, t));
    return w.nation_get_tech_adoption(n, t) - a0;
}

float tech_oil_factor(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    float f = 1.0f;
    for(auto t : w.in_technology) {
        if(t.get_oil_cut() > 0.0f) f *= std::max(0.1f, 1.0f - t.get_oil_cut() * tech_progress(state, n, static_cast<uint8_t>(t.id.index())));
    }
    return f;
}

float tech_emission_factor(game_state const& state) {
    auto const& w = state.world;
    double num = 0.0, den = 0.0;
    for(auto n : w.in_nation) {
        float f = 1.0f;
        for(auto t : w.in_technology) {
            if(t.get_emission_cut() > 0.0f) f *= std::max(0.05f, 1.0f - t.get_emission_cut() * tech_progress(state, n.id, static_cast<uint8_t>(t.id.index())));
        }
        num += n.get_oil_demand0() * f;
        den += n.get_oil_demand0();
    }
    return den > 0.0 ? static_cast<float>(num / den) : 1.0f;
}

float tech_vaccine_factor(game_state const& state) {
    auto const& w = state.world;
    float f = 1.0f;
    for(auto t : w.in_technology) {
        if(t.get_vaccine_speedup() <= 0.0f) continue;
        float best = 0.0f; // frontier adoption, relative to the start
        for(auto n : w.in_nation) best = std::max(best, tech_progress(state, n.id, static_cast<uint8_t>(t.id.index())));
        f *= std::max(0.2f, 1.0f - t.get_vaccine_speedup() * best);
    }
    return f;
}

void update_technology(game_state& state) {
    auto& w = state.world;
    const auto tick = static_cast<tick_t>(state.current_tick);
    if(w.technology_size() == 0) return;
    const auto f = frontier(state);
    const float year = year_of(state, state.current_tick);
    const float prev_year = year - static_cast<float>(monthly_cadence.period_days) / 365.25f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        for(auto t : w.in_technology) {
            const auto ti = static_cast<uint8_t>(t.id.index());
            const float lag0 = n.get_tech_lag(ti);
            const float before = n.get_tech_adoption(ti);
            const float now = adoption(state, t.id, year, adoption_lag(state, t.id, n.id, f) + n.get_tech_offset(ti));
            n.set_tech_adoption(ti, now);
            // Productivity: only the deviation from the 2019 adoption path.
            if(t.get_sector() && t.get_sector_gain() != 0.0f) {
                const float dev_now = now - adoption(state, t.id, year, lag0);
                const float dev_prev = before - adoption(state, t.id, prev_year, lag0);
                const float ratio = (1.0f + t.get_sector_gain() * dev_now) / std::max(0.1f, 1.0f + t.get_sector_gain() * dev_prev);
                const auto s = static_cast<uint8_t>(t.get_sector().id.index());
                for(auto own : n.get_region_ownership()) own.get_region().set_va0(s, own.get_region().get_va0(s) * ratio);
                n.set_va0(s, n.get_va0(s) * ratio);
            }
            if(t.get_connectivity_gain() != 0.0f) {
                n.set_connectivity(std::clamp(n.get_connectivity() + t.get_connectivity_gain() * (now - before), 0.0f, 1.0f));
            }
        }
    }
}

// ---------------------------------------------------------------- strata
namespace {

float jobless_share(game_state const& state, dcon::pop_id p) {
    auto const& w = state.world;
    double employed = 0.0;
    for(uint8_t s = 0; s < state.derived.sector_count; ++s) employed += w.pop_get_employed(p, s);
    const double jobless = w.pop_get_unemployed(p) + w.pop_get_inactive(p);
    return employed + jobless > 0.0 ? static_cast<float>(jobless / (employed + jobless)) : 0.0f;
}

// Moves a share `f` of pop `from`'s jobless members (with their share of job slots,
// savings and income weights) into pop `to`. Conserves everything it moves.
void move_jobless(game_state& state, dcon::pop_id from, dcon::pop_id to, float people) {
    auto& w = state.world;
    const uint8_t S = state.derived.sector_count;
    const float jobless = w.pop_get_unemployed(from) + w.pop_get_inactive(from);
    if(jobless <= 0.0f || people <= 0.0f) return;
    const float take = std::min(people, jobless);
    const float share_u = w.pop_get_unemployed(from) / jobless;
    const float size = std::max(1.0f, w.pop_get_size(from));
    const float frac = take / size;
    w.pop_set_unemployed(from, w.pop_get_unemployed(from) - take * share_u);
    w.pop_set_inactive(from, w.pop_get_inactive(from) - take * (1.0f - share_u));
    w.pop_set_unemployed(to, w.pop_get_unemployed(to) + take * share_u);
    w.pop_set_inactive(to, w.pop_get_inactive(to) + take * (1.0f - share_u));
    for(uint8_t s = 0; s < S; ++s) {
        const float slots = w.pop_get_emp0(from, s) * frac;
        w.pop_set_emp0(from, s, w.pop_get_emp0(from, s) - slots);
        w.pop_set_emp0(to, s, w.pop_get_emp0(to, s) + slots);
        const float slow = w.pop_get_need_slow(from, s) * frac;
        w.pop_set_need_slow(from, s, w.pop_get_need_slow(from, s) - slow);
        w.pop_set_need_slow(to, s, w.pop_get_need_slow(to, s) + slow);
    }
    w.pop_set_size(from, w.pop_get_size(from) - take);
    w.pop_set_size(to, w.pop_get_size(to) + take);
    const auto n = w.pop_get_nation(from);
    transfer(state, account::of_pop(n, from), account::of_pop(n, to), w.pop_get_balance(from) * frac, flow::household_transfers);
    const double trend = w.pop_get_balance_trend(from) * frac;
    w.pop_set_balance_trend(from, w.pop_get_balance_trend(from) - trend);
    w.pop_set_balance_trend(to, w.pop_get_balance_trend(to) + trend);
    const float owner = w.pop_get_owner_weight(from) * frac;
    w.pop_set_owner_weight(from, w.pop_get_owner_weight(from) - owner);
    w.pop_set_owner_weight(to, w.pop_get_owner_weight(to) + owner);
    const float transfer_w = w.pop_get_transfer_weight(from) * frac;
    w.pop_set_transfer_weight(from, w.pop_get_transfer_weight(from) - transfer_w);
    w.pop_set_transfer_weight(to, w.pop_get_transfer_weight(to) + transfer_w);
    const float cd = w.pop_get_c_des(from) * frac;
    w.pop_set_c_des(from, w.pop_get_c_des(from) - cd);
    w.pop_set_c_des(to, w.pop_get_c_des(to) + cd);
}

dcon::pop_id create_stratum(game_state& state, dcon::pop_id parent) {
    auto& w = state.world;
    const auto p = w.create_pop();
    w.pop_set_kind(p, w.pop_get_kind(parent));
    w.pop_set_is_worker(p, 1.0f);
    w.pop_set_nation(p, w.pop_get_nation(parent));
    w.pop_set_home(p, w.pop_get_home(parent));
    w.pop_set_shard(p, w.pop_get_shard(parent));
    w.pop_set_group(p, w.pop_get_group(parent));
    w.pop_set_stratum(p, static_cast<uint8_t>(stratum::precarious));
    w.pop_set_parent(p, parent);
    for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
        w.pop_set_wage0(p, s, w.pop_get_wage0(parent, s));
        w.pop_set_hours(p, s, 1.0f);
    }
    w.pop_set_wage_avg0(p, w.pop_get_wage_avg0(parent));
    w.pop_set_wage_mult(p, w.pop_get_wage_mult(parent));
    w.pop_set_informal(p, w.pop_get_informal(parent));
    w.pop_set_formal_rel(p, w.pop_get_formal_rel(parent));
    w.pop_set_network(p, w.pop_get_network(parent));
    w.pop_set_tax_factor(p, w.pop_get_tax_factor(parent) * 0.8f); // lower incomes, lower average rate
    w.pop_set_cpi_p(p, w.pop_get_cpi_p(parent));
    w.pop_set_saving_rate0(p, w.pop_get_saving_rate0(parent));
    w.pop_set_inc_sigma(p, w.pop_get_inc_sigma(parent));
    w.pop_set_owner_share(p, w.pop_get_owner_share(parent));
    w.pop_set_transfer_share(p, w.pop_get_transfer_share(parent));
    const float per_head = w.pop_get_size(parent) > 0.0f ? 1.0f / w.pop_get_size(parent) : 0.0f;
    w.pop_set_income_perm(p, w.pop_get_income_perm(parent) * per_head);
    w.pop_set_income0(p, w.pop_get_income0(parent) * per_head);
    w.pop_set_income_expected(p, w.pop_get_income_expected(parent) * per_head);
    for(uint8_t a = 0; a < static_cast<uint8_t>(value_axis::count); ++a) w.pop_set_values(p, a, w.pop_get_values(parent, a));
    for(uint8_t i = 0; i < static_cast<uint8_t>(issue::count); ++i) w.pop_set_salience(p, i, w.pop_get_salience(parent, i));
    for(uint8_t j = 0; j < w.pop_get_party_support_size(); ++j) w.pop_set_party_support(p, j, w.pop_get_party_support(parent, j));
    w.force_create_pop_location(p, w.pop_get_home(parent));
    return p;
}

} // namespace

void update_strata(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    std::vector<dcon::pop_id> base, strata;
    for(auto p : w.in_pop) {
        if(!is_due(static_cast<uint32_t>(p.get_home().id.index()), tick, monthly_cadence) || p.get_kind() >= worker_kinds) continue;
        (p.get_stratum() == static_cast<uint8_t>(stratum::base) ? base : strata).push_back(p.id);
    }
    // Merge first (a converged stratum folds back into its parent).
    for(auto c : strata) {
        const auto parent = w.pop_get_parent(c);
        if(!parent || !w.pop_is_valid(parent)) continue;
        if(std::abs(jobless_share(state, c) - jobless_share(state, parent)) > static_cast<float>(P.merge_jobless_gap)) continue;
        const uint8_t S = state.derived.sector_count;
        for(uint8_t s = 0; s < S; ++s) {
            w.pop_set_employed(parent, s, w.pop_get_employed(parent, s) + w.pop_get_employed(c, s));
            w.pop_set_emp0(parent, s, w.pop_get_emp0(parent, s) + w.pop_get_emp0(c, s));
            w.pop_set_need_slow(parent, s, w.pop_get_need_slow(parent, s) + w.pop_get_need_slow(c, s));
        }
        w.pop_set_unemployed(parent, w.pop_get_unemployed(parent) + w.pop_get_unemployed(c));
        w.pop_set_inactive(parent, w.pop_get_inactive(parent) + w.pop_get_inactive(c));
        w.pop_set_size(parent, w.pop_get_size(parent) + w.pop_get_size(c));
        w.pop_set_owner_weight(parent, w.pop_get_owner_weight(parent) + w.pop_get_owner_weight(c));
        w.pop_set_transfer_weight(parent, w.pop_get_transfer_weight(parent) + w.pop_get_transfer_weight(c));
        w.pop_set_c_des(parent, w.pop_get_c_des(parent) + w.pop_get_c_des(c));
        const auto n = w.pop_get_nation(c);
        transfer(state, account::of_pop(n, c), account::of_pop(n, parent), w.pop_get_balance(c), flow::household_transfers);
        w.pop_set_balance_trend(parent, w.pop_get_balance_trend(parent) + w.pop_get_balance_trend(c));
        w.delete_pop(c);
    }
    // Split: a large excess of jobless members forms a precarious stratum. Parents that
    // already have one are found in one pass (a scan of every pop per candidate was
    // quadratic in a world-scale downturn).
    std::vector<uint8_t> has_stratum;
    if(!base.empty()) {
        has_stratum.assign(w.pop_size(), 0);
        for(auto c : w.in_pop) {
            const auto parent = c.get_parent();
            if(parent && c.get_stratum() == static_cast<uint8_t>(stratum::precarious) && static_cast<std::size_t>(parent.id.index()) < has_stratum.size()) has_stratum[static_cast<std::size_t>(parent.id.index())] = 1;
        }
    }
    for(auto p : base) {
        if(w.pop_get_size(p) < static_cast<float>(P.split_min_size)) continue;
        // Jobless share beyond the nation's natural rate.
        const float gap = jobless_share(state, p) - w.nation_get_unemployment0(w.pop_get_nation(p));
        if(gap < static_cast<float>(P.split_jobless_gap)) continue;
        if(has_stratum[static_cast<std::size_t>(p.index())]) continue;
        double force = w.pop_get_unemployed(p) + w.pop_get_inactive(p);
        for(uint8_t s = 0; s < state.derived.sector_count; ++s) force += w.pop_get_employed(p, s);
        const auto child = create_stratum(state, p);
        move_jobless(state, p, child, static_cast<float>(gap * force));
        has_stratum[static_cast<std::size_t>(p.index())] = 1;
    }
}


} // namespace geosim::sim
