// Organisations with geopolitical weight beyond the UN system (docs/06-geopolitics-ai.md
// #international-organisations). Each kind follows its literature:
//
// Alliances (NATO, CSTO). Members are allied pairwise (call_allies in geopolitics.cpp
// brings them in at war onset). Burden sharing: small members free-ride (Olson &
// Zeckhauser 1966) until the alliance's threat rises, when members below the defence
// guideline close part of the gap (Plumper & Neumayer 2015; the 2014 Wales pledge after
// Crimea). Enlargement: a non-member whose threat has risen since 2019 and who trusts
// the members applies; accession needs every member's consent, so one distrustful member
// can block it (North Atlantic Treaty art. 10; Walt 1987; Reiter 2001).
// Summit clubs (G7, G20, BRICS, SCO). Annual summits socialise members' trust (Johnston
// 2001; Putnam & Bayne 1987). Crisis coordination: when members' mean output gap is deep,
// the summit pledges coordinated stimulus, raising members' fiscal responses for months
// (G20 London 2009; Drezner 2014). Sanction alignment: members move their sanctions
// toward their partners' (G7 on Russia 2014; Drezner 1999). Counter-hegemonic clubs
// shield members from each other's sanctions (BRICS, SCO; Stuenkel 2015); regime-
// security clubs back each other's governments against insurgents (SCO's "three evils",
// CSTO; Ambrosio 2008; Aris 2009).
// Cartel (OPEC+). Monthly meeting: below the price band the quota is cut in proportion
// to the shortfall, above it raised; members comply less the more fiscal stress they
// face (Kaufmann et al. 2004), the largest producer acts as swing producer and complies
// fully (Fattouh & Mahadeva 2013). When members' compliance collapses, the swing
// producer may start a price war, flooding the market for a few months (Fattouh &
// Economou 2020 on March 2020); OPEC's power is limited throughout (Colgan 2014).
// Development lenders (World Bank, NDB). Members poorer than `dev_income` in need
// (output loss, disasters, IMF programmes, insurgency) borrow concessionally, within the
// lender's annual capacity (Kenny 2020; Humphrey 2016 on the NDB).
// Regional organisations. Democracy clauses suspend a member after a coup until elections
// (or `suspension_years`); sanctioning organisations (ECOWAS) add sanctions; clauses
// deter coups (Powell, Lasley & Schiel 2016). Non-interference organisations (ASEAN) do
// neither (Acharya 2001). Members' disputes are mediated by their organisation
// (Hansen, Mitchell & Nemeth 2008). Customs unions share one external tariff (firms.cpp).

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

uint16_t ix(dcon::nation_id n) { return static_cast<uint16_t>(n.index()); }

void log_entry(game_state& state, dcon::nation_id n, uint8_t code, float detail, float before, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    if(n) w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, code);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, detail);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

// Active and not suspended.
bool in_good_standing(game_state const& state, dcon::membership_id m) {
    return membership_active(state, m) && state.world.membership_get_role(m) != static_cast<uint8_t>(org_role::suspended);
}

template<typename F>
void each_member(game_state const& state, dcon::organisation_id o, F&& f) {
    for(auto m : state.world.organisation_get_membership_as_organisation(o)) {
        if(!membership_active(state, m.id) || m.get_role() == static_cast<uint8_t>(org_role::suspended)) continue;
        f(m.get_member().id);
    }
}

float max_threat_rise(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    float t = 0.0f;
    for(auto m : w.in_nation) {
        if(m.id != n) t = std::max(t, w.nation_get_threat_to(n, ix(m.id)));
    }
    return std::max(0.0f, t - w.nation_get_threat_max0(n));
}

void ally_members(game_state& state, dcon::organisation_id o) {
    auto& w = state.world;
    std::vector<dcon::nation_id> members;
    each_member(state, o, [&](dcon::nation_id n) { members.push_back(n); });
    for(auto a : members) {
        for(auto b : members) {
            if(a != b) w.nation_set_alliance(a, ix(b), 1.0f);
        }
    }
}

