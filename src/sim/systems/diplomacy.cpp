// Disputes, crisis bargaining and diplomacy (docs/06-geopolitics-ai.md#conflict).
//
// Militarised disputes. Wars are the rare end of an escalation ladder (the
// Correlates of War MID hostility levels: 2 threat, 3 display, 4 use of force,
// 5 war; Jones, Bremer & Singer 1996). The war hazard of geopolitics.cpp starts
// disputes, `mid_ratio` times as often; an open dispute then escalates, ends when one
// side backs down, or is settled, each month (monthly, per pair):
//  - escalation rises with the stakes (claims, resource rents, rivalry), with poor
//    information and outside democratic pairs (Fearon 1995; Senese & Vasquez 2008);
//    two nuclear powers almost never take the last step to war but skirmish more
//    below it (stability-instability paradox: Snyder 1965; Jervis 1989);
//  - the weaker side backs down more often, a democratic leader less once escalated
//    and her opponent more (audience costs tie hands: Fearon 1994; Schultz 2001;
//    Weeks 2008); backing down costs the leader legitimacy and some credibility;
//  - settlement grows with a trusted, capable mediator (Beardsley 2011; Kydd 2003).
// Every dispute deepens the pair's rivalry, which raises future dispute hazards,
// lowers trust and drives arms races (Diehl & Goertz 2000; Richardson 1960 in
// institutions.cpp); rivalry decays over decades.
// Sanctions bargaining: a target concedes with a hazard that grows with its cost
// (export dependence on the sender), more readily to friends (the sanctions paradox;
// Drezner 1999) and as a democracy (Lektzian & Souva 2007); the sender then lifts
// them (Hufbauer, Schott, Elliott & Oegg 2007).
// Intervention in civil wars: allies and trusting strong partners back the
// government, rivals back the rebels (Regan 2002; Cunningham 2010; Salehyan 2009).
// Crisis aid from rich donors to friends in need - war, insurgency, IMF programmes
// and disasters (Alesina & Dollar 2000; Strömberg 2007) - and arms imports: a share
// of procurement above baseline leaks to suppliers chosen by capability, trust and
// alliance (SIPRI arms transfers; Kinsella 1994).

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};

void log_pair(game_state& state, dcon::nation_id n, dcon::nation_id m, uint8_t code, float before, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, code);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, m ? static_cast<float>(m.index()) : NAN);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

float set_hostility(game_state& state, dcon::nation_id n, dcon::nation_id m, float h) {
    auto& w = state.world;
    w.nation_set_hostility(n, static_cast<uint16_t>(m.index()), h);
    w.nation_set_hostility(m, static_cast<uint16_t>(n.index()), h);
    return h;
}

void deepen_rivalry(game_state& state, dcon::nation_id n, dcon::nation_id m, float amount) {
    auto& w = state.world;
    const auto kn = static_cast<uint16_t>(n.index()), km = static_cast<uint16_t>(m.index());
    const float r = std::min(1.0f, w.nation_get_rivalry(n, km) + amount);
    w.nation_set_rivalry(n, km, r);
    w.nation_set_rivalry(m, kn, r);
}

float audience(game_state const& state, dcon::nation_id x) {
    // Electoral accountability; personalist autocrats face little (Weeks 2008).
    return static_cast<float>(state.params.audience_cost) * state.world.nation_get_electoral(x);
}
} // namespace

float mediation_score(game_state const& state, dcon::nation_id n, dcon::nation_id m) {
    auto const& w = state.world;
    float max_cinc = 0.0f;
    for(auto t : w.in_nation) max_cinc = std::max(max_cinc, t.get_cinc());
    float best = 0.0f;
    for(auto t : w.in_nation) {
        if(t.id == n || t.id == m || t.get_at_war(static_cast<uint16_t>(n.index())) > 0.0f || t.get_at_war(static_cast<uint16_t>(m.index())) > 0.0f) continue;
        const float trusted = std::min(w.nation_get_trust(n, static_cast<uint16_t>(t.id.index())), w.nation_get_trust(m, static_cast<uint16_t>(t.id.index())));
        const float leverage = max_cinc > 0.0f ? 0.5f + 0.5f * t.get_cinc() / max_cinc : 0.5f;
        best = std::max(best, trusted * leverage);
    }
    // A shared regional organisation mediates among its members (Hansen, Mitchell & Nemeth 2008).
    return std::max(best, regional_mediation_score(state, n, m));
}

