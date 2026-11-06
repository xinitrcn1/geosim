// Player actions (docs/06-geopolitics-ai.md#player-actions): the diplomatic and political
// moves the simulation already models, opened to the agent the player controls. Every
// action is answered by the counterpart with the rules the AI itself uses, so the player
// gets no outcome the model would not produce:
//  - sanctions: a level held until cleared (geopolitics.cpp still applies UN floors);
//  - disputes and wars: escalation and backing down with audience costs (Fearon 1994;
//    Weeks 2008), war with allies called (call_allies) and an aggressor's loss of trust;
//  - peace: accepted by a side that is losing ground or paying heavily (bargaining model:
//    Fearon 1995; Reiter 2009), as in geopolitics.cpp's settlement hazard;
//  - aid and treaties (agreements.cpp): a recipient takes aid unless it distrusts the donor;
//    conditional aid and migration deals need trust; a climate pledge is unilateral;
//  - organisations: applications decided by the organisation's rule (organisations.cpp,
//    geo_orgs.cpp), leaving is unilateral;
//  - rebels: power sharing accepted at a mutually hurting stalemate (Zartman 1989) or by a
//    losing side;
//  - lobbying: firms and unions press their government on one lever; the government's AI
//    weighs it as advice in proportion to the lobby's economic weight (Grossman & Helpman
//    1994), for half a year.
//
// Between agents of every kind (the Diplomacy subtabs: central banks, firms, unions,
// militaries, governors, rebels):
//  - pressing: any agent can press a domestic agent that has levers (government, central
//    bank, board, union, governor) for a value on one of them. The pressure is advice the
//    target's AI weighs for six months (ai.cpp apply_pressure) by the presser's leverage
//    over it (press_weight): governments over dependent central banks (Cukierman 1992;
//    Binder 2021), state-owned boards and centralised governors; central banks over unions
//    under coordinated bargaining (Hall & Franzese 1998); unions over boards by coverage;
//    militaries over governments where institutions are weak (Finer 1962);
//  - governments: replace the central-bank governor (courts protect an independent one),
//    rescue a firm (cash for state equity), a social pact with the union (wage moderation;
//    Avdagic, Rhodes & Visser 2011) or curbs on unions, pay or purge the military
//    (coup-proofing: Powell 2012; Quinlivan 1999), regional transfers, counter-insurgency,
//    arming another nation's rebels (Salehyan 2009), joint exercises with foreign militaries;
//  - central banks: swap lines between central banks (2008 and 2020 dollar lines), emergency
//    liquidity for firms and regions (the 2020 municipal facility), freezing rebel funds;
//  - firms and unions: strikes, general strikes, jobs-for-wages deals (concession
//    bargaining), complaints to rights bodies;
//  - militaries: a coup against their government (Powell & Thyne 2011), counter-insurgency;
//  - governors: demands for autonomy (bargained where governments need regional support);
//  - rebels: foreign backing from the government's rivals, courting military defectors,
//    appeals to rights bodies.
// Pairs with no modelled channel (a central bank and a foreign army, say) list no options.

#include "core/calendar.hpp"
#include "core/keys.hpp"
#include "sim/ledger.hpp"
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

uint16_t ix(dcon::nation_id n) { return static_cast<uint16_t>(n.index()); }

dcon::nation_id home_of(game_state const& state, dcon::agent_id a) {
    return a ? state.world.agent_get_home(a) : dcon::nation_id{};
}

void journal_action(game_state& state, dcon::agent_id actor, dcon::nation_id n, dcon::nation_id other, action_kind k, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, actor);
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, journal_player_action);
    w.decision_set_before(d, static_cast<float>(k));
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, other ? static_cast<float>(other.index()) : NAN);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

bool at_war(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    return state.world.nation_get_at_war(a, ix(b)) > 0.0f;
}

// How much a side at war has lost: occupation of its land and the war's cost.
float losing(game_state const& state, dcon::nation_id loser, dcon::nation_id winner) {
    auto const& w = state.world;
    return w.nation_get_control(winner, ix(loser)) + 3.0f * w.nation_get_war_cost(loser) + (1.0f - w.nation_get_willingness(loser));
}

float stalemate_of(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const auto rebel = w.nation_get_rebel_agent(n);
    if(!rebel) return 0.0f;
    return std::max(0.0f, 1.0f - std::abs(w.agent_get_strength(rebel) - w.nation_get_state_capacity(n)) * 2.0f);
}

dcon::treaty_id sign_treaty(game_state& state, treaty_kind k, dcon::nation_id a, dcon::nation_id b, float terms) {
    auto& w = state.world;
    const auto t = w.create_treaty();
    w.treaty_set_kind(t, static_cast<uint8_t>(k));
    w.treaty_set_a(t, a);
    w.treaty_set_b(t, b);
    w.treaty_set_signed_tick(t, state.current_tick);
    w.treaty_set_terms(t, terms);
    w.treaty_set_compliance(t, 1.0f);
    w.treaty_set_active(t, 1);
    return t;
}

void settle_rebels(game_state& state, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    // The most excluded mobilised group gains a share of power (Hartzell & Hoddie 2003).
    dcon::social_group_id grp;
    float worst = 0.0f;
    for(auto g : w.in_social_group) {
        if(g.get_nation() == n && g.get_excluded() * g.get_mobilisation() > worst) {
            worst = g.get_excluded() * g.get_mobilisation();
            grp = g.id;
        }
    }
    if(grp) w.social_group_set_excluded(grp, w.social_group_get_excluded(grp) * (1.0f - static_cast<float>(P.power_sharing)));
    sign_treaty(state, treaty_kind::power_sharing, n, {}, static_cast<float>(P.power_sharing));
    for(auto own : w.nation_get_region_ownership(n)) own.get_region().set_insurgency(0.0f);
    if(auto rebel = w.nation_get_rebel_agent(n)) w.agent_set_strength(rebel, 0.0f);
}


agent_kind kind_of(game_state const& state, dcon::agent_id a) { return static_cast<agent_kind>(state.world.agent_get_kind(a)); }

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

// A board's weight in its nation: its firm's share of national output (x5, as lobbying).
float board_weight(game_state const& state, dcon::agent_id board, dcon::nation_id n) {
    double firm = 0.0, total = 0.0;
    for(auto f : state.world.in_firm) {
        if(f.get_nation() != n) continue;
        total += f.get_share();
        if(f.get_board() == board) firm += f.get_share();
    }
    return total > 0.0 ? static_cast<float>(firm / total) * 5.0f : 0.1f;
}

