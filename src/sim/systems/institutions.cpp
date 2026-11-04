// Institutions (docs/05-politics-institutions.md): legislatures, veto players,
// confidence votes, courts and the military.
//
// Legislation. Levers set by law (fiscal packages, taxes, tariffs, the minimum
// wage, public investment, defence, internet laws) go through the legislature as
// bills. Each party's seats support a bill if it governs (discipline) or if the
// bill moves policy toward the party's position in value space; a salient threat
// adds cross-party support (crisis legislation). Passing needs a majority plus a
// premium for every veto point (bicameralism, presidential veto, federalism;
// Henisz POLCON; Tsebelis 2002). A bill short of support passes in part - the
// change the pivotal veto players accept - and executives without strong
// legislative constraints can decree part of it. Regimes without competitive
// legislatures decree.
//
// Confidence. In parliamentary systems a minority government, or one whose bills
// keep failing, can lose a confidence vote; a snap election follows.
//
// Courts. An independent judiciary (liberal index) strikes down levers forced
// beyond their legal range.
//
// Military. A military agent's loyalty follows legitimacy, its budget relative to
// the start, war losses and its ideological distance to the executive (Powell
// 2012; Svolik 2012). Coups need low loyalty and an opportunity (unrest, low
// legitimacy). Facing mass protest beyond ~3.5% participation, a disloyal
// military defects and the executive falls (Chenoweth & Stephan 2011): a regime
// change, with elections where they exist.

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

void journal_entry(game_state& state, dcon::nation_id n, uint8_t code, float pragmatic, float before, float after, float prior) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, code);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, pragmatic);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, prior);
}

dcon::agent_id military_of(game_state const& state, dcon::nation_id n) {
    return state.world.nation_get_military_agent(n);
}

} // namespace

float enact(game_state& state, dcon::nation_id n, uint8_t lev, float status_quo, float proposed, value_axis axis, float shift_per_unit,
            bool crisis) {
    auto& w = state.world;
    auto const& P = state.params;
    const float change = proposed - status_quo;
    if(std::abs(change) < 1e-4f || !w.nation_get_competitive(n)) {
        return proposed; // no bill, or decree in a non-competitive regime
    }
    const auto a = static_cast<uint8_t>(axis);
    const float before = w.nation_get_policy_position(n, a);
    const float after = before + shift_per_unit * change;
    float support = 0.0f;
    for(auto m : w.nation_get_party_membership(n)) {
        auto p = m.get_party();
        const float seats = p.get_seat_share();
        float yes = 0.0f;
        if(p.get_in_government()) {
            yes = static_cast<float>(P.bill_discipline);
        } else {
            const float gain = std::abs(p.get_position(a) - before) - std::abs(p.get_position(a) - after);
            yes = 1.0f / (1.0f + std::exp(-20.0f * gain));
            if(crisis) yes = std::min(1.0f, yes + static_cast<float>(P.bill_rally) * w.nation_get_threat(n));
        }
        support += seats * yes;
    }
    const float veto = w.nation_get_veto(n);
    const float needed = static_cast<float>(P.bill_threshold + P.bill_veto_threshold * veto);
    float fraction = 1.0f;
    if(support < needed) {
        const float partial = std::pow(std::clamp(support / needed, 0.0f, 1.0f), 1.0f + 3.0f * veto);
        const float decree = (1.0f - w.nation_get_liberal(n)) * (w.nation_get_presidential(n) ? 0.5f : 0.3f);
        fraction = std::max(partial, decree);
        w.nation_set_bills_failed(n, w.nation_get_bills_failed(n) + 1.0f);
    }
    const float enacted = status_quo + change * fraction;
    if(std::abs(change) > 0.02f * std::max(1.0f, std::abs(status_quo))) {
        journal_entry(state, n, journal_bill, static_cast<float>(lev), status_quo, enacted, support);
    }
    return enacted;
}

