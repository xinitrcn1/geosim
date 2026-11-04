// Geopolitics (docs/06-geopolitics-ai.md): every 28 days per nation shard.
//
// Relations are nation arrays indexed by the partner nation: trust, threat,
// trade share (gravity), sanctions, alliance, war.
// - Trust drifts toward a target set by alliances, regime distance, sanctions,
//   claims and war. Threat = partner's relative power (a CINC-like GDP and
//   population index) x proximity x distrust x claims (Waltz; Jervis 1978).
// - Sanctions are executive decisions per partner: pragmatic (security gain vs
//   trade dependence), ideological (regime distance) and the status quo as
//   the anchor. Escalations need legislative ratification in competitive
//   systems (Putnam 1988, two-level games).
// - War onset follows the bargaining model (Fearon 1995): claims (issue
//   indivisibility), power shifts (commitment problems), information gaps,
//   with democratic and commercial peace and alliances reducing it. Drawn from
//   the counter-based RNG. Wars cut trade, destroy capacity in claimed and
//   capital regions, kill, and end with a probability rising with their cost.

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

float distance_km(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    auto const& w = state.world;
    constexpr float rad = std::numbers::pi_v<float> / 180.0f;
    const float la1 = w.nation_get_lat(a) * rad, la2 = w.nation_get_lat(b) * rad;
    const float dlat = la2 - la1, dlon = (w.nation_get_lon(b) - w.nation_get_lon(a)) * rad;
    const float h = std::sin(dlat / 2) * std::sin(dlat / 2) + std::cos(la1) * std::cos(la2) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0f * 6371.0f * std::asin(std::min(1.0f, std::sqrt(h)));
}

float claim_strength(game_state const& state, dcon::nation_id claimant, dcon::nation_id holder) {
    float s = 0.0f;
    for(auto c : state.world.nation_get_region_claim(claimant)) {
        if(c.get_claimed().get_owner() == holder) s = std::max(s, c.get_strength());
    }
    return s;
}

float regime_distance(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    return std::abs(state.world.nation_get_voice(a) - state.world.nation_get_voice(b)) / 5.0f;
}

void log_event(game_state& state, dcon::nation_id n, dcon::nation_id other, uint8_t code, float before, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, code);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, static_cast<float>(other.index())); // partner nation index
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

} // namespace

// Gravity trade shares (exports of a to b ~ GDP_b / distance) and starting
// relations; called once by the scenario builder after curated relations.
void init_geopolitics(game_state& state) {
    auto& w = state.world;
    for(auto a : w.in_nation) {
        double total = 0.0;
        for(auto b : w.in_nation) {
            if(a.id == b.id) continue;
            total += b.get_gdp0() / std::max(500.0f, distance_km(state, a.id, b.id));
        }
        for(auto b : w.in_nation) {
            const auto k = static_cast<uint8_t>(b.id.index());
            const float share = a.id == b.id || total <= 0.0
                                    ? 0.0f
                                    : static_cast<float>(b.get_gdp0() / std::max(500.0f, distance_km(state, a.id, b.id)) / total);
            a.set_trade_share(k, share);
            a.set_trust(k, a.id == b.id ? 1.0f
                                        : std::clamp(0.5f + 0.25f * a.get_alliance(k) - 0.4f * regime_distance(state, a.id, b.id)
                                                         - 0.3f * a.get_sanction(k), 0.0f, 1.0f));
        }
    }
    update_power(state);
}

void update_power(game_state& state) {
    auto& w = state.world;
    double gdp = 0.0, pop = 0.0;
    for(auto n : w.in_nation) {
        gdp += n.get_gdp0() * n.get_gf();
        pop += n.get_population();
    }
    for(auto n : w.in_nation) {
        const double g = n.get_gdp_real() > 0.0f ? n.get_gdp_real() * 365.0 : n.get_gdp0();
        n.set_power(static_cast<float>(0.6 * g / std::max(1.0, gdp) + 0.4 * n.get_population() / std::max(1.0, pop)));
    }
}

// Export / import factor relative to the start, from bilateral trade shares,
// sanctions and war (used by the economy instead of a world-average index).
float trade_access(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    auto const& P = state.params;
    double access = 0.0;
    for(auto m : w.in_nation) {
        if(m.id == n) continue;
        const auto k = static_cast<uint8_t>(m.id.index());
        const auto kn = static_cast<uint8_t>(n.index());
        const float barrier = std::max({w.nation_get_sanction(n, k), w.nation_get_sanction(m.id, kn)});
        const float war = std::max(w.nation_get_at_war(n, k), w.nation_get_at_war(m.id, kn));
        const double base = static_cast<double>(m.get_m0()) * m.get_gf();
        const double demand = base > 0.0 ? m.get_imports() / base : 1.0;
        access += w.nation_get_trade_share(n, k) * demand * (1.0 - P.sanction_trade_cut * barrier) * (1.0 - war);
    }
    return static_cast<float>(access);
}