// Leverage of `actor` over a domestic agent `target` (0: no channel). The weight is the
// advice weight of the pressure in the target's choice (1 = as strong as its own view).
float press_weight(game_state const& state, dcon::agent_id actor, dcon::agent_id target) {
    auto const& w = state.world;
    const auto n = home_of(state, actor);
    if(!n || !target || home_of(state, target) != n || actor == target) return 0.0f;
    const auto ak = kind_of(state, actor), tk = kind_of(state, target);
    const float cbi = w.nation_get_cb_independence(n);
    const float coord = w.nation_get_coordination(n);
    const float decent = w.nation_get_decentralisation(n);
    auto soe = [&] {
        const auto f = firm_of(state, target);
        return f ? w.firm_get_soe_share(f) : w.nation_get_soe_share(n);
    };
    float k = 0.0f;
    switch(tk) {
    case agent_kind::executive:
        switch(ak) {
        case agent_kind::board: k = board_weight(state, actor, n); break;
        case agent_kind::union_federation: k = w.nation_get_union_density(n); break;
        case agent_kind::central_bank: k = 0.5f * cbi; break;
        case agent_kind::military: k = 0.6f * (1.0f - w.nation_get_liberal(n)); break;
        case agent_kind::governor: k = 0.1f + 0.4f * decent; break;
        default: break;
        }
        break;
    case agent_kind::central_bank:
        switch(ak) {
        case agent_kind::executive: k = 1.0f - cbi; break;
        case agent_kind::board:
        case agent_kind::union_federation: k = 0.1f * (1.0f - cbi); break;
        case agent_kind::governor: k = 0.05f * (1.0f - cbi); break;
        default: break;
        }
        break;
    case agent_kind::board:
        switch(ak) {
        case agent_kind::executive: k = 0.1f + 0.8f * soe(); break;
        case agent_kind::union_federation: k = 0.5f * w.nation_get_bargaining_coverage(n); break;
        case agent_kind::governor: k = 0.05f + 0.1f * soe(); break;
        default: break;
        }
        break;
    case agent_kind::union_federation:
        switch(ak) {
        case agent_kind::executive: k = 0.5f * coord; break;
        case agent_kind::central_bank: k = cbi * coord; break;
        case agent_kind::board: k = 0.3f * coord; break;
        default: break;
        }
        break;
    case agent_kind::governor:
        switch(ak) {
        case agent_kind::executive: k = 0.3f + 0.6f * (1.0f - decent); break;
        case agent_kind::governor: k = 0.2f; break;
        case agent_kind::board: k = 0.1f; break;
        case agent_kind::union_federation: k = 0.3f * w.nation_get_union_density(n); break;
        default: break;
        }
        break;
    default: break; // militaries and rebels have no levers to press on
    }
    return std::clamp(k, 0.0f, 1.0f) < 0.02f && k > 0.0f ? 0.02f : std::clamp(k, 0.0f, 1.0f);
}

// Is lever slot `lev` one of `kind`'s levers (NPIs are the government's)?
bool owns_lever(game_state const& state, agent_kind kind, uint8_t lev) {
    auto const& D = state.derived;
    if(lev >= D.lever_count) return false;
    if(lev < D.lever_base) return kind == agent_kind::executive;
    return lever_table[lev - D.lever_base].owner == kind;
}

uint8_t lever_slot(game_state const& state, lever l) { return static_cast<uint8_t>(state.derived.lever_base + static_cast<uint8_t>(l)); }


double nominal_gdp(game_state const& state, dcon::nation_id n) {
    return state.world.nation_get_gdp0(n) * state.world.nation_get_core_price(n);
}

// A firm's baseline daily costs (value added of its share of the sector), USD.
double firm_daily(game_state const& state, dcon::firm_id f) {
    auto const& w = state.world;
    const auto n = w.firm_get_nation(f);
    const auto s = static_cast<uint8_t>(w.firm_get_sector(f).id.index());
    return std::max(1.0, static_cast<double>(w.firm_get_share(f)) * w.nation_get_va0(n, s) * w.nation_get_core_price(n));
}

float mean_protest_drive(game_state& state, dcon::nation_id n, float add) {
    auto& w = state.world;
    float pop = 0.0f, p = 0.0f;
    for(auto own : w.nation_get_region_ownership(n)) {
        auto r = own.get_region();
        r.set_protest(std::clamp(r.get_protest() + add, 0.0f, 1.0f));
        p += r.get_protest() * r.get_population();
        pop += r.get_population();
    }
    return pop > 0.0f ? p / pop : 0.0f;
}

bool lends_reserves(game_state const& state, dcon::nation_id n) {
    return state.world.nation_get_reserve_currency(n) >= 0.3f || state.world.nation_get_reserves(n) >= 8.0f;
}

void rebels_defeated(game_state& state, dcon::nation_id n) {
    auto& w = state.world;
    for(auto own : w.nation_get_region_ownership(n)) own.get_region().set_insurgency(0.0f);
    if(auto rebel = w.nation_get_rebel_agent(n)) w.agent_set_strength(rebel, 0.0f);
}

std::string fmt(char const* f, double x) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, x);
    return buf;
}

} // namespace

void put_pressure(game_state& state, dcon::agent_id target, dcon::agent_id from, uint8_t lev, float value, float weight, int days) {
    auto& w = state.world;
    w.agent_set_pressure_lever(target, lev);
    w.agent_set_pressure_value(target, value);
    w.agent_set_pressure_weight(target, weight);
    w.agent_set_pressure_until(target, state.current_tick + days);
    w.agent_set_pressure_from(target, from);
}

void settle_rebellion(game_state& state, dcon::nation_id n) { settle_rebels(state, n); }

