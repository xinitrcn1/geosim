// Requests between agents (docs/06-geopolitics-ai.md#requests). Once a month (sharded by
// agent) an AI agent whose situation calls for it asks another agent for something:
//  - a central bank worried by inflation or debt asks its government to restrain
//    spending (central banks lean on fiscal policy; Sargent & Wallace 1981);
//  - a government facing rising unemployment asks its central bank to cut rates
//    (Binder 2021 on political pressure on central banks);
//  - a union whose members' real wages are eroding asks for a higher minimum wage;
//  - the board of a firm running out of cash asks for a rescue, or offers its union
//    jobs for wage restraint (concession bargaining);
//  - a disloyal military asks for a pay rise (coup-proofing by pay; Powell 2012);
//  - the governor of a restless region asks for a regional transfer;
//  - a central bank short of reserves under depreciation asks a foreign central bank
//    for a swap line (the 2008 and 2020 dollar lines);
//  - rebels at a stalemate ask for power sharing (Zartman 1989).
// The asked agent answers by its own rule at once. When a player controls it and lets
// it wait for their answer (agent.answer_requests), the request stays open for
// request_window_days; then the agent's AI answers for them. Answers are journalled
// (journal_request) and the request deleted.

#include "core/keys.hpp"
#include "core/scheduler.hpp"
#include "sim/levers.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

uint8_t lever_slot(game_state const& state, lever l) { return static_cast<uint8_t>(state.derived.lever_base + static_cast<uint8_t>(l)); }

agent_kind kind_of(game_state const& state, dcon::agent_id a) { return static_cast<agent_kind>(state.world.agent_get_kind(a)); }

float debt_ratio(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const double gdp = w.nation_get_gdp0(n) * w.nation_get_core_price(n);
    return gdp > 0.0 ? static_cast<float>(std::max(0.0, -w.nation_get_bal_gov(n)) / gdp) : 0.0f;
}

float debt_rise(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const float debt0 = w.nation_get_gdp0(n) > 0.0 ? static_cast<float>(w.nation_get_gov_debt0(n) / w.nation_get_gdp0(n)) : 0.0f;
    return debt_ratio(state, n) - debt0;
}

bool fiscal_space(game_state const& state, dcon::nation_id n) {
    return debt_ratio(state, n) < 0.8f * static_cast<float>(state.params.ai_debt_limit) && !state.world.nation_get_defaulted(n);
}

float stalemate(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const auto rebel = w.nation_get_rebel_agent(n);
    if(!rebel) return 0.0f;
    return std::max(0.0f, 1.0f - std::abs(w.agent_get_strength(rebel) - w.nation_get_state_capacity(n)) * 2.0f);
}

dcon::firm_id firm_of(game_state const& state, dcon::agent_id board) {
    for(auto f : state.world.in_firm) {
        if(f.get_board() == board) return f.id;
    }
    return {};
}

dcon::region_id region_of(game_state const& state, dcon::agent_id governor) {
    for(auto r : state.world.in_region) {
        if(r.get_governor() == governor) return r.id;
    }
    return {};
}

bool lends_reserves(game_state const& state, dcon::nation_id n) {
    return state.world.nation_get_reserve_currency(n) >= 0.3f || state.world.nation_get_reserves(n) >= 8.0f;
}

char const* kind_noun(agent_kind k) {
    switch(k) {
    case agent_kind::executive: return "government";
    case agent_kind::central_bank: return "central bank";
    case agent_kind::board: return "board";
    case agent_kind::union_federation: return "union federation";
    case agent_kind::military: return "military";
    case agent_kind::governor: return "governor";
    case agent_kind::rebel: return "rebel movement";
    }
    return "agent";
}

std::string who(game_state const& state, dcon::agent_id a) {
    auto const& w = state.world;
    const dcon::nation_id n = w.agent_get_home(a);
    std::string s = n ? core::unpack_key(w.nation_get_iso3(n)) + " " : std::string{};
    s += kind_noun(kind_of(state, a));
    if(kind_of(state, a) == agent_kind::governor) {
        if(const auto r = region_of(state, a)) s += " of " + core::unpack_key(w.region_get_key(r));
    }
    return s;
}

