// Agreements with terms and compliance, and rebels (docs/06-geopolitics-ai.md).
//
// Treaty objects (`treaty`: kind, parties, terms, compliance, active) for:
//  - climate pledges (Paris Agreement, in force 2016): each nation pledges a carbon
//    price path for 2030, higher where self-expression values are strong; compliance
//    is the price reached over the pledged path; sustained breach costs credibility -
//    pledge-and-review relies on reputation, not enforcement (Victor 2011; Keohane &
//    Victor 2016).
//  - migration deals (EU-Turkey 2016 style): a rich destination with rising arrivals
//    and low asylum openness pays a poorer origin to control departures; arrivals
//    along that route fall (Greenhill 2010 on coercive engineered migration).
//  - sovereign debt relief: a nation in default for six months restructures with its
//    foreign creditors, a haircut on external debt, and the default ends (Cruces &
//    Trebesch 2013; Reinhart & Trebesch 2016).
//  - conditional aid (World Bank / bilateral programmes): poor, trusting recipients
//    receive aid that is suspended when they backslide (Dreher, Sturm & Vreeland
//    2009; Molenaers, Dellepiane & Faust 2015 on budget-support suspensions).
//  - power sharing with rebels (below).
// Rebels (agent_kind::rebel): an insurgency creates a rebel agent whose strength
// grows with grievance, exclusion and rugged terrain and falls with state capacity
// and outside support for the government. At a mutually hurting stalemate the sides
// negotiate (Zartman 1989): a power-sharing deal cuts the excluded group's exclusion
// and ends the insurgency (Hartzell & Hoddie 2003); a weak rebellion is defeated.

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

void log_agreement(game_state& state, dcon::nation_id n, dcon::nation_id m, treaty_kind k, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, journal_agreement);
    w.decision_set_before(d, static_cast<float>(k));
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, m ? static_cast<float>(m.index()) : NAN);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

dcon::treaty_id sign(game_state& state, treaty_kind k, dcon::nation_id a, dcon::nation_id b, float terms) {
    auto& w = state.world;
    const auto t = w.create_treaty();
    w.treaty_set_kind(t, static_cast<uint8_t>(k));
    w.treaty_set_a(t, a);
    w.treaty_set_b(t, b);
    w.treaty_set_signed_tick(t, state.current_tick);
    w.treaty_set_terms(t, terms);
    w.treaty_set_compliance(t, 1.0f);
    w.treaty_set_active(t, 1);
    log_agreement(state, a, b, k, 1.0f);
    return t;
}

float unrest_of(game_state const& state, dcon::nation_id n) {
    float u = 0.0f, pop = 0.0f;
    for(auto own : state.world.nation_get_region_ownership(n)) {
        u += own.get_region().get_unrest() * own.get_region().get_population();
        pop += own.get_region().get_population();
    }
    return pop > 0.0f ? u / pop : 0.0f;
}
} // namespace

bool has_treaty(game_state const& state, treaty_kind k, dcon::nation_id a, dcon::nation_id b) {
    for(auto t : state.world.in_treaty) {
        if(t.get_active() && t.get_kind() == static_cast<uint8_t>(k) && ((t.get_a() == a && t.get_b() == b) || (t.get_a() == b && t.get_b() == a))) return true;
    }
    return false;
}

void init_agreements(game_state& state) {
    auto& w = state.world;
    const auto se = static_cast<uint8_t>(value_axis::self_expression);
    for(auto n : w.in_nation) {
        // Paris pledges: the 2030 carbon price (USD/t) the nation commits to.
        const float pledge = n.get_carbon_price0() + 60.0f * std::clamp(0.5f + n.get_window_center(se), 0.0f, 1.5f) * n.get_liberal();
        n.set_climate_pledge(pledge);
        if(pledge > n.get_carbon_price0() + 1.0f) {
            auto& ww = state.world;
            const auto t = ww.create_treaty();
            ww.treaty_set_kind(t, static_cast<uint8_t>(treaty_kind::climate));
            ww.treaty_set_a(t, n.id);
            ww.treaty_set_signed_tick(t, 0);
            ww.treaty_set_terms(t, pledge);
            ww.treaty_set_compliance(t, 1.0f);
            ww.treaty_set_active(t, 1);
        }
    }
}

