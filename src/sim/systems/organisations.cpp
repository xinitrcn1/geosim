// International organisations (docs/06-geopolitics-ai.md#international-organisations).
//
// Generic machinery. Every organisation (defs/organisations.txt) has members
// (`membership`: role member / elected / permanent, term end, exit date) and a
// decision rule: majority, veto (votes needed and no permanent member against),
// qualified majority (share of states and of their population) or consensus. Seats
// held by nations outside the scenario vote as the rest of the world. Elected seats
// rotate when terms end, larger states winning more often (Dreher et al. 2014).
// A member votes for acting against a target by its alignment with the drafter
// minus its ties to the target (alliance, trust); its own case it always opposes.
//
// Behaviour by kind:
//  - assembly (UN General Assembly): each September members vote on a set of
//    resolutions placed along the main dimension of UN voting, the US-led liberal
//    order (Bailey, Strezhnev & Voeten 2017); each nation's ideal point follows its
//    liberal institutions, self-expression values and alliance with the leading
//    liberal power; pairwise agreement (Signorino & Ritter 1999) relative to 2019
//    shifts trust and weights aid.
//  - security (UN Security Council): wars and insurgencies put drafts on the agenda:
//    a ceasefire demand (raises the settlement hazard; Fortna 2004), sanctions on an
//    occupier (every non-allied state applies at least `un_sanction_level`; Bapat &
//    Morgan 2009) and a peace operation after war or insurgency (cuts recurrence;
//    Fortna 2008; Hultman, Kathman & Shannon 2014); permanent members veto to shield
//    clients (Voeten 2001). Elected members draw more aid (Kuziemko & Werker 2006).
//  - trade (WTO): complaints against tariff rises above `wto_bound`, panel reports,
//    compliance or authorised retaliation; with the Appellate Body paralysed the
//    loser appeals into the void (Bown 2009; Hoekman & Mavroidis 2021).
//  - union (EU): members share one external tariff (customs union, firms.cpp) and,
//    when the union-wide output gap is deep, adopt by qualified majority a common
//    recovery fund financed by common debt and allocated by output loss
//    (NextGenerationEU 2020); exit dates end membership (the UK on 2020-01-31).
//  - rights (UN Human Rights Council): members vote to name and shame states whose
//    repression is high; the target loses legitimacy and others' trust
//    (Hafner-Burton 2008; Lebovic & Voeten 2006 on the politics of shaming).
//  - health (WHO): members' surveillance sharing raises the detection that
//    transparent states see (ai.cpp; IHR 2005).

#include "core/calendar.hpp"
#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/scenario.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
constexpr uint8_t ga_resolutions = 10;

uint16_t ix(dcon::nation_id n) { return static_cast<uint16_t>(n.index()); }

dcon::nation_id rest_of_world(game_state const& state) { return find_nation(state, "ROW"); }

void log_org(game_state& state, dcon::nation_id actor, dcon::nation_id target, uint8_t code, float before, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, actor ? w.nation_get_agent_from_executive_seat(actor) : dcon::agent_id{});
    w.decision_set_target(d, actor ? actor : target);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, code);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, target ? static_cast<float>(target.index()) : NAN);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

// Support of voter v for acting against target t on a draft by sponsor s.
float support(game_state const& state, dcon::nation_id v, dcon::nation_id s, dcon::nation_id t) {
    auto const& w = state.world;
    if(v == t) return -1.0f;
    const float with_sponsor = s ? un_alignment(state, v, s) : 0.5f;
    const float ties = t ? std::max(w.nation_get_alliance(v, ix(t)), w.nation_get_trust(v, ix(t))) : 0.0f;
    return with_sponsor - ties;
}
} // namespace

dcon::organisation_id organisation_of(game_state const& state, org_kind k) {
    return state.derived.org[static_cast<uint8_t>(k)];
}

