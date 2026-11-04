// The remaining policy catalogue and its economic channels (docs/05#policy-catalogue,
// docs/03). Every lever is relative to the calibrated 2019 settings (zero or one at
// the start), with pragmatic and ideological targets in the usual blend; bills go
// through the legislature (enact).
//
// Executive (weekly, from ai.cpp):
//  - police (bill, % GDP above baseline): pragmatic with crime and unrest, more for
//    order-minded rulers; cuts crime (Levitt 1997; Chalfin & McCrary 2017).
//  - asylum (openness relative to 2019): humanitarian, held back by joblessness and
//    by traditionalist interior ministers; scales refugee arrivals (Hatton 2009).
//  - vat, corporate_tax (bills, pp): consolidation raises VAT first (Alesina,
//    Favero & Giavazzi 2019 on tax- vs spending-based plans); weak investment invites
//    corporate tax cuts (tax competition; Devereux, Lockwood & Redoano 2008), the left
//    raises them. VAT passes into prices (Benzarti et al. 2020).
//  - wealth_tax (bill, % per year on top wealth): the left and rising inequality;
//    the base erodes with avoidance (Jakobsen et al. 2020; Saez & Zucman 2019).
//  - rd (bill, % GDP above baseline): a growth programme or a technology race
//    against a rising threat; raises trend growth (Romer 1990; Ugur et al. 2016).
//  - subsidy (share of the energy and food price rise absorbed): a populist answer
//    to price spikes, larger where the winning coalition is small; costs the budget
//    and is regressive (Coady et al. 2015).
//  - epl (bill, index points): flexibilisation under high unemployment, resisted by
//    strong unions and the left (Boeri & Jimeno 2005; OECD EPL).
//  - energy_mandate (bill, 0..1): renewables mandates follow carbon concern and green
//    governments; speed clean-capacity diffusion (Johnstone, Hascic & Popp 2010).
//  - vaccine_priority (0..1): doses to the old first (Bubar et al. 2021).
// Central bank (daily with the rate decision):
//  - fx_intervention (0..1): sells reserves against fast depreciation while reserves
//    last (Fratzscher et al. 2019; finance.cpp uses it);
//  - macroprudential (countercyclical buffer, pp): against credit booms above trend
//    (Basel III CCyB; Cerutti, Claessens & Laeven 2017; credit.cpp uses it).
// Monthly channels (update_reforms): taxes and their revenue, the wealth tax flow,
// fuel and food subsidies, vintage capital (embodied technology: Solow 1960;
// Greenwood, Hercowitz & Krusell 1997), regional house prices with supply
// elasticity (Saiz 2010), sub-national fiscal capacity and equalisation (Boadway &
// Shah 2009).

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

float blend_lever(game_state& state, dcon::agent_id a, uint8_t lev, float pragmatic, float ideal, float rho) {
    auto& w = state.world;
    float x = (1.0f - rho) * pragmatic + rho * ideal;
    if(w.agent_get_override_mode(a, lev) != 0) x = w.agent_get_override_value(a, lev);
    w.agent_set_last_pragmatic(a, lev, pragmatic);
    w.agent_set_last_ideological(a, lev, ideal);
    return x;
}

float debt_rise(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const double gdp = w.nation_get_gdp0(n) * w.nation_get_core_price(n);
    const float debt = gdp > 0.0 ? static_cast<float>(std::max(0.0, -w.nation_get_bal_gov(n)) / gdp) : 0.0f;
    const float debt0 = w.nation_get_gdp0(n) > 0.0 ? static_cast<float>(w.nation_get_gov_debt0(n) / w.nation_get_gdp0(n)) : 0.0f;
    return std::max(0.0f, debt - debt0 - 0.1f);
}

float mean_unrest(game_state const& state, dcon::nation_id n) {
    float u = 0.0f, pop = 0.0f;
    for(auto own : state.world.nation_get_region_ownership(n)) {
        u += own.get_region().get_unrest() * own.get_region().get_population();
        pop += own.get_region().get_population();
    }
    return pop > 0.0f ? u / pop : 0.0f;
}
} // namespace

// Energy and food share of the CPI rise above 2019 (the part a subsidy absorbs).
float price_spike(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const float core = std::max(1e-3f, w.nation_get_core_price(n));
    return std::max(0.0f, w.nation_get_cpi_raw(n) / core - 1.0f);
}

