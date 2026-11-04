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
// - War is emergent from the economy: the hazard rises with the target's
//   resource rents (rapacity; Collier & Hoeffler 2004; Dube & Vargas 2013),
//   with domestic strain in regimes that face no election (diversion; Levy
//   1989; Tir 2010), and with gaps in realised growth that shift power
//   (Organski & Kugler 1980; Fearon's commitment problem); trade dependence
//   lowers it (Oneal & Russett 1999). Military capability is built on the
//   economy: output, industry and a technology base for precision strike and
//   drones (Horowitz 2010; Fuhrmann & Horowitz 2017). Precision strike raises
//   the destruction an attacker inflicts and reaches energy infrastructure
//   across the defender, not only the front (Calcara et al. 2022 caution that
//   defences adapt: the effect scales with the attacker's edge). Wars put
//   the belligerents on a war economy (military procurement, war_spending).
// - Insurgency (civil war): sustained insurgency-level unrest can turn into
//   armed rebellion where the state is weak, groups are excluded and the
//   young lack jobs (Fearon & Laitin 2003; Cederman, Wimmer & Min 2010;
//   Collier & Hoeffler 2004), with deaths and destruction in the region.

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

// Effective trade distance: sea route for seaborne trade, roads for overland trade
// (logistics.cpp), or the great circle when there is no shipping network.
static float effective_distance(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    const float d = state.world.nation_get_trade_distance(a, static_cast<uint8_t>(b.index()));
    return std::max(500.0f, d > 0.0f ? d : distance_km(state, a, b));
}

// Gravity weight (Tinbergen 1962; Head & Mayer 2014): destination GDP over distance
// (elasticity ~1), with a shared land border roughly doubling trade (contiguity).
static double gravity(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    const float land = state.world.nation_get_land_share(a, static_cast<uint8_t>(b.index()));
    return state.world.nation_get_gdp0(b) / effective_distance(state, a, b) * (1.0 + 0.9 * land);
}

