// Money and finance (docs/03-economy.md#money-banking-and-finance), every 28 days.
//
// - Real exchange rates (1 = start). Floating currencies appreciate with a
//   current-account improvement and a positive interest differential vs the
//   US (uncovered interest parity), and depreciate with rising sovereign risk
//   (capital flight). Pegs follow their anchor; currency unions share one rate.
//   The economy uses the rate for trade volumes and import-price inflation.
//   Two capital-account channels add depreciation pressure: currency substitution
//   in dollarised economies when real rates fall (households move deposits into
//   foreign currency; Ize & Levy Yeyati 2003), and credit-financed purchases of
//   foreign assets (gold, FX; credit.cpp). Commodity terms of trade enter the
//   current account (commodities.cpp).
// - Sovereign spreads from debt, deficits, state capacity, legitimacy and
//   reserve-currency status (Reinhart & Rogoff 2009); they feed the term premium
//   and so the yields at which debt rolls over (bonds.cpp).
// - Default when the spread explodes: a haircut posted through the ledger
//   (creditors in the finance sector lose), a legitimacy hit, a risk premium.
// - Remittances: the payer-weighted income index of migrant workers' host
//   economies, used by the economy's remittance rates.

#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

float worker_income_index(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    double inc = 0.0;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            if(loc.get_pop().get_kind() < worker_kinds) inc += loc.get_pop().get_inc_work(); // migrants remit from earnings
        }
    }
    const float base = w.nation_get_worker_income0(n);
    const float cpi = std::max(0.01f, w.nation_get_cpi(n));
    return base > 0.0f ? static_cast<float>(inc / base) / cpi : 1.0f;
}

float current_account_shift(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const float y = std::max(1.0f, w.nation_get_gdp_real(n));
    const float p = std::max(0.01f, w.nation_get_core_price(n));
    // Commodity terms of trade (nominal, daily) in real terms.
    return (w.nation_get_exports(n) - w.nation_get_imports(n) + w.nation_get_commodity_tot(n) / p) / y - w.nation_get_ca0(n);
}

} // namespace

float compute_spread(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    auto const& P = state.params;
    const double gdp = std::max(1.0, w.nation_get_gdp0(n) * w.nation_get_core_price(n));
    const float debt = static_cast<float>(std::max(0.0, -w.nation_get_bal_gov(n)) / gdp);
    const float deficit = std::max(0.0f, -w.nation_get_gap_smoothed(n)) * 0.5f; // cyclical deficit proxy
    const float s = static_cast<float>(P.spread_debt) * std::max(0.0f, debt - 0.6f) + static_cast<float>(P.spread_deficit) * deficit
                    + static_cast<float>(P.spread_capacity) * (1.0f - w.nation_get_state_capacity(n))
                    + static_cast<float>(P.spread_legitimacy) * (1.0f - w.nation_get_legitimacy(n))
                    - static_cast<float>(P.spread_reserve) * w.nation_get_reserve_currency(n) + (w.nation_get_defaulted(n) ? 3.0f : 0.0f);
    return std::max(0.0f, s);
}