std::string fmt(char const* f, double x) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, x);
    return buf;
}

// Raises a request; answered at once unless a player answers for the target.
void raise(game_state& state, dcon::agent_id from, dcon::agent_id to, request_kind k, uint8_t lev, float value) {
    auto& w = state.world;
    if(!from || !to || from == to) return;
    const auto r = w.create_request();
    w.request_set_from(r, from);
    w.request_set_to(r, to);
    w.request_set_kind(r, static_cast<uint8_t>(k));
    w.request_set_lever(r, lev);
    w.request_set_value(r, value);
    w.request_set_created(r, state.current_tick);
    w.agent_set_last_request(from, state.current_tick);
    if(w.agent_get_player_controlled(to) && w.agent_get_answer_requests(to)) {
        w.request_set_expires(r, state.current_tick + std::max(1, static_cast<int32_t>(state.params.request_window_days)));
        return;
    }
    w.request_set_expires(r, state.current_tick);
    (void)answer_request(state, r, request_ai_answer(state, r), 0);
}

// What agent `a` (AI-driven, not the player's) asks for this month, if anything.
void consider(game_state& state, dcon::agent_id a) {
    auto& w = state.world;
    auto const& P = state.params;
    const dcon::nation_id n = w.agent_get_home(a);
    if(!n) return;
    const auto gov = w.nation_get_agent_from_executive_seat(n);
    switch(kind_of(state, a)) {
    case agent_kind::central_bank: {
        if(w.nation_get_bank_from_monetary_authority(n) != a) return; // a shared bank speaks for its first nation
        // Depreciation with thin reserves: a swap line from the most trusting lender.
        if(w.nation_get_reserves(n) < 3.0f && w.nation_get_fx_change(n) < -0.1f && !lends_reserves(state, n)) {
            dcon::nation_id best;
            float trust = -1.0f;
            for(auto m : w.in_nation) {
                if(m.id == n || !lends_reserves(state, m.id)) continue;
                const float t = m.get_trust(static_cast<uint16_t>(n.index()));
                if(t > trust) trust = t, best = m.id;
            }
            if(best) {
                raise(state, a, w.nation_get_bank_from_monetary_authority(best), request_kind::swap_line, 0, 2.0f);
                return;
            }
        }
        // An independent bank leans on a government whose spending fuels inflation or debt.
        if(gov && w.nation_get_cb_independence(n) >= 0.5f && (w.nation_get_inflation_expect(n) > w.nation_get_inflation_trend(n) + 0.015f || debt_rise(state, n) > 0.15f)) {
            const uint8_t lev = lever_slot(state, lever::public_investment);
            raise(state, a, gov, request_kind::restrain_spending, lev, std::max(-5.0f, w.agent_get_last_choice(gov, lev) - 1.0f));
        }
        break;
    }
    case agent_kind::executive: {
        const auto bank = w.nation_get_bank_from_monetary_authority(n);
        if(!bank || w.agent_get_home(bank) != n) return; // a currency union's bank answers to no one government
        const float rate = w.nation_get_policy_rate(n);
        if(w.nation_get_unemployment(n) > w.nation_get_unemployment0(n) + 0.02f && rate > static_cast<float>(P.effective_lower_bound) + 0.5f) {
            raise(state, a, bank, request_kind::cut_rates, lever_slot(state, lever::policy_rate), std::max(static_cast<float>(P.effective_lower_bound), rate - 0.5f));
        }
        break;
    }
    case agent_kind::union_federation: {
        const float erosion = w.nation_get_cpi(n) / std::max(0.01f, w.nation_get_core_price(n)) - 1.0f;
        if(gov && (erosion > 0.03f || (w.nation_get_min_wage_bite(n) < 0.35f && w.nation_get_union_density(n) > 0.25f && erosion > 0.01f))) {
            const uint8_t lev = lever_slot(state, lever::min_wage);
            raise(state, a, gov, request_kind::raise_min_wage, lev, std::min(3.0f, std::max(1.0f, w.agent_get_last_choice(gov, lev)) * 1.05f));
        }
        break;
    }
    case agent_kind::board: {
        const auto f = firm_of(state, a);
        if(!f || w.firm_get_cash_days(f) >= 0.4f * w.firm_get_cash_days0(f)) return;
        const auto uni = w.nation_get_union_agent(n);
        // Slack labour markets: first try the union; otherwise the government.
        if(uni && w.nation_get_unemployment(n) > w.nation_get_unemployment0(n) + 0.01f && (w.agent_get_last_request(a) / 365) % 2 == 0) {
            raise(state, a, uni, request_kind::wage_restraint, lever_slot(state, lever::wage_push), 0.0f);
        } else if(gov) {
            raise(state, a, gov, request_kind::rescue, 0, 0.2f);
        }
        break;
    }
    case agent_kind::military: {
        const bool cut = w.nation_get_defense_share0(n) > 0.0f && w.nation_get_defense_share(n) < 0.9f * w.nation_get_defense_share0(n);
        if(gov && (w.nation_get_military_loyalty(n) < 0.5f || cut)) raise(state, a, gov, request_kind::military_pay, 0, 0.1f);
        break;
    }
    case agent_kind::governor: {
        const auto r = region_of(state, a);
        if(gov && r && w.region_get_unrest(r) > 2.0f) raise(state, a, gov, request_kind::regional_aid, 0, 0.05f);
        break;
    }
    case agent_kind::rebel:
        if(gov && w.agent_get_strength(a) > 0.0f && stalemate(state, n) > 0.5f) raise(state, a, gov, request_kind::settlement, 0, 0.0f);
        break;
    }
}

} // namespace