// Gravity trade shares (exports of a to b) and starting relations; called once by
// the scenario builder after curated relations.
void init_geopolitics(game_state& state) {
    auto& w = state.world;
    for(auto a : w.in_nation) {
        double total = 0.0;
        for(auto b : w.in_nation) {
            if(a.id == b.id) continue;
            total += gravity(state, a.id, b.id);
        }
        for(auto b : w.in_nation) {
            const auto k = static_cast<uint8_t>(b.id.index());
            const float share = a.id == b.id || total <= 0.0 ? 0.0f : static_cast<float>(gravity(state, a.id, b.id) / total);
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
    auto const& D = state.derived;
    double gdp = 0.0, pop = 0.0;
    for(auto n : w.in_nation) {
        gdp += n.get_gdp0() * n.get_gf();
        pop += n.get_population();
    }
    for(auto n : w.in_nation) {
        const double g = n.get_gdp_real() > 0.0f ? n.get_gdp_real() * 365.0 : n.get_gdp0();
        n.set_power(static_cast<float>(0.6 * g / std::max(1.0, gdp) + 0.4 * n.get_population() / std::max(1.0, pop)));
        // Military capability: power x industry (manufacturing output vs. its baseline) x
        // a technology base (skills x state capacity) for precision strike and drones.
        float industry = 1.0f;
        if(D.manufacturing_sector) {
            const auto ms = static_cast<uint8_t>(D.manufacturing_sector.index());
            const float base = n.get_va0(ms) * std::max(0.01f, n.get_gf());
            industry = base > 0.0f ? std::clamp(n.get_output(ms) / base, 0.2f, 2.0f) : 1.0f;
        }
        const float tech = std::min(1.0f, 2.0f * n.get_tertiary_share()) * n.get_state_capacity();
        n.set_military(n.get_power() * (0.5f + 0.5f * industry) * (1.0f + tech));
        // Resource rents relative to GDP (oil, gas, coal, metals at current prices).
        double rents = 0.0;
        if(D.oil) rents += static_cast<double>(n.get_oil_production()) * 1e6 * 365.0 * w.commodity_get_price(D.oil);
        for(auto c : w.in_commodity) {
            if(c.id == D.oil || c.get_world_value() <= 0.0f) continue;
            const auto ci = static_cast<uint8_t>(c.id.index());
            if(ci < n.get_commodity_prod_size()) rents += static_cast<double>(n.get_commodity_prod(ci)) * c.get_world_value() * 1e9 * c.get_price() / c.get_price0();
        }
        n.set_resource_rents(static_cast<float>(rents / std::max(1.0, n.get_gdp0() * n.get_gf())));
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
        // Persistent shipping delays on the route also cut the pair's trade (logistics.cpp).
        access += w.nation_get_trade_share(n, k) * demand * (1.0 - P.sanction_trade_cut * barrier) * (1.0 - war) * w.nation_get_pair_cost(n, k);
    }
    return static_cast<float>(access);
}

// Insurgency (civil war) per region, every 28 days on the nation's shard.
static void update_insurgency(game_state& state, dcon::nation_id n, float years) {
    auto& w = state.world;
    auto const& P = state.params;
    float excluded = 0.0f;
    for(auto g : w.in_social_group) {
        if(g.get_nation() == n) excluded = std::max(excluded, g.get_excluded() * g.get_mobilisation() * 5.0f);
    }
    excluded = std::min(1.0f, excluded);
    for(auto own : w.nation_get_region_ownership(n)) {
        auto r = own.get_region();
        r.set_crisis_days(r.get_unrest() >= 3.5f ? r.get_crisis_days() + 28.0f : std::max(0.0f, r.get_crisis_days() - 28.0f));
        const auto ri = static_cast<uint32_t>(r.id.index());
        const float u = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::conflict), static_cast<uint32_t>(state.current_tick), 0x40000000u + ri);
        if(r.get_insurgency() <= 0.0f) {
            if(r.get_crisis_days() >= static_cast<float>(P.insurgency_days)) {
                const float youth_jobless = std::max(0.0f, w.nation_get_unemployment(n) * w.nation_get_youth_unemp_rel(n) - 0.1f);
                const float hazard = static_cast<float>(P.insurgency_rate) * years * (1.0f - w.nation_get_state_capacity(n))
                                     * (0.3f + excluded) * (1.0f + 3.0f * youth_jobless);
                if(u < hazard) {
                    r.set_insurgency(1.0f);
                    log_event(state, n, n, journal_insurgency, 0.0f, 1.0f);
                }
            }
        } else {
            w.region_set_war_deaths(r.id, r.get_war_deaths() + r.get_population() * static_cast<float>(P.war_casualty_rate) * 0.5f * years);
            for(uint8_t s = 0; s < state.derived.sector_count; ++s) r.set_va0(s, r.get_va0(s) * (1.0f - static_cast<float>(P.war_destruction_rate) * 0.5f * years));
            w.nation_set_state_capacity(n, std::max(0.05f, w.nation_get_state_capacity(n) - 0.02f * years));
            const float end = static_cast<float>(P.insurgency_end_rate) * years * w.nation_get_state_capacity(n) * (r.get_unrest() < 2.0f ? 3.0f : 1.0f);
            if(u < end) {
                r.set_insurgency(0.0f);
                log_event(state, n, n, journal_insurgency, 1.0f, 0.0f);
            }
        }
    }
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
        update_insurgency(state, n.id, years);
        // War effort: the economy is on a war footing while any interstate war lasts.
        float effort = 0.0f;
        for(auto m : w.in_nation) effort = std::max(effort, n.get_at_war(static_cast<uint8_t>(m.id.index())));
        n.set_war_effort(effort);
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
                    // Economic drivers: rents to capture, domestic strain to divert, realised
                    // growth gaps that shift power.
                    const float rents = static_cast<float>(P.war_rents) * std::max(n.get_resource_rents(), m.get_resource_rents())
                                        * std::min(1.0f, claims + 0.2f);
                    auto unrest_of = [&](dcon::nation_id x) {
                        float u = 0.0f, pop = 0.0f;
                        for(auto own : w.nation_get_region_ownership(x)) {
                            u += own.get_region().get_unrest() * own.get_region().get_population();
                            pop += own.get_region().get_population();
                        }
                        return pop > 0.0f ? u / pop : 0.0f;
                    };
                    const float diversion = static_cast<float>(P.war_diversion)
                                            * std::max((1.0f - n.get_electoral()) * std::max(0.0f, unrest_of(n.id) - 1.0f),
                                                       (1.0f - m.get_electoral()) * std::max(0.0f, unrest_of(m.id) - 1.0f));
                    auto level = [&](dcon::nation_id x) {
                        return w.nation_get_gdp_real(x) / std::max(1.0f, w.nation_get_gdp0_daily(x) * w.nation_get_gf(x));
                    };
                    const float transition = static_cast<float>(P.war_transition) * std::abs(level(n.id) - level(m.id));
                    const float p = static_cast<float>(P.war_base_rate) * years * (1.0f + 20.0f * claims) * (1.0f + shift + transition) * info
                                    * (1.0f + rents) * (1.0f + diversion)
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
                    // Costs: casualties and destruction in the front regions (capital or claimed),
                    // scaled by the enemy's relative capability and precision-strike edge; strikes
                    // also reach energy infrastructure across the defender.
                    for(auto side : {n.id, m.id}) {
                        const auto enemy = side == n.id ? m.id : n.id;
                        const float own_mil = w.nation_get_military(side), enemy_mil = w.nation_get_military(enemy);
                        const float edge = enemy_mil / std::max(1e-6f, own_mil + enemy_mil);
                        const float enemy_tech = std::min(1.0f, 2.0f * w.nation_get_tertiary_share(enemy)) * w.nation_get_state_capacity(enemy);
                        const float own_tech = std::min(1.0f, 2.0f * w.nation_get_tertiary_share(side)) * w.nation_get_state_capacity(side);
                        const float strike = 1.0f + static_cast<float>(P.drone_strike) * std::max(0.0f, enemy_tech - 0.5f * own_tech);
                        const float destruction = static_cast<float>(P.war_destruction_rate) * 2.0f * edge * strike * years;
                        const auto front = capital_region(state, side);
                        if(front) {
                            w.region_set_war_deaths(front, w.region_get_war_deaths(front) + w.region_get_population(front) * static_cast<float>(P.war_casualty_rate) * 2.0f * edge * years);
                            for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
                                w.region_set_va0(front, s, w.region_get_va0(front, s) * (1.0f - destruction));
                            }
                        }
                        if(state.derived.energy_sector) {
                            const auto es = static_cast<uint8_t>(state.derived.energy_sector.index());
                            const float hit = static_cast<float>(P.drone_infrastructure) * enemy_tech * years;
                            for(auto own : w.nation_get_region_ownership(side)) own.get_region().set_va0(es, own.get_region().get_va0(es) * (1.0f - hit));
                        }
                        w.nation_set_war_cost(side, w.nation_get_war_cost(side) + destruction);
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