std::vector<action_option> available_actions(game_state const& state, dcon::agent_id actor, dcon::nation_id target, dcon::organisation_id organisation,
                                               dcon::agent_id target_agent) {
    auto const& w = state.world;
    std::vector<action_option> out;
    if(!actor) return out;
    const auto kind = static_cast<agent_kind>(w.agent_get_kind(actor));
    const auto self = home_of(state, actor);
    auto add = [&](action_kind k, char const* label, bool ok, std::string reason) {
        out.push_back({k, label, ok, std::move(reason)});
        return &out.back();
    };
    auto amount = [](action_option* o, char const* unit, float lo, float hi, float step, float initial) {
        o->input = action_value::amount;
        o->unit = unit;
        o->lo = lo;
        o->hi = hi;
        o->step = step;
        o->initial = initial;
    };
    if(target_agent) {
        // Toward another agent (Diplomacy subtabs other than Governments and Organisations).
        if(!w.agent_is_valid(target_agent) || target_agent == actor) return out;
        const auto tk = kind_of(state, target_agent);
        const auto there = home_of(state, target_agent);
        const bool dom = there == self;
        const float pw = press_weight(state, actor, target_agent);
        const bool has_levers = tk == agent_kind::executive || tk == agent_kind::central_bank || tk == agent_kind::board || tk == agent_kind::union_federation
                                || tk == agent_kind::governor;
        if(dom && has_levers && pw > 0.0f) {
            add(action_kind::press, "Press for a policy", true, fmt("advice weighed at %.2f of its own view for six months", pw))->input = action_value::lever;
        }
        const float strength = tk == agent_kind::rebel ? w.agent_get_strength(target_agent) : 0.0f;
        switch(kind) {
        case agent_kind::executive:
            if(tk == agent_kind::central_bank && dom) {
                const float guard = w.nation_get_judicial(self) * w.nation_get_cb_independence(self);
                add(action_kind::replace_cb_governor, "Replace the governor", guard <= 0.45f,
                    guard <= 0.45f ? fmt("independence %.2f falls; markets and your credibility suffer", w.nation_get_cb_independence(self))
                                   : "the courts protect an independent governor's tenure");
            } else if(tk == agent_kind::board && dom) {
                amount(add(action_kind::rescue_firm, "Rescue package", true, "cash for state equity (% of GDP)"), "% of GDP", 0.05f, 3.0f, 0.05f, 0.2f);
            } else if(tk == agent_kind::union_federation && dom) {
                add(action_kind::social_pact, "Propose a social pact", true, "wage moderation for a year; unions sign in slack labour markets or under coordination");
                add(action_kind::restrict_unions, "Restrict union rights", true, "density and coverage fall; protest, and courts may strike it down");
            } else if(tk == agent_kind::military && dom) {
                amount(add(action_kind::military_pay, "Raise military pay", true, fmt("loyalty %.2f; a one-off bonus (% of GDP)", w.nation_get_military_loyalty(self))),
                       "% of GDP", 0.05f, 2.0f, 0.05f, 0.2f);
                add(action_kind::purge_officers, "Purge the officer corps", true, "loyalty now, fighting power lost (coup-proofing)");
            } else if(tk == agent_kind::military && !dom && there) {
                const bool hostile = at_war(state, self, there) || w.nation_get_hostility(self, ix(there)) >= 2.0f;
                add(action_kind::joint_exercises, "Hold joint exercises", !hostile, hostile ? "a hostile nation" : "mutual trust grows if they trust you");
            } else if(tk == agent_kind::governor && dom) {
                amount(add(action_kind::regional_transfer, "Regional transfer", true, "public investment in the region (% of GDP)"), "% of GDP", 0.01f, 1.0f, 0.01f, 0.05f);
            } else if(tk == agent_kind::rebel && dom) {
                add(action_kind::offer_power_sharing, "Offer power sharing", strength > 0.0f,
                    strength > 0.0f ? fmt("stalemate %.2f (accepted above 0.3 or if they are weaker than the state)", stalemate_of(state, self)) : "no rebellion");
                add(action_kind::counterinsurgency, "Counter-insurgency offensive", strength > 0.0f,
                    strength > 0.0f ? fmt("strength %.2f; costs grievance where they fight", strength) : "no rebellion");
            } else if(tk == agent_kind::rebel && !dom) {
                add(action_kind::arm_rebels, "Arm the rebels", strength > 0.0f, strength > 0.0f ? "their government will hold it against you" : "no active rebellion");
            }
            break;
        case agent_kind::central_bank:
            if(tk == agent_kind::central_bank && !dom) {
                const bool lend = lends_reserves(state, self);
                amount(add(action_kind::swap_line, lend ? "Offer a swap line" : "Request a swap line", lend || lends_reserves(state, there),
                           lend ? "lend them reserves (months of imports)" : lends_reserves(state, there) ? "they lend if they trust you" : "they hold no reserves to lend"),
                       "months of imports", 0.5f, 6.0f, 0.5f, 2.0f);
            } else if((tk == agent_kind::board || tk == agent_kind::governor) && dom) {
                amount(add(action_kind::emergency_liquidity, tk == agent_kind::board ? "Emergency liquidity" : "Municipal liquidity facility", true,
                           tk == agent_kind::board ? "lend to the firm against collateral (% of GDP)" : "buy the region's short-term debt (% of GDP)"),
                       "% of GDP", 0.05f, 3.0f, 0.05f, 0.2f);
            } else if(tk == agent_kind::rebel && dom) {
                add(action_kind::freeze_rebel_funds, "Freeze rebel finances", strength > 0.0f && w.nation_get_state_capacity(self) >= 0.4f,
                    strength <= 0.0f ? "no rebellion" : w.nation_get_state_capacity(self) >= 0.4f ? "works where the state reaches the banks" : "the state is too weak to enforce it");
            }
            break;
        case agent_kind::board:
            if(tk == agent_kind::union_federation && dom) add(action_kind::wage_deal, "Offer jobs for wage restraint", true, "you commit to keep staff; the union moderates wages");
            break;
        case agent_kind::union_federation:
            if(tk == agent_kind::board && dom) add(action_kind::strike, "Strike at the firm", true, fmt("union density %.2f: costs the firm cash, wins job guarantees", w.nation_get_union_density(self)));
            break;
        case agent_kind::military:
            if(tk == agent_kind::military && !dom && there) {
                const bool hostile = at_war(state, self, there) || w.nation_get_hostility(self, ix(there)) >= 2.0f;
                add(action_kind::joint_exercises, "Hold joint exercises", !hostile, hostile ? "a hostile nation" : "mutual trust grows if they trust you");
            } else if(tk == agent_kind::rebel && dom) {
                add(action_kind::counterinsurgency, "Counter-insurgency offensive", strength > 0.0f,
                    strength > 0.0f ? fmt("strength %.2f; costs grievance where you fight", strength) : "no rebellion");
            }
            break;
        case agent_kind::rebel:
            if(tk == agent_kind::military && dom) {
                add(action_kind::court_defectors, "Court defectors", w.agent_get_strength(actor) > 0.0f,
                    fmt("their loyalty %.2f: defections where it is low", w.nation_get_military_loyalty(self)));
            }
            break;
        default: break;
        }
        return out;
    }
    switch(kind) {
    case agent_kind::executive: {
        if(target && target != self) {
            const auto k = ix(target);
            const bool war = at_war(state, self, target);
            const float h = w.nation_get_hostility(self, k);
            add(action_kind::set_sanction, "Set sanctions", true, fmt("current level %.2f", w.nation_get_sanction(self, k)));
            add(action_kind::clear_sanction, "Leave sanctions to the government", w.nation_get_sanction_set(self, k) >= 0.0f, "no sanction level is held");
            add(action_kind::escalate_dispute, "Escalate (raise hostility)", !war, war ? "already at war" : fmt("hostility %.0f of 5", h));
            add(action_kind::back_down, "Back down", !war && h >= 2.0f, h >= 2.0f ? "concede the dispute (costs credibility at home)" : "no open dispute");
            const bool allied = w.nation_get_alliance(self, k) > 0.0f;
            add(action_kind::declare_war, "Declare war", !war && !allied, war ? "already at war" : allied ? "an ally" : "allies of both sides are called");
            add(action_kind::offer_peace, "Offer peace", war, war ? "accepted if they are losing or war-weary" : "not at war");
            add(action_kind::send_aid, "Send aid", true, "a one-off grant (% of your GDP)");
            const bool poor = w.nation_get_gdp_pc0(target) < static_cast<float>(state.params.aid_income);
            add(action_kind::propose_treaty, "Propose a treaty", true,
                poor ? "conditional aid or a migration deal" : "a migration deal (conditional aid only for poorer nations)");
        } else if(!target && !organisation) {
            add(action_kind::climate_pledge, "Pledge a 2030 carbon price", true, fmt("current pledge %.0f USD/t", w.nation_get_climate_pledge(self)));
        }
        if(organisation) {
            const auto m = membership_in(state, organisation, self);
            add(action_kind::apply_membership, "Apply to join", !m, m ? "already a member" : "members decide by the organisation's rule");
            add(action_kind::leave_membership, "Leave", bool(m), m ? "unilateral" : "not a member");
        }
        break;
    }
    case agent_kind::rebel: {
        const bool active = w.agent_get_strength(actor) > 0.0f;
        if(target == self) {
            add(action_kind::accept_settlement, "Seek a settlement", active, active ? "the government accepts if it is weak or the war is a stalemate" : "the rebellion is over");
        } else if(target) {
            add(action_kind::foreign_backing, "Seek their backing", active, active ? "rivals of your government arm you" : "the rebellion is over");
        }
        break;
    }
    case agent_kind::board:
    case agent_kind::union_federation:
    case agent_kind::central_bank:
    case agent_kind::military:
    case agent_kind::governor:
        if(target == self && self) {
            const auto gov = w.nation_get_agent_from_executive_seat(self);
            const float pw = press_weight(state, actor, gov);
            add(action_kind::lobby, kind == agent_kind::board || kind == agent_kind::union_federation ? "Lobby the government" : "Press the government", pw > 0.0f,
                fmt("press for a value on one lever (advice weighed at %.2f)", pw))->input = action_value::lever;
            if(kind == agent_kind::union_federation) {
                add(action_kind::general_strike, "Call a general strike", true,
                    fmt("press hard on one lever (weight %.2f); protest and lost output", 2.0f * w.nation_get_union_density(self)))->input = action_value::lever;
            } else if(kind == agent_kind::military) {
                add(action_kind::seize_power, "Seize power", true,
                    fmt("legitimacy %.2f: coups succeed against weak, unpopular governments", w.nation_get_legitimacy(self)));
            } else if(kind == agent_kind::governor) {
                add(action_kind::demand_autonomy, "Demand more autonomy", true,
                    fmt("decentralisation %.2f; granted by governments that need regional support", w.nation_get_decentralisation(self)));
            }
        }
        break;
    default: break;
    }
    if(organisation && (kind == agent_kind::union_federation || kind == agent_kind::rebel)) {
        const bool rights = w.organisation_get_kind(organisation) == static_cast<uint8_t>(org_kind::rights);
        add(action_kind::rights_complaint, "File a complaint", rights, rights ? "the body shames your government if it represses" : "only rights bodies hear complaints");
    }
    return out;
}

