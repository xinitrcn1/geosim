// Policy catalogue, cabinets, governors and lobbies (docs/05-politics-institutions.md).
//
// Executive levers (weekly, ai.cpp calls decide_policies):
//  - repression (0..1): pragmatic when protest rises, ideological for order-minded
//    rulers; capped by the military's loyalty. It deters joining at once but leaves
//    a memory that feeds later backlash and erodes legitimacy (Davenport 2007;
//    Francisco 1995; Chenoweth & Stephan 2011 for its backfire on mass movements).
//  - immigration openness (a bill): pragmatic with labour shortages, ideological
//    by the governing party's traditional / nationalist position.
//  - retirement age (a bill): pragmatic when debt rises above its 2019 level
//    (pension reforms follow fiscal pressure), resisted by the left; later
//    retirement lowers pension spending (economy.cpp).
//  - education and health spending above baseline (bills): education raises the
//    tertiary share with a lag (human capital), health adds hospital capacity.
//  - capital controls (0..1): pragmatic in a sudden stop (Rey 2013: the trilemma
//    with free capital flows); they cut portfolio flows both ways.
// Cabinets: in coalitions portfolios go to parties roughly by seats (Gamson's law;
// Laver & Shepsle 1996); the finance, health and interior ministers' parties set the
// ideology of their levers (fiscal hawkishness, health weight, order).
// Governors (multi-region nations): regional NPI stringency relative to the
// national level - opposition governors deviate more (partisan federalism).
// Lobbies: associations of tradable sectors losing ground to imports press for
// protection (Grossman & Helpman 1994; Trefler 1993).

#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {

float unrest_of(game_state const& state, dcon::nation_id n) {
    float u = 0.0f, pop = 0.0f;
    for(auto own : state.world.nation_get_region_ownership(n)) {
        u += own.get_region().get_unrest() * own.get_region().get_population();
        pop += own.get_region().get_population();
    }
    return pop > 0.0f ? u / pop : 0.0f;
}

float lever_choice(game_state& state, dcon::agent_id a, uint8_t lev, float pragmatic, float ideal, float rho) {
    auto& w = state.world;
    float x = (1.0f - rho) * pragmatic + rho * ideal;
    if(w.agent_get_override_mode(a, lev) != 0) x = w.agent_get_override_value(a, lev);
    w.agent_set_last_pragmatic(a, lev, pragmatic);
    w.agent_set_last_ideological(a, lev, ideal);
    return x;
}

} // namespace

// Coalition portfolios by Gamson's law: the largest party takes the head of
// government and the first pick; others pick in seat order.
void allocate_portfolios(game_state& state, dcon::nation_id n) {
    auto& w = state.world;
    std::vector<dcon::party_id> gov;
    for(auto m : w.nation_get_party_membership(n)) {
        if(m.get_party().get_in_government()) gov.push_back(m.get_party().id);
    }
    std::sort(gov.begin(), gov.end(), [&](auto a, auto b) { return w.party_get_seat_share(a) > w.party_get_seat_share(b); });
    if(gov.empty()) {
        if(auto inc = w.nation_get_incumbent(n)) gov.push_back(inc);
    }
    if(gov.empty()) return;
    double total = 0.0;
    for(auto p : gov) total += w.party_get_seat_share(p);
    // Portfolio counts proportional to seats (largest remainders), assigned in a fixed order.
    const auto P = static_cast<uint8_t>(portfolio::count);
    std::vector<int> count(gov.size(), 0);
    int given = 0;
    for(std::size_t i = 0; i < gov.size(); ++i) {
        count[i] = total > 0.0 ? static_cast<int>(std::floor(P * w.party_get_seat_share(gov[i]) / total)) : (i == 0 ? P : 0);
        given += count[i];
    }
    for(std::size_t i = 0; given < P; i = (i + 1) % gov.size()) {
        ++count[i];
        ++given;
    }
    uint8_t slot = 0;
    for(std::size_t i = 0; i < gov.size(); ++i) {
        for(int k = 0; k < count[i] && slot < P; ++k) w.nation_set_portfolio_party(n, slot++, gov[i]);
    }
}

// Ideology of a portfolio's lever: its minister's party position on an axis (or the
// incumbent's when the portfolio is unallocated).
float minister_position(game_state const& state, dcon::nation_id n, portfolio pf, value_axis axis) {
    auto const& w = state.world;
    auto p = w.nation_get_portfolio_party(n, static_cast<uint8_t>(pf));
    if(!p) p = w.nation_get_incumbent(n);
    return p ? w.party_get_position(p, static_cast<uint8_t>(axis)) : 0.0f;
}

