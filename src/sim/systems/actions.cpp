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

std::string fmt(char const* f, double x) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, x);
    return buf;
}

} // namespace

std::vector<action_option> available_actions(game_state const& state, dcon::agent_id actor, dcon::nation_id target, dcon::organisation_id organisation) {
    auto const& w = state.world;
    std::vector<action_option> out;
    if(!actor) return out;
    const auto kind = static_cast<agent_kind>(w.agent_get_kind(actor));
    const auto self = home_of(state, actor);
    auto add = [&](action_kind k, char const* label, bool ok, std::string reason) { out.push_back({k, label, ok, std::move(reason)}); };
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
        } else {
            add(action_kind::climate_pledge, "Pledge a 2030 carbon price", true, fmt("current pledge %.0f USD/t", w.nation_get_climate_pledge(self)));
            const auto rebel = w.nation_get_rebel_agent(self);
            const bool rebels = rebel && w.agent_get_strength(rebel) > 0.0f;
            add(action_kind::offer_power_sharing, "Offer the rebels power sharing", rebels,
                rebels ? fmt("stalemate %.2f (accepted above 0.3 or if they are weaker than the state)", stalemate_of(state, self)) : "no rebellion");
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
        add(action_kind::accept_settlement, "Seek a settlement", active, active ? "the government accepts if it is weak or the war is a stalemate" : "the rebellion is over");
        break;
    }
    case agent_kind::board:
    case agent_kind::union_federation:
        add(action_kind::lobby, "Lobby the government", bool(self), "press for a value on one lever (advice weighed by your economic weight)");
        break;
    default: break;
    }
    return out;
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
        journal_action(state, q.actor, self, t, q.kind, ok ? 1.0f : 0.0f);
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
    case action_kind::lobby: {
        if(q.lever >= state.derived.lever_count) return done(false, "Choose a lever");
        // Weight: a board by its firm's share of national output, a union by its density.
        float weight = 0.0f;
        const auto kind = static_cast<agent_kind>(w.agent_get_kind(q.actor));
        if(kind == agent_kind::union_federation) weight = w.nation_get_union_density(self);
        else {
            double firm = 0.0, total = 0.0;
            for(auto f : w.in_firm) {
                if(f.get_nation() != self) continue;
                total += f.get_share();
                if(f.get_board() == q.actor) firm += f.get_share();
            }
            weight = total > 0.0 ? static_cast<float>(firm / total) * 5.0f : 0.1f;
        }
        weight = std::clamp(weight, 0.02f, 1.0f);
        w.nation_set_lobby_lever(self, q.lever);
        w.nation_set_lobby_value(self, q.value);
        w.nation_set_lobby_weight(self, weight);
        w.nation_set_lobby_until(self, state.current_tick + 182);
        return done(true, fmt("The government will weigh your case (weight %.2f) for six months", weight));
    }
    default: break;
    }
    return done(false, "Unknown action");
}

} // namespace geosim::sim
