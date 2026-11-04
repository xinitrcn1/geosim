// Asset prices and wealth effects (docs/03-economy.md#money-banking-and-finance), every 28 days.
//
// - House prices: the fundamental price-to-rent ratio is the inverse of the user
//   cost of housing (Poterba 1984; Himmelberg, Mayer & Sinai 2005): real
//   mortgage rate + depreciation + risk. Rents follow household real income.
//   Credit supply lifts prices beyond fundamentals (Favara & Imbs 2015; Mian &
//   Sufi 2009). Prices adjust slowly toward that target.
// - Equity: discounted smoothed profits at the real long rate plus an equity
//   risk premium (Gordon-growth valuation).
// - Wealth effects: households spend a share of the change in housing (~5 cents
//   per dollar; Mian, Rao & Sufi 2013; Case, Quigley & Shiller 2005) and equity
//   wealth (~2 cents) a year; the economy scales consumption plans by it.

#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
}

void update_assets(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const float gf = std::max(0.01f, n.get_gf());
        const float income = n.get_c0() > 0.0f ? n.get_consumption() / (n.get_c0() * gf) : 1.0f;
        const float real_mortgage = (n.get_lending_rate() - n.get_lending_rate0()) / 100.0f
                                    - (n.get_inflation_expect() - n.get_inflation_trend());
        const float uc0 = static_cast<float>(P.house_user_cost_base);
        const float user_cost = std::max(0.01f, uc0 + real_mortgage);
        const double path = static_cast<double>(n.get_core_price()) * gf;
        const float credit_gap = n.get_loans_hh0() > 0.0 ? static_cast<float>(n.get_loans_hh() / (n.get_loans_hh0() * path) - 1.0) : 0.0f;
        const float target = income * uc0 / user_cost * std::exp(static_cast<float>(P.house_credit) * credit_gap);
        float hp = n.get_house_price() > 0.0f ? n.get_house_price() : 1.0f;
        hp += (target - hp) * std::min(1.0f, years / static_cast<float>(P.house_price_years));
        n.set_house_price(hp);
        // Equity: profits over the real long rate plus a risk premium.
        const float r0 = std::max(0.0f, n.get_yield10_0() / 100.0f - n.get_inflation_trend());
        const float r1 = std::max(0.0f, n.get_yield10() / 100.0f - n.get_inflation_expect());
        const float erp = static_cast<float>(P.equity_risk_premium);
        const float profits = n.get_profit0() > 0.0f ? n.get_profit_smoothed() / (n.get_profit0() * static_cast<float>(path)) : 1.0f;
        float ep = n.get_equity_price() > 0.0f ? n.get_equity_price() : 1.0f;
        const float ep_target = std::max(0.05f, profits) * (r0 + erp) / (r1 + erp);
        ep += (ep_target - ep) * std::min(1.0f, years * 4.0f); // markets reprice within a quarter
        n.set_equity_price(ep);
        // Annual spending out of wealth changes, as a share of baseline consumption.
        const float gdp_c = n.get_c0() > 0.0f ? n.get_gdp0_daily() / n.get_c0() : 1.5f;
        n.set_wealth_effect(static_cast<float>(P.mpc_housing * P.housing_wealth_gdp) * (hp - 1.0f) * gdp_c
                            + static_cast<float>(P.mpc_equity * P.equity_wealth_gdp) * (ep - 1.0f) * gdp_c);
        n.set_housing_wealth(static_cast<float>(P.housing_wealth_gdp) * hp);
    }
}

} // namespace geosim::sim
