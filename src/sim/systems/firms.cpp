// Firms, bankruptcies, boards and trade policy (docs/03-economy.md#firms).
//
// Firm cohorts: one SME and one large-firm cohort per nation and market sector,
// each holding a share of the sector's capacity. Weekly (economy shard):
//  - Cash flow: revenue follows the sector's output; costs fall less than output
//    (fixed costs, hoarded labour); government support (firm support, job
//    retention pay) covers part of the gap. The cohort's cash buffer, in days of
//    costs, absorbs it (SMEs ~27 days; JPMorgan Chase Institute 2016).
//  - Exit: the failure rate rises convexly as buffers run down (financial
//    fragility; Delli Gatti et al. 2005; COVID SME failures: Gourinchas,
//    Kalemli-Ozcan, Penciakova & Sander 2020). Failed firms' capacity and jobs
//    disappear except the share survivors take over (Caballero & Hammour 1994);
//    their loans add to non-performing loans (credit.cpp).
//  - Entry: new firms enter when buffers (profits) are above normal and demand is
//    there, rebuilding capacity over time (Dosi, Fagiolo & Roventini 2010).
// Boards: the large cohort of each sector is run by a board agent choosing
//  - labour hoarding in a downturn: pragmatic when the shock looks temporary
//    (closures), ideological by varieties of capitalism (coordinated market
//    economies hoard; Hall & Soskice 2001; Hall & Gingerich 2009), and pulled
//    toward the executive's employment goal by state ownership (Shleifer &
//    Vishny 1994);
//  - an investment drive: pragmatic from expected utilisation and the cost of
//    capital, ideological (shareholder-value boards pay out rather than invest;
//    Lazonick & O'Sullivan 2000); state ownership tilts toward investment when
//    the executive is growth-first.
// Trade policy (executive): an average import tariff. Pragmatic: protect when
// manufacturing jobs are lost, hold back when inflation is high; ideological:
// nationalist governing parties protect (Colantone & Stanig 2018; Rodrik 2018).
// Tariffs pass fully into import prices (Amiti, Redding & Weinstein 2019;
// Fajgelbaum et al. 2020), cut import volumes, raise revenue and invite
// retaliation (geopolitics.cpp). Tourism follows travel, not tariffs, so a
// nation can welcome visitors and still shut out goods.

#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {

constexpr float margin0 = 0.1f; // baseline profit margin over costs paid out (steady state)

struct sector_state {
    float util = 1.0f;     // output / potential
    float payroll = 1.0f;  // paid labour (employment x hours) / baseline employment
    float cut = 0.0f;      // NPI capacity cut (closures)
};

sector_state sector_of(game_state const& state, dcon::nation_id n, uint8_t s) {
    auto const& w = state.world;
    sector_state out;
    const float pot = w.nation_get_va0(n, s) * std::max(0.01f, w.nation_get_gf(n));
    out.util = pot > 0.0f ? w.nation_get_output(n, s) / pot : 1.0f;
    double paid = 0.0, base = 0.0;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto p = loc.get_pop();
            if(p.get_kind() >= worker_kinds) continue;
            paid += p.get_employed(s) * p.get_hours(s);
            base += p.get_emp0(s);
        }
    }
    out.payroll = base > 0.0 ? static_cast<float>(paid / base) : 1.0f;
    out.cut = w.nation_get_npi_cut(n, s);
    return out;
}

// Scales a sector's capacity and jobs in a nation by `factor` (bankruptcies, entry).
void scale_sector(game_state& state, dcon::nation_id n, uint8_t s, float factor) {
    auto& w = state.world;
    for(auto own : w.nation_get_region_ownership(n)) {
        auto r = own.get_region();
        r.set_va0(s, r.get_va0(s) * factor);
        for(auto loc : r.get_pop_location()) {
            auto p = loc.get_pop();
            if(p.get_kind() >= worker_kinds) continue;
            p.set_emp0(s, p.get_emp0(s) * factor);
            if(factor < 1.0f) {
                // Jobs of failed firms go at once: their workers join the unemployed.
                const float lost = p.get_employed(s) * (1.0f - factor);
                p.set_employed(s, p.get_employed(s) - lost);
                p.set_need_slow(s, p.get_need_slow(s) * factor);
                p.set_unemployed(p.get_unemployed() + lost);
            }
        }
    }
    w.nation_set_va0(n, s, w.nation_get_va0(n, s) * factor);
}

