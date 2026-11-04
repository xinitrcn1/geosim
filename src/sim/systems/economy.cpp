// Economy (docs/03-economy.md): weekly per nation, sharded by nation index.
//
// Passes on a day (only nations whose weekly shard is due change; SIMD passes
// blend lanes with a mask so every vector does the same work):
//   1. nation pre-pass (scalar)    NPI cuts, adaptation, supply-chain factors
//   2. region capacity   (SIMD)    potential x NPI cut x sickness x sector factors
//   3. nation pass       (scalar)  demand, output = min(demand, capacity), prices,
//                                  fiscal disbursement, per-weight rates for pops,
//                                  nation-level ledger flows
//   4. region output     (SIMD)    regional share of national output, utilisation
//   5. pop pass          (SIMD)    layoffs / hours / rehiring, incomes, taxes,
//                                  permanent income, desired consumption
//   6. reduction         (serial)  pop flows -> ledger transfers in index order
//                                  (keeps money conservation exact), nation totals
// Pop and region values that depend on the nation are gathered through the
// cached `nation` / `owner` ids.

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

constexpr int max_sectors = 32;

float clamp01(float x) noexcept {
    return std::clamp(x, 0.0f, 1.0f);
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
    auto const& P = state.params;
    return (1.0f - static_cast<float>(P.tourism_own_border) * npi_fraction(state, n, k))
           * (1.0f - static_cast<float>(P.tourism_world_border) * others);
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
        const double base = static_cast<double>(m.get_m0()) * m.get_gf();
        if(base > 0.0) {
            num += m.get_imports();
            den += base;
        }
    }
    return den > 0.0 ? static_cast<float>(num / den) : 1.0f;
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
        const double pot = static_cast<double>(m.get_va0(s)) * m.get_gf();
        if(pot > 0.0) {
            num += m.get_x0() * std::min(1.0, static_cast<double>(m.get_capacity(s)) / pot);
            den += m.get_x0();
        }
    }
    return den > 0.0 ? static_cast<float>(num / den) : 1.0f;
}

bool due(game_state const& state, dcon::nation_id n, bool probe) {
    return probe || is_due(static_cast<uint32_t>(n.index()), static_cast<tick_t>(state.current_tick), weekly);
}

// ---------------------------------------------------------------- 1. nation pre-pass
void nation_prepass(game_state& state, dcon::nation_id n, bool apply) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t S = D.sector_count;
    w.nation_set_gf(n, growth_factor(state, n));
    // Business adaptation: the output cost of a given stringency fell by about half
    // between the first and second waves (OECD Economic Outlook, Dec 2020).
    const float adaptation = static_cast<float>(P.adaptation_max)
                             * std::min(1.0f, w.nation_get_npi_exposure(n) / static_cast<float>(P.adaptation_days));
    // Under a suppression strategy, measures are confined to small areas.
    const float local = static_cast<float>(P.npi_targeted_reach) * (1.0f - 0.5f * w.nation_get_tti_strength(n));
    const float man_index = apply ? world_manufacturing_index(state, n) : 1.0f;
    for(uint8_t s = 0; s < S; ++s) {
        const dcon::sector_id sid{s};
        float cut = 0.0f;
        for(uint8_t k = 0; k < D.npi_count; ++k) {
            const float reach = 1.0f - (1.0f - local) * w.nation_get_npi_targeted(n, k);
            cut += w.sector_get_npi_capacity(sid, k) * npi_fraction(state, n, k) * reach;
        }
        w.nation_set_npi_cut(n, s, std::min(0.95f, cut * (1.0f - adaptation)));
        float factor = 1.0f;
        if(sid == D.manufacturing_sector) {
            factor = 1.0f - static_cast<float>(P.supply_chain_share) * (1.0f - man_index);
        }
        w.nation_set_sector_factor(n, s, factor);
    }
}