void decide_reforms(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const float rho = w.agent_get_rho(a);
    const auto lev = [&](lever l) { return static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(l)); };
    const float econ = minister_position(state, n, portfolio::finance, value_axis::economic_right) - w.nation_get_gov_econ0(n);
    const float tan = std::max(0.0f, -minister_position(state, n, portfolio::interior, value_axis::self_expression) - w.nation_get_gov_tan0(n));
    const float consolidate = goal_bias(state, n, goal::consolidate);
    const float u_rise = std::max(0.0f, w.nation_get_unemployment(n) - w.nation_get_unemployment0(n));
    const float debt = debt_rise(state, n);

    // Police.
    {
        const float p = 0.5f * std::max(0.0f, w.nation_get_crime(n) - 1.0f) + 0.1f * std::max(0.0f, mean_unrest(state, n) - 1.0f)
                        + plan_bias(state, n, goal::regime_security, 1);
        const float before = w.nation_get_police(n);
        float x = before + (blend_lever(state, a, lev(lever::police), p, p * (1.0f + w.agent_get_order_ideal(a)) + 0.2f * tan, rho) - before) * 0.1f;
        x = enact(state, n, lev(lever::police), before, x, value_axis::self_expression, 0.05f, false);
        w.nation_set_police(n, std::clamp(x, -0.5f, 2.0f));
    }
    // Asylum.
    {
        const float p = std::clamp(1.0f - 5.0f * u_rise, 0.3f, 1.5f);
        const float before = w.nation_get_asylum_openness(n);
        const float x = before + (blend_lever(state, a, lev(lever::asylum), p, p - 0.5f * tan, rho) - before) * 0.1f;
        w.nation_set_asylum_openness(n, std::clamp(x, 0.0f, 2.0f));
    }
    // VAT and corporate tax.
    {
        const float p = 10.0f * debt * (1.0f + consolidate) + 5.0f * plan_bias(state, n, goal::consolidate, 1);
        const float before = w.nation_get_vat_shift(n);
        float x = before + (blend_lever(state, a, lev(lever::vat), p, p * std::max(0.0f, 1.0f + econ), rho) - before) * 0.05f;
        x = enact(state, n, lev(lever::vat), before, x, value_axis::economic_right, -0.01f, false);
        w.nation_set_vat_shift(n, std::clamp(x, -5.0f, 8.0f));
    }
    {
        const float inv_ref = w.nation_get_investment_ref(n);
        const float weak = inv_ref > 0.0f ? std::max(0.0f, 1.0f - w.nation_get_investment(n) / inv_ref) : 0.0f;
        const float p = -10.0f * weak + 5.0f * debt - 5.0f * plan_bias(state, n, goal::restore_growth, 2);
        const float before = w.nation_get_corporate_shift(n);
        float x = before + (blend_lever(state, a, lev(lever::corporate_tax), p, p - 10.0f * econ, rho) - before) * 0.05f;
        x = enact(state, n, lev(lever::corporate_tax), before, x, value_axis::economic_right, -0.01f, false);
        w.nation_set_corporate_shift(n, std::clamp(x, -10.0f, 10.0f));
    }
    // Wealth tax.
    {
        const float p = std::max(0.0f, 10.0f * (w.nation_get_gini(n) - w.nation_get_gini_ref(n)) + 2.0f * debt);
        const float before = w.nation_get_wealth_tax(n);
        float x = before + (blend_lever(state, a, lev(lever::wealth_tax), p, std::max(0.0f, p - 2.0f * econ), rho) - before) * 0.05f;
        x = enact(state, n, lev(lever::wealth_tax), before, x, value_axis::economic_right, -0.02f, false);
        w.nation_set_wealth_tax(n, std::clamp(x, 0.0f, 3.0f));
    }
    // Public R&D.
    {
        float tmax = 0.0f;
        for(auto m : w.in_nation) {
            if(m.id != n) tmax = std::max(tmax, w.nation_get_threat_to(n, static_cast<uint16_t>(m.id.index())));
        }
        const float p = 0.5f * goal_bias(state, n, goal::restore_growth) / std::max(0.01f, static_cast<float>(P.goal_gain))
                        + 2.0f * std::max(0.0f, tmax - w.nation_get_threat_max0(n))
                        + plan_bias(state, n, goal::restore_growth, 1) + plan_bias(state, n, goal::deter_threat, 1) + plan_bias(state, n, goal::secure_energy, 2);
        const float before = w.nation_get_rd_spending(n);
        float x = before + (blend_lever(state, a, lev(lever::rd), p, p, rho) - before) * 0.05f;
        x = enact(state, n, lev(lever::rd), before, x, value_axis::economic_right, -0.01f, false);
        w.nation_set_rd_spending(n, std::clamp(x, -0.5f, 2.0f));
    }
    // Fuel and food subsidies.
    {
        const float spike = price_spike(state, n);
        const float p = std::clamp(3.0f * spike, 0.0f, 0.8f) * (1.0f - consolidate);
        const float ideal = std::clamp(p * (1.0f + 0.5f * (1.0f - w.nation_get_winning_coalition(n))), 0.0f, 0.9f);
        const float before = w.nation_get_subsidy(n);
        const float x = before + (blend_lever(state, a, lev(lever::subsidy), p, ideal, rho) - before) * 0.3f;
        w.nation_set_subsidy(n, std::clamp(x, 0.0f, 0.9f));
    }
    // Employment protection.
    {
        const float p = -5.0f * u_rise * (1.0f - w.nation_get_union_density(n));
        const float before = w.nation_get_epl_shift(n);
        float x = before + (blend_lever(state, a, lev(lever::epl), p, p - 0.5f * econ, rho) - before) * 0.05f;
        x = enact(state, n, lev(lever::epl), before, x, value_axis::economic_right, 0.02f, false);
        w.nation_set_epl_shift(n, std::clamp(x, -1.5f, 1.0f));
    }
    // Renewables mandate.
    {
        const float p = std::clamp((w.nation_get_carbon_price(n) - w.nation_get_carbon_price0(n)) / 50.0f + plan_bias(state, n, goal::secure_energy, 1), 0.0f, 1.0f);
        const auto se = static_cast<uint8_t>(value_axis::self_expression);
        float green = 0.0f;
        if(auto inc = w.nation_get_incumbent(n)) green = std::max(0.0f, w.party_get_position(inc, se) - w.nation_get_window_center(n, se));
        const float before = w.nation_get_energy_mandate(n);
        float x = before + (blend_lever(state, a, lev(lever::energy_mandate), p, std::clamp(p + green, 0.0f, 1.0f), rho) - before) * 0.05f;
        x = enact(state, n, lev(lever::energy_mandate), before, x, value_axis::self_expression, 0.01f, false);
        w.nation_set_energy_mandate(n, std::clamp(x, 0.0f, 1.0f));
    }
    // Vaccine priority: old first, more where the old weigh more in the coalition.
    {
        const float p = std::min(1.0f, 0.8f + plan_bias(state, n, goal::contain_epidemic, 1));
        const float x = blend_lever(state, a, lev(lever::vaccine_priority), p, std::clamp(p + 0.2f * w.agent_get_health_weight(a) - 0.1f, 0.0f, 1.0f), rho);
        w.nation_set_vaccine_priority(n, std::clamp(x, 0.0f, 1.0f));
    }
}