// ---------------------------------------------------------------- alliances
void alliance_enlargement(game_state& state, dcon::organisation_id o) {
    auto& w = state.world;
    auto const& P = state.params;
    std::vector<dcon::nation_id> members;
    each_member(state, o, [&](dcon::nation_id n) { members.push_back(n); });
    if(members.empty()) return;
    for(auto c : w.in_nation) {
        if(membership_in(state, o, c.id) || is_member(state, org_kind::alliance, c.id)) continue; // already in an alliance
        if(max_threat_rise(state, c.id) < static_cast<float>(P.accession_threat)) continue;
        // The applicant balances against its main threat (Walt 1987): the source, outside
        // the alliance, must also threaten the members, and the applicant must trust them
        // more than a stranger (0.5).
        dcon::nation_id source;
        float worst = 0.0f;
        for(auto x : w.in_nation) {
            if(x.id != c.id && c.get_threat_to(ix(x.id)) > worst) {
                worst = c.get_threat_to(ix(x.id));
                source = x.id;
            }
        }
        if(!source || membership_in(state, o, source)) continue;
        float trust = 0.0f, shared = 0.0f;
        for(auto m : members) {
            trust += c.get_trust(ix(m));
            shared += w.nation_get_threat_to(m, ix(source));
        }
        trust /= static_cast<float>(members.size());
        shared /= static_cast<float>(members.size());
        if(trust < 0.6f || shared < static_cast<float>(P.accession_shared)) continue;
        // Consensus: a member that distrusts the applicant, or fears entrapment in a war
        // against a source it does not itself fear (Snyder 1984), blocks it.
        dcon::nation_id blocker;
        for(auto m : members) {
            if(w.nation_get_trust(m, ix(c.id)) < static_cast<float>(P.accession_trust)
               || w.nation_get_threat_to(m, ix(source)) < 0.5f * static_cast<float>(P.accession_shared)) {
                blocker = m;
            }
        }
        if(blocker) {
            log_entry(state, c.id, journal_org_join, static_cast<float>(o.index()), 0.0f, 0.0f);
            continue;
        }
        const auto m = w.force_create_membership(o, c.id);
        w.membership_set_role(m, static_cast<uint8_t>(org_role::member));
        w.membership_set_term_end(m, -1);
        w.membership_set_exit_tick(m, -1);
        w.membership_set_join_tick(m, -1);
        w.membership_set_suspended_until(m, -1);
        ally_members(state, o);
        log_entry(state, c.id, journal_org_join, static_cast<float>(o.index()), 0.0f, 1.0f);
    }
}

// ---------------------------------------------------------------- clubs
void club_summit(game_state& state, dcon::organisation_id o) {
    auto& w = state.world;
    auto const& P = state.params;
    std::vector<dcon::nation_id> members;
    each_member(state, o, [&](dcon::nation_id n) { members.push_back(n); });
    if(members.size() < 2) return;
    for(auto a : members) {
        for(auto b : members) {
            if(a == b) continue;
            const float t = w.nation_get_trust(a, ix(b));
            w.nation_set_trust(a, ix(b), t + static_cast<float>(P.club_trust) * std::max(0.0f, 0.7f - t));
        }
    }
    float stimulus = 0.0f;
    if(w.organisation_get_flags(o) & org_crisis_coordination) {
        double gap = 0.0, gdp = 0.0;
        for(auto m : members) {
            gap += w.nation_get_gdp0(m) * w.nation_get_gap_smoothed(m);
            gdp += w.nation_get_gdp0(m);
        }
        if(gdp > 0.0 && -100.0 * gap / gdp > P.club_slump) {
            for(auto m : members) w.nation_set_coordinated_stimulus(m, static_cast<float>(P.club_stimulus_days));
            stimulus = 1.0f;
        }
    }
    log_entry(state, members.front(), journal_summit, static_cast<float>(o.index()), 0.0f, stimulus);
}