// ---------------------------------------------------------------- 3. nation pass
void nation_pass(game_state& state, dcon::nation_id n, float dt, bool apply) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t S = D.sector_count;
    const float p = w.nation_get_core_price(n);
    const float gf = w.nation_get_gf(n);
    const float r = w.nation_get_policy_rate(n);
    const float r0 = w.nation_get_policy_rate0(n);
    const float m_share = w.nation_get_import_share(n);
    const float u0 = w.nation_get_unemployment0(n);
    const float u = w.nation_get_unemployment(n);

    std::array<float, max_sectors> pot{};
    std::array<float, max_sectors> cap{};
    for(auto own : w.nation_get_region_ownership(n)) {
        auto reg = own.get_region();
        for(uint8_t s = 0; s < S; ++s) {
            pot[s] += reg.get_va0(s) * gf;
            cap[s] += reg.get_capacity(s);
        }
    }

    // ---- demand
    const float deaths_pm = reported_deaths_per_million_daily(state, n);
    // Consumers' response to deaths faded strongly over 2020 (behavioural fatigue).
    const float fear_fatigue = static_cast<float>(P.fear_fatigue_max) * std::min(1.0f, w.nation_get_npi_exposure(n) / static_cast<float>(P.fatigue_days));
    const float fear = 1.0f / (1.0f + static_cast<float>(P.voluntary_demand_k) * (1.0f - fear_fatigue) * deaths_pm);
    const float conf = std::max(0.5f, 1.0f - static_cast<float>(P.precautionary) * std::max(0.0f, u - u0));
    const float rate_c = 1.0f - static_cast<float>(P.rate_c_elasticity) * (r - r0);
    w.nation_set_conf_rate(n, conf * rate_c);

    const double c_des_total = w.nation_get_c_des_total(n);
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
        // Exports of contact services are partly tourism: they follow travel, not trade.
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
    const float prod0 = w.nation_get_oil_production0(n);
    const float oil_share = w.nation_get_oil_share_of_energy(n);
    if(D.energy_sector && prod0 > 0.0f) {
        const auto es = static_cast<uint8_t>(D.energy_sector.index());
        const float dr = pot[es] > 0.0f ? std::min(1.0f, dem[es] / pot[es]) : 1.0f;
        out[es] = pot[es] * ((1.0f - oil_share) * dr + oil_share * w.nation_get_oil_production(n) / prod0);
    }

    float hh_real = 0.0f, gdp = 0.0f, pot_total = 0.0f, hh_gov_real = 0.0f, import_demand = 0.0f;
    for(uint8_t s = 0; s < S; ++s) {
        const float fill = dem[s] > 0.0f ? std::min(1.0f, out[s] / dem[s]) : 1.0f;
        hh_real += hh[s] * fill;
        if(dcon::sector_id{s} == D.gov_sector) {
            hh_gov_real = hh[s] * fill;
        }
        gdp += out[s];
        pot_total += pot[s];
        import_demand += out[s] * w.sector_get_import_intensity(dcon::sector_id{s});
        w.nation_set_fill(n, s, cap[s] > 0.0f ? out[s] / cap[s] : 0.0f);
    }
    const float c_eff = hh_real / (1.0f - m_share);
    // Imports follow domestic output weighted by import intensity (goods > services).
    const float base = w.nation_get_import_base(n) * gf;
    const float imports = w.nation_get_m0(n) * gf * (base > 0.0f ? import_demand / base : 1.0f);
    const float util = pot_total > 0.0f ? gdp / pot_total : 1.0f;
    const double c_nom = static_cast<double>(c_eff) * p;
    w.nation_set_cons_fill(n, c_des_total > 0.0 ? static_cast<float>(c_nom / c_des_total) : 1.0f);
    w.nation_set_fee_share(n, c_eff > 0.0f ? hh_gov_real / c_eff : 0.0f);

    // ---- prices
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
        w.nation_set_core_price(n, p * std::exp(std::log1p(pi) * dt / 365.0f));
        pe += (pi - pe) * std::min(1.0f, dt / static_cast<float>(P.expectations_days));
        w.nation_set_inflation_expect(n, pe);
        const float oil_rel = D.oil ? w.commodity_get_price(D.oil) / w.commodity_get_price0(D.oil) : 1.0f;
        w.nation_set_cpi(n, w.nation_get_core_price(n) * (1.0f + static_cast<float>(P.oil_cpi_share) * (oil_rel - 1.0f)));
    }

    // ---- labour-market parameters for the pop pass
    const float coverage = w.nation_get_stw_coverage(n);
    const float epl = w.nation_get_epl(n);
    // OxCGRT E1 (income support) also records wage-subsidy schemes; where a job-retention
    // scheme covers workers, only the uncovered share boosts unemployment benefits.
    const float e1 = (D.npi_income_support != 0xFF ? npi_fraction(state, n, D.npi_income_support) : 0.0f) * (1.0f - coverage);
    const float repl = std::min(1.2f, w.nation_get_benefit_replacement(n) + static_cast<float>(P.e1_replacement_boost) * e1);
    const float cov = std::min(1.0f, w.nation_get_benefit_coverage(n) + static_cast<float>(P.e1_coverage_boost) * e1);
    w.nation_set_benefit_mult(n, repl * cov);
    w.nation_set_benefit_base(n, w.nation_get_benefit_replacement(n) * w.nation_get_benefit_coverage(n));
    // Only the scheme's take-up share of lost hours is paid.
    w.nation_set_stw_mult(n, coverage > 0.0f ? coverage * w.nation_get_stw_replacement(n) : 0.0f);
    // Job retention and employment protection keep a share of the pre-crisis jobs attached.
    w.nation_set_absorb(n, coverage + (1.0f - coverage) * epl * static_cast<float>(P.epl_hoarding));
    const float stay = D.npi_stay_home != 0xFF ? npi_fraction(state, n, D.npi_stay_home) : 0.0f;
    // Laid-off workers who cannot search (informal economy, stay-at-home orders) leave the labour force.
    w.nation_set_lf_exit(n, clamp01(static_cast<float>(P.lf_exit_base + P.lf_exit_informal * w.nation_get_informality(n)
                                                       + P.lf_exit_lockdown * stay)));
    double base_bill = 0.0;
    for(uint8_t s = 0; s < S; ++s) {
        if(dcon::sector_id{s} != D.gov_sector) {
            base_bill += static_cast<double>(w.sector_get_labor_share(dcon::sector_id{s})) * pot[s] * p;
        }
    }
    // Firm support (wage subsidies, PPP-style loans) slows layoffs.
    const float support = base_bill > 0.0 ? static_cast<float>(w.nation_get_firm_support_rate(n) / base_bill) : 0.0f;
    const float support_cut = std::max(0.3f, 1.0f - static_cast<float>(P.firm_support_effect) * support);
    w.nation_set_layoff_speed_eff(n, static_cast<float>(P.layoff_speed) * (1.0f - 0.5f * epl) * support_cut);

    // ---- fiscal: announced packages first fund the crisis programmes (job retention,
    // benefit top-ups) paid in the last step; the rest is disbursed gradually.
    double fiscal_hh = 0.0;
    double fiscal_firms = 0.0;
    if(apply) {
        double pending = w.nation_get_fiscal_pending(n);
        pending -= std::min(pending, w.nation_get_programmes(n));
        const double paid = pending * std::min(1.0, static_cast<double>(dt) / P.fiscal_disburse_days);
        w.nation_set_fiscal_pending(n, pending - paid);
        fiscal_firms = paid * P.fiscal_firm_share;
        fiscal_hh = paid - fiscal_firms;
        w.nation_set_firm_support_rate(n, static_cast<float>(fiscal_firms / dt));
    }

    // ---- incomes paid by the nation to pops (rates per weight unit)
    const float oil_price = D.oil ? w.commodity_get_price(D.oil) : 0.0f;
    const float oil_rel = D.oil ? oil_price / w.commodity_get_price0(D.oil) : 1.0f;
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
    const double fees_nom = static_cast<double>(hh_gov_real) * p;
    const double c_to_firms = std::max(0.0, c_nom - fees_nom);
    const double indirect = w.nation_get_tax_indirect(n) * c_to_firms;
    const double oil_value = static_cast<double>(w.nation_get_oil_production(n)) * 1e6 * oil_price;
    const double resource = w.nation_get_resource_tax(n) * oil_value;
    const double profit = firm_va - w.nation_get_wage_bill_private(n) - indirect - resource;
    double ps = w.nation_get_profit_smoothed(n);
    if(apply) {
        ps += (profit - ps) * std::min(1.0, static_cast<double>(dt) / P.profit_smoothing_days);
        w.nation_set_profit_smoothed(n, static_cast<float>(ps));
    }
    const double firm_tax = w.nation_get_tax_firm(n) * std::max(0.0, profit);
    const double dividends = w.nation_get_dividend_payout(n) * std::max(0.0, ps);
    const double debt = apply ? std::max(0.0, -w.nation_get_bal_gov(n)) : w.nation_get_gov_debt0(n);
    const double gov_interest = debt * (P.gov_effective_rate + 0.0025 * (r - r0)) / 365.0;
    const double pensions = static_cast<double>(w.nation_get_pensions0(n)) * p * gf;
    auto per = [](double total, float weight) { return weight > 0.0f ? static_cast<float>(total / weight) : 0.0f; };
    w.nation_set_rate_transfer(n, per(pensions, w.nation_get_transfer_weight_total(n)));
    w.nation_set_rate_fiscal(n, per(fiscal_hh / dt, w.nation_get_size_total(n)));
    w.nation_set_rate_dividend(n, per(dividends, w.nation_get_owner_weight_total(n)));
    w.nation_set_rate_interest(n, per(gov_interest, w.nation_get_owner_weight_total(n)));

    if(apply) {
        const double gov_purchases = static_cast<double>(g_real) * (1.0 - w.nation_get_comp_g(n, static_cast<uint8_t>(D.gov_sector.index()))) * p;
        const auto firms = account::firms(n);
        const auto gov = account::government(n);
        const auto fin = account::finance(n);
        const auto row = account::rest_of_world(n);
        const double ddt = dt;
        transfer(state, gov, firms, gov_purchases * ddt, flow::gov_purchases);
        transfer(state, gov, firms, fiscal_firms, flow::fiscal_firms);
        transfer(state, row, firms, static_cast<double>(x_real) * p * ddt, flow::exports);
        transfer(state, firms, row, static_cast<double>(imports) * p * ddt, flow::imports);
        transfer(state, firms, gov, indirect * ddt, flow::tax_indirect);
        transfer(state, firms, gov, resource * ddt, flow::tax_resource);
        transfer(state, firms, gov, firm_tax * ddt, flow::tax_firms);
        transfer(state, gov, fin, gov_interest * ddt, flow::interest_gov);

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
}

} // namespace