void decide_bank_tools(game_state& state, dcon::agent_id bank) {
    auto& w = state.world;
    auto const& D = state.derived;
    const float rho = w.agent_get_rho(bank);
    const auto lev = [&](lever l) { return static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(l)); };
    for(auto m : w.agent_get_monetary_authority(bank)) {
        const auto n = m.get_member().id;
        // FX intervention against depreciation faster than 10% a year, while reserves last.
        const float fall = std::max(0.0f, -w.nation_get_fx_change(n) - 0.1f);
        const float p = w.nation_get_reserves(n) > 1.0f ? std::clamp(5.0f * fall, 0.0f, 1.0f) : 0.0f;
        w.nation_set_fx_intervention(n, std::clamp(blend_lever(state, bank, lev(lever::fx_intervention), p, p, rho), 0.0f, 1.0f));
        // Countercyclical buffer against credit above its 2019 path.
        const double path = static_cast<double>(w.nation_get_core_price(n)) * w.nation_get_gf(n);
        const double L0 = (w.nation_get_loans_hh0(n) + w.nation_get_loans_firm0(n)) * path;
        const float gap = L0 > 0.0 ? static_cast<float>((w.nation_get_loans_hh(n) + w.nation_get_loans_firm(n)) / L0 - 1.0) : 0.0f;
        const float pb = std::clamp(25.0f * (gap - 0.02f), 0.0f, 2.5f);
        const float before = w.nation_get_macroprudential(n);
        // Buffers are released at once in a bust, built up slowly.
        const float target = blend_lever(state, bank, lev(lever::macroprudential), pb, pb, rho);
        w.nation_set_macroprudential(n, target < before ? target : before + (target - before) * 0.02f);
    }
}

// Fiscal capacity of a region: its value added per head relative to the nation's,
// as far as revenue is decentralised and not equalised (Boadway & Shah 2009).
float fiscal_capacity_of(game_state const& state, dcon::nation_id n, dcon::region_id r) {
    auto const& w = state.world;
    auto const& P = state.params;
    float gdp_n = 0.0f, pop_n = 0.0f;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(uint8_t s = 0; s < state.derived.sector_count; ++s) gdp_n += own.get_region().get_va0(s);
        pop_n += own.get_region().get_population();
    }
    float gdp_r = 0.0f;
    for(uint8_t s = 0; s < state.derived.sector_count; ++s) gdp_r += w.region_get_va0(r, s);
    const float pop_r = w.region_get_population(r);
    const float rel = gdp_n > 0.0f && pop_n > 0.0f && pop_r > 0.0f ? (gdp_r / pop_r) / (gdp_n / pop_n) : 1.0f;
    const float gap = (1.0f - rel) * w.nation_get_decentralisation(n) * (1.0f - static_cast<float>(P.equalisation_gain) * w.nation_get_equalisation(n));
    return std::clamp(1.0f - gap, 0.2f, 2.0f);
}

