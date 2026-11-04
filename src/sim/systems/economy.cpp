// Economy step for one nation (docs/03-economy.md).
//
// Order inside a step: capacity (NPIs, sickness, supply chains, oil) ->
// demand (households from permanent income, government, investment
// accelerator, exports from partners' imports) -> output = min(demand,
// capacity) -> labour (layoffs, hoarding, job retention, matching) -> prices
// -> income and ledger flows. All money moves through sim::transfer, so the
// SFC invariant holds by construction.

#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace geosim::sim {

namespace {

constexpr int max_sectors = 32;
constexpr int max_pops = 16;

struct pop_set {
    std::array<dcon::pop_id, max_pops> id{};
    int count = 0;
};

pop_set pops_of(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    pop_set out;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            if(out.count < max_pops) {
                out.id[static_cast<std::size_t>(out.count++)] = loc.get_pop().id;
            }
        }
    }
    return out;
}

float clamp01(float x) noexcept {
    return std::clamp(x, 0.0f, 1.0f);
}

float wage_mult(params const& P, uint8_t kind) noexcept {
    switch(kind) {
    case 0:
        return static_cast<float>(P.wage_low);
    case 1:
        return static_cast<float>(P.wage_mid);
    default:
        return static_cast<float>(P.wage_high);
    }
}

// Partner-weighted index of foreign import demand (1 = baseline).
float world_import_index(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    double num = 0.0;
    double den = 0.0;
    for(auto m : w.in_nation) {
        if(m.id == n) {
            continue;
        }
        const double base = static_cast<double>(m.get_m0()) * growth_factor(state, m.id);
        if(base <= 0.0) {
            continue;
        }
        num += m.get_imports();
        den += base;
    }
    return den > 0.0 ? static_cast<float>(num / den) : 1.0f;
}

// Inbound travel (tourism) relative to normal: own entry controls times the
// average travel controls of everyone else (UNWTO: arrivals -73% in 2020).
float travel_index(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const auto k = state.derived.npi_travel;
    if(k == 0xFF) {
        return 1.0f;
    }
    double num = 0.0;
    double den = 0.0;
    for(auto m : w.in_nation) {
        if(m.id != n) {
            num += static_cast<double>(m.get_population()) * npi_fraction(state, m.id, k);
            den += m.get_population();
        }
    }
    const float others = den > 0.0 ? static_cast<float>(num / den) : 0.0f;
    const auto& P = state.params;
    return (1.0f - static_cast<float>(P.tourism_own_border) * npi_fraction(state, n, k))
           * (1.0f - static_cast<float>(P.tourism_world_border) * others);
}

// Export-weighted manufacturing capacity of partners (1 = normal).
float world_manufacturing_index(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const auto man = state.derived.manufacturing_sector;
    if(!man) {
        return 1.0f;
    }
    const auto s = static_cast<uint8_t>(man.index());
    double num = 0.0;
    double den = 0.0;
    for(auto m : w.in_nation) {
        if(m.id == n) {
            continue;
        }
        const double pot = static_cast<double>(m.get_va0(s)) * growth_factor(state, m.id);
        if(pot <= 0.0) {
            continue;
        }
        const double weight = m.get_x0();
        num += weight * std::min(1.0, static_cast<double>(m.get_capacity(s)) / pot);
        den += weight;
    }
    return den > 0.0 ? static_cast<float>(num / den) : 1.0f;
}

} // namespace