std::vector<dcon::state_id> redraw_targets(game_state const& state, dcon::region_id r) {
    auto const& w = state.world;
    std::vector<dcon::state_id> out;
    if(!r) return out;
    const auto own = w.region_get_state(r);
    const auto n = w.region_get_owner(r);
    auto consider = [&](dcon::region_id o) {
        const auto st = w.region_get_state(o);
        if(st && st != own && w.region_get_owner(o) == n && w.state_get_nation(st) == n && std::find(out.begin(), out.end(), st) == out.end()) out.push_back(st);
    };
    for(auto a : w.region_get_region_adjacency_as_a(r)) consider(a.get_b());
    for(auto a : w.region_get_region_adjacency_as_b(r)) consider(a.get_a());
    return out;
}

redraw_verdict check_redraw(game_state const& state, dcon::agent_id actor, dcon::region_id r, dcon::state_id to) {
    auto const& w = state.world;
    redraw_verdict v;
    const auto self = home_of(state, actor);
    if(!actor || !self || static_cast<agent_kind>(w.agent_get_kind(actor)) != agent_kind::executive) {
        v.reason = "Only a government redraws its states";
        return v;
    }
    if(!r || w.region_get_owner(r) != self) {
        v.reason = "Not one of your regions";
        return v;
    }
    const auto from = w.region_get_state(r);
    if(to && (to == from || w.state_get_nation(to) != self)) {
        v.reason = "Choose another of your states";
        return v;
    }
    if(to) {
        const auto t = redraw_targets(state, r);
        if(std::find(t.begin(), t.end(), to) == t.end()) {
            v.reason = "That state does not border this region";
            return v;
        }
    }
    // The share of the source state the region takes with it (dissolving it at 1).
    double total = 0.0;
    std::size_t members = 0;
    for(auto rr : w.in_region) {
        if(rr.get_state() == from) {
            total += rr.get_population();
            ++members;
        }
    }
    const double share = total > 0.0 ? w.region_get_population(r) / total : 1.0;
    if(!to && members <= 1) {
        v.reason = "It is already a state of its own";
        return v;
    }
    const float liberal = w.nation_get_liberal(self);
    const float judicial = w.nation_get_judicial(self);
    const float seats = w.nation_get_gov_seat_share(self);
    if(liberal < 0.3f) {
        v.ok = true;
        v.reason = "By decree";
        return v;
    }
    if(liberal < 0.6f) {
        v.ok = seats > 0.5f;
        v.reason = v.ok ? fmt("Passed by the government's majority (%.0f%% of seats)", 100.0 * seats)
                        : fmt("No majority: the government holds %.0f%% of seats", 100.0 * seats);
        return v;
    }
    // Liberal democracy: a supermajority growing with constitutional entrenchment (courts).
    const float needed = 0.5f + 0.17f * judicial;
    if(seats < needed) {
        v.reason = fmt("A constitutional change: %.0f%% of seats needed", 100.0 * needed) + fmt(", the government holds %.0f%%", 100.0 * seats);
        return v;
    }
    if(judicial > 0.6f && share > 0.25) {
        v.reason = members <= 1 || share >= 0.999 ? "The courts will not let a state be dissolved" : "The courts strike down so large a transfer";
        return v;
    }
    // Decentralised (federal) nations: the states concerned must consent - their governors
    // must sit with the government.
    if(w.nation_get_decentralisation(self) > 0.4f) {
        auto aligned = [&](dcon::state_id st) {
            if(!st) return true;
            const auto g = w.state_get_governor(st);
            if(!g) return true;
            const float gap = std::fabs(w.agent_get_order_ideal(g) - w.agent_get_order_ideal(actor));
            return gap < 0.15f;
        };
        if(!aligned(from) || !aligned(to)) {
            v.reason = "The states concerned withhold consent";
            return v;
        }
    }
    v.ok = true;
    v.reason = "Approved by the legislature and the states concerned";
    return v;
}