bool request_ai_answer(game_state const& state, dcon::request_id r) {
    auto const& w = state.world;
    const auto from = w.request_get_from(r), to = w.request_get_to(r);
    const dcon::nation_id n = w.agent_get_home(to);
    if(!n) return false;
    switch(static_cast<request_kind>(w.request_get_kind(r))) {
    case request_kind::restrain_spending:
        // Hawks and indebted governments listen; an independent bank's warning weighs.
        return w.agent_get_fiscal_hawk(to) + 0.5f * w.nation_get_cb_independence(n) + std::max(0.0f, debt_rise(state, n)) > 0.8f;
    case request_kind::cut_rates:
        // A dependent bank complies; an independent one only without inflation pressure.
        return w.nation_get_cb_independence(n) < 0.5f || w.nation_get_inflation_expect(n) < w.nation_get_inflation_trend(n) + 0.005f;
    case request_kind::raise_min_wage: {
        float left = 0.0f;
        if(auto inc = w.nation_get_incumbent(n)) left = -w.party_get_position(inc, static_cast<uint8_t>(value_axis::economic_right));
        return left > 0.0f || w.nation_get_union_density(n) > 0.3f || w.nation_get_approval(n) < 0.4f;
    }
    case request_kind::rescue: {
        const auto f = firm_of(state, from);
        if(!f || !fiscal_space(state, n)) return false;
        return w.firm_get_soe_share(f) >= 0.3f || w.firm_get_share(f) >= 0.1f || w.agent_get_credit_bias(to) > 0.3f;
    }
    case request_kind::military_pay:
        return w.nation_get_military_loyalty(n) < 0.4f || w.nation_get_legitimacy(n) < 0.5f || w.agent_get_order_ideal(to) > 0.4f;
    case request_kind::regional_aid: {
        const auto reg = region_of(state, from);
        return fiscal_space(state, n) && (w.nation_get_gov_seat_share(n) < 0.55f || (reg && w.region_get_unrest(reg) > 2.5f));
    }
    case request_kind::swap_line: {
        const dcon::nation_id borrower = w.agent_get_home(from);
        return borrower && lends_reserves(state, n)
               && (w.nation_get_trust(n, static_cast<uint16_t>(borrower.index())) >= 0.5f || w.nation_get_alliance(n, static_cast<uint16_t>(borrower.index())) > 0.0f);
    }
    case request_kind::settlement: {
        const auto rebel = w.nation_get_rebel_agent(n);
        return rebel && (stalemate(state, n) >= 0.5f || w.agent_get_strength(rebel) >= w.nation_get_state_capacity(n));
    }
    case request_kind::wage_restraint:
        return w.nation_get_unemployment(n) > w.nation_get_unemployment0(n) + 0.01f || w.nation_get_coordination(n) >= 0.5f;
    default: return false;
    }
}