// ---------------------------------------------------------------- cartel
void cartel_meeting(game_state& state, dcon::organisation_id o) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    if(!D.oil) return;
    const float rel = w.commodity_get_price(D.oil) / std::max(1e-3f, w.commodity_get_price0(D.oil));
    float q = w.organisation_get_quota(o);
    const float before = q;
    // Price war in progress (days > 0): the swing producer floods the market until it ends.
    // A war disciplines the cheaters: a year of high compliance follows (days < 0), as with
    // the April 2020 OPEC+ cut after the March price war (Fattouh & Economou 2020).
    const float war_days = w.organisation_get_price_war_days(o);
    const bool disciplined = war_days < 0.0f;
    if(war_days > 0.0f) {
        const float left = war_days - 28.0f;
        w.organisation_set_price_war_days(o, left > 0.0f ? left : -365.0f);
        q = 1.0f;
    } else if(disciplined) {
        w.organisation_set_price_war_days(o, std::min(0.0f, war_days + 28.0f));
    }
    if(war_days <= 0.0f) {
        if(rel < w.organisation_get_price_low(o)) {
            q -= static_cast<float>(P.cartel_cut_speed) * (w.organisation_get_price_low(o) - rel);
        } else if(rel > w.organisation_get_price_high(o)) {
            q += static_cast<float>(P.cartel_cut_speed) * (rel - w.organisation_get_price_high(o));
        } else {
            q += (1.0f - q) * 0.1f; // quotas unwind inside the band
        }
    }
    q = std::clamp(q, 0.75f, 1.1f);
    w.organisation_set_quota(o, q);
    // Swing producer: the largest producer among the members.
    dcon::nation_id swing;
    each_member(state, o, [&](dcon::nation_id n) {
        if(!swing || w.nation_get_oil_production0(n) > w.nation_get_oil_production0(swing)) swing = n;
    });
    float compliance_sum = 0.0f, weight = 0.0f;
    each_member(state, o, [&](dcon::nation_id n) {
        if(w.nation_get_oil_production0(n) <= 0.0f) return;
        // Fiscal stress: lost output and an oil price below its 2019 level (the fiscal
        // breakeven of most members is near it) push members to over-produce.
        const float stress = std::max(0.0f, -w.nation_get_gap_smoothed(n)) * 5.0f + std::max(0.0f, 1.0f - rel);
        float comply = n == swing ? 1.0f : std::clamp(1.0f - static_cast<float>(P.cartel_cheating) * stress, 0.3f, 1.0f);
        if(disciplined) comply = std::max(comply, 0.9f);
        // The swing producer bears a larger share of cuts (and floods in a price war).
        float own = 1.0f - (1.0f - q) * comply * (n == swing ? 1.5f : 1.0f);
        if(n == swing && w.organisation_get_price_war_days(o) > 0.0f) own = 1.15f;
        w.nation_set_cartel_quota(n, std::clamp(own, 0.6f, 1.2f));
        if(n != swing) {
            compliance_sum += comply * w.nation_get_oil_production0(n);
            weight += w.nation_get_oil_production0(n);
        }
    });
    if(!swing) return;
    // Collapse of discipline in a falling market: the swing producer starts a price war.
    const float compliance = weight > 0.0f ? compliance_sum / weight : 1.0f;
    if(q < 0.97f && compliance < static_cast<float>(P.price_war_trigger) && w.organisation_get_price_war_days(o) == 0.0f) {
        w.organisation_set_price_war_days(o, static_cast<float>(P.price_war_days));
        log_entry(state, swing, journal_cartel, static_cast<float>(o.index()), before, -1.0f);
    } else if(std::abs(q - before) > 0.01f) {
        log_entry(state, swing, journal_cartel, static_cast<float>(o.index()), before, q);
    }
}

// ---------------------------------------------------------------- development lenders
void development_lending(game_state& state, dcon::organisation_id o) {
    auto& w = state.world;
    auto const& P = state.params;
    const float years = 28.0f / 365.0f;
    double budget = static_cast<double>(w.organisation_get_capacity_bn(o)) * 1e9 * years;
    struct need {
        dcon::nation_id n;
        double amount;
    };
    std::vector<need> needs;
    double total = 0.0;
    each_member(state, o, [&](dcon::nation_id n) {
        if(w.nation_get_gdp_pc0(n) > static_cast<float>(P.dev_income)) return;
        float insurgent = 0.0f;
        for(auto own : w.nation_get_region_ownership(n)) insurgent = std::max(insurgent, own.get_region().get_insurgency());
        const float need_level = std::min(1.0f, 10.0f * std::max(0.0f, -w.nation_get_gap_smoothed(n)) + 0.5f * insurgent
                                                     + (w.nation_get_imf_program(n) > 0.0f ? 0.3f : 0.0f));
        if(need_level <= 0.0f) return;
        const double gdp = w.nation_get_gdp0(n) * w.nation_get_core_price(n) * w.nation_get_gf(n);
        const double amount = P.dev_lending_gdp * gdp * years * need_level;
        needs.push_back({n, amount});
        total += amount;
    });
    const double scale = total > budget && total > 0.0 ? budget / total : 1.0;
    for(auto const& [n, amount] : needs) {
        cause_scope c(state, journal_cause(n, journal_treaty));
        transfer(state, account::rest_of_world(n), account::government(n), amount * scale, flow::development_lending);
    }
}

} // namespace

// ---------------------------------------------------------------- public hooks