// Board labour hoarding: pragmatic (temporary shocks are worth riding out) blended
// with the board's ideology (coordination, state ownership).
float board_hoarding_choice(float rho, float coord, float soe, float temporary) {
    const float pragmatic = std::clamp(0.2f + 0.6f * temporary, 0.0f, 1.0f);
    const float ideal = std::clamp(coord + 0.5f * soe, 0.0f, 1.0f);
    return std::clamp((1.0f - rho) * pragmatic + rho * ideal, 0.0f, 1.0f);
}

} // namespace

void init_firms(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    for(auto n : w.in_nation) {
        if(auto inc = n.get_incumbent()) {
            const auto se = static_cast<uint8_t>(value_axis::self_expression);
            n.set_nationalism0(std::max(0.0f, n.get_window_center(se) - w.party_get_position(inc, se)));
        }
        const float sme = std::clamp(0.5f + 0.3f * n.get_informality(), 0.3f, 0.85f);
        for(auto sec : w.in_sector) {
            if(sec.id == D.gov_sector) continue;
            for(uint8_t large = 0; large < 2; ++large) {
                const auto f = w.create_firm();
                w.firm_set_nation(f, n.id);
                w.firm_set_sector(f, sec.id);
                w.firm_set_large(f, large);
                w.firm_set_share(f, large ? 1.0f - sme : sme);
                const float cash = static_cast<float>(large ? P.large_cash_days : P.sme_cash_days);
                w.firm_set_cash_days(f, cash);
                w.firm_set_cash_days0(f, cash);
                w.firm_set_leverage(f, large ? 0.5f : 0.3f);
                w.firm_set_soe_share(f, large ? n.get_soe_share() : 0.0f);
                // Coordinated boards answer to stakeholders: more rigid, less short-termist.
                const float rho = 0.2f + 0.4f * n.get_coordination();
                const float h0 = board_hoarding_choice(rho, n.get_coordination(), large ? n.get_soe_share() : 0.0f, 0.0f);
                w.firm_set_hoarding(f, h0);
                if(large) {
                    const auto b = w.create_agent();
                    w.agent_set_kind(b, static_cast<uint8_t>(agent_kind::board));
                    w.agent_set_home(b, n.id);
                    w.agent_set_rho_base(b, rho);
                    w.agent_set_rho(b, rho);
                    w.firm_set_board(f, b);
                    n.set_board_hoarding(h0);
                    n.set_board_hoarding0(h0);
                }
            }
        }
    }
    // Customs and currency unions have one external tariff (decide_tariff harmonises it each
    // week). Members' 2019 tariff data differ only by their import mix, so the union's
    // GDP-weighted rate is everyone's starting tariff and baseline: otherwise the first
    // harmonisation is a trade shock that never happened (EU members in the world dataset).
    for(auto n : w.in_nation) {
        double sum = 0.0, weight = 0.0;
        std::vector<dcon::nation_id> members;
        if(const auto cu = customs_union_of(state, n.id)) {
            for(auto m : w.organisation_get_membership_as_organisation(cu)) members.push_back(m.get_member().id);
        } else if(auto bank = w.nation_get_bank_from_monetary_authority(n.id); bank && !peg_anchor(state, n.id)) {
            for(auto m : w.agent_get_monetary_authority(bank)) members.push_back(m.get_member().id);
        }
        if(members.size() < 2) continue;
        for(auto m : members) {
            sum += w.nation_get_gdp0(m) * w.nation_get_tariff0(m);
            weight += w.nation_get_gdp0(m);
        }
        if(weight <= 0.0) continue;
        const auto common = static_cast<float>(sum / weight);
        for(auto m : members) {
            w.nation_set_tariff(m, common);
            w.nation_set_tariff0(m, common);
        }
    }
}