dcon::membership_id membership_in(game_state const& state, dcon::organisation_id o, dcon::nation_id n) {
    if(!o || !n) return {};
    for(auto m : state.world.nation_get_membership_as_member(n)) {
        if(m.get_organisation() == o) return m.id;
    }
    return {};
}

bool is_member(game_state const& state, org_kind k, dcon::nation_id n) {
    return bool(membership_in(state, organisation_of(state, k), n));
}

int org_role_of(game_state const& state, org_kind k, dcon::nation_id n) {
    const auto m = membership_in(state, organisation_of(state, k), n);
    return m ? static_cast<int>(state.world.membership_get_role(m)) : -1;
}

float org_vote(game_state& state, dcon::organisation_id o, dcon::nation_id sponsor, dcon::nation_id target, float shift, uint8_t code, float kind_tag) {
    auto& w = state.world;
    const auto rule = static_cast<org_rule>(w.organisation_get_rule(o));
    int yes = 0, members = 0;
    double pop_yes = 0.0, pop_all = 0.0;
    bool veto = false;
    for(auto m : w.organisation_get_membership_as_organisation(o)) {
        const auto v = m.get_member().id;
        const float s = support(state, v, sponsor, target) + shift;
        const bool aye = s > 0.0f;
        ++members;
        yes += aye ? 1 : 0;
        pop_all += w.nation_get_population(v);
        pop_yes += aye ? w.nation_get_population(v) : 0.0;
        if(rule == org_rule::veto && m.get_role() == static_cast<uint8_t>(org_role::permanent) && s < static_cast<float>(state.params.unsc_veto_threshold)) veto = true;
        if(rule == org_rule::consensus && !aye) veto = true;
    }
    // Seats held outside the scenario vote as the rest of the world.
    const int seats = w.organisation_get_seats(o);
    if(seats > members) {
        const auto row = rest_of_world(state);
        const int outside = seats - members;
        if(row && support(state, row, sponsor, target) + shift > 0.0f) yes += outside;
        members = seats;
    }
    bool passed = false;
    switch(rule) {
    case org_rule::majority: passed = members > 0 && yes >= w.organisation_get_threshold(o) * members; break;
    case org_rule::veto: passed = !veto && yes >= w.organisation_get_votes_needed(o); break;
    case org_rule::qualified:
        passed = members > 0 && yes >= w.organisation_get_threshold(o) * members && pop_all > 0.0
                 && pop_yes >= w.organisation_get_population_threshold(o) * pop_all;
        break;
    case org_rule::consensus: passed = !veto && members > 0; break;
    }
    const float result = veto ? 0.0f : (passed ? 1.0f : -1.0f);
    if(code != 0) log_org(state, sponsor, target, code, kind_tag, result);
    return result;
}

float un_alignment(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    auto const& w = state.world;
    // Agreement on the last General Assembly session when it was held, else ideal points.
    const auto ga = organisation_of(state, org_kind::assembly);
    if(ga && w.organisation_get_last_session(ga) >= 0 && w.nation_get_un_votes_size() >= ga_resolutions) {
        int same = 0;
        for(uint8_t k = 0; k < ga_resolutions; ++k) same += w.nation_get_un_votes(a, k) == w.nation_get_un_votes(b, k) ? 1 : 0;
        return static_cast<float>(same) / ga_resolutions;
    }
    return 1.0f - std::min(1.0f, std::abs(w.nation_get_un_ideal(a) - w.nation_get_un_ideal(b)) / 2.0f);
}

void compute_un_ideals(game_state& state) {
    auto& w = state.world;
    const auto row = rest_of_world(state);
    dcon::nation_id leader; // the leading liberal power: the largest liberal economy
    for(auto n : w.in_nation) {
        if(n.id == row || n.get_liberal() < 0.7f) continue;
        if(!leader || n.get_gdp0() > w.nation_get_gdp0(leader)) leader = n.id;
    }
    const auto se = static_cast<uint8_t>(value_axis::self_expression);
    for(auto n : w.in_nation) {
        const float allied = leader && n.id != leader ? w.nation_get_alliance(n.id, ix(leader)) : (n.id == leader ? 1.0f : 0.0f);
        n.set_un_ideal(std::clamp(2.0f * n.get_liberal() + 0.8f * n.get_window_center(se) + 0.6f * allied - 1.2f, -1.0f, 1.0f));
    }
}