action_result perform_action(game_state& state, action_request const& q) {
    auto& w = state.world;
    auto const& P = state.params;
    action_result res;
    const auto self = home_of(state, q.actor);
    if(!q.actor || !self) {
        res.message = "No acting agent";
        return res;
    }
    const auto t = q.target;
    auto done = [&](bool ok, std::string msg) {
        res.accepted = ok;
        res.message = std::move(msg);
        if(!q.silent) journal_action(state, q.actor, self, t, q.kind, ok ? 1.0f : 0.0f);
        return res;
    };
    switch(q.kind) {
    case action_kind::set_sanction: {
        if(!t || t == self) return done(false, "Choose another nation");
        const float v = std::clamp(q.value, 0.0f, 1.0f);
        w.nation_set_sanction_set(self, ix(t), v);
        w.nation_set_sanction(self, ix(t), std::max(v, at_war(state, self, t) ? 1.0f : 0.0f));
        return done(true, fmt("Sanctions set to %.2f", v));
    }
    case action_kind::clear_sanction:
        if(!t) return done(false, "Choose a nation");
        w.nation_set_sanction_set(self, ix(t), -1.0f);
        return done(true, "Sanctions handed back to the government's judgement");
    case action_kind::escalate_dispute: {
        if(!t || t == self || at_war(state, self, t)) return done(false, "Not possible");
        const float h = std::min(4.0f, std::max(1.0f, w.nation_get_hostility(self, ix(t))) + 1.0f);
        if(h >= 2.0f && w.nation_get_hostility(self, ix(t)) < 2.0f) {
            start_dispute(state, self, t);
        }
        w.nation_set_hostility(self, ix(t), h);
        w.nation_set_hostility(t, ix(self), h);
        return done(true, fmt("Hostility raised to %.0f", h));
    }
    case action_kind::back_down: {
        if(!t || w.nation_get_hostility(self, ix(t)) < 2.0f || at_war(state, self, t)) return done(false, "No open dispute");
        w.nation_set_hostility(self, ix(t), 0.0f);
        w.nation_set_hostility(t, ix(self), 0.0f);
        // Audience costs: leaders who back down after escalating lose standing (Fearon 1994).
        const float cost = static_cast<float>(P.audience_cost) * w.nation_get_electoral(self) * 0.1f;
        w.nation_set_legitimacy(self, std::max(0.0f, w.nation_get_legitimacy(self) - cost));
        lose_credibility(state, self, 0.05f);
        return done(true, fmt("Dispute conceded (legitimacy -%.3f)", cost));
    }
    case action_kind::declare_war: {
        if(!t || t == self || at_war(state, self, t)) return done(false, "Not possible");
        const bool provoked = w.nation_get_hostility(self, ix(t)) >= 2.0f;
        start_war(state, self, t);
        if(!provoked) {
            // Unprovoked aggression: others' trust in the aggressor falls (Weeks 2008).
            for(auto x : w.in_nation) {
                if(x.id == self) continue;
                x.set_trust(ix(self), std::max(0.0f, x.get_trust(ix(self)) - 0.1f));
            }
        }
        return done(true, provoked ? "War declared" : "War declared without a dispute: other nations trust you less");
    }
    case action_kind::offer_peace: {
        if(!t || !at_war(state, self, t)) return done(false, "Not at war");
        // The target accepts if it is losing more than the offering side (or is exhausted).
        const float them = losing(state, t, self), us = losing(state, self, t);
        if(them < us + 0.1f && w.nation_get_willingness(t) > 0.3f) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Rejected: they expect to gain by fighting on (their losses %.2f, yours %.2f)", them, us);
            return done(false, buf);
        }
        w.nation_set_at_war(self, ix(t), 0.0f);
        w.nation_set_at_war(t, ix(self), 0.0f);
        w.nation_set_hostility(self, ix(t), 0.0f);
        w.nation_set_hostility(t, ix(self), 0.0f);
        w.nation_set_sanction(self, ix(t), 0.5f);
        w.nation_set_sanction(t, ix(self), 0.5f);
        const auto d = w.create_decision();
        w.decision_set_agent(d, q.actor);
        w.decision_set_target(d, self);
        w.decision_set_tick(d, state.current_tick);
        w.decision_set_lever(d, journal_peace);
        w.decision_set_before(d, 1.0f);
        w.decision_set_after(d, 0.0f);
        w.decision_set_pragmatic(d, static_cast<float>(t.index()));
        w.decision_set_ideological(d, NAN);
        w.decision_set_prior(d, NAN);
        return done(true, "Peace accepted");
    }
    case action_kind::send_aid: {
        if(!t || t == self) return done(false, "Choose another nation");
        if(w.nation_get_trust(t, ix(self)) < 0.2f) return done(false, "Refused: they distrust you");
        const double donor = w.nation_get_gdp0(self) * w.nation_get_core_price(self);
        const double cap = P.aid_recipient_cap * w.nation_get_gdp0(t) * w.nation_get_core_price(t) * 5.0; // one-off: up to five years' cap
        const double amount = std::min(std::clamp(static_cast<double>(q.value), 0.0, 5.0) / 100.0 * donor, cap);
        cause_scope c(state, journal_cause(self, journal_player_action));
        transfer(state, account::government(self), account::rest_of_world(self), amount, flow::aid);
        transfer(state, account::rest_of_world(t), account::government(t), amount, flow::aid);
        w.nation_set_trust(t, ix(self), std::min(1.0f, w.nation_get_trust(t, ix(self)) + 0.05f));
        return done(true, fmt("Aid of %.2f bn USD delivered", amount / 1e9));
    }
    case action_kind::propose_treaty: {
        if(!t || t == self) return done(false, "Choose another nation");
        const float trust = w.nation_get_trust(t, ix(self));
        switch(q.treaty) {
        case treaty_kind::conditional_aid:
            if(w.nation_get_gdp_pc0(t) >= static_cast<float>(P.aid_income)) return done(false, "Conditional aid is for poorer nations");
            if(has_treaty(state, treaty_kind::conditional_aid, self, t)) return done(false, "Already in force");
            if(trust < 0.3f) return done(false, "Rejected: they distrust you");
            sign_treaty(state, treaty_kind::conditional_aid, self, t, w.nation_get_liberal(t));
            return done(true, "Conditional aid agreed (suspended if they backslide)");
        case treaty_kind::migration:
            if(has_treaty(state, treaty_kind::migration, self, t)) return done(false, "Already in force");
            if(trust < 0.3f) return done(false, "Rejected: they distrust you");
            sign_treaty(state, treaty_kind::migration, self, t, 1.0f);
            return done(true, "Migration deal agreed: you pay them to control departures for four years");
        default: return done(false, "Not a treaty you can propose (debt relief follows six months of default; climate pledges are unilateral)");
        }
    }
    case action_kind::climate_pledge: {
        const float v = std::clamp(q.value, 0.0f, 1000.0f);
        w.nation_set_climate_pledge(self, v);
        dcon::treaty_id existing;
        for(auto tr : w.in_treaty) {
            if(tr.get_active() && tr.get_kind() == static_cast<uint8_t>(treaty_kind::climate) && tr.get_a() == self) existing = tr.id;
        }
        if(existing) w.treaty_set_terms(existing, v);
        else sign_treaty(state, treaty_kind::climate, self, {}, v);
        return done(true, fmt("Pledged %.0f USD/t by 2030 (breaking it costs credibility)", v));
    }
    case action_kind::apply_membership: {
        const auto o = q.organisation;
        if(!o || membership_in(state, o, self)) return done(false, "Not possible");
        // Members vote by their trust in the applicant (and, for alliances, a shared threat).
        int yes = 0, members = 0;
        dcon::nation_id blocker;
        for(auto m : w.organisation_get_membership_as_organisation(o)) {
            if(!membership_active(state, m.id)) continue;
            const auto mn = m.get_member().id;
            ++members;
            bool y = w.nation_get_trust(mn, ix(self)) >= static_cast<float>(P.accession_trust);
            if(w.organisation_get_kind(o) == static_cast<uint8_t>(org_kind::alliance)) y = y && w.nation_get_alliance(mn, ix(self)) >= 0.0f;
            if(y) ++yes;
            else blocker = mn;
        }
        const auto rule = static_cast<org_rule>(w.organisation_get_rule(o));
        const bool ok = members == 0 || (rule == org_rule::consensus || rule == org_rule::veto ? !blocker
                                                                                              : static_cast<float>(yes) / static_cast<float>(members) >= std::max(0.5f, w.organisation_get_threshold(o)));
        if(!ok) {
            return done(false, blocker && (rule == org_rule::consensus || rule == org_rule::veto)
                                   ? "Blocked by " + core::unpack_key(w.nation_get_iso3(blocker))
                                   : fmt("Rejected: %.0f%% of members in favour", 100.0 * yes / std::max(1, members)));
        }
        const auto m = w.force_create_membership(o, self);
        w.membership_set_role(m, static_cast<uint8_t>(org_role::member));
        w.membership_set_term_end(m, -1);
        w.membership_set_exit_tick(m, -1);
        w.membership_set_join_tick(m, state.current_tick);
        w.membership_set_suspended_until(m, -1);
        if(w.organisation_get_kind(o) == static_cast<uint8_t>(org_kind::alliance)) {
            for(auto mm : w.organisation_get_membership_as_organisation(o)) {
                const auto x = mm.get_member().id;
                if(x == self) continue;
                w.nation_set_alliance(x, ix(self), 1.0f);
                w.nation_set_alliance(self, ix(x), 1.0f);
            }
        }
        return done(true, "Admitted to " + core::unpack_key(w.organisation_get_key(o)));
    }
    case action_kind::leave_membership: {
        const auto m = membership_in(state, q.organisation, self);
        if(!m) return done(false, "Not a member");
        if(w.organisation_get_kind(q.organisation) == static_cast<uint8_t>(org_kind::alliance)) {
            for(auto mm : w.organisation_get_membership_as_organisation(q.organisation)) {
                const auto x = mm.get_member().id;
                if(x == self) continue;
                w.nation_set_alliance(x, ix(self), 0.0f);
                w.nation_set_alliance(self, ix(x), 0.0f);
            }
        }
        w.delete_membership(m); // effective at once
        return done(true, "Left " + core::unpack_key(w.organisation_get_key(q.organisation)));
    }
    case action_kind::offer_power_sharing: {
        const auto rebel = w.nation_get_rebel_agent(self);
        if(!rebel || w.agent_get_strength(rebel) <= 0.0f) return done(false, "No rebellion");
        const float s = w.agent_get_strength(rebel);
        if(stalemate_of(state, self) < 0.3f && s >= w.nation_get_state_capacity(self)) return done(false, "Rejected: the rebels expect to win");
        settle_rebels(state, self);
        return done(true, "Power-sharing settlement signed; the insurgency ends");
    }
    case action_kind::accept_settlement: {
        const auto n = self; // the rebels' nation
        const float s = w.agent_get_strength(q.actor);
        if(s <= 0.0f) return done(false, "The rebellion is over");
        // A player-controlled government decides itself; otherwise its rule: accept if weak or stalemated.
        if(stalemate_of(state, n) < 0.5f && s < w.nation_get_state_capacity(n)) return done(false, "Rejected: the government expects to prevail");
        settle_rebels(state, n);
        return done(true, "The government agreed to share power");
    }
    case action_kind::lobby:
    case action_kind::press:
    case action_kind::general_strike: {
        const auto target = q.kind == action_kind::press ? q.target_agent : w.nation_get_agent_from_executive_seat(self);
        if(!target || !w.agent_is_valid(target)) return done(false, "No one to press");
        if(!owns_lever(state, kind_of(state, target), q.lever)) return done(false, "Choose one of their levers");
        float weight = press_weight(state, q.actor, target);
        if(q.kind == action_kind::general_strike) {
            if(kind_of(state, q.actor) != agent_kind::union_federation) return done(false, "Only unions call strikes");
            // A general strike: twice the density's weight, mass protest and lost output.
            const float density = w.nation_get_union_density(self);
            weight = std::clamp(2.0f * density, 0.05f, 1.5f);
            mean_protest_drive(state, self, 0.1f * density);
            for(auto own : w.nation_get_region_ownership(self)) {
                auto r = own.get_region();
                r.set_unrest(r.get_unrest() + 0.5f * density);
            }
            w.nation_set_approval(self, std::max(0.0f, w.nation_get_approval(self) - 0.05f * density));
        }
        if(weight <= 0.0f) return done(false, "You have no leverage over them");
        put_pressure(state, target, q.actor, q.lever, q.value, weight);
        return done(true, fmt(q.kind == action_kind::general_strike ? "The strike presses the government (weight %.2f) for six months"
                                                                   : "They will weigh your case (weight %.2f) for six months",
                              weight));
    }
    case action_kind::replace_cb_governor: {
        const auto bank = q.target_agent;
        if(!bank || kind_of(state, bank) != agent_kind::central_bank || home_of(state, bank) != self) return done(false, "Not your central bank");
        const float cbi = w.nation_get_cb_independence(self);
        if(w.nation_get_judicial(self) * cbi > 0.45f) return done(false, "The courts uphold the governor's tenure");
        // A dependent bank leans toward the government's rate; investors price the lost
        // anchor (Binder 2021 on political pressure and inflation expectations).
        w.nation_set_cb_independence(self, std::max(0.0f, cbi - 0.2f));
        w.nation_set_inflation_expect(self, w.nation_get_inflation_expect(self) + 0.025f * std::min(cbi, 0.2f)); // up to +0.5 pp (a fraction)
        lose_credibility(state, self, 0.05f);
        return done(true, fmt("A loyal governor takes over: independence %.2f", w.nation_get_cb_independence(self)));
    }
    case action_kind::rescue_firm: {
        const auto f = firm_of(state, q.target_agent);
        if(!f || w.firm_get_nation(f) != self) return done(false, "Not a domestic firm");
        const double amount = std::clamp(static_cast<double>(q.value), 0.0, 3.0) / 100.0 * nominal_gdp(state, self);
        cause_scope c(state, journal_cause(self, journal_player_action));
        transfer(state, account::government(self), account::firms(self), amount, flow::fiscal_firms);
        const double daily = firm_daily(state, f);
        const float cash0 = w.firm_get_cash_days0(f);
        w.firm_set_cash_days(f, std::min(4.0f * cash0, w.firm_get_cash_days(f) + static_cast<float>(amount / daily)));
        // Cash for equity: the state's stake grows with the rescue relative to a year of value added.
        w.firm_set_soe_share(f, std::min(1.0f, w.firm_get_soe_share(f) + static_cast<float>(std::min(0.5, amount / (daily * 365.0)))));
        return done(true, fmt("%.2f bn USD injected; the state's stake rises", amount / 1e9));
    }
    case action_kind::social_pact: {
        const auto u = q.target_agent;
        if(!u || kind_of(state, u) != agent_kind::union_federation || home_of(state, u) != self) return done(false, "Not your union federation");
        const bool slack = w.nation_get_unemployment(self) > w.nation_get_unemployment0(self) + 0.01f;
        const bool coordinated = w.nation_get_coordination(self) >= 0.5f;
        float left = 0.0f;
        if(auto inc = w.nation_get_incumbent(self)) left = -w.party_get_position(inc, static_cast<uint8_t>(value_axis::economic_right));
        if(!slack && !coordinated && left < 0.1f) return done(false, "Rejected: the union sees nothing to gain");
        const uint8_t lev = lever_slot(state, lever::wage_push);
        put_pressure(state, u, q.actor, lev, std::min(0.0f, w.agent_get_last_choice(u, lev)), 1.0f, 365);
        w.nation_set_legitimacy(self, std::min(1.0f, w.nation_get_legitimacy(self) + 0.01f));
        return done(true, "Pact signed: wage moderation for a year");
    }
    case action_kind::restrict_unions: {
        if(!q.target_agent || kind_of(state, q.target_agent) != agent_kind::union_federation || home_of(state, q.target_agent) != self) return done(false, "Not your union federation");
        if(w.nation_get_judicial(self) * w.nation_get_liberal(self) > 0.4f) return done(false, "Struck down: the courts protect freedom of association");
        const float density = w.nation_get_union_density(self);
        w.nation_set_union_density(self, density * 0.85f);
        w.nation_set_bargaining_coverage(self, w.nation_get_bargaining_coverage(self) * 0.9f);
        w.nation_set_liberal(self, std::max(0.0f, w.nation_get_liberal(self) - 0.02f));
        mean_protest_drive(state, self, 0.1f * density);
        return done(true, fmt("Union density falls to %.2f; protests follow", w.nation_get_union_density(self)));
    }
    case action_kind::military_pay: {
        if(!q.target_agent || kind_of(state, q.target_agent) != agent_kind::military || home_of(state, q.target_agent) != self) return done(false, "Not your military");
        const float share = std::clamp(q.value, 0.0f, 2.0f);
        const double amount = share / 100.0 * nominal_gdp(state, self);
        cause_scope c(state, journal_cause(self, journal_player_action));
        transfer(state, account::government(self), account::firms(self), amount, flow::defense);
        // Coup-proofing by pay: spending per soldier buys loyalty (Powell 2012; Leon 2014).
        const float gain = std::min(0.3f, 0.3f * share);
        w.nation_set_military_loyalty(self, std::min(1.0f, w.nation_get_military_loyalty(self) + gain));
        return done(true, fmt("Loyalty rises to %.2f", w.nation_get_military_loyalty(self)));
    }
    case action_kind::purge_officers: {
        if(!q.target_agent || kind_of(state, q.target_agent) != agent_kind::military || home_of(state, q.target_agent) != self) return done(false, "Not your military");
        // Loyalists replace the competent: loyalty now, fighting power lost (Quinlivan 1999;
        // Talmadge 2015).
        w.nation_set_military_loyalty(self, std::max(w.nation_get_military_loyalty(self), 0.85f));
        w.nation_set_military(self, w.nation_get_military(self) * 0.85f);
        w.nation_set_willingness(self, w.nation_get_willingness(self) * 0.9f);
        return done(true, "Officer corps purged: loyal, and weaker");
    }
    case action_kind::joint_exercises: {
        const auto there = home_of(state, q.target_agent);
        if(!there || there == self || kind_of(state, q.target_agent) != agent_kind::military) return done(false, "Choose a foreign military");
        if(at_war(state, self, there) || w.nation_get_hostility(self, ix(there)) >= 2.0f) return done(false, "Not with a hostile nation");
        if(w.nation_get_trust(there, ix(self)) < 0.4f) return done(false, "Declined: they distrust you");
        w.nation_set_trust(there, ix(self), std::min(1.0f, w.nation_get_trust(there, ix(self)) + 0.05f));
        w.nation_set_trust(self, ix(there), std::min(1.0f, w.nation_get_trust(self, ix(there)) + 0.05f));
        // Their rivals read it as a signal.
        for(auto x : w.in_nation) {
            if(x.id == self || x.id == there) continue;
            if(w.nation_get_rivalry(x.id, ix(there)) > 0.3f) x.set_threat_to(ix(self), std::min(1.0f, x.get_threat_to(ix(self)) + 0.02f));
        }
        return done(true, "Exercises held: mutual trust grows; their rivals take note");
    }
    case action_kind::redraw_state: {
        const auto verdict = check_redraw(state, q.actor, q.region, q.state_to);
        if(!verdict.ok) return done(false, verdict.reason);
        const auto r = q.region;
        const auto from = w.region_get_state(r);
        auto to = q.state_to;
        if(!to) { // a new state, with a governor of its own
            to = w.create_state();
            w.state_set_nation(to, self);
            w.state_set_row(to, -1);
            if(from && w.state_get_governor(from)) {
                const auto g = w.create_agent();
                const auto g0 = w.state_get_governor(from);
                w.agent_set_kind(g, static_cast<uint8_t>(agent_kind::governor));
                w.agent_set_home(g, self);
                w.agent_set_order_ideal(g, w.agent_get_order_ideal(g0));
                w.agent_set_rho_base(g, w.agent_get_rho_base(g0));
                w.agent_set_rho(g, w.agent_get_rho_base(g0));
                w.state_set_governor(to, g);
            }
        }
        w.region_set_state(r, to);
        w.region_set_governor(r, w.state_get_governor(to));
        w.state_set_last_redrawn(to, state.current_tick);
        if(from) w.state_set_last_redrawn(from, state.current_tick);
        for(dcon::state_id st : std::array<dcon::state_id, 2>{from, to}) {
            if(!st) continue;
            float p = 0.0f;
            for(auto rr : w.in_region) {
                if(rr.get_state() == st) p += rr.get_population();
            }
            w.state_set_population(st, p);
        }
        // In a democracy, redrawing for advantage costs some legitimacy (gerrymandering is
        // unpopular: Norris 2017 electoral integrity), and the moved region resents it.
        const float liberal = w.nation_get_liberal(self);
        w.nation_set_legitimacy(self, std::max(0.0f, w.nation_get_legitimacy(self) - 0.01f * liberal));
        w.region_set_unrest(r, w.region_get_unrest(r) + 0.2f * liberal);
        return done(true, verdict.reason);
    }
    case action_kind::regional_transfer: {
        const auto r = region_of(state, q.target_agent);
        if(!r || w.region_get_owner(r) != self) return done(false, "Not one of your regions");
        const double amount = std::clamp(static_cast<double>(q.value), 0.0, 1.0) / 100.0 * nominal_gdp(state, self);
        cause_scope c(state, journal_cause(self, journal_player_action));
        transfer(state, account::government(self), account::firms(self), amount, flow::public_investment);
        // Public capital in the region (its capacity) and goodwill.
        double va = 0.0;
        for(uint8_t s = 0; s < state.derived.sector_count; ++s) va += w.region_get_va0(r, s);
        const float boost = va > 0.0 ? static_cast<float>(std::min(0.05, 0.1 * amount / (va * 365.0 * w.nation_get_core_price(self)))) : 0.0f;
        for(uint8_t s = 0; s < state.derived.sector_count; ++s) w.region_set_capacity(r, s, w.region_get_capacity(r, s) * (1.0f + boost));
        w.region_set_unrest(r, w.region_get_unrest(r) * 0.8f);
        w.region_set_trust_r(r, std::min(1.0f, w.region_get_trust_r(r) + 0.02f));
        return done(true, fmt("%.2f bn USD invested in the region", amount / 1e9));
    }
    case action_kind::counterinsurgency: {
        const auto rebel = q.target_agent;
        if(!rebel || kind_of(state, rebel) != agent_kind::rebel || home_of(state, rebel) != self) return done(false, "Not your rebels");
        const float s = w.agent_get_strength(rebel);
        if(s <= 0.0f) return done(false, "No rebellion");
        const float loyalty = w.nation_get_military_loyalty(self);
        if(loyalty < 0.3f) return done(false, "Refused: the army will not fight");
        // Force against insurgents works with capacity and costs grievance where it falls
        // (Lyall & Wilson 2009 on mechanised counter-insurgency).
        const float hit = 0.2f * w.nation_get_state_capacity(self) * loyalty;
        w.agent_set_strength(rebel, std::max(0.0f, s - hit));
        for(auto own : w.nation_get_region_ownership(self)) {
            auto r = own.get_region();
            if(r.get_insurgency() > 0.0f) r.set_unrest(r.get_unrest() + 0.3f);
        }
        if(w.agent_get_strength(rebel) < 0.05f) {
            rebels_defeated(state, self);
            return done(true, "The rebellion is crushed");
        }
        return done(true, fmt("Rebel strength falls to %.2f", w.agent_get_strength(rebel)));
    }
    case action_kind::arm_rebels: {
        const auto rebel = q.target_agent;
        const auto there = home_of(state, rebel);
        if(!rebel || kind_of(state, rebel) != agent_kind::rebel || !there || there == self) return done(false, "Choose a foreign rebellion");
        if(w.agent_get_strength(rebel) <= 0.0f) return done(false, "No active rebellion");
        const float gain = 0.1f * std::min(1.0f, 10.0f * w.nation_get_cinc(self));
        w.agent_set_strength(rebel, std::min(1.0f, w.agent_get_strength(rebel) + gain));
        w.nation_set_trust(there, ix(self), std::max(0.0f, w.nation_get_trust(there, ix(self)) - 0.2f));
        w.nation_set_rivalry(there, ix(self), std::min(1.0f, w.nation_get_rivalry(there, ix(self)) + 0.2f));
        w.nation_set_rivalry(self, ix(there), w.nation_get_rivalry(there, ix(self)));
        return done(true, fmt("Arms delivered: rebel strength %.2f", w.agent_get_strength(rebel)));
    }
    case action_kind::swap_line: {
        const auto there = home_of(state, q.target_agent);
        if(!there || there == self || kind_of(state, q.target_agent) != agent_kind::central_bank) return done(false, "Choose a foreign central bank");
        const float months = std::clamp(q.value, 0.0f, 6.0f);
        if(lends_reserves(state, self)) {
            // Offering: they draw on it if they need it.
            w.nation_set_reserves(there, w.nation_get_reserves(there) + months);
            w.nation_set_trust(there, ix(self), std::min(1.0f, w.nation_get_trust(there, ix(self)) + 0.05f));
            return done(true, fmt("Swap line opened: their reserves rise to %.1f months", w.nation_get_reserves(there)));
        }
        if(!lends_reserves(state, there)) return done(false, "They hold no reserves to lend");
        if(w.nation_get_trust(there, ix(self)) < 0.5f && w.nation_get_alliance(there, ix(self)) <= 0.0f) return done(false, "Declined: they do not trust you enough");
        w.nation_set_reserves(self, w.nation_get_reserves(self) + months);
        return done(true, fmt("Swap line granted: reserves %.1f months", w.nation_get_reserves(self)));
    }
    case action_kind::emergency_liquidity: {
        const auto tk = kind_of(state, q.target_agent);
        const double amount = std::clamp(static_cast<double>(q.value), 0.0, 3.0) / 100.0 * nominal_gdp(state, self);
        cause_scope c(state, journal_cause(self, journal_player_action));
        if(tk == agent_kind::board) {
            const auto f = firm_of(state, q.target_agent);
            if(!f || w.firm_get_nation(f) != self) return done(false, "Not a domestic firm");
            // A loan, not a gift: cash now, more leverage.
            transfer(state, account::finance(self), account::firms(self), amount, flow::credit);
            const double daily = firm_daily(state, f);
            w.firm_set_cash_days(f, std::min(4.0f * w.firm_get_cash_days0(f), w.firm_get_cash_days(f) + static_cast<float>(amount / daily)));
            w.firm_set_leverage(f, w.firm_get_leverage(f) + static_cast<float>(amount / (daily * 365.0)));
            return done(true, fmt("%.2f bn USD lent to the firm", amount / 1e9));
        }
        const auto r = region_of(state, q.target_agent);
        if(!r || w.region_get_owner(r) != self) return done(false, "Not one of your regions");
        transfer(state, account::finance(self), account::government(self), amount, flow::credit);
        w.region_set_fiscal_capacity(r, std::min(2.0f, w.region_get_fiscal_capacity(r) + 0.05f));
        return done(true, fmt("%.2f bn USD of the region's debt bought", amount / 1e9));
    }
    case action_kind::freeze_rebel_funds: {
        const auto rebel = q.target_agent;
        if(!rebel || kind_of(state, rebel) != agent_kind::rebel || home_of(state, rebel) != self) return done(false, "Not your rebels");
        if(w.agent_get_strength(rebel) <= 0.0f) return done(false, "No rebellion");
        if(w.nation_get_state_capacity(self) < 0.4f) return done(false, "The state cannot enforce it");
        w.agent_set_strength(rebel, std::max(0.0f, w.agent_get_strength(rebel) - 0.05f * w.nation_get_state_capacity(self)));
        return done(true, fmt("Rebel strength falls to %.2f", w.agent_get_strength(rebel)));
    }
    case action_kind::strike: {
        const auto f = firm_of(state, q.target_agent);
        if(!f || w.firm_get_nation(f) != self) return done(false, "Not a domestic firm");
        const float density = w.nation_get_union_density(self), coverage = w.nation_get_bargaining_coverage(self);
        // Days of output lost scale with organisation; the firm concedes job guarantees
        // where the union is strong (Kennan 1986 on strike costs).
        const float days = 10.0f * density;
        w.firm_set_cash_days(f, w.firm_get_cash_days(f) - days);
        if(density * (0.5f + coverage) < 0.15f) return done(false, fmt("The strike fizzles (density %.2f): the firm holds out", density));
        put_pressure(state, q.target_agent, q.actor, lever_slot(state, lever::hoarding), 1.0f, std::clamp(density + coverage, 0.1f, 1.5f));
        return done(true, fmt("The firm concedes job guarantees after %.0f days of lost output", days));
    }
    case action_kind::wage_deal: {
        const auto u = q.target_agent;
        if(!u || kind_of(state, u) != agent_kind::union_federation || home_of(state, u) != self) return done(false, "Not your union federation");
        const auto f = firm_of(state, q.actor);
        const bool slack = w.nation_get_unemployment(self) > w.nation_get_unemployment0(self) + 0.01f;
        const bool distressed = f && w.firm_get_cash_days(f) < 0.5f * w.firm_get_cash_days0(f);
        if(!slack && !distressed) return done(false, "Rejected: jobs are not at risk");
        const uint8_t lw = lever_slot(state, lever::wage_push);
        put_pressure(state, u, q.actor, lw, std::min(0.0f, w.agent_get_last_choice(u, lw)), std::clamp(0.5f * board_weight(state, q.actor, self), 0.05f, 1.0f), 365);
        // The board's own commitment: keep staff.
        put_pressure(state, q.actor, u, lever_slot(state, lever::hoarding), 0.9f, 1.0f, 365);
        return done(true, "Deal: wage restraint for job guarantees, for a year");
    }
    case action_kind::rights_complaint: {
        const auto o = q.organisation;
        if(!o || w.organisation_get_kind(o) != static_cast<uint8_t>(org_kind::rights)) return done(false, "Only rights bodies hear complaints");
        const bool repressed = w.nation_get_repression(self) >= 0.25f || w.nation_get_liberal(self) < w.nation_get_liberal0(self) - 0.05f;
        if(!repressed) return done(false, "Dismissed: no case to answer");
        w.nation_set_shamed(self, 1.0f);
        w.nation_set_legitimacy(self, std::max(0.0f, w.nation_get_legitimacy(self) - 0.02f));
        return done(true, "Upheld: your government is named and shamed");
    }
    case action_kind::seize_power: {
        if(kind_of(state, q.actor) != agent_kind::military) return done(false, "Only the military can");
        // Coups succeed against illegitimate governments, are deterred by democracy clauses,
        // and fail where the regime is popular or protest backs it (Powell 2012; Powell &
        // Thyne 2011; Singh 2014 on coordination among officers).
        float protest = 0.0f, pop = 0.0f;
        for(auto own : w.nation_get_region_ownership(self)) {
            protest += own.get_region().get_protest() * own.get_region().get_population();
            pop += own.get_region().get_population();
        }
        protest = pop > 0.0f ? protest / pop : 0.0f;
        const float odds = (1.0f - w.nation_get_legitimacy(self)) * (1.0f - coup_deterrence(state, self)) * (1.0f - 0.5f * w.nation_get_electoral(self)) + protest;
        if(odds < 0.5f) {
            // A failed coup: officers purged, the government rallies support.
            w.nation_set_military_loyalty(self, 0.9f);
            w.nation_set_military(self, w.nation_get_military(self) * 0.8f);
            w.nation_set_legitimacy(self, std::min(1.0f, w.nation_get_legitimacy(self) + 0.05f));
            return done(false, fmt("The coup fails (odds %.2f): the plotters are purged", odds));
        }
        apply_coup(state, self);
        w.nation_set_military_loyalty(self, 1.0f);
        return done(true, "The military takes power");
    }
    case action_kind::demand_autonomy: {
        if(kind_of(state, q.actor) != agent_kind::governor) return done(false, "Only governors can");
        const auto r = region_of(state, q.actor);
        const bool needs = w.nation_get_gov_seat_share(self) < 0.5f || w.nation_get_legitimacy(self) < 0.4f || (r && w.region_get_unrest(r) > 2.0f);
        if(!needs) return done(false, "Refused: the government does not need your support");
        w.nation_set_decentralisation(self, std::min(1.0f, w.nation_get_decentralisation(self) + 0.05f));
        return done(true, fmt("Granted: decentralisation %.2f", w.nation_get_decentralisation(self)));
    }
    case action_kind::foreign_backing: {
        const auto t = q.target;
        if(!t || t == self) return done(false, "Choose a foreign government");
        if(w.agent_get_strength(q.actor) <= 0.0f) return done(false, "The rebellion is over");
        const bool hostile = w.nation_get_rivalry(t, ix(self)) >= 0.3f || w.nation_get_trust(t, ix(self)) < 0.2f || at_war(state, t, self);
        if(!hostile || w.nation_get_alliance(t, ix(self)) > 0.0f) return done(false, "Refused: they are on good terms with your government");
        const float gain = 0.1f * std::min(1.0f, 10.0f * w.nation_get_cinc(t));
        w.agent_set_strength(q.actor, std::min(1.0f, w.agent_get_strength(q.actor) + gain));
        w.nation_set_trust(self, ix(t), std::max(0.0f, w.nation_get_trust(self, ix(t)) - 0.1f));
        return done(true, fmt("Backed: strength %.2f", w.agent_get_strength(q.actor)));
    }
    case action_kind::court_defectors: {
        if(w.agent_get_strength(q.actor) <= 0.0f) return done(false, "The rebellion is over");
        const float loyalty = w.nation_get_military_loyalty(self);
        if(loyalty >= 0.5f) return done(false, "No takers: the army is loyal");
        w.nation_set_military_loyalty(self, std::max(0.0f, loyalty - 0.1f));
        w.agent_set_strength(q.actor, std::min(1.0f, w.agent_get_strength(q.actor) + 0.05f));
        return done(true, fmt("Units defect: army loyalty %.2f", w.nation_get_military_loyalty(self)));
    }
    default: break;
    }
    return done(false, "Unknown action");
}

} // namespace geosim::sim