void update_firms(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t lh = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::hoarding));
    const uint8_t li = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::investment));

    std::vector<sector_state> sectors(D.sector_count);
    for(auto n : w.in_nation) {
        // Same step as the economy (weekly, or 28 days for nations at low detail).
        if(n.get_econ_due() < 0.5f) continue;
        const float dt = n.get_econ_dt();
        for(uint8_t s = 0; s < D.sector_count; ++s) sectors[s] = sector_of(state, n.id, s);
        // Government support relative to the private wage bill (firm support + job retention pay).
        const double bill = std::max(1.0, n.get_wage_bill_private());
        const float support = static_cast<float>((n.get_firm_support_rate() + n.get_programmes() / dt) / bill);
        double hoard_w = 0.0, hoard_sum = 0.0, inv_sum = 0.0, exits_npl = 0.0, exit_sum = 0.0, share_sum = 0.0;
        const auto exec = n.get_executive_seat().get_agent();
        for(auto f : w.in_firm) {
            if(f.get_nation() != n.id) continue;
            const auto s = static_cast<uint8_t>(f.get_sector().id.index());
            auto const& st = sectors[s];
            const bool large = f.get_large() != 0;
            // Cash flow in days of baseline costs per day.
            const float costs = static_cast<float>(P.firm_fixed_cost) + static_cast<float>(1.0 - P.firm_fixed_cost) * st.payroll;
            const float flow = st.util / (1.0f - margin0) - costs - margin0 / (1.0f - margin0)
                               + static_cast<float>(P.firm_support_cover) * support;
            const float cash0 = f.get_cash_days0();
            float cash = f.get_cash_days() + flow * dt;
            // Survivors rebuild buffers over about a year (retained earnings, new equity).
            cash += (cash0 - cash) * dt / 365.0f;
            cash = std::clamp(cash, -cash0, 4.0f * cash0);
            f.set_cash_days(cash);
            // Exit and entry (annual rates), net capacity change for the cohort.
            const float base = static_cast<float>(large ? P.firm_exit_base_large : P.firm_exit_base_sme);
            const float distress = std::clamp(1.0f - cash / cash0, 0.0f, 2.0f);
            const float exit = base * (1.0f + static_cast<float>(P.firm_fragility) * distress * distress);
            // Few enter into a slump (demand well below potential).
            const float entry = base * std::max(0.0f, 1.0f + static_cast<float>(P.firm_entry_gain) * (cash / cash0 - 1.0f))
                                * std::clamp(st.util / 0.95f, 0.0f, 1.0f);
            f.set_exit_rate(exit);
            const float years = dt / 365.0f;
            const float net = ((entry - base) - (exit - base) * static_cast<float>(1.0 - P.firm_reallocation)) * years;
            const float factor = std::clamp(1.0f + net * f.get_share(), 0.9f, 1.1f);
            if(factor != 1.0f) scale_sector(state, n.id, s, factor);
            f.set_exits(f.get_exits() + exit * years);
            f.set_entries(f.get_entries() + entry * years);
            // Loans of failed firms above the normal rate go bad.
            exits_npl += static_cast<double>(std::max(0.0f, exit - base)) * years * f.get_leverage() * f.get_share() * w.nation_get_va0(n.id, s);
            exit_sum += static_cast<double>(exit) * f.get_share() * w.nation_get_va0(n.id, s);
            share_sum += static_cast<double>(f.get_share()) * w.nation_get_va0(n.id, s);

            // Board decisions (large firms).
            if(auto b = f.get_board()) {
                const float rho = w.agent_get_rho(b);
                const float coord = n.get_coordination();
                const float soe = f.get_soe_share();
                // Hoarding: temporary (closure-driven) shocks are worth riding out.
                const float shock = std::max(0.0f, 1.0f - st.util);
                const float temporary = shock > 0.0f ? std::clamp(st.cut / shock, 0.0f, 1.0f) : 0.0f;
                // Player overrides and other agents' pressure (strikes, deals) apply here;
                // board levers used to bypass them.
                const float h = apply_override(state, b, lh, board_hoarding_choice(rho, coord, soe, temporary), 0.0f, 1.0f);
                f.set_hoarding(std::clamp(h, 0.0f, 1.0f));
                // Investment drive (-0.5 .. 0.5 around the baseline plan).
                const float pragmatic_i = std::clamp(n.get_util_smoothed() - 1.0f
                                                         - 0.02f * (n.get_lending_rate() - n.get_lending_rate0()), -0.5f, 0.5f);
                // Ideology shapes the response, not the resting level: shareholder-value boards
                // cut harder in downturns; state-owned boards of a growth-first executive invest
                // against the cycle.
                const float bias = exec ? w.agent_get_credit_bias(exec) : 0.0f;
                const float swing = 1.0f + rho * ((0.5f - coord) - (pragmatic_i < 0.0f ? 2.0f * soe * bias : 0.0f));
                // The economy's accelerator already moves investment with utilisation; the board
                // adds only its ideological deviation from that response.
                const float inv = std::clamp(apply_override(state, b, li, std::clamp(pragmatic_i * (swing - 1.0f), -0.5f, 0.5f), -1.0f, 1.0f), -1.0f, 1.0f);
                f.set_investment_drive(inv);
                const double wgt = static_cast<double>(f.get_share()) * w.nation_get_va0(n.id, s);
                hoard_sum += wgt * h;
                inv_sum += wgt * inv;
                hoard_w += wgt;
            }
        }
        if(hoard_w > 0.0) {
            n.set_board_hoarding(static_cast<float>(hoard_sum / hoard_w));
            n.set_board_investment(static_cast<float>(inv_sum / hoard_w));
        }
        n.set_firm_exit_rate(share_sum > 0.0 ? static_cast<float>(exit_sum / share_sum) : 0.0f);
        // Bad loans from failures (share of the loan book; decays as they are worked out).
        const double loans = std::max(1.0, n.get_loans_firm());
        const float add = static_cast<float>(exits_npl * 365.0 * P.firm_lgd / loans);
        n.set_firm_exit_npl(n.get_firm_exit_npl() * std::exp(-dt / 365.0f) + add);
    }
}