// One General Assembly session: resolutions at positions spread along the
// dimension; a member votes yes when its ideal point lies beyond the position.
void hold_assembly_session(game_state& state, dcon::organisation_id ga) {
    auto& w = state.world;
    compute_un_ideals(state);
    const auto year = static_cast<uint32_t>(core::date_from_tick(state.current_tick).year);
    for(uint8_t k = 0; k < ga_resolutions; ++k) {
        const float pos = -0.9f + 1.8f * core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::elections), year, 0x6A000000u + k);
        for(auto m : w.organisation_get_membership_as_organisation(ga)) {
            w.nation_set_un_votes(m.get_member(), k, m.get_member().get_un_ideal() > pos ? 1.0f : 0.0f);
        }
    }
    w.organisation_set_last_session(ga, state.current_tick);
}

void init_organisations(game_state& state) {
    auto& w = state.world;
    w.nation_resize_un_votes(ga_resolutions);
    if(const auto ga = organisation_of(state, org_kind::assembly)) hold_assembly_session(state, ga);
    else compute_un_ideals(state);
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) a.set_un_alignment0(ix(b.id), un_alignment(state, a.id, b.id));
    }
}

float un_sanction_floor(game_state const& state, dcon::nation_id n, dcon::nation_id target) {
    auto const& w = state.world;
    const float un = w.nation_get_un_sanctioned(target) > 0.0f && w.nation_get_alliance(n, ix(target)) <= 0.0f
                         ? static_cast<float>(state.params.un_sanction_level) : 0.0f;
    return std::max(un, w.nation_get_wto_retaliation(n, ix(target)));
}

float peacekeeping_factor(game_state const& state, dcon::nation_id n, dcon::nation_id m) {
    auto const& w = state.world;
    const bool pk = w.nation_get_peacekeeping(n) > 0.0f || (m && w.nation_get_peacekeeping(m) > 0.0f);
    return pk ? 1.0f - static_cast<float>(state.params.peacekeeping_effect) : 1.0f;
}

namespace {

// Exits, expiring elected terms and elections (1 January).
void update_memberships(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    std::vector<dcon::membership_id> gone;
    for(auto m : w.in_membership) {
        if(m.get_exit_tick() >= 0 && m.get_exit_tick() <= state.current_tick) gone.push_back(m.id);
        else if(m.get_role() == static_cast<uint8_t>(org_role::elected) && m.get_term_end() >= 0 && m.get_term_end() < state.current_tick) gone.push_back(m.id);
    }
    for(auto m : gone) w.delete_membership(m);
    const auto date = core::date_from_tick(state.current_tick);
    if(!(date.month == 1 && date.day == 1) || state.current_tick == 0) return;
    const auto row = rest_of_world(state);
    double gdp_max = 0.0;
    for(auto n : w.in_nation) gdp_max = std::max(gdp_max, n.get_gdp0());
    for(auto o : w.in_organisation) {
        const int elected = o.get_elected_seats();
        if(elected <= 0 || o.get_term_years() <= 0.0f) continue;
        int held = 0;
        for(auto m : o.get_membership_as_organisation()) held += m.get_role() == static_cast<uint8_t>(org_role::elected) ? 1 : 0;
        // Each scenario nation competes for the seats coming free this year.
        const float rate = static_cast<float>(P.unsc_election_rate) * static_cast<float>(elected) / 10.0f;
        for(auto n : w.in_nation) {
            if(n.id == row || held >= elected || membership_in(state, o.id, n.id)) continue;
            const float size = static_cast<float>(1.0 + std::log1p(9.0 * n.get_gdp0() / std::max(1.0, gdp_max)));
            if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::conflict), static_cast<uint32_t>(state.current_tick),
                               0x7C000000u + ix(n.id) * 64u + static_cast<uint32_t>(o.id.index())) < rate * size) {
                const auto m = w.force_create_membership(o.id, n.id);
                w.membership_set_role(m, static_cast<uint8_t>(org_role::elected));
                w.membership_set_term_end(m, state.current_tick + static_cast<int32_t>(o.get_term_years() * 365.0f) - 1);
                w.membership_set_exit_tick(m, -1);
                ++held;
            }
        }
    }
}

