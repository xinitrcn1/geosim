// International organisations: the UN and the WTO (docs/06-geopolitics-ai.md#international-organisations).
//
// General Assembly (monthly): each nation's ideal point on the main dimension of UN
// voting (the US-led liberal order; Bailey, Strezhnev & Voeten 2017) follows its
// liberal institutions, self-expression values and alliance ties with the leading
// liberal power; voting alignment between two nations, relative to 2019, shifts
// their trust (UN alignment as foreign-policy similarity; Signorino & Ritter 1999).
// Security Council (monthly, global): wars and insurgencies put items on the agenda.
// A draft passes with nine votes and no veto (UN Charter art. 27). Members vote by
// alignment with the drafter against ties to the target; permanent members veto to
// shield clients (Voeten 2001; Russia and China on Syria). Seats outside the slice
// vote as the rest of the world. Outcomes:
//  - ceasefire demand during a war: raises the settlement hazard (Fortna 2004);
//  - sanctions on an occupier: every member applies at least `un_sanction_level`
//    (multilateral sanctions bite more: Bapat & Morgan 2009);
//  - a peace operation after a war or insurgency: cuts dispute and insurgency
//    recurrence for its duration (Fortna 2008; Hultman, Kathman & Shannon 2014).
// Elected members serve two years; each 1 January the expiring seats are refilled,
// larger states more often (Dreher et al. 2014); members draw donor goodwill
// (Kuziemko & Werker 2006).
// WTO dispute settlement (monthly): a member whose exports face a tariff rise above
// `wto_bound` on a partner files a complaint; the panel reports after
// `wto_panel_days`; the complainant usually wins; the loser complies (tariff back
// toward its bound) more readily with high credibility and dependence, otherwise
// retaliation is authorised. With the Appellate Body paralysed (since 2019-12-11)
// a loser can appeal into the void and nothing follows (Bown 2009; Hoekman &
// Mavroidis 2021).

#include "core/calendar.hpp"
#include "core/rng.hpp"
#include "core/scheduler.hpp"
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

void log_un(game_state& state, dcon::nation_id actor, dcon::nation_id target, uint8_t code, float before, float after) {
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

dcon::nation_id rest_of_world(game_state const& state) { return find_nation(state, "ROW"); }

// Support of voter v for acting against target t on a draft by sponsor s.
float support(game_state const& state, dcon::nation_id v, dcon::nation_id s, dcon::nation_id t) {
    auto const& w = state.world;
    if(v == t) return -1.0f;
    const float with_sponsor = s ? un_alignment(state, v, s) : 0.5f;
    const float ties = std::max(w.nation_get_alliance(v, ix(t)), w.nation_get_trust(v, ix(t)));
    return with_sponsor - ties;
}
} // namespace

float un_alignment(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    auto const& w = state.world;
    return 1.0f - std::min(1.0f, std::abs(w.nation_get_un_ideal(a) - w.nation_get_un_ideal(b)) / 2.0f);
}

