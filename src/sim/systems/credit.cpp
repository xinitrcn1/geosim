// Bank credit (docs/03-economy.md#credit-and-banking), every 28 days per nation.
//
// The calibrated 2019 economy already contains the credit that financed it, so
// credit is modelled as a *deviation from its baseline path* (the loan stock
// growing with nominal trend output). Only departures from that path move
// spending and incomes, which keeps the calibrated steady state exact.
//
// Loans (households, firms) grow faster than their path when:
//  - the real lending rate falls (policy rate, long yields, NPL risk premium,
//    minus expected inflation): credit demand (Bernanke & Gertler 1995);
//  - the output gap is positive (procyclical demand), and slower when
//    unemployment rises (lending standards tighten; Lown & Morgan 2006);
//  - the executive directs state-owned banks to lend (Sapienza 2004; Dinc 2005;
//    Bircan & Saka 2021 for Turkey) - an AI lever, `lever::directed_credit`;
// and slower when bank capital falls short of requirements (credit crunch;
// Peek & Rosengren 1995), when non-performing loans are high (weak banks lend
// less; Jimenez, Ongena, Peydro & Saurina 2012) and when the stock is already
// above its path (deleveraging; Mian & Sufi 2014).
//
// Flows (loans create deposits; McLeay, Radia & Thomas 2014):
//  - credit impulse (Biggs, Mayer & Pick 2010): new lending above the path is
//    partly spent (household durables, firm investment; economy.cpp adds it to
//    demand and the ledger pays it finance -> firms), partly held as deposits,
//    and - in dollarised economies - partly converted to foreign assets (gold,
//    FX), a capital outflow that weakens the currency (finance.cpp);
//  - loan interest above / below the path is paid by borrowers to banks;
//  - bad loans: the NPL ratio follows unemployment, the output gap, depreciation
//    x FX-denominated loans (balance-sheet effect; Cespedes, Chang & Velasco
//    2004) and past booms (Schularick & Taylor 2012), with a recognition lag;
//    losses come out of bank capital, and the government pays the guaranteed
//    share of losses on directed loans.

#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
}

// Nominal lending rate (pct): policy and long yields, the lending spread and an NPL premium.
float lending_rate(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    auto const& P = state.params;
    const float iw = static_cast<float>(P.i_long_weight);
    const float base = (1.0f - iw) * w.nation_get_policy_rate(n) + iw * w.nation_get_yield10(n);
    return base + w.nation_get_lending_spread(n)
           + static_cast<float>(P.npl_spread) * 100.0f * (w.nation_get_npl(n) - w.nation_get_npl0(n));
}