void update_institutions(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        n.set_bills_failed(n.get_bills_failed() * std::exp(-years)); // memory of failed bills fades over a year

        // Confidence votes (parliamentary, competitive).
        if(n.get_competitive() && !n.get_presidential() && n.get_next_election() > state.current_tick + 90) {
            const float minority = n.get_gov_seat_share() < 0.5f ? 1.0f : 0.2f;
            const float hazard = static_cast<float>(P.no_confidence_rate) * years * minority * (1.0f - n.get_approval()) * (0.5f + n.get_bills_failed());
            if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::elections), static_cast<uint32_t>(state.current_tick),
                               0x10000u + static_cast<uint32_t>(n.id.index())) < hazard) {
                n.set_next_election(state.current_tick + static_cast<int32_t>(P.snap_election_days));
                journal_entry(state, n.id, journal_no_confidence, 0.0f, n.get_gov_seat_share(), n.get_approval(), NAN);
            }
        }

        // Courts: forced levers beyond the legal range are struck down.
        n.set_judicial(n.get_liberal());
        if(auto exec = n.get_executive_seat().get_agent()) {
            for(uint8_t l = 0; l < D.lever_count; ++l) {
                if(exec.get_override_mode(l) != static_cast<uint8_t>(override_mode::force)) continue;
                const float u = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::elections),
                                                static_cast<uint32_t>(state.current_tick), 0x20000u + static_cast<uint32_t>(n.id.index()) * 64u + l);
                if(u < static_cast<float>(P.court_rate) * n.get_judicial()) {
                    exec.set_override_mode(l, static_cast<uint8_t>(override_mode::none));
                    journal_entry(state, n.id, journal_court, static_cast<float>(l), 1.0f, 0.0f, NAN);
                }
            }
        }

        // Military loyalty.
        const auto mil = military_of(state, n.id);
        if(!mil) continue;
        const auto exec = n.get_executive_seat().get_agent();
        const float budget = n.get_defense_share0() > 0.0f ? n.get_defense_share() / n.get_defense_share0() : 1.0f;
        const float distance = exec ? std::abs(w.agent_get_order_ideal(mil) - exec.get_order_ideal()) : 0.0f;
        const float target = std::clamp(0.4f + 0.4f * n.get_legitimacy() + 0.15f * (budget - 1.0f) - 0.5f * n.get_war_cost() - 0.3f * distance, 0.0f, 1.0f);
        float loyalty = n.get_military_loyalty();
        loyalty += (target - loyalty) * std::min(1.0f, years / static_cast<float>(P.military_loyalty_years));
        n.set_military_loyalty(loyalty);

        // Defection under mass protest: regime change.
        float protest = 0.0f, pop = 0.0f;
        for(auto own : n.get_region_ownership()) {
            protest += own.get_region().get_protest() * own.get_region().get_population();
            pop += own.get_region().get_population();
        }
        protest = pop > 0.0f ? protest / pop : 0.0f;
        if(protest >= static_cast<float>(P.defection_participation) && loyalty < 0.5f) {
            journal_entry(state, n.id, journal_regime_change, protest, loyalty, 1.0f, NAN);
            // The new rulers answer to the crowd: more liberal, less order-minded, legitimacy reset.
            n.set_liberal(std::min(1.0f, n.get_liberal() + 0.15f));
            n.set_electoral(std::min(1.0f, n.get_electoral() + 0.15f));
            n.set_legitimacy(0.6f);
            n.set_military_loyalty(0.6f);
            if(exec) {
                exec.set_order_ideal(std::max(0.0f, exec.get_order_ideal() - 0.2f));
                exec.set_rho_base(std::max(0.1f, exec.get_rho_base() - 0.1f));
            }
            n.set_info_control(n.get_info_control0());
            if(n.get_competitive() || n.get_electoral() > 0.4f) {
                n.set_competitive(1);
                if(n.get_electoral_system() == static_cast<uint8_t>(electoral_system::none)) {
                    n.set_electoral_system(static_cast<uint8_t>(electoral_system::pr));
                }
                n.set_next_election(state.current_tick + 180);
            }
            for(auto own : n.get_region_ownership()) own.get_region().set_protest(own.get_region().get_protest() * 0.3f);
        }
    }
}

// Defence spending (% GDP), a bill: pragmatic from external threat and war,
// ideological (order-minded rulers spend more).
void decide_defense(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::defense));
    float threat = 0.0f;
    for(auto m : w.in_nation) {
        if(m.id != n) threat = std::max(threat, w.nation_get_threat_to(n, static_cast<uint8_t>(m.id.index())));
    }
    const float base = w.nation_get_defense_share0(n);
    // The 2019 budget answers the threats of 2019; spending rises with threat beyond a
    // moderate level and on a war footing.
    const float pragmatic = base * (1.0f + static_cast<float>(P.defense_threat) * 40.0f * std::max(0.0f, threat - 0.3f)) + 3.0f * w.nation_get_war_effort(n);
    const float ideal = pragmatic * (1.0f + 0.5f * (w.agent_get_order_ideal(a) - 0.2f));
    const float rho = w.agent_get_rho_base(a);
    const float proposed = std::max(0.0f, (1.0f - rho) * pragmatic + rho * ideal);
    const float before = w.nation_get_defense_share(n);
    float choice = before + (proposed - before) * 0.1f; // budgets move slowly
    if(w.agent_get_override_mode(a, lev) != 0) choice = w.agent_get_override_value(a, lev);
    choice = enact(state, n, lev, before, choice, value_axis::self_expression, -0.02f, false);
    w.agent_set_last_pragmatic(a, lev, pragmatic);
    w.agent_set_last_ideological(a, lev, ideal);
    w.agent_set_last_choice(a, lev, choice);
    w.nation_set_defense_share(n, std::clamp(choice, 0.0f, 30.0f));
}

} // namespace geosim::sim