void decide_policies(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const float rho = w.agent_get_rho_base(a);
    const auto lev = [&](lever l) { return static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(l)); };
    const auto se = static_cast<uint8_t>(value_axis::self_expression);
    // The interior minister's party sets the order ideology of security levers,
    // relative to the 2019 government (status quo calibrated).
    const float tan = std::max(0.0f, -minister_position(state, n, portfolio::interior, value_axis::self_expression) - w.nation_get_gov_tan0(n));

    // Repression: pragmatic in rising protest; capped by the military's loyalty.
    {
        const float unrest = unrest_of(state, n);
        const float liberal_cap = 1.0f - w.nation_get_liberal(n);
        const float pragmatic = std::clamp((unrest - 2.0f) / 2.0f, 0.0f, 1.0f) * liberal_cap;
        const float ideal = std::clamp(pragmatic * (1.0f + 2.0f * w.agent_get_order_ideal(a)) + tan, 0.0f, 1.0f) * liberal_cap;
        float x = lever_choice(state, a, lev(lever::repression), pragmatic, ideal, w.agent_get_rho(a));
        x = std::min(x, w.nation_get_military_loyalty(n) + 0.2f); // security forces must obey
        w.nation_set_repression(n, std::clamp(x, 0.0f, 1.0f));
    }
    // Immigration openness (relative to the start, 1 = status quo).
    {
        const float shortage = std::max(0.0f, w.nation_get_unemployment0(n) - w.nation_get_unemployment(n));
        const float pragmatic = 1.0f + static_cast<float>(P.immigration_shortage) * shortage * 10.0f
                                - 2.0f * std::max(0.0f, w.nation_get_unemployment(n) - w.nation_get_unemployment0(n));
        const float ideal = pragmatic - 0.5f * tan;
        const float before = w.nation_get_immigration_openness(n);
        float x = before + (lever_choice(state, a, lev(lever::immigration), pragmatic, ideal, rho) - before) * 0.1f;
        x = enact(state, n, lev(lever::immigration), before, x, value_axis::self_expression, 0.1f, false);
        w.nation_set_immigration_openness(n, std::clamp(x, 0.0f, 2.0f));
    }
    // Retirement age: fiscal pressure vs the left.
    {
        const double gdp = w.nation_get_gdp0(n) * w.nation_get_core_price(n);
        const float debt = static_cast<float>(std::max(0.0, -w.nation_get_bal_gov(n)) / gdp);
        const float debt0 = static_cast<float>(w.nation_get_gov_debt0(n) / w.nation_get_gdp0(n));
        const float pragmatic = static_cast<float>(P.retirement_debt) * std::max(0.0f, debt - debt0 - 0.1f) * 10.0f;
        const float econ = minister_position(state, n, portfolio::finance, value_axis::economic_right);
        const float ideal = pragmatic * std::max(0.0f, 1.0f + econ);
        const float before = w.nation_get_retirement_age_shift(n);
        float x = before + (lever_choice(state, a, lev(lever::retirement_age), pragmatic, ideal, rho) - before) * 0.05f;
        x = enact(state, n, lev(lever::retirement_age), before, x, value_axis::economic_right, 0.05f, false);
        w.nation_set_retirement_age_shift(n, std::clamp(x, -3.0f, 7.0f));
    }
    // Education and health spending (% GDP above baseline): a finance minister to
    // the left of the 2019 government spends more on public goods.
    {
        const float econ = minister_position(state, n, portfolio::finance, value_axis::economic_right) - w.nation_get_gov_econ0(n);
        const float edu_p = 0.0f, edu_i = std::clamp(-0.5f * econ, -1.0f, 1.0f);
        const float before_e = w.nation_get_education_spending(n);
        float xe = before_e + (lever_choice(state, a, lev(lever::education), edu_p, edu_i, rho) - before_e) * 0.05f;
        xe = enact(state, n, lev(lever::education), before_e, xe, value_axis::economic_right, -0.05f, false);
        w.nation_set_education_spending(n, std::clamp(xe, -2.0f, 3.0f));
        // Health: an epidemic threat makes it pragmatic.
        const float hp = static_cast<float>(P.health_threat) * w.nation_get_threat(n);
        const float hi = hp + std::clamp(-0.5f * econ, -1.0f, 1.0f);
        const float before_h = w.nation_get_health_spending(n);
        float xh = before_h + (lever_choice(state, a, lev(lever::health), hp, hi, rho) - before_h) * 0.1f;
        xh = enact(state, n, lev(lever::health), before_h, xh, value_axis::economic_right, -0.05f, true);
        w.nation_set_health_spending(n, std::clamp(xh, -2.0f, 5.0f));
    }
    // Capital controls in a sudden stop.
    {
        const float outflow = std::max(0.0f, -w.nation_get_portfolio_flow(n));
        const float pragmatic = std::clamp((outflow - static_cast<float>(P.capital_controls_trigger)) / 10.0f, 0.0f, 1.0f)
                                * (w.nation_get_reserve_currency(n) >= 1.0f ? 0.0f : 1.0f);
        const float ideal = std::clamp(pragmatic + 0.3f * tan, 0.0f, 1.0f);
        const float x = lever_choice(state, a, lev(lever::capital_controls), pragmatic, ideal, w.agent_get_rho(a));
        w.nation_set_capital_controls(n, std::clamp(x, 0.0f, 1.0f));
    }
}

