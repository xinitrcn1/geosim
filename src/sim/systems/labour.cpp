// Wages, unions and the minimum wage (docs/03-economy.md#labor-market), every 28 days.
//
// The calibrated wage structure is the 2019 baseline; the real wage index moves
// from there:
//  - wage curve: real wages fall with local unemployment (elasticity ~0.1;
//    Blanchflower & Oswald 1994), less where collective bargaining covers many
//    workers (bargained wages are rigid);
//  - inflation surprises: prices outrunning wages erode real pay automatically
//    (wages follow core prices, consumers pay the CPI); covered workers recover
//    part of the loss (indexation clauses);
//  - unions: a union-federation agent pushes for real-wage gains (pragmatic:
//    recover purchasing power, held back by unemployment; ideological:
//    militancy of dense, left-leaning movements), applied through bargaining
//    coverage;
//  - downward nominal rigidity: nominal wages rarely fall (Dickens et al. 2007).
// Effects (economy.cpp): wages paid scale with the index; labour demand falls
// with real labour costs (elasticity ~0.3; Lichter, Peichl & Siegloch 2015);
// wage growth feeds unit labour costs and inflation; real-wage losses where
// unions are strong feed strikes (society.cpp).
// The minimum wage (executive lever, a bill) lifts low-skill pay in proportion to
// its bite (minimum / median wage), with small job losses (Cengiz, Dube, Lindner
// & Zipperer 2019).

#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
}

void update_labour(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const float cov = n.get_bargaining_coverage();
        const float u = std::max(0.005f, n.get_unemployment());
        const float u0 = std::max(0.005f, n.get_unemployment0());
        // Purchasing-power loss of wage earners (CPI vs the core prices wages follow).
        const float erosion = std::max(0.0f, n.get_cpi() / std::max(0.01f, n.get_core_price()) - 1.0f);

        // Union federation: wage push (annual log change).
        float push = 0.0f;
        if(auto ua = n.get_union_agent()) {
            const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::wage_push));
            const float pragmatic = static_cast<float>(P.wage_indexation) * erosion - 0.5f * std::max(0.0f, u - u0);
            float left = 0.0f;
            if(auto inc = n.get_incumbent()) left = std::max(0.0f, -w.party_get_position(inc, static_cast<uint8_t>(value_axis::economic_right)));
            const float ideal = pragmatic + static_cast<float>(P.union_militancy) * n.get_union_density() * 10.0f * left;
            const float rho = w.agent_get_rho_base(ua);
            push = (1.0f - rho) * pragmatic + rho * ideal;
            if(w.agent_get_override_mode(ua, lev) != 0) push = w.agent_get_override_value(ua, lev);
            w.agent_set_last_pragmatic(ua, lev, pragmatic);
            w.agent_set_last_ideological(ua, lev, ideal);
            w.agent_set_last_choice(ua, lev, push);
        }
        n.set_union_push(push);

        // Real wage index: wage curve (dampened by coverage) and bargained push.
        const float curve = -static_cast<float>(P.wage_curve) * (1.0f - static_cast<float>(P.wage_rigidity) * cov) * std::log(u / u0);
        // Nash bargaining (Pissarides 2000): the wage moves with labour productivity in
        // proportion to workers' bargaining power and with the outside option (benefits)
        // in proportion to the rest.
        const float power = static_cast<float>(P.nash_power_base + P.nash_power_coverage * cov);
        const float employment = std::max(0.3f, (1.0f - u) / std::max(0.05f, 1.0f - u0));
        // Persistent productivity (smoothed output gap per worker), not the composition
        // jump when low-wage jobs go first; the outside option is the permanent benefit
        // system (benefit_base changes with reforms, not with crisis top-ups).
        const float productivity = (1.0f + n.get_gap_smoothed()) / std::max(0.3f, employment);
        const float outside = 1.0f; // benefit_base is the calibrated regime; reforms would move it
        const float nash = 1.0f + power * (std::clamp(productivity, 0.8f, 1.2f) - 1.0f)
                           + (1.0f - power) * n.get_benefit_replacement() * (outside - 1.0f) * 0.3f;
        const float target = std::exp(curve) * (1.0f + cov * push) * std::clamp(nash, 0.7f, 1.3f);
        float wr = n.get_wage_real();
        float next = wr + (target - wr) * std::min(1.0f, years / static_cast<float>(P.wage_adjust_years));
        // Downward nominal rigidity: nominal wages (real index x core prices) do not fall.
        const float infl = n.get_inflation_expect();
        const float floor = wr * std::exp(-std::max(0.0f, infl) * years);
        next = std::max(next, floor);
        n.set_wage_nominal_growth((next / std::max(1e-6f, wr) - 1.0f) / years);
        n.set_wage_real(next);

        // Minimum wage: low-skill pay rises with the bite.
        const float mw = n.get_min_wage0() > 0.0f ? n.get_min_wage() / n.get_min_wage0() : 1.0f;
        const float low_mult = 1.0f + static_cast<float>(P.min_wage_low_share) * n.get_min_wage_bite() * std::log(std::max(0.1f, mw));
        for(auto own : n.get_region_ownership()) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                if(p.get_kind() >= worker_kinds) continue;
                p.set_wage_mult(next * (p.get_kind() == static_cast<uint8_t>(pop_kind::low_skill) ? low_mult : 1.0f));
            }
        }
    }
}

// Minimum wage relative to the start (a bill). Pragmatic: keep its real value
// (index to the CPI) when inflation erodes it; ideological: left governments raise it.
void decide_min_wage(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& D = state.derived;
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::min_wage));
    if(w.nation_get_min_wage_bite(n) <= 0.0f) return; // no statutory minimum
    const float pragmatic = w.nation_get_min_wage0(n) * std::max(1.0f, w.nation_get_cpi(n) / std::max(0.01f, w.nation_get_core_price(n)));
    float left = 0.0f;
    if(auto inc = w.nation_get_incumbent(n)) left = -w.party_get_position(inc, static_cast<uint8_t>(value_axis::economic_right));
    // Relative to the 2019 government: a shift to the left raises the minimum wage.
    const float ideal = pragmatic * (1.0f + 0.1f * std::max(0.0f, left + w.nation_get_gov_econ0(n)));
    const float rho = w.agent_get_rho_base(a);
    const float before = w.nation_get_min_wage(n);
    float proposed = (1.0f - rho) * pragmatic + rho * ideal;
    if(w.agent_get_override_mode(a, lev) != 0) proposed = w.agent_get_override_value(a, lev);
    // Minimum wages are reviewed yearly: move a twelfth of the gap per 28 days at most.
    proposed = before + (proposed - before) * 0.3f;
    const float enacted = enact(state, n, lev, before, proposed, value_axis::economic_right, -0.2f, false);
    w.agent_set_last_pragmatic(a, lev, pragmatic);
    w.agent_set_last_ideological(a, lev, ideal);
    w.agent_set_last_choice(a, lev, enacted);
    w.nation_set_min_wage(n, std::max(0.1f, enacted));
}

} // namespace geosim::sim
