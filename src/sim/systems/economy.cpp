// Economy (docs/03-economy.md): weekly per nation, sharded by nation index.
//
// Passes on a day (only nations whose weekly shard is due change; SIMD passes
// blend lanes with a mask so every vector does the same work):
//   1. nation pre-pass (scalar)    NPI cuts, adaptation, supply-chain factors
//   2. region capacity   (SIMD)    potential x NPI cut x sickness x sector factors
//   3. nation pass       (scalar)  demand (incl. credit impulse, trade costs),
//                                  the production network (dynamic input-output with
//                                  input stocks; Pichler et al. 2022), prices,
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

// Weekly shard (every nation at full detail; spatial LOD was removed).
bool due(game_state const& state, dcon::nation_id n, bool probe) {
    if(probe) return true;
    return is_due(static_cast<uint32_t>(n.index()), static_cast<tick_t>(state.current_tick), weekly);
}

float step_days(game_state const&, dcon::nation_id, bool probe) {
    return probe ? 1.0f : static_cast<float>(weekly.period_days);
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
    for(uint8_t s = 0; s < S; ++s) {
        const dcon::sector_id sid{s};
        float cut = 0.0f;
        for(uint8_t k = 0; k < D.npi_count; ++k) {
            const float reach = 1.0f - (1.0f - local) * w.nation_get_npi_targeted(n, k);
            cut += w.sector_get_npi_capacity(sid, k) * npi_fraction(state, n, k) * reach;
        }
        w.nation_set_npi_cut(n, s, std::min(0.95f, cut * (1.0f - adaptation)));
        // Supply-chain shortages act through input stocks in the production network
        // (nation_pass). Cyber operations cut energy and business-services capacity
        // (critical infrastructure, IT systems; internet.cpp).
        w.nation_set_sector_factor(n, s, 1.0f - w.sector_get_cyber_exposure(sid) * w.nation_get_cyber_damage(n));
    }
}


bool core_key_is(uint32_t key, char const* text) {
    uint32_t k = 0;
    for(int i = 0; i < 4 && text[i]; ++i) k |= static_cast<uint32_t>(static_cast<unsigned char>(text[i])) << (8 * i);
    return key == k;
}

