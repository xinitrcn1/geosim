// Taxes, compliance and public investment (docs/03-economy.md#fiscal).
//
// - Progressivity: each pop's effective household tax rate follows the
//   Heathcote-Storesletten-Violante form, rate ~ (income / mean)^tau, with tau
//   from data and the national level calibrated to 2019 revenue
//   (scenario.cpp). The executive's tax lever scales the level (a bill).
// - Behavioural response: taxable income falls with the net-of-tax rate
//   (elasticity ~0.25; Saez, Slemrod & Giertz 2012), so revenue rises less than
//   proportionally with the rate.
// - Compliance: the share of statutory taxes collected follows legitimacy,
//   social trust and state capacity (quasi-voluntary compliance; Levi 1988;
//   Slemrod 2007; Allingham & Sandmo 1972 for the evasion trade-off). It moves
//   slowly; the calibrated start is the reference.
// - Public investment (a bill): pragmatic in slack with fiscal space;
//   ideological by the governing party's economic position. It buys
//   construction now and builds public capital, which raises potential output
//   (elasticity ~0.1; Bom & Ligthart 2014; longrun.cpp).

#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
}

// Effective household tax multiplier (rate scale x compliance x behavioural base).
float household_tax_multiplier(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    auto const& P = state.params;
    const float t0 = w.nation_get_tax_hh(n);
    const float scale = w.nation_get_tax_scale(n);
    const float t = std::min(0.9f, t0 * scale);
    const float base = std::pow(std::max(0.01f, (1.0f - t) / std::max(0.01f, 1.0f - t0)), static_cast<float>(P.eti));
    const float comp = w.nation_get_compliance0(n) > 0.0f ? w.nation_get_compliance(n) / w.nation_get_compliance0(n) : 1.0f;
    return scale * base * comp;
}

float compliance_ratio(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    return w.nation_get_compliance0(n) > 0.0f ? w.nation_get_compliance(n) / w.nation_get_compliance0(n) : 1.0f;
}

float compliance_target(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    auto const& P = state.params;
    return std::clamp(0.3f + static_cast<float>(P.compliance_legitimacy) * w.nation_get_legitimacy(n)
                          + static_cast<float>(P.compliance_trust) * w.nation_get_social_trust(n) + 0.3f * w.nation_get_state_capacity(n)
                          - 0.2f * w.nation_get_informality(n),
                      0.2f, 1.0f);
}

void update_fiscal(game_state& state) {
    auto& w = state.world;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        float c = n.get_compliance();
        c += (compliance_target(state, n.id) - c) * std::min(1.0f, years / 3.0f);
        n.set_compliance(c);
        // Public capital: investment above baseline accumulates, depreciating 4% a year.
        const double gdp = std::max(1.0, n.get_gdp0() * n.get_gf());
        const float k = n.get_public_capital();
        n.set_public_capital(k + static_cast<float>(n.get_public_investment() / 100.0) * years - 0.04f * k * years);
        (void)gdp;
    }
}

void decide_fiscal_structure(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const float rho = w.agent_get_rho_base(a);
    float econ = 0.0f;
    if(auto inc = w.nation_get_incumbent(n)) econ = w.party_get_position(inc, static_cast<uint8_t>(value_axis::economic_right));
    const float econ_shift = econ - w.nation_get_gov_econ0(n); // relative to the 2019 government

    // Household tax scale: consolidate when spreads blow out; left governments tax more.
    {
        const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::tax_scale));
        const float pragmatic = 1.0f + static_cast<float>(P.consolidation_spread) * std::max(0.0f, w.nation_get_spread(n) - w.nation_get_spread0(n) - 1.0f);
        const float ideal = pragmatic * (1.0f - static_cast<float>(P.tax_ideology) * econ_shift);
        float proposed = (1.0f - rho) * pragmatic + rho * ideal;
        if(w.agent_get_override_mode(a, lev) != 0) proposed = w.agent_get_override_value(a, lev);
        const float before = w.nation_get_tax_scale(n);
        proposed = before + (proposed - before) * 0.1f; // tax reforms take time
        const float enacted = enact(state, n, lev, before, proposed, value_axis::economic_right, -0.5f, false);
        w.agent_set_last_pragmatic(a, lev, pragmatic);
        w.agent_set_last_ideological(a, lev, ideal);
        w.agent_set_last_choice(a, lev, enacted);
        w.nation_set_tax_scale(n, std::clamp(enacted, 0.3f, 2.0f));
    }
    // Public investment above baseline (% GDP per year).
    {
        const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::public_investment));
        const double gdp_nominal = w.nation_get_gdp0(n) * w.nation_get_core_price(n);
        const float debt = static_cast<float>(std::max(0.0, -w.nation_get_bal_gov(n)) / gdp_nominal);
        const float space = std::clamp(1.0f - debt / static_cast<float>(P.ai_debt_limit), 0.0f, 1.0f);
        const float slack = std::max(0.0f, -w.nation_get_gap_smoothed(n)) * 100.0f;
        const float pragmatic = static_cast<float>(P.public_investment_slack) * slack * space;
        const float ideal = pragmatic * (1.0f - std::clamp(econ_shift, -1.0f, 1.0f));
        float proposed = (1.0f - rho) * pragmatic + rho * ideal;
        if(w.agent_get_override_mode(a, lev) != 0) proposed = w.agent_get_override_value(a, lev);
        const float before = w.nation_get_public_investment(n);
        proposed = before + (proposed - before) * 0.2f;
        const float enacted = enact(state, n, lev, before, proposed, value_axis::economic_right, -0.03f, true);
        w.agent_set_last_choice(a, lev, enacted);
        w.nation_set_public_investment(n, std::clamp(enacted, -2.0f, 10.0f));
    }
}

} // namespace geosim::sim
