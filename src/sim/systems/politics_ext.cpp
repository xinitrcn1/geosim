// Politics extensions (docs/05-politics-institutions.md): courts as appointed bodies,
// term limits, identity legitimacy and willingness to serve.
//
// - Courts (monthly): seats fall vacant at random (mean `court_vacancy_years`).
//   Executives in liberal systems appoint professionals (independence drifts back to
//   the institutional level); autocratising executives pack the court, cutting
//   independence (Helmke 2017; Ginsburg & Moustafa 2008; Hungary, Poland, Turkey).
// - Term limits (at elections, presidential systems): a leader at the limit steps
//   down, a new leader of the same party takes over; where institutions are weak the
//   incumbent may evade the limit at a cost to liberal institutions (Versteeg et al.
//   2020; Baturo 2014).
// - Identity legitimacy: inclusion of identity groups (power-sharing, exclusion and
//   mobilisation relative to 2019; Gilley 2009; Wimmer 2002) - the third pillar of
//   legitimacy in politics.cpp. Per-pop legitimacy enters through each pop's grievance.
// - Willingness to serve (consent to fight; Levi 1997; Horowitz, Simpson & Stam 2011):
//   legitimacy, traditional values and threat raise it, war weariness lowers it
//   (Mueller 1973); it scales committed force on the front and the war-settlement hazard.

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};

void log_entry(game_state& state, dcon::nation_id n, uint8_t code, float before, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, code);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, NAN);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

float exclusion_of(game_state const& state, dcon::nation_id n) {
    float x = 0.0f;
    for(auto g : state.world.in_social_group) {
        if(g.get_nation() == n) x += g.get_pop_share() * g.get_excluded() * (0.5f + g.get_mobilisation());
    }
    return x;
}
} // namespace

void term_limit_check(game_state& state, dcon::nation_id n, bool same_party) {
    auto& w = state.world;
    auto const& P = state.params;
    const float limit = w.nation_get_term_limit_years(n);
    if(limit <= 0.0f || !w.nation_get_presidential(n) || !same_party) return;
    if(w.nation_get_tenure_days(n) < limit * 365.0f - 30.0f) return;
    const float evade = (1.0f - w.nation_get_liberal(n)) * static_cast<float>(P.term_evasion);
    if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::elections), static_cast<uint32_t>(state.current_tick),
                       0x6C000000u + static_cast<uint32_t>(n.index())) < evade) {
        w.nation_set_liberal(n, std::max(0.0f, w.nation_get_liberal(n) - 0.05f));
        w.nation_set_judicial(n, std::max(0.0f, w.nation_get_judicial(n) - 0.05f));
        log_entry(state, n, journal_term_limit, w.nation_get_tenure_days(n) / 365.0f, 1.0f);
        return;
    }
    // A successor from the same party: personal rigidity resets toward the party norm.
    if(auto exec = w.nation_get_agent_from_executive_seat(n)) {
        w.agent_set_rho_base(exec, 0.5f * w.agent_get_rho_base(exec) + 0.15f);
    }
    w.nation_set_tenure_days(n, 0.0f);
    log_entry(state, n, journal_term_limit, limit, 0.0f);
}

void init_politics_ext(game_state& state) {
    for(auto n : state.world.in_nation) {
        n.set_judicial(n.get_liberal());
        n.set_exclusion0(exclusion_of(state, n.id));
        n.set_identity_legitimacy(0.7f);
        n.set_willingness(0.5f);
        n.set_court_vacancy_days(0.0f);
    }
}

void update_politics_ext(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float days = static_cast<float>(monthly_cadence.period_days);
    const float years = days / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        n.set_tenure_days(n.get_tenure_days() + days);
        // Court vacancies and appointments.
        const float p = years / static_cast<float>(P.court_vacancy_years) ;
        if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::elections), static_cast<uint32_t>(state.current_tick),
                           0x6D000000u + static_cast<uint32_t>(n.id.index())) < p) {
            const float before = n.get_judicial();
            float after = before;
            if(n.get_liberal() < 0.5f) after = std::max(0.0f, before - static_cast<float>(P.court_capture) * (1.0f - n.get_liberal()));
            else after = before + (n.get_liberal() - before) * 0.3f;
            n.set_judicial(after);
            if(std::abs(after - before) > 0.01f) log_entry(state, n.id, journal_court_seat, before, after);
        }
        // Identity legitimacy.
        const float excl = exclusion_of(state, n.id);
        n.set_identity_legitimacy(std::clamp(0.7f - 5.0f * static_cast<float>(P.identity_weight) * (excl - n.get_exclusion0()), 0.0f, 1.0f));
        // Willingness to serve, relative to 0.5 at the start.
        float threat = 0.0f;
        for(auto m : w.in_nation) {
            if(m.id != n.id) threat = std::max(threat, n.get_threat_to(static_cast<uint16_t>(m.id.index())));
        }
        const float weariness = std::min(1.0f, 5.0f * n.get_war_cost());
        const float target = std::clamp(0.5f + 0.5f * (n.get_legitimacy() - n.get_legitimacy0()) + 0.5f * (threat - n.get_threat_max0())
                                            + 0.3f * n.get_occupied() - 0.4f * weariness, 0.05f, 1.0f);
        n.set_willingness(n.get_willingness() + (target - n.get_willingness()) * std::min(1.0f, years * 2.0f));
    }
}

} // namespace geosim::sim