// ---------------------------------------------------------------- production network
// Dynamic input-output model with input stocks (Pichler, Pangallo, del Rio-Chanona,
// Lafond & Farmer 2022), on top of the value-added demand of the final-demand
// block. `dem` is the value added each sector would need to meet final demand
// (the RAS-calibrated "VA embodied" demand), `cap` its value-added capacity.
//  - gross requirement x_req = dem / v (v = value-added share of gross output);
//    final demand for each domestic product f = x_req - domestic intermediate use;
//  - each sector orders its inputs to cover planned production and to bring its
//    input stocks back to `inventory_days` of use (orders: io_order_days);
//  - production = min(capacity, demand incl. orders, what critical input stocks
//    allow) - Leontief in critical inputs (goods, energy), services can be short
//    for a while without stopping production;
//  - a constrained sector rations all its customers in proportion (final demand
//    and downstream orders), so shortages travel down the chain and demand
//    shortfalls travel up it, with stocks as buffers;
//  - imported inputs arrive in proportion to foreign capacity (partners'
//    manufacturing) x the logistics arrival rate (transit delays, logistics.cpp).
// In the calibrated steady state stocks sit at target and x = x_req, so output
// equals min(dem, cap) exactly as without the network.
void production_network(game_state& state, dcon::nation_id n, std::array<float, max_sectors> const& dem,
                        std::array<float, max_sectors> const& cap, std::array<float, max_sectors>& out,
                        std::array<float, max_sectors>& final_fill, float dt, bool apply, float& restock_ratio) {
    restock_ratio = 1.0f;
    auto& w = state.world;
    auto const& P = state.params;
    const uint8_t S = state.derived.sector_count;
    if(P.io_network == 0.0) {
        for(uint8_t s = 0; s < S; ++s) {
            out[s] = std::min(dem[s], cap[s]);
            final_fill[s] = dem[s] > 0.0f ? std::min(1.0f, out[s] / dem[s]) : 1.0f;
        }
        return;
    }
    std::array<float, max_sectors> v{}, x_req{}, x_cap{}, x_plan{}, f{}, demand{}, x{}, rho{};
    for(uint8_t s = 0; s < S; ++s) {
        v[s] = std::max(0.05f, w.sector_get_value_added_share(dcon::sector_id{s}));
        x_req[s] = dem[s] / v[s];
        x_cap[s] = cap[s] / v[s];
        x_plan[s] = std::min(x_req[s], x_cap[s]);
    }
    auto A = [&](uint8_t r, uint8_t u) { return w.sector_get_io_coeff(dcon::sector_id{u}, r); }; // input r per unit of u
    for(uint8_t r = 0; r < S; ++r) {
        const float dom = 1.0f - w.nation_get_io_import(n, r);
        float use = 0.0f;
        for(uint8_t u = 0; u < S; ++u) use += dom * A(r, u) * x_req[u];
        f[r] = std::max(0.0f, x_req[r] - use);
    }
    // Foreign supply of imported inputs: partners' manufacturing x arrivals by sea.
    const float foreign = apply ? std::clamp(world_manufacturing_index(state, n) * w.nation_get_input_availability(n), 0.0f, 1.5f) : 1.0f;
    const float order_rate = 1.0f / static_cast<float>(P.io_order_days);
    // Orders and demand.
    std::array<std::array<float, max_sectors>, max_sectors> order{}, order_m{};
    for(uint8_t r = 0; r < S; ++r) demand[r] = f[r];
    for(uint8_t u = 0; u < S; ++u) {
        const float days = w.sector_get_inventory_days(dcon::sector_id{u});
        for(uint8_t r = 0; r < S; ++r) {
            const float a = A(r, u);
            if(a <= 0.0f) continue;
            const float m = w.nation_get_io_import(n, r);
            const auto k = static_cast<uint8_t>(r * S + u);
            const float target_d = (1.0f - m) * a * x_plan[u] * days;
            const float target_m = m * a * x_plan[u] * days;
            order[r][u] = std::max(0.0f, (1.0f - m) * a * x_plan[u] + (target_d - w.nation_get_io_stock(n, k)) * order_rate);
            order_m[r][u] = std::max(0.0f, m * a * x_plan[u] + (target_m - w.nation_get_io_stock_m(n, k)) * order_rate);
            demand[r] += order[r][u];
        }
    }
    // Imported inputs ordered above (restocking) or below current use, as a ratio of use.
    {
        double ordered = 0.0, used = 0.0;
        for(uint8_t u = 0; u < S; ++u) {
            for(uint8_t r = 0; r < S; ++r) {
                ordered += order_m[r][u];
                used += w.nation_get_io_import(n, r) * A(r, u) * x_plan[u];
            }
        }
        restock_ratio = used > 0.0 ? static_cast<float>(ordered / used) : 1.0f;
    }
    // Production: capacity, demand, critical input stocks.
    for(uint8_t u = 0; u < S; ++u) {
        float x_inp = 1e30f;
        for(uint8_t r = 0; r < S; ++r) {
            const float a = A(r, u);
            if(a <= 0.0f || w.sector_get_critical_input(dcon::sector_id{r}) <= 0.0f) continue;
            const auto k = static_cast<uint8_t>(r * S + u);
            // Stocks plus this step's deliveries at the expected rate.
            const float avail = w.nation_get_io_stock(n, k) + w.nation_get_io_stock_m(n, k)
                                + dt * (order[r][u] * std::min(1.0f, x_cap[r] / std::max(1e-9f, demand[r]))
                                        + order_m[r][u] * (w.sector_get_shipped(dcon::sector_id{r}) > 0.0f ? foreign : 1.0f));
            x_inp = std::min(x_inp, avail / (a * dt));
        }
        x[u] = std::min({x_cap[u], demand[u], x_inp});
        out[u] = x[u] * v[u];
    }
    for(uint8_t r = 0; r < S; ++r) {
        rho[r] = demand[r] > 0.0f ? std::min(1.0f, x[r] / demand[r]) : 1.0f;
        final_fill[r] = rho[r];
    }
    if(!apply) {
        return;
    }
    // Stocks: deliveries in, use out.
    for(uint8_t u = 0; u < S; ++u) {
        for(uint8_t r = 0; r < S; ++r) {
            const float a = A(r, u);
            if(a <= 0.0f) continue;
            const float m = w.nation_get_io_import(n, r);
            const auto k = static_cast<uint8_t>(r * S + u);
            const float arrive_m = order_m[r][u] * (w.sector_get_shipped(dcon::sector_id{r}) > 0.0f ? foreign : 1.0f);
            w.nation_set_io_stock(n, k, std::max(0.0f, w.nation_get_io_stock(n, k) + (rho[r] * order[r][u] - (1.0f - m) * a * x[u]) * dt));
            w.nation_set_io_stock_m(n, k, std::max(0.0f, w.nation_get_io_stock_m(n, k) + (arrive_m - m * a * x[u]) * dt));
        }
        w.nation_set_gross_output(n, u, x[u]);
        w.nation_set_io_demand(n, u, demand[u]);
        w.nation_set_io_constraint(n, u, x_req[u] > 0.0f ? x[u] / std::min(x_req[u], std::max(1e-9f, x_cap[u])) : 1.0f);
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
    // Wealth effects from house and equity prices (assets.cpp) shift spending plans.
    w.nation_set_conf_rate(n, conf * rate_c * std::max(0.5f, 1.0f + (apply ? w.nation_get_wealth_effect(n) : 0.0f)));

    const double c_des_total = w.nation_get_c_des_total(n);
    // Household spending plans plus the credit impulse spent on goods (credit.cpp).
    const float credit_c = apply ? w.nation_get_credit_flow_hh(n) / p : 0.0f;
    const float c_real = static_cast<float>(c_des_total) / p + credit_c;
    const float hh_dom = (1.0f - m_share) * c_real;
    const float g_real = w.nation_get_g0(n) * gf;
    const float accel = std::clamp(1.0f + static_cast<float>(P.accelerator) * (w.nation_get_util_smoothed(n) - 1.0f),
                                   static_cast<float>(P.investment_floor), 1.3f);
    // Cost of capital: policy rate and long yields (bonds.cpp); firms with foreign-currency
    // debt cut investment when the currency falls (balance-sheet effect; Cespedes et al. 2004).
    const float iw = static_cast<float>(P.i_long_weight);
    const float rate_dev = (1.0f - iw) * (r - r0) + iw * (w.nation_get_yield10(n) - w.nation_get_yield10_0(n));
    const float log_e0 = std::log(std::max(0.05f, w.nation_get_exchange_rate(n)));
    const float balance_sheet = 1.0f - static_cast<float>(P.fx_balance_sheet) * w.nation_get_fx_loan_share(n) * std::max(0.0f, -log_e0);
    const float i_real = w.nation_get_i0(n) * gf * accel * (1.0f - static_cast<float>(P.rate_i_elasticity) * rate_dev) * std::max(0.3f, balance_sheet)
                         * (1.0f + (apply ? w.nation_get_board_investment(n) : 0.0f))
                         * (1.0f - static_cast<float>(P.crime_investment) * std::max(0.0f, w.nation_get_crime(n) - 1.0f))
                         + (apply ? w.nation_get_credit_flow_firm(n) / p : 0.0f)
                         // FDI above the baseline funds new investment (finance.cpp).
                         + (apply ? static_cast<float>(P.fdi_investment_share) * w.nation_get_fdi_flow(n) / 100.0f * w.nation_get_gdp0_daily(n) * gf : 0.0f);
    // Exports follow partners' import demand through bilateral trade shares, cut by
    // sanctions and war (relative to the starting access).
    const float access0 = std::max(1e-6f, w.nation_get_trade_access0(n));
    // Real exchange rate (1 = start): appreciation makes exports dearer, imports cheaper.
    const float log_e = std::log(std::max(0.05f, w.nation_get_exchange_rate(n)));
    // Freight costs and delays are a trade cost on the goods share of exports (logistics.cpp).
    const float trade_cost = apply ? 1.0f - w.nation_get_goods_export_share(n) * (1.0f - w.nation_get_trade_cost_factor(n)) : 1.0f;
    // Filtering above the start cuts digitally delivered services trade (Freund & Weinhold 2004).
    const float digital = apply ? 1.0f - (1.0f - w.nation_get_goods_export_share(n)) * static_cast<float>(P.net_digital_trade)
                                             * std::max(0.0f, w.nation_get_net_filtering(n) - w.nation_get_net_filtering0(n))
                                : 1.0f;
    const float x_real = w.nation_get_x0(n) * gf * (apply ? trade_access(state, n) / access0 : 1.0f)
                         * std::exp(-static_cast<float>(P.fx_export_elasticity) * log_e) * trade_cost * digital;
    const float tourism = apply ? travel_index(state, n) : 1.0f;

    std::array<float, max_sectors> hh{};
    std::array<float, max_sectors> dem{};
    std::array<float, max_sectors> out{};
    float blocked_contact = 0.0f;
    // Linear expenditure system (Stone 1954): subsistence quantities of food and energy
    // are bought first; the rest of the budget is shared by marginal shares, deflated by
    // sector relative prices (markups; below). With base prices and spending this
    // reproduces the calibrated composition.
    const float hh_base = (1.0f - m_share) * w.nation_get_c0(n) * gf;
    float gamma_total = 0.0f, gamma_cost = 0.0f;
    std::array<float, max_sectors> gamma{};
    for(uint8_t s = 0; s < S; ++s) {
        gamma[s] = w.sector_get_subsistence(dcon::sector_id{s}) * w.nation_get_comp_c(n, s) * hh_base;
        gamma_total += gamma[s];
        gamma_cost += gamma[s] * w.nation_get_sector_price(n, s);
    }
    const float discretionary = std::max(0.0f, hh_dom - gamma_cost);
    for(uint8_t s = 0; s < S; ++s) {
        const dcon::sector_id sid{s};
        const float beta = hh_base > gamma_total ? (w.nation_get_comp_c(n, s) * hh_base - gamma[s]) / (hh_base - gamma_total) : w.nation_get_comp_c(n, s);
        hh[s] = gamma[s] + beta * discretionary / std::max(0.2f, w.nation_get_sector_price(n, s));
        const float vd = w.sector_get_voluntary_distancing(sid);
        const float avoided = hh[s] * vd * (1.0f - fear);
        hh[s] -= avoided;
        blocked_contact += avoided;
        // Exports of contact services are partly tourism: they follow travel, not trade.
        const float x_factor = vd > 0.0f ? 1.0f - w.nation_get_tourism_share(n) * (1.0f - tourism) : 1.0f;
        dem[s] = hh[s] + w.nation_get_comp_g(n, s) * g_real + w.nation_get_comp_i(n, s) * (1.0f - m_share) * i_real
                 + w.nation_get_comp_x(n, s) * (1.0f - m_share) * x_real * x_factor;
        out[s] = std::min(dem[s], cap[s]);
        if(vd > 0.0f && dem[s] > 0.0f) {
            blocked_contact += (dem[s] - out[s]) * hh[s] / dem[s];
        }
    }
    // War: military procurement from domestic manufacturing, paid by the government.
    const float war_g = apply ? static_cast<float>(P.war_spending) * w.nation_get_war_effort(n) * w.nation_get_gdp0_daily(n) * gf : 0.0f;
    // Public investment above baseline buys construction; defence above its 2019 share
    // buys manufacturing (fiscal.cpp, institutions.cpp).
    const float pub_g = apply ? w.nation_get_public_investment(n) / 100.0f * w.nation_get_gdp0_daily(n) * gf : 0.0f;
    const float def_g = apply ? (w.nation_get_defense_share(n) - w.nation_get_defense_share0(n)) / 100.0f * w.nation_get_gdp0_daily(n) * gf : 0.0f;
    // Education and health above baseline (policy.cpp) buy public services.
    const float svc_g = apply ? (w.nation_get_education_spending(n) + w.nation_get_health_spending(n) + w.nation_get_police(n) + w.nation_get_rd_spending(n))
                                    / 100.0f * w.nation_get_gdp0_daily(n) * gf : 0.0f;
    for(auto sec : w.in_sector) {
        if(core_key_is(sec.get_key(), "con")) dem[sec.id.index()] += pub_g * (1.0f - m_share);
    }
    if(D.gov_sector) dem[static_cast<uint8_t>(D.gov_sector.index())] += svc_g;
    if(D.manufacturing_sector) {
        const auto ms = static_cast<uint8_t>(D.manufacturing_sector.index());
        dem[ms] += (war_g + def_g) * (1.0f - m_share);
        const float redirect = static_cast<float>(P.substitution) * blocked_contact;
        hh[ms] += redirect;
        dem[ms] += redirect;
        out[ms] = std::min(dem[ms], cap[ms]);
    }
    std::array<float, max_sectors> final_fill{};
    float restock = 1.0f;
    production_network(state, n, dem, cap, out, final_fill, dt, apply, restock);
    const float prod0 = w.nation_get_oil_production0(n);
    const float oil_share = w.nation_get_oil_share_of_energy(n);
    if(D.energy_sector && prod0 > 0.0f) {
        const auto es = static_cast<uint8_t>(D.energy_sector.index());
        const float dr = pot[es] > 0.0f ? std::min(1.0f, dem[es] / pot[es]) : 1.0f;
        out[es] = pot[es] * ((1.0f - oil_share) * dr + oil_share * w.nation_get_oil_production(n) / prod0);
    }

    float hh_real = 0.0f, gdp = 0.0f, pot_total = 0.0f, hh_gov_real = 0.0f, import_demand = 0.0f;
    for(uint8_t s = 0; s < S; ++s) {
        const float fill = final_fill[s];
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
    // Intermediate imports follow the production network's orders (restocking raises them).
    const float intermediate = static_cast<float>(P.io_import_trade_share);
    const float imports = w.nation_get_m0(n) * gf * (base > 0.0f ? import_demand / base : 1.0f)
                          * ((1.0f - intermediate) + intermediate * restock)
                          * std::exp(static_cast<float>(P.fx_import_elasticity) * log_e) * (apply ? w.nation_get_trade_cost_factor(n) : 1.0f)
                          * (apply ? std::exp(-static_cast<float>(P.tariff_elasticity) * (w.nation_get_tariff(n) - w.nation_get_tariff0(n))) : 1.0f);
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
        // Depreciation passes partly into prices (import costs).
        // Pass-through rises with trend inflation (Taylor 2000; nation fx_passthrough_n).
        // Wage growth beyond productivity raises unit labour costs (labour.cpp).
        const float pi = pe + static_cast<float>(P.phillips) * gs - w.nation_get_fx_passthrough_n(n) * w.nation_get_fx_change(n)
                         + 0.5f * w.nation_get_wage_nominal_growth(n);
        // Freight-driven import costs reach consumer prices through the import share.
        const float ici = w.nation_get_import_cost_index(n);
        const float prev = std::max(0.5f, w.nation_get_import_cost_prev(n));
        float freight_level = std::pow(ici / prev, static_cast<float>(P.freight_passthrough) * m_share);
        w.nation_set_import_cost_prev(n, ici);
        // Tariffs pass fully into import prices (Amiti, Redding & Weinstein 2019).
        const float tariff_level = std::pow((1.0f + w.nation_get_tariff(n)) / (1.0f + w.nation_get_tariff_prev(n)), m_share);
        w.nation_set_tariff_prev(n, w.nation_get_tariff(n));
        freight_level *= tariff_level;
        w.nation_set_core_price(n, p * std::exp(std::log1p(pi) * dt / 365.0f) * freight_level);
        pe += (pi - pe) * std::min(1.0f, dt / static_cast<float>(P.expectations_days));
        w.nation_set_inflation_expect(n, pe);
        const float oil_rel = D.oil ? w.commodity_get_price(D.oil) / w.commodity_get_price0(D.oil) : 1.0f;
        // Energy and food above core inflation, part absorbed by subsidies; VAT changes
        // pass into consumer prices (reforms.cpp).
        const float raw = w.nation_get_core_price(n) * (1.0f + static_cast<float>(P.oil_cpi_share) * (oil_rel - 1.0f))
                          * commodity_cpi_factor(state, n) * (1.0f + 0.05f * carbon_cost_share(state, n));
        w.nation_set_cpi_raw(n, raw);
        const float core_p = w.nation_get_core_price(n);
        w.nation_set_cpi(n, (core_p + (raw - core_p) * (raw > core_p ? 1.0f - w.nation_get_subsidy(n) : 1.0f))
                                * (1.0f + static_cast<float>(P.vat_pass) * w.nation_get_vat_shift(n) / 100.0f));
        // Sector relative prices: markups follow capacity pressure (Kalecki 1954); a
        // Calvo share resets each step (Calvo 1983). Normalised to the consumption basket,
        // so they shift the composition of demand, not aggregate inflation.
        {
            const float reset = 1.0f - std::pow(1.0f - static_cast<float>(P.calvo_monthly), dt / 28.0f);
            float mean = 0.0f;
            for(uint8_t s = 0; s < S; ++s) {
                const float pressure = cap[s] > 0.0f ? std::clamp(dem[s] / cap[s] - 1.0f, -0.5f, 0.5f) : 0.0f;
                const float target = std::exp(static_cast<float>(P.markup_utilisation) * pressure);
                const float ps = w.nation_get_sector_price(n, s) + (target - w.nation_get_sector_price(n, s)) * reset;
                w.nation_set_sector_price(n, s, ps);
                mean += w.nation_get_comp_c(n, s) * ps;
            }
            if(mean > 0.0f) {
                for(uint8_t s = 0; s < S; ++s) w.nation_set_sector_price(n, s, w.nation_get_sector_price(n, s) / mean);
            }
        }
    }

    // ---- labour-market parameters for the pop pass
    const float coverage = w.nation_get_stw_coverage(n);
    const float epl = std::max(0.0f, w.nation_get_epl(n) + static_cast<float>(P.epl_layoff) * 2.0f * w.nation_get_epl_shift(n));
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
    // Boards hoard labour beyond what EPL forces (coordinated economies; firms.cpp).
    const float board = static_cast<float>(P.board_hoarding_gain) * (w.nation_get_board_hoarding(n) - w.nation_get_board_hoarding0(n));
    w.nation_set_absorb(n, std::clamp(coverage + (1.0f - coverage) * (epl * static_cast<float>(P.epl_hoarding) + board), 0.0f, 1.0f));
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
        // Small winning coalitions channel support to firms and insiders (selectorate theory).
        const double firm_share = std::clamp(P.fiscal_firm_share + P.selectorate_firm_tilt * (0.35 - w.nation_get_winning_coalition(n)), 0.1, 0.8);
        fiscal_firms = paid * firm_share;
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
    const double comp = apply ? compliance_ratio(state, n) : 1.0;
    const double indirect = w.nation_get_tax_indirect(n) * comp * c_to_firms;
    const double oil_value = static_cast<double>(w.nation_get_oil_production(n)) * 1e6 * oil_price;
    const double resource = w.nation_get_resource_tax(n) * oil_value;
    // Non-oil commodities: terms-of-trade gains from abroad, the state's rent share,
    // and loan interest above / below the baseline path (credit.cpp).
    const double commodity_tot = apply ? w.nation_get_commodity_tot(n) : 0.0;
    const double commodity_rent = apply ? w.nation_get_commodity_rent(n) : 0.0;
    const double loan_interest_firm = apply ? w.nation_get_loan_interest_firm(n) : 0.0;
    firm_va += commodity_tot;
    const double profit = firm_va - w.nation_get_wage_bill_private(n) - indirect - resource - commodity_rent - loan_interest_firm;
    double ps = w.nation_get_profit_smoothed(n);
    if(apply) {
        ps += (profit - ps) * std::min(1.0, static_cast<double>(dt) / P.profit_smoothing_days);
        w.nation_set_profit_smoothed(n, static_cast<float>(ps));
    }
    const double firm_tax = w.nation_get_tax_firm(n) * comp * std::max(0.0, profit);
    const double dividends = w.nation_get_dividend_payout(n) * std::max(0.0, ps);
    const double debt = apply ? std::max(0.0, -w.nation_get_bal_gov(n)) : w.nation_get_gov_debt0(n);
    // Effective rate on the debt: rolls over at market yields (bonds.cpp).
    const double gov_interest = debt * static_cast<double>(w.nation_get_gov_rate(n)) / 365.0;
    // Later retirement (policy.cpp) cuts pension outlays (OECD Pensions at a Glance).
    const double pensions = static_cast<double>(w.nation_get_pensions0(n)) * p * gf
                            * std::max(0.5, 1.0 - state.params.retirement_pension_saving * w.nation_get_retirement_age_shift(n))
                            * (apply ? static_cast<double>(pension_ageing_factor(state, n)) : 1.0); // ageing (society.cpp)
    auto per = [](double total, float weight) { return weight > 0.0f ? static_cast<float>(total / weight) : 0.0f; };
    w.nation_set_rate_transfer(n, per(pensions, w.nation_get_transfer_weight_total(n)));
    w.nation_set_rate_fiscal(n, per(fiscal_hh / dt, w.nation_get_size_total(n)));
    w.nation_set_rate_dividend(n, per(dividends, w.nation_get_owner_weight_total(n)));
    w.nation_set_rate_interest(n, per(gov_interest, w.nation_get_owner_weight_total(n)));
    // Remittances: received follow migrants' host incomes (world index), paid follow own workers'.
    float remit_index = 1.0f;
    for(auto c : w.in_climate) remit_index = c.get_remittance_index();
    w.nation_set_rate_remit_in(n, per(static_cast<double>(w.nation_get_remit_in0(n)) * remit_index * p, w.nation_get_transfer_weight_total(n)));
    w.nation_set_rate_remit_out(n, per(static_cast<double>(w.nation_get_remit_out0(n)) * (apply ? payer_income_index(state, n) : 1.0f) * p,
                                       w.nation_get_size_total(n)));

    if(apply) {
        const double gov_purchases = static_cast<double>(g_real) * (1.0 - w.nation_get_comp_g(n, static_cast<uint8_t>(D.gov_sector.index()))) * p;
        const auto firms = account::firms(n);
        const auto gov = account::government(n);
        const auto fin = account::finance(n);
        const auto row = account::rest_of_world(n);
        const double ddt = dt;
        transfer(state, gov, firms, gov_purchases * ddt, flow::gov_purchases);
        {
            cause_scope c(state, lever_cause(state, n, lever::fiscal));
            transfer(state, gov, firms, fiscal_firms, flow::fiscal_firms);
        }
        {
            cause_scope c(state, journal_cause(n, journal_war));
            transfer(state, gov, firms, static_cast<double>(war_g) * p * ddt, flow::war_spending);
        }
        {
            cause_scope c(state, lever_cause(state, n, lever::public_investment));
            transfer(state, gov, firms, static_cast<double>(pub_g) * p * ddt, flow::public_investment);
        }
        {
            cause_scope c(state, lever_cause(state, n, lever::health));
            transfer(state, gov, firms, static_cast<double>(svc_g) * p * ddt, flow::public_services);
        }
        {
            cause_scope c(state, lever_cause(state, n, lever::defense));
            transfer(state, gov, firms, static_cast<double>(def_g) * p * ddt, flow::defense);
        }
        w.nation_set_defense_spend(n, def_g);
        {
            // Carbon-price revenue above the start (climate.cpp).
            cause_scope c(state, lever_cause(state, n, lever::carbon_price));
            const double ene = D.energy_sector ? static_cast<double>(out[static_cast<uint8_t>(D.energy_sector.index())]) * p : 0.0;
            transfer(state, firms, gov, ene * carbon_cost_share(state, n) * w.nation_get_fossil_share(n) * ddt, flow::carbon_tax);
        }
        {
            cause_scope c(state, lever_cause(state, n, lever::tariff));
            transfer(state, firms, gov, static_cast<double>(imports) * p * (w.nation_get_tariff(n) - w.nation_get_tariff0(n)) * ddt, flow::tariffs);
        }
        transfer(state, row, firms, static_cast<double>(x_real) * p * ddt, flow::exports);
        transfer(state, firms, row, static_cast<double>(imports) * p * ddt, flow::imports);
        transfer(state, firms, gov, indirect * ddt, flow::tax_indirect);
        transfer(state, firms, gov, resource * ddt, flow::tax_resource);
        transfer(state, firms, gov, firm_tax * ddt, flow::tax_firms);
        transfer(state, gov, fin, gov_interest * ddt, flow::interest_gov);
        transfer(state, row, firms, commodity_tot * ddt, flow::commodity_terms);
        transfer(state, firms, gov, commodity_rent * ddt, flow::tax_resource);
        transfer(state, firms, fin, loan_interest_firm * ddt, flow::interest_loans);
        // Credit impulse spent on goods: banks pay the sellers (loans create deposits).
        {
            cause_scope c(state, w.nation_get_directed_credit(n) > 0.0f ? lever_cause(state, n, lever::directed_credit) : 0u);
            transfer(state, fin, firms, (static_cast<double>(w.nation_get_credit_flow_hh(n)) + w.nation_get_credit_flow_firm(n)) * ddt, flow::credit);
        }

        for(uint8_t s = 0; s < S; ++s) {
            w.nation_set_output(n, s, out[s]);
            w.nation_set_capacity(n, s, cap[s]);
            w.nation_set_demand(n, s, dem[s]);
        }
        w.nation_set_consumption(n, c_eff);
        w.nation_set_investment(n, i_real);
        w.nation_set_gov_spend(n, g_real + war_g + pub_g + def_g + svc_g);
        w.nation_set_exports(n, x_real);
        record_bilateral_exports(state, n, x_real);
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
    const uint8_t S = D.sector_count;
    const auto gov_sector = static_cast<uint8_t>(D.gov_sector.index());

    std::vector<dcon::nation_id> due_nations;
    for(auto n : w.in_nation) {
        const bool d = due(state, n.id, probe);
        n.set_econ_due(d ? 1.0f : 0.0f);
        n.set_econ_dt(step_days(state, n.id, probe));
        if(d) {
            due_nations.push_back(n.id);
        }
    }
    if(due_nations.empty()) {
        return;
    }
    // Lanes of nations not due today keep their values (probe: everyone is due).
    auto lane_due = [&](auto nat) { return w.nation_get_econ_due(nat) > ve::fp_vector(0.5f); };

    // 1. nation pre-pass
    for(auto n : due_nations) {
        nation_prepass(state, n, apply);
    }

    // 2. region capacity
    const ve::fp_vector sick(static_cast<float>(P.sickness_absence));
    w.execute_parallel_over_region([&](auto ids) {
        const auto nat = w.region_get_owner(ids);
        const auto mask = lane_due(nat);
        // Sickness absence: infectious working-age people, all pathogens.
        ve::fp_vector young_inf(0.0f);
        for(uint8_t p = 0; p < D.pathogen_count; ++p) {
            young_inf = young_inf + w.region_get_ep_i(ids, epi_index(p, age_young));
        }
        const uint8_t y0 = epi_index(0, age_young);
        const auto young_n = w.region_get_ep_s(ids, y0) + w.region_get_ep_e(ids, y0) + w.region_get_ep_i(ids, y0) + w.region_get_ep_r(ids, y0);
        const auto absent = ve::min(ve::fp_vector(0.5f), sick * young_inf / ve::max(young_n, ve::fp_vector(1.0f)));
        const auto gf = w.nation_get_gf(nat);
        for(uint8_t s = 0; s < S; ++s) {
            const auto cap = w.region_get_va0(ids, s) * gf * (ve::fp_vector(1.0f) - w.nation_get_npi_cut(nat, s))
                             * (ve::fp_vector(1.0f) - absent) * w.nation_get_sector_factor(nat, s) * w.region_get_unrest_capacity(ids)
                             * (ve::fp_vector(1.0f) - w.nation_get_occupied(nat)); // territory held by an enemy (geopolitics.cpp)
            w.region_set_capacity(ids, s, ve::select(mask, cap, w.region_get_capacity(ids, s)));
        }
    });

    // 3. nation pass
    for(auto n : due_nations) {
        nation_pass(state, n, w.nation_get_econ_dt(n), apply);
    }

    // 4. region output and utilisation
    w.execute_parallel_over_region([&](auto ids) {
        const auto nat = w.region_get_owner(ids);
        const auto mask = lane_due(nat);
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

    const ve::fp_vector one(1.0f);
    const ve::fp_vector zero(0.0f);

    const ve::fp_vector match(static_cast<float>(P.match_efficiency));
    const ve::fp_vector lf_return(static_cast<float>(P.lf_return_rate) / 7.0f);
    const ve::fp_vector inc_days(static_cast<float>(P.income_smoothing_days));
    const ve::fp_vector wealth_rate(static_cast<float>(P.wealth_spend_rate / 365.0));
    const ve::fp_vector lag(static_cast<float>(P.layoff_lag_days));
    const ve::fp_vector lag_epl(static_cast<float>(P.layoff_lag_epl));
    const ve::fp_vector lf_inf(static_cast<float>(P.lf_exit_informal));
    const ve::fp_vector net_drag(static_cast<float>(P.network_return_drag));
    const ve::fp_vector lab_el(static_cast<float>(P.labor_demand_elasticity));
    // Effective household tax (scale x compliance x behavioural base), per nation, for the pop pass.
    for(auto n : due_nations) w.nation_set_tax_multiplier(n, apply ? household_tax_multiplier(state, n) : 1.0f);
    w.execute_parallel_over_pop([&](auto ids) {
        const auto nat = w.pop_get_nation(ids);
        const auto reg = w.pop_get_home(ids);
        const auto mask = lane_due(nat);
        // Step length per lane (weekly, or 28 days for nations at low detail).
        const auto vdt = w.nation_get_econ_dt(nat);
        const auto week_frac = vdt / ve::fp_vector(7.0f);
        const auto ret = ve::min(ve::fp_vector(1.0f), lf_return * vdt);
        const auto smooth_inc = ve::min(ve::fp_vector(1.0f), vdt / inc_days);
        const auto gf = w.nation_get_gf(nat);
        const auto p = w.nation_get_core_price(nat);
        // Informal jobs (by identity group; scenario groups) get no job retention, no
        // employment protection and (scaled by formal_rel) less unemployment insurance;
        // laid-off informal workers leave the labour force more often (ILO 2020).
        const auto informal = w.pop_get_informal(ids);
        const auto formal = one - informal;
        const auto absorb = w.nation_get_absorb(nat) * formal;
        const auto speed = ve::min(one, w.nation_get_layoff_speed_eff(nat) * week_frac);
        const auto exit = ve::min(one, ve::max(zero, w.nation_get_lf_exit(nat) + lf_inf * (informal - w.nation_get_informality(nat))));
        const auto stw_mult = w.nation_get_stw_mult(nat) * formal;
        const auto tau = lag * (one + lag_epl * w.nation_get_epl(nat));
        const auto slow = ve::min(one, vdt / tau);

        auto unemployed = w.pop_get_unemployed(ids);
        auto inactive = w.pop_get_inactive(ids);
        ve::fp_vector work(0.0f), pub(0.0f), stw(0.0f), programmes(0.0f);
        for(uint8_t s = 0; s < S; ++s) {
            const auto e = w.pop_get_employed(ids, s);
            const auto wage = w.pop_get_wage0(ids, s) * gf * p * w.pop_get_wage_mult(ids);
            if(s == gov_sector) {
                pub = pub + e * wage; // public employment is held
                continue;
            }
            const auto base = w.pop_get_emp0(ids, s);
            // Labour demand falls with real labour costs (Lichter, Peichl & Siegloch 2015).
            const auto need = base * w.region_get_util(reg, s) * ve::max(ve::fp_vector(0.5f), one - lab_el * (w.pop_get_wage_mult(ids) - one));
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
        // Networked informal migrants who went home return slowly (Munshi & Rosenzweig 2016).
        const auto back = inactive * ret * (one - net_drag * w.pop_get_network(ids) * informal);
        inactive = inactive - back;
        unemployed = unemployed + back;

        const auto avg_wage = w.pop_get_wage_avg0(ids) * gf * p;
        const auto formal_rel = w.pop_get_formal_rel(ids);
        const auto benefit = unemployed * avg_wage * w.nation_get_benefit_mult(nat) * formal_rel;
        programmes = programmes + unemployed * avg_wage * ve::max(zero, w.nation_get_benefit_mult(nat) - w.nation_get_benefit_base(nat)) * formal_rel;
        const auto transfers = w.pop_get_transfer_weight(ids) * w.nation_get_rate_transfer(nat);
        const auto fiscal = w.pop_get_size(ids) * w.nation_get_rate_fiscal(nat);
        const auto dividends = w.pop_get_owner_weight(ids) * w.nation_get_rate_dividend(nat);
        const auto interest = w.pop_get_owner_weight(ids) * w.nation_get_rate_interest(nat);
        const auto gross = work + pub + stw;
        const auto tax = w.nation_get_tax_hh(nat) * w.nation_get_tax_multiplier(nat) * w.pop_get_tax_factor(ids) * (gross + dividends + interest);
        const auto remit_in = w.pop_get_transfer_weight(ids) * w.nation_get_rate_remit_in(nat);
        const auto remit_out = w.pop_get_size(ids) * w.nation_get_rate_remit_out(nat);
        const auto loan_interest = w.pop_get_size(ids) * w.nation_get_rate_loan_interest(nat); // above/below the credit path
        const auto other = transfers + fiscal + dividends + interest + remit_in - remit_out - loan_interest;
        const auto net = gross + benefit + other - tax;
        const auto taxable = gross + other;
        const auto net_factor = ve::select(taxable > zero, one - tax / ve::max(taxable, ve::fp_vector(1e-9f)), one);

        // Permanent income is smoothed in real terms relative to the trend path (gf):
        // households expect trend growth, so smoothing does not make spending lag the
        // growth of potential (a lag that compounds into a demand drift; docs/13).
        const auto inc_perm_old = w.pop_get_income_perm(ids);
        const auto inc_perm = probe ? inc_perm_old : inc_perm_old + (net / (p * gf) - inc_perm_old) * smooth_inc;
        const auto c_new = ve::max(zero, (one - w.pop_get_saving_rate0(ids)) * inc_perm * gf * p * w.nation_get_conf_rate(nat)
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
        w.pop_set_f_remit_in(ids, ve::select(mask, remit_in, zero));
        w.pop_set_f_remit_out(ids, ve::select(mask, remit_out, zero));
        if(!probe) {
            w.pop_set_c_des(ids, ve::select(mask, c_new, w.pop_get_c_des(ids)));
        }
    });

    // 6. reduction: ledger transfers in index order, nation totals.
    struct totals {
        double c_des = 0, wages = 0, programmes = 0, employed = 0, unemployed = 0;
    };
    std::vector<totals> tot(w.nation_size());
    for(auto pp : w.in_pop) {
        const dcon::nation_id n = pp.get_nation().id;
        if(w.nation_get_econ_due(n) < 0.5f) {
            continue;
        }
        const double ddt = w.nation_get_econ_dt(n);
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
        {
            cause_scope c(state, lever_cause(state, n, lever::stw_coverage));
            transfer(state, gov, me, pp.get_f_stw() * ddt, flow::job_retention_pay);
        }
        transfer(state, gov, me, pp.get_f_benefit() * ddt, flow::unemployment_benefits);
        transfer(state, gov, me, pp.get_f_transfer() * ddt, flow::pensions);
        {
            cause_scope c(state, lever_cause(state, n, lever::fiscal));
            transfer(state, gov, me, pp.get_f_fiscal() * ddt, flow::fiscal_households);
        }
        transfer(state, firms, me, pp.get_f_dividend() * ddt, flow::dividends);
        transfer(state, account::finance(n), me, pp.get_f_interest() * ddt, flow::interest_households);
        transfer(state, me, gov, pp.get_f_tax() * ddt, flow::tax_households);
        transfer(state, me, firms, pp.get_f_spend_firms() * ddt, flow::consumption);
        transfer(state, me, gov, pp.get_f_spend_gov() * ddt, flow::consumption);
        transfer(state, account::rest_of_world(n), me, pp.get_f_remit_in() * ddt, flow::remittances_in);
        transfer(state, me, account::rest_of_world(n), pp.get_f_remit_out() * ddt, flow::remittances_out);
        transfer(state, me, account::finance(n), static_cast<double>(pp.get_size()) * w.nation_get_rate_loan_interest(n) * ddt, flow::interest_loans);
        {
            const float st = w.nation_get_size_total(n);
            if(st > 0.0f) {
                transfer(state, account::finance(n), me, static_cast<double>(pp.get_size() / st) * w.nation_get_credit_deposit_hh(n) * ddt,
                         flow::credit_deposits);
            }
        }
        pp.set_balance_trend(pp.get_balance_trend()
                             + static_cast<double>(pp.get_saving_rate0()) * pp.get_income_perm() * w.nation_get_gf(n) * w.nation_get_core_price(n) * ddt);
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