void update_economy(game_state& state, economy_mode mode) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const bool apply = mode == economy_mode::simulate;
    const bool probe = !apply;
    const float dt = apply ? static_cast<float>(weekly.period_days) : 1.0f;
    const uint8_t S = D.sector_count;
    const auto gov_sector = static_cast<uint8_t>(D.gov_sector.index());

    std::vector<dcon::nation_id> due_nations;
    for(auto n : w.in_nation) {
        if(due(state, n.id, probe)) {
            due_nations.push_back(n.id);
        }
    }
    if(due_nations.empty()) {
        return;
    }
    // Lanes of nations not due today keep their values (probe: everyone is due).
    const ve::int_vector today(static_cast<int32_t>(static_cast<uint32_t>(state.current_tick) % weekly.shards));
    auto lane_due = [&](auto shard) { return probe ? (shard == shard) : (shard == today); };

    // 1. nation pre-pass
    for(auto n : due_nations) {
        nation_prepass(state, n, apply);
    }

    // 2. region capacity
    const ve::fp_vector sick(static_cast<float>(P.sickness_absence));
    w.execute_parallel_over_region([&](auto ids) {
        const auto nat = w.region_get_owner(ids);
        const auto mask = lane_due(w.region_get_shard(ids));
        const auto young_inf = w.region_get_ep_i(ids, age_young);
        const auto young_n = w.region_get_ep_s(ids, age_young) + w.region_get_ep_e(ids, age_young) + young_inf
                             + w.region_get_ep_r(ids, age_young);
        const auto absent = sick * young_inf / ve::max(young_n, ve::fp_vector(1.0f));
        const auto gf = w.nation_get_gf(nat);
        for(uint8_t s = 0; s < S; ++s) {
            const auto cap = w.region_get_va0(ids, s) * gf * (ve::fp_vector(1.0f) - w.nation_get_npi_cut(nat, s))
                             * (ve::fp_vector(1.0f) - absent) * w.nation_get_sector_factor(nat, s) * w.region_get_unrest_capacity(ids);
            w.region_set_capacity(ids, s, ve::select(mask, cap, w.region_get_capacity(ids, s)));
        }
    });

    // 3. nation pass
    for(auto n : due_nations) {
        nation_pass(state, n, dt, apply);
    }

    // 4. region output and utilisation
    w.execute_parallel_over_region([&](auto ids) {
        const auto nat = w.region_get_owner(ids);
        const auto mask = lane_due(w.region_get_shard(ids));
        const auto gf = w.nation_get_gf(nat);
        for(uint8_t s = 0; s < S; ++s) {
            const auto out = w.region_get_capacity(ids, s) * w.nation_get_fill(nat, s);
            const auto pot = w.region_get_va0(ids, s) * gf;
            const auto util = ve::select(pot > ve::fp_vector(0.0f), out / ve::max(pot, ve::fp_vector(1e-9f)), ve::fp_vector(1.0f));
            w.region_set_output(ids, s, ve::select(mask, out, w.region_get_output(ids, s)));
            w.region_set_util(ids, s, ve::select(mask, util, w.region_get_util(ids, s)));
        }
    });

    // 5. pop pass
    const ve::fp_vector vdt(dt);
    const ve::fp_vector one(1.0f);
    const ve::fp_vector zero(0.0f);
    const ve::fp_vector week_frac(dt / 7.0f);
    const ve::fp_vector match(static_cast<float>(P.match_efficiency));
    const ve::fp_vector ret(std::min(1.0f, static_cast<float>(P.lf_return_rate) * dt / 7.0f));
    const ve::fp_vector smooth_inc(std::min(1.0f, dt / static_cast<float>(P.income_smoothing_days)));
    const ve::fp_vector wealth_rate(static_cast<float>(P.wealth_spend_rate / 365.0));
    const ve::fp_vector lag(static_cast<float>(P.layoff_lag_days));
    const ve::fp_vector lag_epl(static_cast<float>(P.layoff_lag_epl));
    w.execute_parallel_over_pop([&](auto ids) {
        const auto nat = w.pop_get_nation(ids);
        const auto reg = w.pop_get_home(ids);
        const auto mask = lane_due(w.pop_get_shard(ids));
        const auto gf = w.nation_get_gf(nat);
        const auto p = w.nation_get_core_price(nat);
        const auto absorb = w.nation_get_absorb(nat);
        const auto speed = ve::min(one, w.nation_get_layoff_speed_eff(nat) * week_frac);
        const auto exit = w.nation_get_lf_exit(nat);
        const auto stw_mult = w.nation_get_stw_mult(nat);
        const auto tau = lag * (one + lag_epl * w.nation_get_epl(nat));
        const auto slow = ve::min(one, vdt / tau);

        auto unemployed = w.pop_get_unemployed(ids);
        auto inactive = w.pop_get_inactive(ids);
        ve::fp_vector work(0.0f), pub(0.0f), stw(0.0f), programmes(0.0f);
        for(uint8_t s = 0; s < S; ++s) {
            const auto e = w.pop_get_employed(ids, s);
            const auto wage = w.pop_get_wage0(ids, s) * gf * p;
            if(s == gov_sector) {
                pub = pub + e * wage; // public employment is held
                continue;
            }
            const auto base = w.pop_get_emp0(ids, s);
            const auto need = base * w.region_get_util(reg, s);
            const auto need_slow = w.pop_get_need_slow(ids, s) + (need - w.pop_get_need_slow(ids, s)) * slow;
            const auto layoff_need = ve::max(need, need_slow);
            const auto shrink = layoff_need < e;
            const auto target = ve::min(e, layoff_need + absorb * ve::max(zero, base - layoff_need));
            const auto layoffs = ve::select(shrink, (e - target) * speed, zero);
            const auto vacancies = ve::max(zero, need - e);
            const auto hires = ve::select(shrink, zero,
                                          ve::min(vacancies, ve::min(unemployed, match * ve::sqrt(unemployed * vacancies) * week_frac)));
            const auto e2 = e - layoffs + hires;
            unemployed = unemployed + layoffs * (one - exit) - hires;
            inactive = inactive + layoffs * exit;
            const auto hours = ve::max(ve::fp_vector(0.05f), ve::min(one, need / ve::max(e2, one)));
            const auto idle = e2 * (one - hours) * wage * stw_mult;
            work = work + e2 * hours * wage;
            stw = stw + idle;
            programmes = programmes + idle;
            w.pop_set_employed(ids, s, ve::select(mask, e2, e));
            w.pop_set_hours(ids, s, ve::select(mask, hours, w.pop_get_hours(ids, s)));
            w.pop_set_need_slow(ids, s, ve::select(mask, need_slow, w.pop_get_need_slow(ids, s)));
        }
        // Crisis-inactive people drift back into job search.
        const auto back = inactive * ret;
        inactive = inactive - back;
        unemployed = unemployed + back;

        const auto avg_wage = w.pop_get_wage_avg0(ids) * gf * p;
        const auto benefit = unemployed * avg_wage * w.nation_get_benefit_mult(nat);
        programmes = programmes + unemployed * avg_wage * ve::max(zero, w.nation_get_benefit_mult(nat) - w.nation_get_benefit_base(nat));
        const auto transfers = w.pop_get_transfer_weight(ids) * w.nation_get_rate_transfer(nat);
        const auto fiscal = w.pop_get_size(ids) * w.nation_get_rate_fiscal(nat);
        const auto dividends = w.pop_get_owner_weight(ids) * w.nation_get_rate_dividend(nat);
        const auto interest = w.pop_get_owner_weight(ids) * w.nation_get_rate_interest(nat);
        const auto gross = work + pub + stw;
        const auto tax = w.nation_get_tax_hh(nat) * (gross + dividends + interest);
        const auto other = transfers + fiscal + dividends + interest;
        const auto net = gross + benefit + other - tax;
        const auto taxable = gross + other;
        const auto net_factor = ve::select(taxable > zero, one - tax / ve::max(taxable, ve::fp_vector(1e-9f)), one);

        // Permanent income is smoothed in real terms; desired consumption for the next step.
        const auto inc_perm_old = w.pop_get_income_perm(ids);
        const auto inc_perm = probe ? inc_perm_old : inc_perm_old + (net / p - inc_perm_old) * smooth_inc;
        const auto c_new = ve::max(zero, (one - w.pop_get_saving_rate0(ids)) * inc_perm * p * w.nation_get_conf_rate(nat)
                                             + ve::max(zero, w.pop_get_excess_deposits(ids)) * wealth_rate);
        // This step's spending: last step's plan, scaled by what the market could supply.
        const auto spend = w.pop_get_c_des(ids) * w.nation_get_cons_fill(nat);
        const auto spend_gov = spend * w.nation_get_fee_share(nat);

        w.pop_set_unemployed(ids, ve::select(mask, unemployed, w.pop_get_unemployed(ids)));
        w.pop_set_inactive(ids, ve::select(mask, inactive, w.pop_get_inactive(ids)));
        w.pop_set_income(ids, ve::select(mask, net, w.pop_get_income(ids)));
        w.pop_set_inc_work(ids, ve::select(mask, gross * net_factor, w.pop_get_inc_work(ids)));
        w.pop_set_inc_benefit(ids, ve::select(mask, benefit, w.pop_get_inc_benefit(ids)));
        w.pop_set_inc_other(ids, ve::select(mask, other * net_factor, w.pop_get_inc_other(ids)));
        w.pop_set_income_perm(ids, ve::select(mask, inc_perm, inc_perm_old));
        w.pop_set_consumption(ids, ve::select(mask, spend, w.pop_get_consumption(ids)));
        w.pop_set_f_wage_private(ids, ve::select(mask, work, zero));
        w.pop_set_f_wage_public(ids, ve::select(mask, pub, zero));
        w.pop_set_f_stw(ids, ve::select(mask, stw, zero));
        w.pop_set_f_benefit(ids, ve::select(mask, benefit, zero));
        w.pop_set_f_transfer(ids, ve::select(mask, transfers, zero));
        w.pop_set_f_fiscal(ids, ve::select(mask, fiscal, zero));
        w.pop_set_f_dividend(ids, ve::select(mask, dividends, zero));
        w.pop_set_f_interest(ids, ve::select(mask, interest, zero));
        w.pop_set_f_tax(ids, ve::select(mask, tax, zero));
        w.pop_set_f_spend_firms(ids, ve::select(mask, spend - spend_gov, zero));
        w.pop_set_f_spend_gov(ids, ve::select(mask, spend_gov, zero));
        w.pop_set_f_programmes(ids, ve::select(mask, programmes, zero));
        if(!probe) {
            w.pop_set_c_des(ids, ve::select(mask, c_new, w.pop_get_c_des(ids)));
        }
    });

    // 6. reduction: ledger transfers in index order, nation totals.
    struct totals {
        double c_des = 0, wages = 0, programmes = 0, employed = 0, unemployed = 0;
    };
    std::vector<totals> tot(w.nation_size());
    const double ddt = dt;
    for(auto pp : w.in_pop) {
        const dcon::nation_id n = pp.get_nation().id;
        if(!due(state, n, probe)) {
            continue;
        }
        auto& t = tot[static_cast<std::size_t>(n.index())];
        t.c_des += pp.get_c_des();
        t.wages += pp.get_f_wage_private();
        t.programmes += static_cast<double>(pp.get_f_programmes()) * ddt;
        t.unemployed += pp.get_unemployed();
        for(uint8_t s = 0; s < S; ++s) {
            t.employed += pp.get_employed(s);
        }
        if(probe) {
            continue;
        }
        const auto me = account::of_pop(n, pp.id);
        const auto firms = account::firms(n);
        const auto gov = account::government(n);
        transfer(state, firms, me, pp.get_f_wage_private() * ddt, flow::wages_private);
        transfer(state, gov, me, pp.get_f_wage_public() * ddt, flow::wages_public);
        transfer(state, gov, me, pp.get_f_stw() * ddt, flow::job_retention_pay);
        transfer(state, gov, me, pp.get_f_benefit() * ddt, flow::unemployment_benefits);
        transfer(state, gov, me, pp.get_f_transfer() * ddt, flow::pensions);
        transfer(state, gov, me, pp.get_f_fiscal() * ddt, flow::fiscal_households);
        transfer(state, firms, me, pp.get_f_dividend() * ddt, flow::dividends);
        transfer(state, account::finance(n), me, pp.get_f_interest() * ddt, flow::interest_households);
        transfer(state, me, gov, pp.get_f_tax() * ddt, flow::tax_households);
        transfer(state, me, firms, pp.get_f_spend_firms() * ddt, flow::consumption);
        transfer(state, me, gov, pp.get_f_spend_gov() * ddt, flow::consumption);
        pp.set_balance_trend(pp.get_balance_trend()
                             + static_cast<double>(pp.get_saving_rate0()) * pp.get_income_perm() * w.nation_get_core_price(n) * ddt);
        pp.set_excess_deposits(static_cast<float>(pp.get_balance() - pp.get_balance_trend()));
    }
    for(auto n : due_nations) {
        auto const& t = tot[static_cast<std::size_t>(n.index())];
        if(!probe) {
            w.nation_set_c_des_total(n, t.c_des);
        }
        w.nation_set_wage_bill_private(n, t.wages);
        w.nation_set_programmes(n, t.programmes);
        const double lf = t.employed + t.unemployed;
        if(apply) {
            w.nation_set_unemployment(n, lf > 0.0 ? static_cast<float>(t.unemployed / lf) : 0.0f);
        }
    }
}

} // namespace geosim::sim