// Executive trade policy: the average import tariff (weekly, with fiscal decisions).
void decide_tariff(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::tariff));
    const float t0 = w.nation_get_tariff0(n);
    float job_loss = 0.0f;
    if(D.manufacturing_sector) {
        const auto ms = static_cast<uint8_t>(D.manufacturing_sector.index());
        const auto st = sector_of(state, n, ms);
        job_loss = std::max(0.0f, 1.0f - st.payroll);
    }
    const float infl = std::max(0.0f, w.nation_get_inflation_expect(n) - w.nation_get_inflation_trend(n));
    // Countercyclical protection (Bown & Crowley 2013): job losses and a strong currency.
    const float appreciation = std::max(0.0f, std::log(std::max(0.05f, w.nation_get_exchange_rate(n))));
    const float pragmatic = std::max(0.0f, t0 + static_cast<float>(P.tariff_pain) * job_loss + static_cast<float>(P.tariff_appreciation) * appreciation
                                               - static_cast<float>(P.tariff_inflation_brake) * infl);
    // Nationalism: how far the governing party sits toward the traditional / survival end.
    float nationalist = 0.0f;
    if(auto inc = w.nation_get_incumbent(n)) {
        const auto se = static_cast<uint8_t>(value_axis::self_expression);
        nationalist = std::max(0.0f, w.nation_get_window_center(n, se) - w.party_get_position(inc, se));
    }
    // Relative to the start: 2019 tariffs already reflect 2019 politics.
    // Sector associations of distressed tradables buy protection (Grossman & Helpman 1994; policy.cpp).
    const float ideal = pragmatic + static_cast<float>(P.tariff_nationalism) * (nationalist - w.nation_get_nationalism0(n))
                        + static_cast<float>(P.lobby_tariff) * 100.0f * w.nation_get_lobby_pressure(n);
    const float rho = w.agent_get_rho_base(a);
    const float raw = (1.0f - rho) * pragmatic + rho * ideal;
    const float before = w.nation_get_tariff(n);
    // Tariffs are set by trade law: they move slowly (a tenth of the gap per week).
    float choice = before + (raw - before) * 0.1f;
    choice = apply_override(state, a, lev, choice, 0.0f, 1.0f); // tariff rate
    // Tariff changes are legislated (bills; institutions.cpp): nationalist moves shift policy
    // toward the traditional / survival end of the self-expression axis.
    choice = std::clamp(enact(state, n, lev, before, choice, value_axis::self_expression, -1.0f, false), 0.0f, 1.0f);
    w.agent_set_last_pragmatic(a, lev, pragmatic);
    w.agent_set_last_ideological(a, lev, ideal);
    w.agent_set_last_ai_choice(a, lev, raw);
    w.agent_set_last_choice(a, lev, choice);
    w.nation_set_tariff(n, choice);
    // A customs union (EU, Mercosur, EAEU, GCC: organisations flagged customs_union) has one
    // external tariff.
    if(const auto cu = customs_union_of(state, n)) {
        double sum = 0.0, weight = 0.0;
        for(auto m : w.organisation_get_membership_as_organisation(cu)) {
            sum += w.nation_get_gdp0(m.get_member()) * w.nation_get_tariff(m.get_member());
            weight += w.nation_get_gdp0(m.get_member());
        }
        if(weight > 0.0) {
            for(auto m : w.organisation_get_membership_as_organisation(cu)) w.nation_set_tariff(m.get_member(), static_cast<float>(sum / weight));
        }
    } else if(auto bank = w.nation_get_bank_from_monetary_authority(n)) { // currency unions without a customs union
        double sum = 0.0, weight = 0.0;
        for(auto m : w.agent_get_monetary_authority(bank)) {
            sum += w.nation_get_gdp0(m.get_member()) * w.nation_get_tariff(m.get_member());
            weight += w.nation_get_gdp0(m.get_member());
        }
        if(weight > 0.0 && !peg_anchor(state, n)) {
            for(auto m : w.agent_get_monetary_authority(bank)) w.nation_set_tariff(m.get_member(), static_cast<float>(sum / weight));
        }
    }
}

} // namespace geosim::sim