request_text describe_request(game_state const& state, dcon::request_id r) {
    auto const& w = state.world;
    const auto from = w.request_get_from(r), to = w.request_get_to(r);
    request_text t;
    const std::string a = who(state, from), b = who(state, to);
    switch(static_cast<request_kind>(w.request_get_kind(r))) {
    case request_kind::restrain_spending:
        t.title = a + " asks the " + kind_noun(kind_of(state, to)) + " to restrain spending";
        t.detail = fmt("Public investment toward %+.2f%% of GDP for a year", w.request_get_value(r));
        break;
    case request_kind::cut_rates:
        t.title = a + " asks the central bank to cut rates";
        t.detail = fmt("Policy rate toward %.2f%% for six months", w.request_get_value(r));
        break;
    case request_kind::raise_min_wage:
        t.title = a + " asks for a higher minimum wage";
        t.detail = fmt("Minimum wage toward %.2f x the start for a year; refusing feeds protest", w.request_get_value(r));
        break;
    case request_kind::rescue: t.title = a + " asks for a rescue"; t.detail = fmt("%.2f%% of GDP for state equity in the firm", w.request_get_value(r)); break;
    case request_kind::military_pay: t.title = a + " asks for a pay rise"; t.detail = fmt("A bonus of %.2f%% of GDP buys loyalty; refusing costs it", w.request_get_value(r)); break;
    case request_kind::regional_aid: t.title = a + " asks for regional aid"; t.detail = fmt("%.2f%% of GDP of public investment in the region; refusing feeds unrest", w.request_get_value(r)); break;
    case request_kind::swap_line: t.title = a + " asks " + b + " for a swap line"; t.detail = fmt("Lend them %.1f months of imports in reserves", w.request_get_value(r)); break;
    case request_kind::settlement: t.title = a + " asks for a settlement"; t.detail = "Power sharing for the most excluded group; the insurgency ends"; break;
    case request_kind::wage_restraint: t.title = a + " offers jobs for wage restraint"; t.detail = "A year of wage moderation; the firm keeps its staff"; break;
    default: t.title = a + " makes a request"; break;
    }
    return t;
}