void update_agreements(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    const auto stream = static_cast<uint32_t>(core::rng_stream::conflict);

    // Existing treaties: compliance and breaches.
    for(auto t : w.in_treaty) {
        if(!t.get_active()) continue;
        const auto a = t.get_a();
        if(!is_due(static_cast<uint32_t>(a.id.index()), tick, monthly_cadence)) continue;
        switch(static_cast<treaty_kind>(t.get_kind())) {
        case treaty_kind::climate: {
            const float elapsed = std::min(1.0f, static_cast<float>(state.current_tick - t.get_signed_tick()) / 3650.0f + 0.4f); // Paris 2016 -> 2030
            const float path = a.get_carbon_price0() + (t.get_terms() - a.get_carbon_price0()) * std::max(0.0f, elapsed - 0.4f) / 0.6f;
            const float c = path > 0.5f ? std::min(1.0f, a.get_carbon_price() / path) : 1.0f;
            t.set_compliance(c);
            if(c < static_cast<float>(P.pledge_slack)) lose_credibility(state, a.id, static_cast<float>(P.pledge_credibility) * years);
            break;
        }
        case treaty_kind::migration: {
            const auto b = t.get_b(); // origin paid by destination a
            // Within the donor's budget and at most aid_recipient_cap of the origin's GDP.
            const double aid = std::min(P.migration_deal_aid * a.get_gdp0() * a.get_core_price(), P.aid_recipient_cap * b.get_gdp0() * b.get_core_price()) * years;
            cause_scope c(state, journal_cause(a.id, journal_agreement));
            transfer(state, account::government(a.id), account::rest_of_world(a.id), aid, flow::aid);
            transfer(state, account::rest_of_world(b.id), account::government(b.id), aid, flow::aid);
            // Ends when arrivals pressure is gone for the destination.
            if(static_cast<float>(state.current_tick - t.get_signed_tick()) > 4.0f * 365.0f) {
                t.set_active(0);
                log_agreement(state, a.id, b.id, treaty_kind::migration, 2.0f);
            }
            break;
        }
        case treaty_kind::conditional_aid: {
            const auto b = t.get_b(); // recipient b, donor a
            if(b.get_repression() > 0.5f || b.get_liberal() < t.get_terms() - 0.1f) {
                t.set_active(0);
                t.set_compliance(0.0f);
                log_agreement(state, a.id, b.id, treaty_kind::conditional_aid, 0.0f);
                break;
            }
            const double aid = std::min(P.aid_rate * a.get_gdp0() * a.get_core_price(), P.aid_recipient_cap * b.get_gdp0() * b.get_core_price()) * years;
            cause_scope c(state, journal_cause(a.id, journal_agreement));
            transfer(state, account::government(a.id), account::rest_of_world(a.id), aid, flow::aid);
            transfer(state, account::rest_of_world(b.id), account::government(b.id), aid, flow::aid);
            break;
        }
        default: break;
        }
    }

    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const auto kn = static_cast<uint16_t>(n.id.index());
        // Debt relief, on average six months into a default.
        if(n.get_defaulted() && n.get_spread() - n.get_spread0() > 0.0f) {
            const auto key = 0x6E000000u + kn;
            if(core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), key) < years * 2.0f) { // ~6 months on average
                const double debt = std::max(0.0, -n.get_bal_gov()) * n.get_foreign_debt_share();
                const double relief = P.debt_relief_haircut * debt;
                cause_scope c(state, journal_cause(n.id, journal_agreement));
                transfer(state, account::rest_of_world(n.id), account::government(n.id), relief, flow::debt_relief);
                n.set_defaulted(0);
                sign(state, treaty_kind::debt_relief, n.id, {}, static_cast<float>(relief / std::max(1.0, n.get_gdp0())));
            }
        }
        // Migration deal: a rich destination under rising arrivals and low openness.
        const float arrivals = n.get_migration_net() / std::max(1.0f, static_cast<float>(n.get_population()));
        if(n.get_gdp_pc0() > static_cast<float>(P.aid_income) && arrivals > 0.003f * years && n.get_asylum_openness() < 0.8f) {
            dcon::nation_id origin;
            float most = 0.0f;
            for(auto m : w.in_nation) {
                const float d = n.get_diaspora(static_cast<uint16_t>(m.id.index()));
                if(m.id != n.id && m.get_gdp_pc0() < n.get_gdp_pc0() * 0.5f && d > most) {
                    most = d;
                    origin = m.id;
                }
            }
            if(origin && !has_treaty(state, treaty_kind::migration, n.id, origin) && w.nation_get_trust(origin, kn) > 0.3f) {
                sign(state, treaty_kind::migration, n.id, origin, 1.0f);
            }
        }
        // Conditional aid: poor recipients and a trusted rich donor.
        if(n.get_gdp_pc0() < 6000.0f && n.get_liberal() > 0.3f) {
            dcon::nation_id donor;
            float best = 0.5f;
            for(auto d : w.in_nation) {
                if(d.id == n.id || d.get_gdp_pc0() < static_cast<float>(P.aid_income)) continue;
                const float tr = d.get_trust(kn);
                if(tr > best) {
                    best = tr;
                    donor = d.id;
                }
            }
            if(donor && !has_treaty(state, treaty_kind::conditional_aid, donor, n.id)
               && core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), 0x6F000000u + kn) < 0.5f * years) {
                sign(state, treaty_kind::conditional_aid, donor, n.id, n.get_liberal());
            }
        }

        // Rebels.
        float insurgent = 0.0f, rugged = 0.0f;
        for(auto own : n.get_region_ownership()) {
            if(own.get_region().get_insurgency() > insurgent) {
                insurgent = own.get_region().get_insurgency();
                rugged = own.get_region().get_ruggedness();
            }
        }
        auto rebel = n.get_rebel_agent();
        if(insurgent > 0.0f) {
            if(!rebel || w.agent_get_strength(rebel) <= 0.0f) {
                if(!rebel) {
                    rebel = w.create_agent();
                    w.agent_set_kind(rebel, static_cast<uint8_t>(agent_kind::rebel));
                    w.agent_set_home(rebel, n.id);
                    n.set_rebel_agent(rebel);
                }
                w.agent_set_strength(rebel, 0.2f);
                const auto d = w.create_decision();
                w.decision_set_agent(d, rebel);
                w.decision_set_target(d, n.id);
                w.decision_set_tick(d, state.current_tick);
                w.decision_set_lever(d, journal_rebels);
                w.decision_set_before(d, 0.0f);
                w.decision_set_after(d, 1.0f);
                w.decision_set_pragmatic(d, NAN);
                w.decision_set_ideological(d, NAN);
                w.decision_set_prior(d, NAN);
            }
            float excluded = 0.0f;
            dcon::social_group_id grp;
            for(auto g : w.in_social_group) {
                if(g.get_nation() == n.id && g.get_excluded() * g.get_mobilisation() > excluded) {
                    excluded = g.get_excluded() * g.get_mobilisation();
                    grp = g.id;
                }
            }
            float pro = 0.0f, anti = 0.0f;
            insurgency_support(state, n.id, pro, anti);
            float s = w.agent_get_strength(rebel);
            s += years * (0.3f * unrest_of(state, n.id) / 4.0f + excluded + 0.3f * rugged + 0.3f * anti - 0.5f * n.get_state_capacity() - 0.3f * pro);
            s = std::clamp(s, 0.0f, 1.0f);
            w.agent_set_strength(rebel, s);
            w.agent_set_demand(rebel, excluded);
            // Mutually hurting stalemate: neither side close to winning, both paying.
            const float stalemate = std::max(0.0f, 1.0f - std::abs(s - n.get_state_capacity()) * 2.0f);
            const float p = static_cast<float>(P.rebel_negotiate) * years * stalemate * (1.0f + mediation_score(state, n.id, n.id));
            const float u = core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), 0x6B000000u + kn);
            float outcome = NAN;
            if(u < p) {
                if(grp) w.social_group_set_excluded(grp, w.social_group_get_excluded(grp) * (1.0f - static_cast<float>(P.power_sharing)));
                sign(state, treaty_kind::power_sharing, n.id, {}, static_cast<float>(P.power_sharing));
                outcome = 0.0f;
            } else if(s < 0.05f) {
                outcome = -1.0f;
            }
            if(!std::isnan(outcome)) {
                for(auto own : n.get_region_ownership()) own.get_region().set_insurgency(0.0f);
                w.agent_set_strength(rebel, 0.0f);
                const auto d = w.create_decision();
                w.decision_set_agent(d, rebel);
                w.decision_set_target(d, n.id);
                w.decision_set_tick(d, state.current_tick);
                w.decision_set_lever(d, journal_rebels);
                w.decision_set_before(d, 1.0f);
                w.decision_set_after(d, outcome);
                w.decision_set_pragmatic(d, NAN);
                w.decision_set_ideological(d, NAN);
                w.decision_set_prior(d, NAN);
            }
        } else if(rebel && w.agent_get_strength(rebel) > 0.0f) {
            w.agent_set_strength(rebel, 0.0f);
        }
    }
}

} // namespace geosim::sim