void init_geo_organisations(game_state& state) {
    auto& w = state.world;
    for(auto o : w.in_organisation) {
        if(o.get_kind() == static_cast<uint8_t>(org_kind::alliance)) ally_members(state, o.id);
        o.set_quota(1.0f);
    }
    for(auto n : w.in_nation) {
        n.set_cartel_quota(1.0f);
        n.set_coordinated_stimulus(0.0f);
    }
    init_mega_events(state);
}

// Defence push of an alliance member below its guideline when the alliance's threat rises.
float alliance_burden_push(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    float push = 0.0f;
    for(auto m : w.nation_get_membership_as_member(n)) {
        auto o = m.get_organisation();
        if(o.get_kind() != static_cast<uint8_t>(org_kind::alliance) || !in_good_standing(state, m.id)) continue;
        float rise = 0.0f;
        each_member(state, o.id, [&](dcon::nation_id x) { rise = std::max(rise, max_threat_rise(state, x)); });
        const float gap = std::max(0.0f, o.get_guideline() - w.nation_get_defense_share(n));
        push = std::max(push, static_cast<float>(state.params.burden_gain) * gap * std::min(1.0f, rise * 5.0f));
    }
    return push;
}

// Sanction choice of n on `target` after club effects: alignment with partners' sanctions
// (G7) and shielding of fellow members of counter-hegemonic clubs (BRICS, SCO).
float club_sanction_shift(game_state const& state, dcon::nation_id n, dcon::nation_id target, float choice) {
    auto const& w = state.world;
    auto const& P = state.params;
    if(shares_org(state, n, target, org_counter_hegemonic)) choice *= 1.0f - static_cast<float>(P.counter_shield);
    for(auto m : w.nation_get_membership_as_member(n)) {
        auto o = m.get_organisation();
        if(!(o.get_flags() & org_sanction_alignment) || !in_good_standing(state, m.id)) continue;
        float sum = 0.0f;
        int k = 0;
        each_member(state, o.id, [&](dcon::nation_id p) {
            if(p == n || p == target) return;
            sum += w.nation_get_sanction(p, ix(target));
            ++k;
        });
        if(k > 0) choice += static_cast<float>(P.sanction_alignment) * std::max(0.0f, sum / static_cast<float>(k) - choice);
    }
    return std::clamp(choice, 0.0f, 1.0f);
}

float coup_deterrence(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    for(auto m : w.nation_get_membership_as_member(n)) {
        if((m.get_organisation().get_flags() & org_democracy_clause) && in_good_standing(state, m.id)) return static_cast<float>(state.params.coup_deterrence);
    }
    return 0.0f;
}

// A coup suspends the nation from organisations with democracy clauses.
void on_coup(game_state& state, dcon::nation_id n) {
    auto& w = state.world;
    for(auto m : w.nation_get_membership_as_member(n)) {
        auto o = m.get_organisation();
        if(!(o.get_flags() & org_democracy_clause) || (o.get_flags() & org_non_interference) || !in_good_standing(state, m.id)) continue;
        m.set_role(static_cast<uint8_t>(org_role::suspended));
        m.set_suspended_until(state.current_tick + static_cast<int32_t>(state.params.suspension_years * 365.0));
        log_entry(state, n, journal_suspension, static_cast<float>(o.id.index()), 0.0f, 1.0f);
    }
}

float regional_mediation_score(game_state const& state, dcon::nation_id n, dcon::nation_id m) {
    return shares_org(state, n, m, org_mediation) ? static_cast<float>(state.params.regional_mediation) : 0.0f;
}

void update_geo_organisations(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) n.set_coordinated_stimulus(std::max(0.0f, n.get_coordinated_stimulus() - 1.0f));
    if(static_cast<uint32_t>(state.current_tick) % monthly_cadence.period_days != 0) return;
    const auto date = core::date_from_tick(state.current_tick, state.epoch_days);
    const bool summit_month_start = date.day <= 28; // the monthly step falling in the summit month
    for(auto o : w.in_organisation) {
        switch(static_cast<org_kind>(o.get_kind())) {
        case org_kind::alliance:
            ally_members(state, o.id); // members (incl. new ones) stay allied
            alliance_enlargement(state, o.id);
            break;
        case org_kind::club:
            if(o.get_summit_month() == date.month && summit_month_start && o.get_last_session() != date.year) {
                club_summit(state, o.id);
                o.set_last_session(date.year);
            }
            break;
        case org_kind::cartel: cartel_meeting(state, o.id); break;
        case org_kind::development: development_lending(state, o.id); break;
        default: break;
        }
    }
}

} // namespace geosim::sim