// The drafter of a security resolution: the permanent member least tied to the target.
dcon::nation_id drafter(game_state const& state, dcon::organisation_id sc, dcon::nation_id target) {
    auto const& w = state.world;
    dcon::nation_id best;
    float lowest = 2.0f;
    for(auto m : w.organisation_get_membership_as_organisation(sc)) {
        const auto v = m.get_member().id;
        if(m.get_role() != static_cast<uint8_t>(org_role::permanent) || v == target) continue;
        const float ties = std::max(w.nation_get_alliance(v, ix(target)), w.nation_get_trust(v, ix(target)));
        if(ties < lowest) {
            lowest = ties;
            best = v;
        }
    }
    return best;
}

void security_council(game_state& state, dcon::organisation_id sc) {
    auto& w = state.world;
    auto const& P = state.params;
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        n.set_peacekeeping(std::max(0.0f, n.get_peacekeeping() - years));
        float occupying = 0.0f;
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            occupying = std::max(occupying, m.get_control(ix(n.id)));
            // Ceasefire demand, once per war (hostility 5 marks a fresh war).
            if(n.id.index() < m.id.index() && n.get_at_war(ix(m.id)) > 0.0f && n.get_hostility(ix(m.id)) == 5.0f) {
                const float h = org_vote(state, sc, drafter(state, sc, n.id), n.id, 0.3f, journal_unsc, 0.0f) > 0.0f ? 5.5f : 5.25f;
                n.set_hostility(ix(m.id), h); // 5.5: ceasefire demanded (read by geopolitics.cpp)
                m.set_hostility(ix(n.id), h);
            }
        }
        if(occupying > 0.05f && n.get_un_sanctioned() <= 0.0f) {
            if(org_vote(state, sc, drafter(state, sc, n.id), n.id, 0.0f, journal_unsc, 1.0f) > 0.0f) n.set_un_sanctioned(1.0f);
        } else if(occupying <= 0.0f && n.get_un_sanctioned() > 0.0f) {
            n.set_un_sanctioned(0.0f);
        }
        float insurgent = 0.0f;
        for(auto own : n.get_region_ownership()) insurgent = std::max(insurgent, own.get_region().get_insurgency());
        const bool post_war = n.get_war_cost() > 0.0f && n.get_war_effort() <= 0.0f;
        const bool permanent = org_role_of(state, org_kind::security, n.id) == static_cast<int>(org_role::permanent);
        if((insurgent > 0.0f || post_war) && n.get_peacekeeping() <= 0.0f && !permanent) {
            const auto d = drafter(state, sc, n.id);
            // A peace operation needs host consent: drafted with a high positive shift.
            if(d && org_vote(state, sc, d, n.id, 0.8f, journal_unsc, 2.0f) > 0.0f) n.set_peacekeeping(static_cast<float>(P.peacekeeping_years));
            if(post_war) n.set_war_cost(0.0f);
        }
    }
}