void start_dispute(game_state& state, dcon::nation_id n, dcon::nation_id m) {
    set_hostility(state, n, m, 2.0f);
    deepen_rivalry(state, n, m, static_cast<float>(state.params.rivalry_dispute));
    log_pair(state, n, m, journal_dispute, 0.0f, 2.0f);
}

void start_war(game_state& state, dcon::nation_id n, dcon::nation_id m) {
    auto& w = state.world;
    const auto kn = static_cast<uint16_t>(n.index()), km = static_cast<uint16_t>(m.index());
    w.nation_set_at_war(n, km, 1.0f);
    w.nation_set_at_war(m, kn, 1.0f);
    w.nation_set_sanction(n, km, 1.0f);
    w.nation_set_sanction(m, kn, 1.0f);
    set_hostility(state, n, m, 5.0f);
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, journal_war);
    w.decision_set_before(d, 0.0f);
    w.decision_set_after(d, 1.0f);
    w.decision_set_pragmatic(d, static_cast<float>(m.index()));
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
    call_allies(state, n, m);
    call_allies(state, m, n);
}

void insurgency_support(game_state const& state, dcon::nation_id n, float& for_government, float& for_rebels) {
    auto const& w = state.world;
    for_government = 0.0f;
    for_rebels = 0.0f;
    const auto kn = static_cast<uint16_t>(n.index());
    for(auto t : w.in_nation) {
        if(t.id == n) continue;
        const float weight = std::min(1.0f, 10.0f * t.get_cinc());
        float friendly = std::max(t.get_alliance(kn), std::max(0.0f, t.get_trust(kn) - 0.6f) * 2.5f);
        // Regime-security clubs back members' governments (SCO, CSTO; Ambrosio 2008).
        if(shares_org(state, t.id, n, org_regime_security)) friendly = std::max(friendly, 0.7f);
        const float hostile = std::max(t.get_rivalry(kn), std::max(0.0f, 0.2f - t.get_trust(kn)) * 5.0f);
        for_government = std::max(for_government, weight * friendly);
        for_rebels = std::max(for_rebels, weight * hostile);
    }
}