void update_credit(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float dt = static_cast<float>(monthly_cadence.period_days);
    const float years = dt / 365.0f;

    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const double L0h = n.get_loans_hh0(), L0f = n.get_loans_firm0();
        if(L0h + L0f <= 0.0) continue;
        const double path = static_cast<double>(n.get_core_price()) * n.get_gf(); // nominal trend index
        const double path_h = L0h * path, path_f = L0f * path;
        double Lh = n.get_loans_hh(), Lf = n.get_loans_firm();
        const double gdp_nom = std::max(1.0, n.get_gdp0() * path);

        // Real lending rate change since the start (pp).
        const float i_l = lending_rate(state, n.id);
        n.set_lending_rate(i_l);
        // real_rate0 is the starting real policy rate; lending_rate0 - policy_rate0 its markup.
        const float real_change = (i_l - 100.0f * n.get_inflation_expect())
                                  - (n.get_real_rate0() + n.get_lending_rate0() - n.get_policy_rate0());
        const float u_rise = std::max(0.0f, n.get_unemployment() - n.get_unemployment0());
        const float credit_gap = static_cast<float>((Lh + Lf) / std::max(1.0, path_h + path_f) - 1.0);
        const float car = n.get_capital_ratio();
        // Common demand and supply terms (annual growth relative to path).
        float g = -static_cast<float>(P.credit_rate_semi) * real_change + static_cast<float>(P.credit_cycle) * n.get_gap_smoothed()
                  - static_cast<float>(P.credit_tightening) * u_rise - static_cast<float>(P.credit_deleveraging) * credit_gap
                  // Weak balance sheets restrain lending only as they deteriorate from the
                  // start: the calibrated 2019 lending already reflects starting NPLs and capital.
                  - static_cast<float>(P.credit_crunch) * std::max(0.0f, std::min(static_cast<float>(P.capital_ratio_min), n.get_capital_ratio0()) - car)
                  - static_cast<float>(P.credit_npl_overhang) * std::max(0.0f, n.get_npl() - std::max(static_cast<float>(P.npl_reference), n.get_npl0()));
        // Countercyclical capital buffer (macroprudential lever, reforms.cpp).
        g -= static_cast<float>(P.ccyb_credit) * n.get_macroprudential() / 100.0f * 10.0f;
        // Spread shocks (events) decay exponentially with time constant unwind_days (the latest event's).
        if(n.get_spread_shock() != 0.0f && n.get_spread_unwind_days() > 0.0f) {
            const float back = n.get_spread_shock() * std::min(1.0f, dt / n.get_spread_unwind_days());
            n.set_lending_spread(n.get_lending_spread() - back);
            n.set_spread_shock(n.get_spread_shock() - back);
        }
        // Directed lending by state banks (% GDP per year), split households / firms.
        const double directed = static_cast<double>(n.get_directed_credit()) / 100.0 * gdp_nom * years;
        const double dir_f = directed * P.directed_firm_share;
        const double dir_h = directed - dir_f;
        const double grow = std::exp(static_cast<double>(g) * years);
        // Firms draw on credit lines when revenues collapse ("dash for cash"); the
        // draws pay running costs, so they are not spent on new investment.
        const double liquidity = std::exp(P.credit_liquidity * std::max(0.0, -static_cast<double>(n.get_gap_smoothed())) * years) - 1.0;
        const double ratio = n.get_loans_prev() > 0.0 ? path / n.get_loans_prev() : 1.0; // path growth since last step
        n.set_loans_prev(path);
        const double new_h = Lh * ratio * grow + dir_h;
        const double draws = Lf * ratio * liquidity;
        const double new_f = Lf * ratio * grow + dir_f + draws;
        // Credit impulse: lending above what the path alone would have given (per day).
        const double imp_h = (new_h - Lh * ratio) / dt;
        const double imp_f = (new_f - Lf * ratio - draws) / dt;
        n.set_loans_directed(n.get_loans_directed() * ratio + directed);
        Lh = new_h;
        Lf = new_f;
        n.set_loans_hh(Lh);
        n.set_loans_firm(Lf);

        // Spending, deposits and capital flight of the impulse (economy.cpp spends it weekly).
        const float fx_leak = static_cast<float>(P.credit_fx_leak) * n.get_fx_deposit_share();
        const double spend_h = imp_h * P.credit_spend_hh * (1.0 - fx_leak);
        n.set_credit_flow_hh(static_cast<float>(spend_h));
        n.set_credit_deposit_hh(static_cast<float>(imp_h * (1.0 - P.credit_spend_hh) * (1.0 - fx_leak)));
        n.set_credit_flow_firm(static_cast<float>(imp_f * P.credit_spend_firm));
        if(draws > 0.0) {
            transfer(state, account::finance(n.id), account::firms(n.id), draws, flow::credit_deposits);
        }
        n.set_capital_flight(static_cast<float>(std::max(0.0, imp_h * P.credit_spend_hh * fx_leak) * 365.0 / gdp_nom));
        if(imp_h * fx_leak > 0.0) {
            // Converted to foreign assets: deposits leave for the rest of the world.
            transfer(state, account::finance(n.id), account::rest_of_world(n.id), imp_h * P.credit_spend_hh * fx_leak * dt, flow::credit_deposits);
        }

        // Interest above / below the path (per day); households pay through the pop pass.
        const float i0 = n.get_lending_rate0();
        const double int_h = (static_cast<double>(i_l) * Lh - static_cast<double>(i0) * path_h) / 100.0 / 365.0;
        const double int_f = (static_cast<double>(i_l) * Lf - static_cast<double>(i0) * path_f) / 100.0 / 365.0;
        const float size_total = n.get_size_total();
        n.set_rate_loan_interest(size_total > 0.0f ? static_cast<float>(int_h / size_total) : 0.0f);
        n.set_loan_interest_firm(static_cast<float>(int_f));

        // Non-performing loans and bank capital.
        const float log_e = std::log(std::max(0.05f, n.get_exchange_rate()));
        const float npl_target = n.get_npl0() + static_cast<float>(P.npl_unemployment) * u_rise
                                 + static_cast<float>(P.npl_gap) * std::max(0.0f, -n.get_gap_smoothed())
                                 + static_cast<float>(P.npl_fx) * n.get_fx_loan_share() * std::max(0.0f, -log_e)
                                 + static_cast<float>(P.npl_boom) * std::max(0.0f, credit_gap)
                                 + n.get_firm_exit_npl() * static_cast<float>(Lf / std::max(1.0, Lh + Lf)); // bankruptcies (firms.cpp)
        float npl = n.get_npl();
        // Forbearance and moratoria slow recognition while crisis support runs.
        const float support = std::clamp(n.get_stw_coverage() + n.get_directed_credit() / 5.0f, 0.0f, 1.0f);
        const float lag = static_cast<float>(P.npl_days) * (1.0f + static_cast<float>(P.npl_forbearance) * support);
        npl += (npl_target - npl) * std::min(1.0f, dt / lag);
        n.set_npl(std::clamp(npl, 0.0f, 0.6f));
        const double L = Lh + Lf;
        const double losses_dev = static_cast<double>(n.get_npl() - n.get_npl0()) * L * P.loss_given_default / P.workout_years * years;
        double K = n.get_bank_capital() * ratio;
        K += P.bank_margin_share * (int_h + int_f) * dt - losses_dev;
        // Guarantees: the state covers part of the losses on directed loans.
        if(losses_dev > 0.0 && L > 0.0) {
            const double calls = P.guarantee_share * losses_dev * n.get_loans_directed() / L;
            cause_scope c(state, lever_cause(state, n.id, lever::directed_credit));
            transfer(state, account::government(n.id), account::finance(n.id), calls, flow::credit_guarantee);
            K += calls;
        }
        n.set_bank_capital(K);
        n.set_capital_ratio(L > 0.0 ? static_cast<float>(K / (P.rwa_density * L)) : n.get_capital_ratio0());

        // Indicators: credit growth y/y-equivalent and impulse (% GDP per year).
        const double start = n.get_loans_start();
        n.set_credit_growth(start > 0.0 ? static_cast<float>(L / start - 1.0) : 0.0f);
        n.set_credit_impulse(static_cast<float>((imp_h + imp_f + draws / dt) * 365.0 / gdp_nom));
    }
}

} // namespace geosim::sim