void init_reforms(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        n.set_investment_ref(n.get_investment());
        n.set_gini_ref(n.get_gini());
        n.set_vintage_tfp(1.0f);
        n.set_equalisation(0.5f);
    }
    // The 2019 capacities are the calibrated baseline (unrest responds to changes).
    for(auto own : w.in_region_ownership) {
        const float c = fiscal_capacity_of(state, own.get_nation().id, own.get_region().id);
        own.get_region().set_fiscal_capacity(c);
        own.get_region().set_fiscal_capacity0(c);
    }
}

void update_reforms(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        // Tax rates from their 2019 levels.
        n.set_tax_indirect(std::max(0.0f, n.get_tax_indirect0() + n.get_vat_shift() / 100.0f));
        n.set_tax_firm(std::clamp(n.get_tax_firm0() + n.get_corporate_shift() / 100.0f, 0.0f, 0.6f));
        const double gdp = n.get_gdp0() * n.get_core_price() * n.get_gf();
        // Wealth tax on the top 1% (private wealth ~4x GDP; Piketty & Zucman 2014), net of
        // avoidance; paid by owner pops in proportion to their capital income weight.
        if(n.get_wealth_tax() > 0.0f) {
            const double base = 4.0 * gdp * std::max(0.05f, n.get_top1_share0()) * std::max(0.0, 1.0 - P.wealth_tax_flight * n.get_wealth_tax());
            const double revenue = n.get_wealth_tax() / 100.0 * base * years;
            double weight = 0.0;
            std::vector<std::pair<dcon::pop_id, double>> payers;
            for(auto own : n.get_region_ownership()) {
                for(auto loc : own.get_region().get_pop_location()) {
                    const double wgt = static_cast<double>(loc.get_pop().get_size()) * loc.get_pop().get_owner_share();
                    if(wgt > 0.0) payers.emplace_back(loc.get_pop().id, wgt);
                    weight += wgt;
                }
            }
            cause_scope c(state, lever_cause(state, n.id, lever::wealth_tax));
            for(auto const& [p, wgt] : payers) transfer(state, account::of_pop(n.id, p), account::government(n.id), revenue * wgt / weight, flow::wealth_tax);
        }
        // Fuel and food subsidies: the budget pays the absorbed share of the price rise
        // on about a fifth of consumption (energy and food), to households by size.
        if(n.get_subsidy() > 0.0f) {
            const double cost = P.subsidy_fiscal * n.get_subsidy() * price_spike(state, n.id) * 0.6 * gdp * years;
            double size = 0.0;
            for(auto own : n.get_region_ownership()) {
                for(auto loc : own.get_region().get_pop_location()) size += loc.get_pop().get_size();
            }
            cause_scope c(state, lever_cause(state, n.id, lever::subsidy));
            for(auto own : n.get_region_ownership()) {
                for(auto loc : own.get_region().get_pop_location()) {
                    if(size > 0.0) transfer(state, account::government(n.id), account::of_pop(n.id, loc.get_pop().id), cost * loc.get_pop().get_size() / size, flow::subsidies);
                }
            }
        }
        // Vintage capital: investment above its 2019 rate renews the stock faster, and new
        // capital embodies newer technology (relative to the 2019 path).
        {
            const float ref = n.get_investment_ref();
            const float ratio = ref > 0.0f ? n.get_investment() / ref : 1.0f;
            n.set_vintage_tfp(n.get_vintage_tfp() * (1.0f + static_cast<float>(P.vintage_embodiment) * 0.01f
                                                                * static_cast<float>(P.depreciation) / 0.05f * (ratio - 1.0f) * years));
        }
        // Sub-national fiscal capacity and equalisation.
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            r.set_fiscal_capacity(fiscal_capacity_of(state, n.id, r.id));
            // Regional house prices: national prices x local demand over a supply elasticity.
            const float growth = r.get_population_r0() > 0.0f ? r.get_population() / r.get_population_r0() : 1.0f;
            r.set_house_price_r(n.get_house_price() * std::pow(std::max(0.2f, growth), 1.0f / static_cast<float>(P.housing_supply)));
        }
    }
}

// Trend-growth terms of these levers relative to 2019 (longrun.cpp).
float reform_growth(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    auto const& P = state.params;
    // Djankov et al. (2010): 10 pp of effective corporate tax ~ -0.4 pp of growth via investment.
    return static_cast<float>(P.rd_tfp) / 100.0f * w.nation_get_rd_spending(n)
           - 0.0004f * w.nation_get_corporate_shift(n)
           + (w.nation_get_vintage_tfp(n) - 1.0f) * 0.1f;
}

} // namespace geosim::sim