void update_geopolitics(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    if(static_cast<uint32_t>(state.current_tick) % monthly_cadence.period_days == 0) {
        update_power(state);
    }
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const auto exec = n.get_executive_seat().get_agent();
        const auto kn = static_cast<uint8_t>(n.id.index());
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const auto k = static_cast<uint8_t>(m.id.index());
            const float claims = claim_strength(state, n.id, m.id) + claim_strength(state, m.id, n.id);
            const float sanctioned = std::max(n.get_sanction(k), m.get_sanction(kn));
            const float war = n.get_at_war(k);

            // Trust and threat.
            const float trust_target = std::clamp(0.5f + 0.25f * n.get_alliance(k) - 0.4f * regime_distance(state, n.id, m.id)
                                                      - 0.3f * sanctioned - 0.5f * war - 0.2f * claims, 0.0f, 1.0f);
            float trust = n.get_trust(k);
            trust += (trust_target - trust) * std::min(1.0f, years / static_cast<float>(P.trust_years));
            n.set_trust(k, trust);
            const float proximity = 1.0f / (1.0f + distance_km(state, n.id, m.id) / 2000.0f);
            const float relative = m.get_power() / std::max(1e-6f, m.get_power() + n.get_power());
            const float threat = relative * proximity * (1.0f - trust) * (1.0f + claims);
            n.set_threat_to(k, threat);

            // Sanctions (two-level: escalation needs a governing majority where elections compete).
            if(exec) {
                const float dependence = n.get_trade_share(k);
                const float pragmatic = std::clamp(static_cast<float>(P.sanction_security_gain) * (threat - 0.2f)
                                                       - static_cast<float>(P.sanction_dependence_cost) * dependence, 0.0f, 1.0f);
                const float ideological = std::clamp(static_cast<float>(P.sanction_ideology_gain) * (regime_distance(state, n.id, m.id) - 0.3f),
                                                     0.0f, 1.0f);
                const float rho = exec.get_rho();
                const bool anchored = P.historical_anchors != 0.0;
                const float wa = anchored ? static_cast<float>(P.anchor_prior_weight) : 0.0f;
                float choice = ((1.0f - rho) * pragmatic + rho * ideological + wa * n.get_sanction_prior(k)) / (1.0f + wa);
                const float before = n.get_sanction(k);
                if(choice > before + 0.1f && n.get_competitive() && n.get_gov_seat_share() <= 0.5f) {
                    choice = before + 0.1f; // the legislature will not ratify more
                }
                choice = std::clamp(choice, war > 0.0f ? 1.0f : 0.0f, 1.0f);
                n.set_sanction(k, choice);
                if(std::abs(choice - before) >= 0.1f) log_event(state, n.id, m.id, journal_sanction, before, choice);
            }

            // War onset (bargaining failure) and settlement; each pair handled by the lower index.
            if(n.id.index() < m.id.index()) {
                if(war <= 0.0f && n.get_alliance(k) <= 0.0f) {
                    const float democratic = n.get_electoral() * m.get_electoral();
                    const float commerce = std::min(1.0f, 5.0f * (n.get_trade_share(k) + m.get_trade_share(kn)));
                    const float shift = std::abs(n.get_trend_growth() - m.get_trend_growth()) * 10.0f;
                    const float info = 1.0f - 0.5f * (n.get_media_freedom() + m.get_media_freedom()) * 0.5f;
                    const float p = static_cast<float>(P.war_base_rate) * years * (1.0f + 20.0f * claims) * (1.0f + shift) * info
                                    * (1.0f - 0.8f * democratic) * (1.0f - 0.7f * commerce) * std::max(threat, m.get_threat_to(kn));
                    if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::conflict), static_cast<uint32_t>(state.current_tick),
                                       static_cast<uint32_t>(n.id.index()) * 1024u + static_cast<uint32_t>(m.id.index())) < p) {
                        n.set_at_war(k, 1.0f);
                        m.set_at_war(kn, 1.0f);
                        n.set_sanction(k, 1.0f);
                        m.set_sanction(kn, 1.0f);
                        log_event(state, n.id, m.id, journal_war, 0.0f, 1.0f);
                    }
                } else if(war > 0.0f) {
                    // Costs: casualties and destruction in the front regions (capital or claimed).
                    for(auto side : {n.id, m.id}) {
                        const auto front = capital_region(state, side);
                        if(!front) continue;
                        w.region_set_war_deaths(front, w.region_get_war_deaths(front) + w.region_get_population(front) * static_cast<float>(P.war_casualty_rate) * years);
                        for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
                            w.region_set_va0(front, s, w.region_get_va0(front, s) * (1.0f - static_cast<float>(P.war_destruction_rate) * years));
                        }
                        w.nation_set_war_cost(side, w.nation_get_war_cost(side) + static_cast<float>(P.war_destruction_rate) * years);
                    }
                    const float settle = static_cast<float>(P.peace_base_rate) * years * (1.0f + 10.0f * (n.get_war_cost() + m.get_war_cost()));
                    if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::conflict), static_cast<uint32_t>(state.current_tick),
                                       0x80000000u + static_cast<uint32_t>(n.id.index()) * 1024u + static_cast<uint32_t>(m.id.index())) < settle) {
                        n.set_at_war(k, 0.0f);
                        m.set_at_war(kn, 0.0f);
                        n.set_sanction(k, 0.5f); // lingering hostility
                        m.set_sanction(kn, 0.5f);
                        log_event(state, n.id, m.id, journal_peace, 1.0f, 0.0f);
                    }
                }
            }
        }
    }
}

} // namespace geosim::sim