action_result answer_request(game_state& state, dcon::request_id r, bool accept, uint8_t by) {
    auto& w = state.world;
    action_result res;
    if(!w.request_is_valid(r)) {
        res.message = "No such request";
        return res;
    }
    const auto from = w.request_get_from(r), to = w.request_get_to(r);
    const auto k = static_cast<request_kind>(w.request_get_kind(r));
    const dcon::nation_id n = w.agent_get_home(to);
    const uint8_t lev = w.request_get_lever(r);
    const float value = w.request_get_value(r);
    auto act = [&](action_kind kind, dcon::agent_id actor, dcon::agent_id target, float v) {
        action_request q;
        q.kind = kind;
        q.actor = actor;
        q.target_agent = target;
        q.value = v;
        q.silent = true;
        return perform_action(state, q);
    };
    bool done = accept;
    std::string msg = accept ? "Accepted" : "Declined";
    if(accept) {
        switch(k) {
        case request_kind::restrain_spending:
        case request_kind::raise_min_wage: put_pressure(state, to, from, lev, value, 2.0f, 365); break;
        case request_kind::cut_rates: put_pressure(state, to, from, lev, value, 2.0f, 182); break;
        case request_kind::wage_restraint:
            put_pressure(state, to, from, lev, std::min(0.0f, w.agent_get_last_choice(to, lev)), 1.0f, 365);
            put_pressure(state, from, to, lever_slot(state, lever::hoarding), 0.9f, 1.0f, 365); // the board keeps its staff
            break;
        case request_kind::rescue: {
            const auto res2 = act(action_kind::rescue_firm, to, from, value);
            done = res2.accepted, msg = res2.message;
            break;
        }
        case request_kind::military_pay: {
            const auto res2 = act(action_kind::military_pay, to, from, value);
            done = res2.accepted, msg = res2.message;
            break;
        }
        case request_kind::regional_aid: {
            const auto res2 = act(action_kind::regional_transfer, to, from, value);
            done = res2.accepted, msg = res2.message;
            break;
        }
        case request_kind::swap_line: {
            const dcon::nation_id borrower = w.agent_get_home(from);
            if(borrower) {
                w.nation_set_reserves(borrower, w.nation_get_reserves(borrower) + value);
                w.nation_set_trust(borrower, static_cast<uint16_t>(n.index()), std::min(1.0f, w.nation_get_trust(borrower, static_cast<uint16_t>(n.index())) + 0.05f));
                msg = fmt("Swap line granted: their reserves %.1f months", w.nation_get_reserves(borrower));
            }
            break;
        }
        case request_kind::settlement:
            if(n) settle_rebellion(state, n);
            msg = "Power sharing agreed; the insurgency ends";
            break;
        default: break;
        }
    } else if(n) {
        // Refusals have a price where the asker has a lever of its own.
        switch(k) {
        case request_kind::raise_min_wage:
            for(auto own : w.nation_get_region_ownership(n)) {
                auto reg = own.get_region();
                reg.set_protest(std::min(1.0f, reg.get_protest() + 0.05f * w.nation_get_union_density(n)));
            }
            msg = "Declined: the union protests";
            break;
        case request_kind::military_pay:
            w.nation_set_military_loyalty(n, std::max(0.0f, w.nation_get_military_loyalty(n) - 0.05f));
            msg = "Declined: the officers resent it";
            break;
        case request_kind::regional_aid:
            if(const auto reg = region_of(state, from)) w.region_set_unrest(reg, w.region_get_unrest(reg) + 0.2f);
            msg = "Declined: the region grows restless";
            break;
        case request_kind::swap_line:
            if(const dcon::nation_id borrower = w.agent_get_home(from)) {
                w.nation_set_trust(borrower, static_cast<uint16_t>(n.index()), std::max(0.0f, w.nation_get_trust(borrower, static_cast<uint16_t>(n.index())) - 0.02f));
            }
            break;
        default: break;
        }
    }
    const auto d = w.create_decision();
    w.decision_set_agent(d, to);
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, journal_request);
    w.decision_set_before(d, static_cast<float>(k));
    w.decision_set_after(d, done ? 1.0f : 0.0f);
    const dcon::nation_id fn = w.agent_get_home(from);
    w.decision_set_pragmatic(d, fn ? static_cast<float>(fn.index()) : NAN);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, static_cast<float>(by));
    w.delete_request(r);
    res.accepted = done;
    res.message = std::move(msg);
    return res;
}

void update_requests(game_state& state) {
    auto& w = state.world;
    const auto tick = static_cast<tick_t>(state.current_tick);
    // Requests whose window closed: the asked agent's AI answers for the player.
    std::vector<dcon::request_id> due;
    for(auto r : w.in_request) {
        // A target the player no longer answers for (control or the toggle dropped) answers now.
        const auto to = r.get_to();
        const bool waiting = to && w.agent_get_player_controlled(to) && w.agent_get_answer_requests(to);
        if(state.current_tick >= r.get_expires() || !waiting) due.push_back(r.id);
    }
    for(auto r : due) (void)answer_request(state, r, request_ai_answer(state, r), 2);
    // New requests, monthly per agent; the player's own agents ask nothing (the player acts).
    const int32_t cooldown = static_cast<int32_t>(state.params.request_cooldown_days);
    std::vector<dcon::agent_id> askers;
    for(auto a : w.in_agent) {
        if(!is_due(static_cast<uint32_t>(a.id.index()), tick, monthly_cadence)) continue;
        if(a.get_player_controlled()) continue;
        if(a.get_last_request() != 0 && state.current_tick - a.get_last_request() < cooldown) continue;
        askers.push_back(a.id);
    }
    for(auto a : askers) consider(state, a);
}

} // namespace geosim::sim