void compute_un_ideals(game_state& state) {
    auto& w = state.world;
    // The leading liberal power: the largest economy among the most liberal quartile.
    const auto row = rest_of_world(state);
    dcon::nation_id leader;
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

void init_un(game_state& state) {
    auto& w = state.world;
    compute_un_ideals(state);
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

void update_un(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    const auto stream = static_cast<uint32_t>(core::rng_stream::conflict);

    // Security Council elections on 1 January.
    const auto date = core::date_from_tick(state.current_tick);
    if(date.month == 1 && date.day == 1 && state.current_tick > 0) {
        double gdp_max = 0.0;
        const auto row = rest_of_world(state);
        for(auto n : w.in_nation) gdp_max = std::max(gdp_max, n.get_gdp0());
        for(auto n : w.in_nation) {
            if(n.get_unsc_seat() == 1 && n.get_unsc_term_end() < state.current_tick) n.set_unsc_seat(0);
            if(n.get_unsc_seat() != 0 || n.id == row) continue;
            const float size = static_cast<float>(1.0 + std::log1p(9.0 * n.get_gdp0() / std::max(1.0, gdp_max)));
            if(core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), 0x7C000000u + ix(n.id)) < static_cast<float>(P.unsc_election_rate) * size) {
                n.set_unsc_seat(1);
                n.set_unsc_term_end(state.current_tick + 730);
            }
        }
    }

    // General Assembly alignment and trust.
    if(static_cast<uint32_t>(state.current_tick) % monthly_cadence.period_days == 0) {
        compute_un_ideals(state);
        // Security Council agenda.
        std::vector<dcon::nation_id> members;
        int permanent = 0, elected_in = 0;
        for(auto n : w.in_nation) {
            if(n.get_unsc_seat() != 0) members.push_back(n.id);
            permanent += n.get_unsc_seat() == 2 ? 1 : 0;
            elected_in += n.get_unsc_seat() == 1 ? 1 : 0;
        }
        const int outside = std::max(0, 10 - elected_in);
        const auto row = rest_of_world(state);
        auto vote = [&](dcon::nation_id sponsor, dcon::nation_id target, uint8_t kind, float threshold_shift) -> float {
            int yes = 0;
            bool veto = false;
            for(auto v : members) {
                const float s = support(state, v, sponsor, target) + threshold_shift;
                if(w.nation_get_unsc_seat(v) == 2 && s < static_cast<float>(P.unsc_veto_threshold)) veto = true;
                yes += s > 0.0f ? 1 : 0;
            }
            if(row && support(state, row, sponsor, target) + threshold_shift > 0.0f) yes += outside;
            const float result = veto ? 0.0f : (yes >= 9 ? 1.0f : -1.0f);
            log_un(state, sponsor, target, journal_unsc, static_cast<float>(kind), result);
            return result;
        };
        // The drafter: the permanent member least tied to the target.
        auto drafter = [&](dcon::nation_id target) {
            dcon::nation_id best;
            float lowest = 2.0f;
            for(auto v : members) {
                if(w.nation_get_unsc_seat(v) != 2 || v == target) continue;
                const float ties = std::max(w.nation_get_alliance(v, ix(target)), w.nation_get_trust(v, ix(target)));
                if(ties < lowest) {
                    lowest = ties;
                    best = v;
                }
            }
            return best;
        };
        for(auto n : w.in_nation) {
            n.set_peacekeeping(std::max(0.0f, n.get_peacekeeping() - years));
            float occupying = 0.0f;
            for(auto m : w.in_nation) {
                if(m.id == n.id) continue;
                occupying = std::max(occupying, m.get_control(ix(n.id)));
                // Ceasefire demand on a war pair (each pair once, first month of the war only).
                if(n.id.index() < m.id.index() && n.get_at_war(ix(m.id)) > 0.0f && n.get_hostility(ix(m.id)) == 5.0f) {
                    if(vote(drafter(n.id), n.id, 0, 0.3f) > 0.0f) {
                        n.set_hostility(ix(m.id), 5.5f); // marks the ceasefire demand (read by geopolitics)
                        m.set_hostility(ix(n.id), 5.5f);
                    } else {
                        n.set_hostility(ix(m.id), 5.25f); // considered, not adopted
                        m.set_hostility(ix(n.id), 5.25f);
                    }
                }
            }
            // Sanctions on an occupier.
            if(occupying > 0.05f && n.get_un_sanctioned() <= 0.0f) {
                if(vote(drafter(n.id), n.id, 1, 0.0f) > 0.0f) n.set_un_sanctioned(1.0f);
            } else if(occupying <= 0.0f && n.get_un_sanctioned() > 0.0f) {
                n.set_un_sanctioned(0.0f);
            }
            // Peace operation after insurgency or recently ended war.
            float insurgent = 0.0f;
            for(auto own : n.get_region_ownership()) insurgent = std::max(insurgent, own.get_region().get_insurgency());
            const bool post_war = n.get_war_cost() > 0.0f && n.get_war_effort() <= 0.0f;
            if((insurgent > 0.0f || post_war) && n.get_peacekeeping() <= 0.0f && n.get_unsc_seat() != 2) {
                // Host consent: the government accepts a mission when it trusts the drafter.
                const auto d = drafter(n.id);
                if(d && vote(d, n.id, 2, 0.8f) > 0.0f) n.set_peacekeeping(static_cast<float>(P.peacekeeping_years));
                if(post_war) n.set_war_cost(0.0f);
            }
        }
    }

    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        // WTO disputes filed by n against partners m.
        if(!n.get_wto_member()) continue;
        for(auto m : w.in_nation) {
            if(m.id == n.id || !m.get_wto_member()) continue;
            const auto km = ix(m.id), kn = ix(n.id);
            float filed = n.get_wto_dispute(km);
            const float excess = (m.get_tariff() - m.get_tariff0()) * 100.0f;
            if(filed <= 0.0f) {
                if(excess > static_cast<float>(P.wto_bound) && n.get_trade_share(km) > 0.02f && n.get_at_war(km) <= 0.0f) {
                    n.set_wto_dispute(km, static_cast<float>(state.current_tick) + 1.0f);
                    log_un(state, n.id, m.id, journal_wto, 0.0f, NAN);
                }
                continue;
            }
            if(static_cast<float>(state.current_tick) + 1.0f - filed < static_cast<float>(P.wto_panel_days)) continue;
            // Panel report.
            const uint32_t key = 0x7E000000u + kn * 1024u + km;
            const bool wins = core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), key) < static_cast<float>(P.wto_win);
            n.set_wto_dispute(km, 0.0f);
            if(!wins || excess <= 0.0f) continue;
            if(P.wto_appellate <= 0.0) {
                log_un(state, n.id, m.id, journal_wto, 1.0f, -1.0f);
                continue;
            }
            const float comply = std::clamp(m.get_credibility() * (0.5f + 5.0f * m.get_trade_share(kn)), 0.0f, 0.95f);
            if(core::uniform01(state.rng_seed, stream, static_cast<uint32_t>(state.current_tick), key, 1) < comply) {
                m.set_tariff(m.get_tariff0() + 0.5f * (m.get_tariff() - m.get_tariff0()));
                n.set_wto_retaliation(km, 0.0f);
                log_un(state, n.id, m.id, journal_wto, 1.0f, 1.0f);
            } else {
                n.set_wto_retaliation(km, static_cast<float>(P.wto_retaliation));
                lose_credibility(state, m.id, 0.05f);
                log_un(state, n.id, m.id, journal_wto, 1.0f, 0.0f);
            }
        }
    }
}

} // namespace geosim::sim