// Monthly: lobbies, governors, public-service effects, repression memory.
void update_policy(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    constexpr cadence monthly_cadence{28, 28};
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        // Repression memory (backlash) and legitimacy cost.
        float mem = n.get_repression_memory();
        mem += n.get_repression() * years * 365.0f / static_cast<float>(P.repression_memory_days)
               - mem * std::min(1.0f, years * 365.0f / static_cast<float>(P.repression_memory_days));
        n.set_repression_memory(mem);
        n.set_legitimacy(std::clamp(n.get_legitimacy() - static_cast<float>(P.repression_legitimacy) * n.get_repression() * years, 0.0f, 1.0f));
        // Lobbies of tradable sectors doing worse than the economy as a whole (import
        // competition, not a general slump: Trefler 1993 finds protection follows
        // import penetration), smoothed over about a year as associations mobilise.
        float util_all = 0.0f, pot_all = 0.0f;
        for(auto sec : w.in_sector) {
            const auto s = static_cast<uint8_t>(sec.id.index());
            util_all += n.get_output(s);
            pot_all += n.get_va0(s) * std::max(0.01f, n.get_gf());
        }
        util_all = pot_all > 0.0f ? util_all / pot_all : 1.0f;
        float pressure = 0.0f;
        for(auto sec : w.in_sector) {
            if(sec.get_shipped() <= 0.0f) continue;
            const auto s = static_cast<uint8_t>(sec.id.index());
            const float pot = n.get_va0(s) * std::max(0.01f, n.get_gf());
            const float util = pot > 0.0f ? n.get_output(s) / pot : 1.0f;
            const float share = n.get_gdp0_daily() > 0.0f ? n.get_va0(s) / n.get_gdp0_daily() : 0.0f;
            pressure += share * std::max(0.0f, util_all - util) * (1.0f + n.get_soe_share());
        }
        pressure = n.get_lobby_pressure() + (pressure - n.get_lobby_pressure()) * std::min(1.0f, years);
        n.set_lobby_pressure(pressure);
        // Education builds skills (lagged); health spending adds hospital beds.
        n.set_tertiary_share(std::clamp(n.get_tertiary_share() + static_cast<float>(P.education_tertiary) * n.get_education_spending() / 100.0f * years * 10.0f, 0.02f, 0.85f));
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            r.set_beds_per_1000(std::max(0.5f, r.get_beds_per_1000() + static_cast<float>(P.health_beds) * n.get_health_spending() * years));
            // Governors: regional NPIs relative to the national level.
            if(auto g = r.get_governor()) {
                const auto inc = n.get_incumbent();
                const float distance = inc ? std::abs(w.agent_get_order_ideal(g) - w.agent_get_order_ideal(n.get_executive_seat().get_agent())) : 0.0f;
                const float local = r.get_new_infections() / std::max(1.0f, r.get_population()) * 1e6f;
                const float pragmatic = std::clamp(1.0f + 0.002f * (local - 50.0f), 0.5f, 1.5f);
                const float offset = pragmatic * (1.0f + static_cast<float>(P.governor_alignment) * distance * (w.agent_get_order_ideal(g) > 0.2f ? 1.0f : -1.0f));
                r.set_npi_offset(std::clamp(offset, 0.3f, 1.7f));
                w.agent_set_last_choice(g, static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::regional_npi)), r.get_npi_offset());
            }
        }
    }
}

} // namespace geosim::sim