void update_economy(game_state& state, dcon::nation_id n, float dt, economy_mode mode) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const bool apply = mode == economy_mode::simulate;
    const uint8_t S = D.sector_count;
    const pop_set pops = pops_of(state, n);

    const float p = w.nation_get_core_price(n);
    const float gf = growth_factor(state, n);
    const float r = w.nation_get_policy_rate(n);
    const float r0 = w.nation_get_policy_rate0(n);
    const float m_share = w.nation_get_import_share(n);
    const float u0 = w.nation_get_unemployment0(n);
    const float u = w.nation_get_unemployment(n);

    // ---------------------------------------------------------------- epidemic linkage
    float young_inf = 0.0f;
    float young_n = 0.0f;
    for(auto own : w.nation_get_region_ownership(n)) {
        auto reg = own.get_region();
        young_inf += reg.get_ep_i(age_young);
        young_n += reg.get_ep_s(age_young) + reg.get_ep_e(age_young) + reg.get_ep_i(age_young) + reg.get_ep_r(age_young);
    }
    const float absent = young_n > 0.0f ? static_cast<float>(P.sickness_absence) * young_inf / young_n : 0.0f;
    const float deaths_pm = reported_deaths_per_million_daily(state, n);
    // Consumers' response to deaths faded strongly over 2020 (behavioural fatigue).
    const float fear_fatigue = static_cast<float>(P.fear_fatigue_max) * std::min(1.0f, w.nation_get_npi_exposure(n) / static_cast<float>(P.fatigue_days));
    const float fear = 1.0f / (1.0f + static_cast<float>(P.voluntary_demand_k) * (1.0f - fear_fatigue) * deaths_pm);

    // ---------------------------------------------------------------- capacity
    std::array<float, max_sectors> pot{};
    std::array<float, max_sectors> cap{};
    const float man_index = apply ? world_manufacturing_index(state, n) : 1.0f;
    const float prod0 = w.nation_get_oil_production0(n);
    const float oil_ratio = prod0 > 0.0f ? w.nation_get_oil_production(n) / prod0 : 1.0f;
    const float oil_share = w.nation_get_oil_share_of_energy(n);
    // Business adaptation: the output cost of a given stringency fell by about half
    // between the first and second waves (OECD Economic Outlook, Dec 2020).
    const float adaptation = static_cast<float>(P.adaptation_max)
                             * std::min(1.0f, w.nation_get_npi_exposure(n) / static_cast<float>(P.adaptation_days));
    for(uint8_t s = 0; s < S; ++s) {
        const dcon::sector_id sid{s};
        pot[s] = w.nation_get_va0(n, s) * gf;
        float cut = 0.0f;
        for(uint8_t k = 0; k < D.npi_count; ++k) {
            // Regional (targeted) measures close only part of the national economy.
            // Under a suppression strategy, measures are confined to small areas.
            const float local = static_cast<float>(P.npi_targeted_reach) * (1.0f - 0.5f * w.nation_get_tti_strength(n));
            const float reach = 1.0f - (1.0f - local) * w.nation_get_npi_targeted(n, k);
            cut += w.sector_get_npi_capacity(sid, k) * npi_fraction(state, n, k) * reach;
        }
        cap[s] = pot[s] * (1.0f - std::min(0.95f, cut * (1.0f - adaptation))) * (1.0f - absent);
        if(sid == D.manufacturing_sector) {
            cap[s] *= 1.0f - static_cast<float>(P.supply_chain_share) * (1.0f - man_index);
        }
    }

    // ---------------------------------------------------------------- demand
    const float conf = std::max(0.5f, 1.0f - static_cast<float>(P.precautionary) * std::max(0.0f, u - u0));
    const float rate_c = 1.0f - static_cast<float>(P.rate_c_elasticity) * (r - r0);
    std::array<float, max_pops> c_des{};
    double c_des_total = 0.0;
    for(int i = 0; i < pops.count; ++i) {
        const auto pid = pops.id[static_cast<std::size_t>(i)];
        // Permanent income is smoothed in real terms (base-year prices), so inflation
        // does not by itself erode planned consumption.
        float inc_perm = w.pop_get_income_perm(pid);
        if(apply) {
            const float a = std::min(1.0f, dt / static_cast<float>(P.income_smoothing_days));
            inc_perm += (w.pop_get_income(pid) / p - inc_perm) * a;
            w.pop_set_income_perm(pid, inc_perm);
        }
        const double excess = w.pop_get_balance(pid) - w.pop_get_balance_trend(pid);
        const float wealth = static_cast<float>(std::max(0.0, excess) * P.wealth_spend_rate / 365.0);
        c_des[static_cast<std::size_t>(i)] = std::max(0.0f, (1.0f - w.pop_get_saving_rate0(pid)) * inc_perm * p * conf * rate_c + wealth);
        c_des_total += c_des[static_cast<std::size_t>(i)];
    }
    const float c_real = static_cast<float>(c_des_total) / p;
    const float hh_dom = (1.0f - m_share) * c_real;
    const float g_real = w.nation_get_g0(n) * gf;
    const float accel = std::clamp(1.0f + static_cast<float>(P.accelerator) * (w.nation_get_util_smoothed(n) - 1.0f),
                                   static_cast<float>(P.investment_floor), 1.3f);
    const float i_real = w.nation_get_i0(n) * gf * accel * (1.0f - static_cast<float>(P.rate_i_elasticity) * (r - r0));
    const float x_real = w.nation_get_x0(n) * gf * (apply ? world_import_index(state, n) : 1.0f);
    const float tourism = apply ? travel_index(state, n) : 1.0f;

    std::array<float, max_sectors> hh{};
    std::array<float, max_sectors> dem{};
    std::array<float, max_sectors> out{};
    float blocked_contact = 0.0f;
    for(uint8_t s = 0; s < S; ++s) {
        const dcon::sector_id sid{s};
        hh[s] = w.nation_get_comp_c(n, s) * hh_dom;
        const float vd = w.sector_get_voluntary_distancing(sid);
        const float avoided = hh[s] * vd * (1.0f - fear);
        hh[s] -= avoided;
        blocked_contact += avoided;
        // Exports of contact services are mostly tourism: they follow travel, not trade.
        const float x_factor = vd > 0.0f ? 1.0f - static_cast<float>(P.tourism_share) * (1.0f - tourism) : 1.0f;
        dem[s] = hh[s] + w.nation_get_comp_g(n, s) * g_real + w.nation_get_comp_i(n, s) * (1.0f - m_share) * i_real
                 + w.nation_get_comp_x(n, s) * (1.0f - m_share) * x_real * x_factor;
        out[s] = std::min(dem[s], cap[s]);
        if(vd > 0.0f && dem[s] > 0.0f) {
            blocked_contact += (dem[s] - out[s]) * hh[s] / dem[s];
        }
    }
    if(D.manufacturing_sector) {
        const auto ms = static_cast<uint8_t>(D.manufacturing_sector.index());
        const float redirect = static_cast<float>(P.substitution) * blocked_contact;
        hh[ms] += redirect;
        dem[ms] += redirect;
        out[ms] = std::min(dem[ms], cap[ms]);
    }
    if(D.energy_sector && prod0 > 0.0f) {
        const auto es = static_cast<uint8_t>(D.energy_sector.index());
        const float dr = pot[es] > 0.0f ? std::min(1.0f, dem[es] / pot[es]) : 1.0f;
        out[es] = pot[es] * ((1.0f - oil_share) * dr + oil_share * oil_ratio);
    }

    float hh_real = 0.0f;
    float gdp = 0.0f;
    float pot_total = 0.0f;
    float hh_gov_real = 0.0f;
    for(uint8_t s = 0; s < S; ++s) {
        const float fill = dem[s] > 0.0f ? std::min(1.0f, out[s] / dem[s]) : 1.0f;
        hh_real += hh[s] * fill;
        if(dcon::sector_id{s} == D.gov_sector) {
            hh_gov_real = hh[s] * fill;
        }
        gdp += out[s];
        pot_total += pot[s];
    }
    const float c_eff = hh_real / (1.0f - m_share);
    // Imports follow domestic demand weighted by import intensity (goods > services), so a
    // shift from services to goods raises imports even when total spending falls.
    float import_demand = 0.0f;
    for(uint8_t s = 0; s < S; ++s) {
        import_demand += out[s] * w.sector_get_import_intensity(dcon::sector_id{s});
    }
    const float base = w.nation_get_import_base(n) * gf;
    const float imports = w.nation_get_m0(n) * gf * (base > 0.0f ? import_demand / base : 1.0f);
    const float util = pot_total > 0.0f ? gdp / pot_total : 1.0f;

    // ---------------------------------------------------------------- labour
    std::array<float, max_sectors> hours{};
    const float coverage = w.nation_get_stw_coverage(n);
    const float epl = w.nation_get_epl(n);
    const float informality = w.nation_get_informality(n);
    double private_wage_bill = 0.0;
    for(uint8_t s = 0; s < S; ++s) {
        hours[s] = w.nation_get_hours(n, s);
    }
    if(apply) {
        // Firm support (wage subsidies, PPP-style loans) slows layoffs.
        double base_bill = 0.0;
        for(uint8_t s = 0; s < S; ++s) {
            if(dcon::sector_id{s} != D.gov_sector) {
                base_bill += static_cast<double>(w.sector_get_labor_share(dcon::sector_id{s})) * pot[s] * p;
            }
        }
        const float support = base_bill > 0.0 ? static_cast<float>(w.nation_get_firm_support_rate(n) / base_bill) : 0.0f;
        const float support_cut = std::max(0.3f, 1.0f - static_cast<float>(P.firm_support_effect) * support);
        // Laid-off workers who cannot search (informal economy, stay-at-home orders) are
        // counted as out of the labour force rather than unemployed.
        const float stay = D.npi_stay_home != 0xFF ? npi_fraction(state, n, D.npi_stay_home) : 0.0f;
        const float exit = clamp01(static_cast<float>(P.lf_exit_base + P.lf_exit_informal * informality + P.lf_exit_lockdown * stay));

        float unemployed_total = 0.0f;
        for(int i = 0; i < pops.count; ++i) {
            const auto pid = pops.id[static_cast<std::size_t>(i)];
            if(w.pop_get_kind(pid) < worker_kinds) {
                unemployed_total += w.pop_get_unemployed(pid);
            }
        }
        for(uint8_t s = 0; s < S; ++s) {
            const dcon::sector_id sid{s};
            float emp = 0.0f;
            for(int i = 0; i < pops.count; ++i) {
                emp += w.pop_get_employed(pops.id[static_cast<std::size_t>(i)], s);
            }
            if(emp <= 0.0f) {
                continue;
            }
            if(sid == D.gov_sector) {
                hours[s] = 1.0f; // public employment is held
                continue;
            }
            const float productivity = w.nation_get_va0(n, s) / std::max(1.0f, w.nation_get_emp0(n, s)) * gf;
            const float need = out[s] / productivity;
            // Layoff decisions follow persistent drops in labour demand (notice periods,
            // employment protection); recalls react to current demand.
            const float tau = static_cast<float>(P.layoff_lag_days) * (1.0f + static_cast<float>(P.layoff_lag_epl) * epl);
            float need_slow = w.nation_get_labor_need(n, s);
            need_slow += (need - need_slow) * std::min(1.0f, dt / tau);
            w.nation_set_labor_need(n, s, need_slow);
            const float base_emp = w.nation_get_emp0(n, s);
            const float layoff_need = std::max(need, need_slow);
            if(layoff_need < emp) {
                // Job retention and employment protection keep a share of the pre-crisis
                // jobs attached (on reduced hours) for as long as they last.
                const float absorb = coverage + (1.0f - coverage) * epl * static_cast<float>(P.epl_hoarding);
                const float target = std::min(emp, layoff_need + absorb * std::max(0.0f, base_emp - layoff_need));
                const float speed = static_cast<float>(P.layoff_speed) * (1.0f - 0.5f * epl) * support_cut;
                const float layoffs = (emp - target) * std::min(1.0f, speed * dt / 7.0f);
                for(int i = 0; i < pops.count; ++i) {
                    const auto pid = pops.id[static_cast<std::size_t>(i)];
                    const float e = w.pop_get_employed(pid, s);
                    const float x = layoffs * e / emp;
                    w.pop_set_employed(pid, s, e - x);
                    w.pop_set_unemployed(pid, w.pop_get_unemployed(pid) + x * (1.0f - exit));
                    w.pop_set_inactive(pid, w.pop_get_inactive(pid) + x * exit);
                    unemployed_total += x * (1.0f - exit);
                }
                emp -= layoffs;
            } else if(need > emp && unemployed_total > 0.0f) {
                const float vacancies = need - emp;
                const float hires = std::min({vacancies, unemployed_total,
                    static_cast<float>(P.match_efficiency) * std::sqrt(unemployed_total * vacancies) * dt / 7.0f});
                float weight_total = 0.0f;
                for(int i = 0; i < pops.count; ++i) {
                    const auto pid = pops.id[static_cast<std::size_t>(i)];
                    const auto k = w.pop_get_kind(pid);
                    if(k < worker_kinds) {
                        weight_total += w.sector_get_skill(sid, k) * w.pop_get_unemployed(pid);
                    }
                }
                if(weight_total > 0.0f) {
                    for(int i = 0; i < pops.count; ++i) {
                        const auto pid = pops.id[static_cast<std::size_t>(i)];
                        const auto k = w.pop_get_kind(pid);
                        if(k >= worker_kinds) {
                            continue;
                        }
                        const float un = w.pop_get_unemployed(pid);
                        const float x = std::min(un, hires * w.sector_get_skill(sid, k) * un / weight_total);
                        w.pop_set_unemployed(pid, un - x);
                        w.pop_set_employed(pid, s, w.pop_get_employed(pid, s) + x);
                        unemployed_total -= x;
                        emp += x;
                    }
                }
            }
            hours[s] = std::clamp(need / std::max(1.0f, emp), 0.05f, 1.0f);
        }
        // Crisis-inactive people drift back into job search.
        float employed_total = 0.0f;
        unemployed_total = 0.0f;
        for(int i = 0; i < pops.count; ++i) {
            const auto pid = pops.id[static_cast<std::size_t>(i)];
            if(w.pop_get_kind(pid) >= worker_kinds) {
                continue;
            }
            const float back = w.pop_get_inactive(pid) * std::min(1.0f, static_cast<float>(P.lf_return_rate) * dt / 7.0f);
            w.pop_set_inactive(pid, w.pop_get_inactive(pid) - back);
            w.pop_set_unemployed(pid, w.pop_get_unemployed(pid) + back);
            unemployed_total += w.pop_get_unemployed(pid);
            for(uint8_t s = 0; s < S; ++s) {
                employed_total += w.pop_get_employed(pid, s);
            }
        }
        const float lf = employed_total + unemployed_total;
        w.nation_set_unemployment(n, lf > 0.0f ? unemployed_total / lf : 0.0f);
        for(uint8_t s = 0; s < S; ++s) {
            w.nation_set_hours(n, s, hours[s]);
        }
    }

    // ---------------------------------------------------------------- prices
    if(apply) {
        const float gap = pot_total > 0.0f ? gdp / pot_total - 1.0f : 0.0f;
        float gs = w.nation_get_gap_smoothed(n);
        gs += (gap - gs) * std::min(1.0f, dt / static_cast<float>(P.gap_smoothing_days));
        w.nation_set_gap_smoothed(n, gs);
        float us = w.nation_get_util_smoothed(n);
        us += (util - us) * std::min(1.0f, dt / static_cast<float>(P.util_smoothing_days));
        w.nation_set_util_smoothed(n, us);

        float pe = w.nation_get_inflation_expect(n);
        const float pi = pe + static_cast<float>(P.phillips) * gs;
        const float core = p * std::exp(std::log1p(pi) * dt / 365.0f);
        pe += (pi - pe) * std::min(1.0f, dt / static_cast<float>(P.expectations_days));
        w.nation_set_inflation_expect(n, pe);
        w.nation_set_core_price(n, core);
        const auto oil = D.oil;
        const float oil_rel = oil ? w.commodity_get_price(oil) / w.commodity_get_price0(oil) : 1.0f;
        w.nation_set_cpi(n, core * (1.0f + static_cast<float>(P.oil_cpi_share) * (oil_rel - 1.0f)));
    }

    // ---------------------------------------------------------------- incomes and ledger
    // Average wage of each skill tier (benefits are a share of it).
    std::array<float, worker_kinds> avg_wage{};
    {
        std::array<double, worker_kinds> num{};
        std::array<double, worker_kinds> den{};
        for(uint8_t s = 0; s < S; ++s) {
            const dcon::sector_id sid{s};
            double mix = 0.0;
            for(uint8_t k = 0; k < worker_kinds; ++k) {
                mix += w.sector_get_skill(sid, k) * wage_mult(P, k);
            }
            const float e0 = std::max(1.0f, w.nation_get_emp0(n, s));
            const double base = w.sector_get_labor_share(sid) * w.nation_get_va0(n, s) / e0 * gf * p / std::max(1e-6, mix);
            for(uint8_t k = 0; k < worker_kinds; ++k) {
                const double weight = static_cast<double>(e0) * w.sector_get_skill(sid, k);
                num[k] += weight * base * wage_mult(P, k);
                den[k] += weight;
            }
        }
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            avg_wage[k] = den[k] > 0.0 ? static_cast<float>(num[k] / den[k]) : 0.0f;
        }
    }

    // OxCGRT E1 (income support) also records wage-subsidy schemes; where a job-retention
    // scheme covers workers, only the uncovered share boosts unemployment benefits.
    const float e1 = (D.npi_income_support != 0xFF ? npi_fraction(state, n, D.npi_income_support) : 0.0f)
                     * (1.0f - w.nation_get_stw_coverage(n));
    const float repl = std::min(1.2f, w.nation_get_benefit_replacement(n) + static_cast<float>(P.e1_replacement_boost) * e1);
    const float cov = std::min(1.0f, w.nation_get_benefit_coverage(n) + static_cast<float>(P.e1_coverage_boost) * e1);
    const float stw_repl = w.nation_get_stw_replacement(n);
    const float pensions = w.nation_get_pensions0(n) * p * gf;

    // Fiscal packages: announced money first funds the crisis programmes paid in
    // this step (job retention, the income-support top-up of benefits); the rest
    // is disbursed gradually as transfers to households and support to firms.
    double fiscal_hh = 0.0;
    double fiscal_firms = 0.0;
    if(apply) {
        double programmes = 0.0;
        for(int i = 0; i < pops.count; ++i) {
            const auto pid = pops.id[static_cast<std::size_t>(i)];
            const auto k = w.pop_get_kind(pid);
            if(k >= worker_kinds) {
                continue;
            }
            const double base_benefit = w.nation_get_benefit_replacement(n) * w.nation_get_benefit_coverage(n);
            programmes += static_cast<double>(w.pop_get_unemployed(pid)) * avg_wage[k] * std::max(0.0, static_cast<double>(repl * cov) - base_benefit);
            if(coverage > 0.0f) {
                for(uint8_t s = 0; s < S; ++s) {
                    if(dcon::sector_id{s} == D.gov_sector) {
                        continue;
                    }
                    const dcon::sector_id sid{s};
                    double mix = 0.0;
                    for(uint8_t kk = 0; kk < worker_kinds; ++kk) {
                        mix += w.sector_get_skill(sid, kk) * wage_mult(P, kk);
                    }
                    const double wage = w.sector_get_labor_share(sid) * w.nation_get_va0(n, s) / std::max(1.0f, w.nation_get_emp0(n, s)) * gf * p
                                        * wage_mult(P, k) / std::max(1e-6, mix);
                    programmes += static_cast<double>(w.pop_get_employed(pid, s)) * (1.0 - hours[s]) * wage * stw_repl * coverage;
                }
            }
        }
        double pending = w.nation_get_fiscal_pending(n);
        pending -= std::min(pending, programmes * dt);
        const double paid = pending * std::min(1.0, static_cast<double>(dt) / P.fiscal_disburse_days);
        w.nation_set_fiscal_pending(n, pending - paid);
        fiscal_firms = paid * P.fiscal_firm_share;
        fiscal_hh = paid - fiscal_firms;
        w.nation_set_firm_support_rate(n, static_cast<float>(fiscal_firms / dt));
    }

    // Nominal value added by firms (oil-price adjusted for producers).
    const auto oil = D.oil;
    const float oil_price = oil ? w.commodity_get_price(oil) : 0.0f;
    const float oil_rel = oil ? oil_price / w.commodity_get_price0(oil) : 1.0f;
    double firm_va = 0.0;
    for(uint8_t s = 0; s < S; ++s) {
        const dcon::sector_id sid{s};
        if(sid == D.gov_sector) {
            continue;
        }
        double v = static_cast<double>(out[s]) * p;
        if(sid == D.energy_sector && prod0 > 0.0f) {
            v *= (1.0f - oil_share) + oil_share * oil_rel;
        }
        firm_va += v;
    }

    const double c_nom = static_cast<double>(c_eff) * p;
    const double fees_nom = static_cast<double>(hh_gov_real) * p; // public services bought by households
    const double c_to_firms = std::max(0.0, c_nom - fees_nom);
    const double indirect = w.nation_get_tax_indirect(n) * c_to_firms;
    const double oil_value = static_cast<double>(w.nation_get_oil_production(n)) * 1e6 * oil_price;
    const double resource = w.nation_get_resource_tax(n) * oil_value;

    // Per-pop receipts (per day).
    std::array<double, max_pops> work{};
    std::array<double, max_pops> wages_pub{};
    std::array<double, max_pops> stw{};
    std::array<double, max_pops> benefit{};
    std::array<double, max_pops> transfers{};
    for(int i = 0; i < pops.count; ++i) {
        const auto pid = pops.id[static_cast<std::size_t>(i)];
        const auto k = w.pop_get_kind(pid);
        if(k < worker_kinds) {
            for(uint8_t s = 0; s < S; ++s) {
                const dcon::sector_id sid{s};
                double mix = 0.0;
                for(uint8_t kk = 0; kk < worker_kinds; ++kk) {
                    mix += w.sector_get_skill(sid, kk) * wage_mult(P, kk);
                }
                const float e0 = std::max(1.0f, w.nation_get_emp0(n, s));
                const double wage = w.sector_get_labor_share(sid) * w.nation_get_va0(n, s) / e0 * gf * p
                                    * wage_mult(P, k) / std::max(1e-6, mix);
                const double e = w.pop_get_employed(pid, s);
                const double h = hours[s];
                if(sid == D.gov_sector) {
                    wages_pub[static_cast<std::size_t>(i)] += e * wage;
                } else {
                    work[static_cast<std::size_t>(i)] += e * h * wage;
                    private_wage_bill += e * h * wage;
                    if(coverage > 0.0f) {
                        // Only the scheme's take-up share of lost hours is paid.
                        stw[static_cast<std::size_t>(i)] += e * (1.0 - h) * wage * stw_repl * coverage;
                    }
                }
            }
            benefit[static_cast<std::size_t>(i)] = static_cast<double>(w.pop_get_unemployed(pid)) * avg_wage[k] * repl * cov;
        }
        transfers[static_cast<std::size_t>(i)] = static_cast<double>(pensions) * w.pop_get_transfer_share(pid);
    }

    const double gov_purchases = static_cast<double>(g_real) * (1.0 - (D.gov_sector ? w.nation_get_comp_g(n, static_cast<uint8_t>(D.gov_sector.index())) : 0.0f)) * p;
    const double exports_nom = static_cast<double>(x_real) * p;
    const double imports_nom = static_cast<double>(imports) * p;
    const double profit = firm_va - private_wage_bill - indirect - resource;
    double ps = w.nation_get_profit_smoothed(n);
    if(apply) {
        ps += (profit - ps) * std::min(1.0, static_cast<double>(dt) / P.profit_smoothing_days);
        w.nation_set_profit_smoothed(n, static_cast<float>(ps));
    }
    const double firm_tax = w.nation_get_tax_firm(n) * std::max(0.0, profit);
    const double dividends = w.nation_get_dividend_payout(n) * std::max(0.0, ps);
    const double debt = apply ? std::max(0.0, -w.nation_get_bal_gov(n)) : w.nation_get_gov_debt0(n);
    const double gov_interest = debt * (P.gov_effective_rate + 0.0025 * (r - r0)) / 365.0;

    double pop_size_total = 0.0;
    for(int i = 0; i < pops.count; ++i) {
        pop_size_total += w.pop_get_size(pops.id[static_cast<std::size_t>(i)]);
    }

    const float tax_hh = w.nation_get_tax_hh(n);
    const auto firms = account::firms(n);
    const auto gov = account::government(n);
    const auto fin = account::finance(n);
    const auto row = account::rest_of_world(n);
    const double ddt = dt;

    for(int i = 0; i < pops.count; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const auto pid = pops.id[idx];
        const double owner = w.pop_get_owner_share(pid);
        const double fiscal_share = pop_size_total > 0.0 ? w.pop_get_size(pid) / pop_size_total : 0.0;
        const double div = dividends * owner;
        const double intr = gov_interest * owner;
        const double fis = (fiscal_hh / ddt) * fiscal_share;
        const double gross_work = work[idx] + wages_pub[idx] + stw[idx];
        const double tax = tax_hh * (gross_work + div + intr);
        const double other = transfers[idx] + fis + div + intr;
        const double net = gross_work + benefit[idx] + other - tax;
        const double net_factor = (gross_work + other) > 0.0 ? 1.0 - tax / (gross_work + other) : 1.0;
        w.pop_set_income(pid, static_cast<float>(net));
        w.pop_set_inc_work(pid, static_cast<float>(gross_work * net_factor));
        w.pop_set_inc_benefit(pid, static_cast<float>(benefit[idx]));
        w.pop_set_inc_other(pid, static_cast<float>(other * net_factor));

        if(!apply) {
            continue;
        }
        const auto me = account::of_pop(n, pid);
        const double spend = c_des_total > 0.0 ? c_nom * c_des[idx] / c_des_total : 0.0;
        const double fee_part = c_nom > 0.0 ? spend * fees_nom / c_nom : 0.0;
        transfer(state, me, firms, (spend - fee_part) * ddt, flow::consumption);
        transfer(state, me, gov, fee_part * ddt, flow::consumption);
        transfer(state, firms, me, work[idx] * ddt, flow::wages_private);
        transfer(state, gov, me, wages_pub[idx] * ddt, flow::wages_public);
        transfer(state, gov, me, stw[idx] * ddt, flow::job_retention_pay);
        transfer(state, gov, me, benefit[idx] * ddt, flow::unemployment_benefits);
        transfer(state, gov, me, transfers[idx] * ddt, flow::pensions);
        transfer(state, gov, me, fis * ddt, flow::fiscal_households);
        transfer(state, firms, me, div * ddt, flow::dividends);
        transfer(state, fin, me, intr * ddt, flow::interest_households);
        transfer(state, me, gov, tax * ddt, flow::tax_households);
        w.pop_set_consumption(pid, static_cast<float>(spend));
        w.pop_set_balance_trend(pid, w.pop_get_balance_trend(pid)
            + static_cast<double>(w.pop_get_saving_rate0(pid)) * w.pop_get_income_perm(pid) * p * ddt);
    }

    if(apply) {
        transfer(state, gov, firms, gov_purchases * ddt, flow::gov_purchases);
        transfer(state, gov, firms, fiscal_firms, flow::fiscal_firms);
        transfer(state, row, firms, exports_nom * ddt, flow::exports);
        transfer(state, firms, row, imports_nom * ddt, flow::imports);
        transfer(state, firms, gov, indirect * ddt, flow::tax_indirect);
        transfer(state, firms, gov, resource * ddt, flow::tax_resource);
        transfer(state, firms, gov, firm_tax * ddt, flow::tax_firms);
        transfer(state, gov, fin, gov_interest * ddt, flow::interest_gov);
    }

    // ---------------------------------------------------------------- store
    if(!apply) {
        return; // the calibration probe only computes incomes
    }
    for(uint8_t s = 0; s < S; ++s) {
        w.nation_set_output(n, s, out[s]);
        w.nation_set_capacity(n, s, cap[s]);
        w.nation_set_demand(n, s, dem[s]);
    }
    w.nation_set_consumption(n, c_eff);
    w.nation_set_investment(n, i_real);
    w.nation_set_gov_spend(n, g_real);
    w.nation_set_exports(n, x_real);
    w.nation_set_imports(n, imports);
    w.nation_set_gdp_real(n, gdp);
}

} // namespace geosim::sim