void human_rights_council(game_state& state, dcon::organisation_id hrc) {
    auto& w = state.world;
    auto const& P = state.params;
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        n.set_shamed(std::max(0.0f, n.get_shamed() - years));
        if(n.get_shamed() > 0.0f) {
            n.set_legitimacy(std::max(0.0f, n.get_legitimacy() - static_cast<float>(P.shaming_legitimacy) * years));
            continue;
        }
        if(n.get_repression() < 0.5f) continue;
        // The most liberal member drafts; votes as in the Security Council.
        dcon::nation_id sponsor;
        for(auto m : w.organisation_get_membership_as_organisation(hrc)) {
            if(!sponsor || m.get_member().get_liberal() > w.nation_get_liberal(sponsor)) sponsor = m.get_member().id;
        }
        if(sponsor && org_vote(state, hrc, sponsor, n.id, -0.2f, journal_unsc, 3.0f) > 0.0f) {
            n.set_shamed(1.0f);
            for(auto m : w.in_nation) {
                if(m.id != n.id && m.get_liberal() > 0.6f) m.set_trust(ix(n.id), std::max(0.0f, m.get_trust(ix(n.id)) - 0.05f));
            }
        }
    }
}

void economic_union(game_state& state, dcon::organisation_id eu) {
    auto& w = state.world;
    auto const& P = state.params;
    // Disbursement of committed grants, financed by common borrowing from the rest of the world.
    for(auto m : w.organisation_get_membership_as_organisation(eu)) {
        const auto v = m.get_member().id;
        const double pending = w.nation_get_eu_grant_pending(v);
        if(pending <= 0.0 || state.current_tick < w.nation_get_eu_grant_start(v)) continue;
        const double pay = std::min(pending, pending * 28.0 / (365.0 * std::max(0.5, P.eu_fund_years))
                                                 + 1e-9 * pending);
        transfer(state, account::rest_of_world(v), account::government(v), pay, flow::eu_fund);
        w.nation_set_eu_grant_pending(v, pending - pay);
    }
    // Recovery fund: when the union-wide output gap is deep and no fund is running.
    if(w.organisation_get_fund(eu) > 0.0f) {
        w.organisation_set_fund(eu, std::max(0.0f, w.organisation_get_fund(eu) - 28.0f / 365.0f));
        return;
    }
    double gap = 0.0, gdp = 0.0;
    for(auto m : w.organisation_get_membership_as_organisation(eu)) {
        gap += m.get_member().get_gdp0() * m.get_member().get_gap_smoothed();
        gdp += m.get_member().get_gdp0();
    }
    if(gdp <= 0.0 || -100.0 * gap / gdp < P.eu_fund_trigger) return;
    // The member hardest hit drafts; adoption by qualified majority.
    dcon::nation_id sponsor;
    for(auto m : w.organisation_get_membership_as_organisation(eu)) {
        if(!sponsor || m.get_member().get_gap_smoothed() < w.nation_get_gap_smoothed(sponsor)) sponsor = m.get_member().id;
    }
    // Members support a common fund unless fiscally hawkish ("frugal") and little hit.
    int yes = 0, members = 0;
    double pop_yes = 0.0, pop_all = 0.0;
    for(auto m : w.organisation_get_membership_as_organisation(eu)) {
        const auto v = m.get_member();
        const auto a = v.get_executive_seat().get_agent();
        const float hawk = a ? w.agent_get_fiscal_hawk(a) : 0.5f;
        const bool aye = -v.get_gap_smoothed() * 10.0f + 0.5f > hawk;
        ++members;
        yes += aye ? 1 : 0;
        pop_all += v.get_population();
        pop_yes += aye ? v.get_population() : 0.0;
    }
    const bool passed = yes >= w.organisation_get_threshold(eu) * members && pop_yes >= w.organisation_get_population_threshold(eu) * pop_all;
    log_org(state, sponsor, {}, journal_eu_fund, static_cast<float>(-100.0 * gap / gdp), passed ? static_cast<float>(P.eu_fund_size) : 0.0f);
    if(!passed) return;
    const double fund = P.eu_fund_size * gdp;
    double loss_total = 0.0;
    for(auto m : w.organisation_get_membership_as_organisation(eu)) loss_total += m.get_member().get_gdp0() * std::max(0.0f, -m.get_member().get_gap_smoothed());
    for(auto m : w.organisation_get_membership_as_organisation(eu)) {
        const auto v = m.get_member().id;
        const double share = loss_total > 0.0 ? w.nation_get_gdp0(v) * std::max(0.0f, -w.nation_get_gap_smoothed(v)) / loss_total : 0.0;
        // Grants are committed now and paid after ratification, over several years.
        w.nation_set_eu_grant_pending(v, w.nation_get_eu_grant_pending(v) + fund * share);
        w.nation_set_eu_grant_start(v, state.current_tick + static_cast<int32_t>(P.eu_fund_lag_days));
    }
    w.organisation_set_fund(eu, 3.0f); // years before another fund
}