void update_finance(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    dcon::nation_id usa;
    for(auto n : w.in_nation) {
        if(n.get_reserve_currency() >= 1.0f) usa = n.id;
    }
    const float r_us = usa ? w.nation_get_policy_rate(usa) - w.nation_get_policy_rate0(usa) : 0.0f;

    // Exchange rates: one decision per currency area (own currency or union), pegs follow.
    for(auto bank : w.in_agent) {
        if(bank.get_kind() != static_cast<uint8_t>(agent_kind::central_bank)) continue;
        auto members = bank.get_monetary_authority();
        if(members.begin() == members.end()) continue;
        const auto lead = (*members.begin()).get_member().id;
        if(!is_due(static_cast<uint32_t>(lead.index()), tick, monthly_cadence)) continue;
        if(peg_anchor(state, lead)) continue; // pegs follow their anchor below
        double ca = 0.0, weight = 0.0, spread_change = 0.0, substitution = 0.0, flight = 0.0;
        for(auto m : members) {
            const auto n = m.get_member().id;
            const double g = w.nation_get_gdp0(n);
            ca += g * current_account_shift(state, n);
            spread_change += g * (w.nation_get_spread(n) - w.nation_get_spread0(n));
            // Currency substitution: deposit dollarisation x fall of the real policy rate (pp / 100).
            const float real_fall = w.nation_get_real_rate0(n) - (w.nation_get_policy_rate(n) - 100.0f * w.nation_get_inflation_expect(n));
            substitution += g * w.nation_get_fx_deposit_share(n) * std::max(0.0f, real_fall) / 100.0f;
            flight += g * w.nation_get_capital_flight(n);
            weight += g;
        }
        ca /= weight;
        spread_change /= weight;
        substitution /= weight;
        flight /= weight;
        const float r_diff = (w.nation_get_policy_rate(lead) - w.nation_get_policy_rate0(lead)) - r_us;
        const float dlog = lead == usa ? 0.0f
                                       : years * (static_cast<float>(P.fx_current_account) * static_cast<float>(ca)
                                                  + static_cast<float>(P.fx_interest) * r_diff / 100.0f
                                                  - static_cast<float>(P.fx_risk) * static_cast<float>(spread_change) / 100.0f
                                                  - static_cast<float>(P.fx_dollarisation) * static_cast<float>(substitution)
                                                  - static_cast<float>(P.fx_flight) * static_cast<float>(flight));
        for(auto m : members) {
            const auto n = m.get_member().id;
            w.nation_set_exchange_rate(n, std::clamp(w.nation_get_exchange_rate(n) * std::exp(dlog), 0.2f, 5.0f));
            w.nation_set_fx_change(n, dlog / years);
        }
    }
    for(auto nat : w.in_nation) {
        const auto n = nat.id;
        const auto anchor = peg_anchor(state, n);
        if(!anchor) continue;
        w.nation_set_fx_change(n, w.nation_get_fx_change(anchor));
        w.nation_set_exchange_rate(n, w.nation_get_exchange_rate(anchor));
    }

    // Spreads and default (the effective debt cost follows yields in bonds.cpp).
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const float s = compute_spread(state, n.id);
        n.set_spread(s);
        if(!n.get_defaulted() && s - n.get_spread0() > static_cast<float>(P.default_spread)) {
            const double debt = std::max(0.0, -n.get_bal_gov());
            transfer(state, account::finance(n.id), account::government(n.id), debt * P.default_haircut, flow::sovereign_default);
            n.set_defaulted(1);
            n.set_legitimacy(std::max(0.0f, n.get_legitimacy() - 0.2f));
            const auto d = w.create_decision();
            w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
            w.decision_set_target(d, n.id);
            w.decision_set_tick(d, state.current_tick);
            w.decision_set_lever(d, journal_default);
            w.decision_set_before(d, static_cast<float>(debt));
            w.decision_set_after(d, static_cast<float>(debt * (1.0 - P.default_haircut)));
            w.decision_set_pragmatic(d, NAN);
            w.decision_set_ideological(d, NAN);
            w.decision_set_prior(d, NAN);
        }
    }

    // Remittance index: host-economy worker incomes, weighted by what each nation pays out.
    if(static_cast<uint32_t>(state.current_tick) % monthly_cadence.period_days == 0) {
        double num = 0.0, den = 0.0;
        for(auto n : w.in_nation) {
            num += n.get_remit_out0() * worker_income_index(state, n.id);
            den += n.get_remit_out0();
        }
        for(auto c : w.in_climate) {
            c.set_remittance_index(den > 0.0 ? static_cast<float>(num / den) : 1.0f);
        }
    }
}

float payer_income_index(game_state const& state, dcon::nation_id n) {
    return worker_income_index(state, n);
}

} // namespace geosim::sim
