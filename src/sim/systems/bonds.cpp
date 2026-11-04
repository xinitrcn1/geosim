// Government bonds and yields (docs/03-economy.md#bonds-and-yields), every 28 days.
//
// The 10-year yield = expected average short rate + term premium:
//  - expectations hypothesis: the policy rate is expected to revert toward a
//    neutral rate (the starting rate, moved with inflation expectations) at
//    `rate_persistence` a year, so the 10-year average weights today's rate by
//    (1 - e^-lT) / (lT) (Campbell & Shiller 1991 for the term structure);
//  - term premium = its starting value + the change in the sovereign spread
//    (debt, deficits, institutions; finance.cpp) + inflation uncertainty
//    - central-bank holdings (QE; Gagnon, Raskin, Remache & Sack 2011:
//    ~5bp per % of GDP purchased) + spillovers from the US term premium (global
//    financial cycle; Rey 2013, Obstfeld 2015) + compensation for expected
//    depreciation demanded by foreign holders (Arslanalp & Tsuda 2014 investor base).
// The government's effective interest rate moves toward the new-issue rate
// (bills at the policy rate, bonds at the 10-year yield) as debt matures and as
// new deficits are financed: average maturity matters (UK ~15 years, Brazil ~4).
// Central banks buy bonds (QE) as an AI lever (`lever::asset_purchases`): the
// pragmatic target rises when the Taylor rate is below the lower bound or when
// spreads blow out (fragmentation; ECB PEPP 2020). Purchases are capped at a
// share of debt; they swap bonds for reserves inside the financial sector, so
// they move no money in the ledger.

#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
}

float expected_short_rate(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    auto const& P = state.params;
    const float neutral = w.nation_get_policy_rate0(n) + 100.0f * (w.nation_get_inflation_expect(n) - w.nation_get_inflation_trend(n));
    const float lt = static_cast<float>(P.rate_persistence * P.bond_horizon_years);
    const float weight = lt > 0.0f ? (1.0f - std::exp(-lt)) / lt : 1.0f;
    return neutral + (w.nation_get_policy_rate(n) - neutral) * weight;
}

void update_bonds(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;

    // World output gap (GDP-weighted): flight to quality toward reserve currencies in downturns
    // (Beber, Brandt & Kavajecz 2009).
    double gap_num = 0.0, gap_den = 0.0;
    for(auto n : w.in_nation) {
        gap_num += n.get_gdp0() * n.get_gap_smoothed();
        gap_den += n.get_gdp0();
    }
    const float world_gap = gap_den > 0.0 ? static_cast<float>(gap_num / gap_den) : 0.0f;
    // US term premium change (spillover source): the reserve-currency issuer.
    float tp_us = 0.0f;
    for(auto n : w.in_nation) {
        if(n.get_reserve_currency() >= 1.0f) {
            tp_us = n.get_yield10() - expected_short_rate(state, n.id) - n.get_term_premium0();
        }
    }

    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const double gdp_nom = std::max(1.0, n.get_gdp0() * n.get_core_price() * n.get_gf());
        const double debt = std::max(0.0, -n.get_bal_gov());

        // Central-bank purchases accumulate (lever chosen in ai.cpp, % GDP per year).
        double cb = n.get_cb_bonds() + static_cast<double>(n.get_qe_rate()) / 100.0 * gdp_nom * years;
        cb = std::clamp(cb, 0.0, P.qe_cap * debt);
        n.set_cb_bonds(cb);

        const float infl_gap = std::abs(n.get_inflation_expect() - n.get_inflation_trend()) * 100.0f;
        const float dep = std::max(0.0f, -n.get_fx_change()); // expected depreciation (annual, log)
        const bool reserve = n.get_reserve_currency() >= 1.0f;
        const float res = n.get_reserve_currency();
        const float tp = n.get_term_premium0() + (n.get_spread() - n.get_spread0()) * (1.0f - static_cast<float>(P.tp_reserve_spread) * res)
                         - static_cast<float>(P.tp_haven) * res * std::max(0.0f, -world_gap) + static_cast<float>(P.tp_inflation) * infl_gap
                         - static_cast<float>(P.tp_qe) * static_cast<float>(cb / gdp_nom * 100.0)
                         + (reserve ? 0.0f : static_cast<float>(P.tp_global) * (1.0f - res) * tp_us)
                         + static_cast<float>(P.tp_fx) * n.get_foreign_debt_share() * dep * 100.0f;
        const float y = expected_short_rate(state, n.id) + tp;
        n.set_yield10(y);

        // Effective rate on the debt: refinancing (1 / maturity per year) and new borrowing.
        const float new_issue = (static_cast<float>(P.short_issue_share) * n.get_policy_rate()
                                 + static_cast<float>(1.0 - P.short_issue_share) * y) / 100.0f;
        const double prev = n.get_debt_prev() > 0.0 ? n.get_debt_prev() : debt;
        const float fresh = debt > 0.0 ? static_cast<float>(std::max(0.0, debt - prev) / debt) : 0.0f;
        const float roll = std::min(1.0f, years / std::max(0.5f, n.get_avg_maturity()) + fresh);
        float rate = n.get_gov_rate();
        rate += (std::max(-0.01f, new_issue) - rate) * roll;
        n.set_gov_rate(rate);
        n.set_debt_prev(debt);
    }
}

} // namespace geosim::sim