void trade_disputes(game_state& state, dcon::organisation_id wto) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const auto stream = static_cast<uint32_t>(core::rng_stream::conflict);
    for(auto mn : w.organisation_get_membership_as_organisation(wto)) {
        const auto n = mn.get_member();
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        for(auto mm : w.organisation_get_membership_as_organisation(wto)) {
            const auto m = mm.get_member();
            if(m.id == n.id) continue;
            const auto km = ix(m.id), kn = ix(n.id);
            const float filed = n.get_wto_dispute(km);
            const float excess = (m.get_tariff() - m.get_tariff0()) * 100.0f;
            if(filed <= 0.0f) {
                if(excess > static_cast<float>(P.wto_bound) && n.get_trade_share(km) > 0.02f && n.get_at_war(km) <= 0.0f) {
                    n.set_wto_dispute(km, static_cast<float>(state.current_tick) + 1.0f);
                    log_org(state, n.id, m.id, journal_wto, 0.0f, NAN);
                }
                continue;
            }
            if(static_cast<float>(state.current_tick) + 1.0f - filed < static_cast<float>(P.wto_panel_days)) continue;
            const uint32_t key = 0x7E000000u + kn * 1024u + km;
            const bool wins = core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), key) < static_cast<float>(P.wto_win);
            n.set_wto_dispute(km, 0.0f);
            if(!wins || excess <= 0.0f) continue;
            if(P.wto_appellate <= 0.0) {
                log_org(state, n.id, m.id, journal_wto, 1.0f, -1.0f);
                continue;
            }
            const float comply = std::clamp(m.get_credibility() * (0.5f + 5.0f * m.get_trade_share(kn)), 0.0f, 0.95f);
            if(core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), key, 1) < comply) {
                m.set_tariff(m.get_tariff0() + 0.5f * (m.get_tariff() - m.get_tariff0()));
                n.set_wto_retaliation(km, 0.0f);
                log_org(state, n.id, m.id, journal_wto, 1.0f, 1.0f);
            } else {
                n.set_wto_retaliation(km, static_cast<float>(P.wto_retaliation));
                lose_credibility(state, m.id, 0.05f);
                log_org(state, n.id, m.id, journal_wto, 1.0f, 0.0f);
            }
        }
    }
}

} // namespace

void update_organisations(game_state& state) {
    auto& w = state.world;
    update_memberships(state);
    const auto date = core::date_from_tick(state.current_tick);
    // The General Assembly's regular session opens in September.
    if(const auto ga = organisation_of(state, org_kind::assembly); ga && date.month == 9 && date.day == 15) hold_assembly_session(state, ga);
    if(const auto wto = organisation_of(state, org_kind::trade)) trade_disputes(state, wto);
    if(static_cast<uint32_t>(state.current_tick) % monthly_cadence.period_days != 0) return;
    if(const auto sc = organisation_of(state, org_kind::security)) security_council(state, sc);
    if(const auto hrc = organisation_of(state, org_kind::rights)) human_rights_council(state, hrc);
    if(const auto eu = organisation_of(state, org_kind::union_)) economic_union(state, eu);
    (void)w;
}

} // namespace geosim::sim