void update_diplomacy(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    const auto stream = static_cast<uint32_t>(core::rng_stream::conflict);
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const auto kn = static_cast<uint16_t>(n.id.index());
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const auto km = static_cast<uint16_t>(m.id.index());
            // Rivalry decays (each pair once).
            if(n.id.index() < m.id.index()) {
                const float r = n.get_rivalry(km) * std::exp(-years / static_cast<float>(P.rivalry_years));
                n.set_rivalry(km, r);
                m.set_rivalry(kn, r);
            }

            // Open disputes (each pair once, on the lower index's shard).
            const float h = n.get_hostility(km);
            if(n.id.index() < m.id.index() && h >= 2.0f && h < 5.0f && n.get_at_war(km) <= 0.0f) {
                const float claims = claim_of(state, n.id, m.id) + claim_of(state, m.id, n.id);
                const float stakes = 0.2f + claims + 0.3f * n.get_rivalry(km)
                                     + static_cast<float>(P.war_rents) * std::max(n.get_resource_rents(), m.get_resource_rents()) * std::min(1.0f, claims + 0.2f);
                const float info = 1.0f - 0.25f * (n.get_media_freedom() + m.get_media_freedom());
                const float democratic = n.get_electoral() * m.get_electoral();
                const bool both_nuclear = n.get_nuclear() > 0.0f && m.get_nuclear() > 0.0f;
                const bool one_nuclear = (n.get_nuclear() > 0.0f) != (m.get_nuclear() > 0.0f);
                float nuke = 1.0f;
                if(h >= 4.0f) nuke = both_nuclear ? static_cast<float>(P.nuclear_war_factor) : one_nuclear ? 0.3f : 1.0f;
                else if(both_nuclear) nuke = 1.0f + static_cast<float>(P.stability_instability);
                const float p_escalate = static_cast<float>(P.escalate_rate) * years * info * stakes * (1.0f - 0.7f * democratic) * nuke;
                const float cn = n.get_cinc() / std::max(1e-9f, n.get_cinc() + m.get_cinc());
                const float tied = std::min(1.0f, (h - 1.0f) / 3.0f); // escalated publicly
                const float an = audience(state, n.id), am = audience(state, m.id);
                const float p_back_n = static_cast<float>(P.backdown_rate) * years * (1.0f - cn) * (1.0f - an * tied) * (1.0f + am);
                const float p_back_m = static_cast<float>(P.backdown_rate) * years * cn * (1.0f - am * tied) * (1.0f + an);
                const float trust = 0.5f * (n.get_trust(km) + m.get_trust(kn));
                const float p_settle = static_cast<float>(P.dispute_settle_rate) * years * (0.5f + trust)
                                       * (1.0f + static_cast<float>(P.mediation_gain) * mediation_score(state, n.id, m.id));
                // One outcome per month at most; hazards above one are normalised.
                float pe = p_escalate, pbn = p_back_n, pbm = p_back_m, ps = p_settle;
                const float total = pe + pbn + pbm + ps;
                if(total > 1.0f) {
                    pe /= total;
                    pbn /= total;
                    pbm /= total;
                    ps /= total;
                }
                const float u = core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), 0x70000000u + kn * 1024u + km);
                if(u < pe) {
                    const float next = h + 1.0f;
                    if(next >= 5.0f) {
                        start_war(state, n.id, m.id);
                    } else {
                        set_hostility(state, n.id, m.id, next);
                        log_pair(state, n.id, m.id, journal_dispute, h, next);
                    }
                } else if(u < pe + pbn + pbm) {
                    const bool n_backs = u < pe + pbn;
                    const auto loser = n_backs ? n.id : m.id;
                    const float a = n_backs ? an : am;
                    w.nation_set_legitimacy(loser, std::max(0.0f, w.nation_get_legitimacy(loser)
                                                                      - static_cast<float>(P.audience_approval) * a * tied / std::max(0.01f, static_cast<float>(P.audience_cost))));
                    lose_credibility(state, loser, 0.05f * tied);
                    set_hostility(state, n.id, m.id, 0.0f);
                    log_pair(state, loser, loser == n.id ? m.id : n.id, journal_dispute, h, 0.0f);
                } else if(u < pe + pbn + pbm + ps) {
                    set_hostility(state, n.id, m.id, 0.0f);
                    n.set_trust(km, std::min(1.0f, n.get_trust(km) + 0.1f));
                    m.set_trust(kn, std::min(1.0f, m.get_trust(kn) + 0.1f));
                    log_pair(state, n.id, m.id, journal_dispute, h, 0.0f);
                }
            }

            // Sanctions bargaining: m is n's target.
            const float sanction = n.get_sanction(km);
            if(sanction >= 0.3f && n.get_at_war(km) <= 0.0f) {
                const float cost = sanction * m.get_trade_share(kn) * 10.0f;
                const float p = static_cast<float>(P.concede_rate) * years * cost * (0.5f + m.get_trust(kn)) * (0.5f + 0.5f * m.get_electoral());
                if(core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), 0x78000000u + kn * 1024u + km) < p) {
                    n.set_sanction(km, 0.0f);
                    n.set_trust(km, std::min(1.0f, n.get_trust(km) + 0.2f));
                    log_pair(state, m.id, n.id, journal_concession, sanction, 0.0f);
                }
            }
        }

        // Crisis aid to n from rich friends.
        float insurgent = 0.0f;
        for(auto own : n.get_region_ownership()) insurgent = std::max(insurgent, own.get_region().get_insurgency());
        const float disaster = std::max(0.0f, n.get_disaster_losses() - n.get_disaster_seen());
        n.set_disaster_seen(n.get_disaster_losses());
        const float need = n.get_occupied() + 0.5f * insurgent + (n.get_imf_program() > 0.0f ? 0.3f : 0.0f) + 5.0f * disaster;
        float received = 0.0f;
        // The recipient's need sets the total (a share of its own GDP); donors share it by
        // their ties, each within its aid budget. Sizing aid by donor GDP instead let
        // hundreds of donors swamp a small economy (docs/13).
        if(need > 0.0f && n.get_gdp_pc0() < static_cast<float>(P.aid_income)) {
            std::vector<std::pair<dcon::nation_id, float>> donors;
            float bonds = 0.0f;
            for(auto d : w.in_nation) {
                if(d.id == n.id || d.get_gdp_pc0() < static_cast<float>(P.aid_income) || d.get_at_war(kn) > 0.0f) continue;
                // Elected Security Council members draw more (Kuziemko & Werker 2006).
                const float bond = std::max(0.0f, d.get_trust(kn) - 0.3f) * (1.0f + d.get_alliance(kn)) * (org_role_of(state, org_kind::security, n.id) == static_cast<int>(org_role::elected) ? 1.5f : 1.0f)
                                   * (0.5f + un_alignment(state, d.id, n.id));
                if(bond <= 0.0f) continue;
                donors.emplace_back(d.id, bond);
                bonds += bond;
            }
            const double recipient_gdp = n.get_gdp0() * n.get_core_price() * n.get_gf();
            const double total = P.aid_need_gdp * recipient_gdp * years * std::min(1.0f, need) * std::min(1.0f, bonds);
            for(auto const& [d, bond] : donors) {
                const double budget = P.aid_rate * w.nation_get_gdp0(d) * w.nation_get_core_price(d) * w.nation_get_gf(d) * years;
                const double amount = std::min(budget, total * bond / bonds);
                cause_scope c(state, journal_cause(n.id, journal_treaty));
                transfer(state, account::government(d), account::rest_of_world(d), amount, flow::aid);
                transfer(state, account::rest_of_world(n.id), account::government(n.id), amount, flow::aid);
                n.set_trust(static_cast<uint16_t>(d.index()), std::min(1.0f, n.get_trust(static_cast<uint16_t>(d.index())) + 0.02f));
                received += static_cast<float>(amount);
            }
        }
        n.set_aid_received(received);

        // Arms imports: a share of procurement above baseline buys from suppliers.
        const double gdp_n = n.get_gdp0() * n.get_core_price() * n.get_gf();
        const double extra = std::max(0.0f, n.get_defense_share() - n.get_defense_share0()) / 100.0 * gdp_n
                             + static_cast<double>(P.war_spending) * n.get_war_effort() * gdp_n;
        const double imports = extra * n.get_arms_import_share() * years;
        if(imports > 0.0) {
            std::vector<std::pair<dcon::nation_id, float>> suppliers;
            float total = 0.0f;
            for(auto s : w.in_nation) {
                if(s.id == n.id || s.get_sanction(kn) > 0.3f || n.get_sanction(static_cast<uint16_t>(s.id.index())) > 0.3f || s.get_at_war(kn) > 0.0f) continue;
                const float weight = s.get_cinc() * std::max(0.0f, s.get_trust(kn) - 0.3f) * (1.0f + 2.0f * s.get_alliance(kn));
                if(weight <= 0.0f) continue;
                suppliers.emplace_back(s.id, weight);
                total += weight;
            }
            for(auto const& [s, weight] : suppliers) {
                const double part = imports * weight / total;
                transfer(state, account::firms(n.id), account::rest_of_world(n.id), part, flow::arms);
                transfer(state, account::rest_of_world(s), account::firms(s), part, flow::arms);
                w.nation_set_arms_exports(s, w.nation_get_arms_exports(s) + static_cast<float>(part));
            }
        }
    }
}

} // namespace geosim::sim
