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

#include "core/keys.hpp"
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

// A shared majority religion; the unaffiliated and residual categories share none.
static bool shared_religion(uint32_t a, uint32_t b) {
    constexpr uint32_t none = core::pack_key("none"), other = core::pack_key("othe"), oth = core::pack_key("oth");
    return a != 0 && a == b && a != none && a != other && a != oth && a != core::pack_key("other");
}

float claim_of(game_state const& state, dcon::nation_id claimant, dcon::nation_id holder) {
    return claim_strength(state, claimant, holder);
}

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

// Threat perception of n from m: relative power x proximity x distrust x claims
// (Waltz 1979; Walt 1987; Jervis 1978).
static float threat_from(game_state const& state, dcon::nation_id n, dcon::nation_id m) {
    auto const& w = state.world;
    const float claims = claim_strength(state, n, m) + claim_strength(state, m, n);
    const float proximity = 1.0f / (1.0f + distance_km(state, n, m) / 2000.0f);
    const float relative = w.nation_get_power(m) / std::max(1e-6f, w.nation_get_power(m) + w.nation_get_power(n));
    return relative * proximity * (1.0f - w.nation_get_trust(n, static_cast<uint16_t>(m.index()))) * (1.0f + claims);
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
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) {
            if(a.id != b.id) a.set_threat_to(static_cast<uint16_t>(b.id.index()), threat_from(state, a.id, b.id));
        }
    }
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
        const float budget = n.get_defense_share0() > 0.0f ? std::clamp(n.get_defense_share() / n.get_defense_share0(), 0.3f, 5.0f) : 1.0f;
        n.set_military(n.get_power() * (0.5f + 0.5f * industry) * (1.0f + tech) * std::sqrt(budget));
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
    // Composite Index of National Capability (Singer, Bremer & Stuckey 1972): the mean
    // of world shares in military expenditure, military personnel, energy use, iron and
    // steel (manufacturing output stands in), population and urban population.
    // Personnel are proxied by population x defence share until COW NMC data arrive.
    std::array<double, 6> tot{};
    auto parts = [&](dcon::nation_id x) {
        const double g = w.nation_get_gdp_real(x) > 0.0f ? w.nation_get_gdp_real(x) * 365.0 : w.nation_get_gdp0(x);
        const double pop_x = w.nation_get_population(x);
        const double steel = D.manufacturing_sector ? w.nation_get_output(x, static_cast<uint8_t>(D.manufacturing_sector.index())) : g;
        return std::array<double, 6>{g * w.nation_get_defense_share(x), pop_x * w.nation_get_defense_share(x), static_cast<double>(w.nation_get_oil_demand(x)),
                                     steel, pop_x, pop_x * w.nation_get_urban_share(x)};
    };
    for(auto n : w.in_nation) {
        const auto v = parts(n.id);
        for(std::size_t i = 0; i < 6; ++i) tot[i] += std::max(0.0, v[i]);
    }
    for(auto n : w.in_nation) {
        const auto v = parts(n.id);
        double c = 0.0;
        for(std::size_t i = 0; i < 6; ++i) c += tot[i] > 0.0 ? std::max(0.0, v[i]) / tot[i] : 0.0;
        n.set_cinc(static_cast<float>(c / 6.0));
    }
}

// Export / import factor relative to the start, from bilateral trade shares,
// sanctions and war (used by the economy instead of a world-average index).
// Export access of n to m relative to gravity: partner import demand, sanctions,
// war, shipping delays, FTAs and Armington competition with m's other suppliers.
static double pair_access(game_state const& state, dcon::nation_id n, dcon::nation_id m) {
    auto const& w = state.world;
    auto const& P = state.params;
    const auto k = static_cast<uint8_t>(m.index());
    const auto kn = static_cast<uint8_t>(n.index());
    const float barrier = std::max({w.nation_get_sanction(n, k), w.nation_get_sanction(m, kn)});
    const float war = std::max(w.nation_get_at_war(n, k), w.nation_get_at_war(m, kn));
    const double base = static_cast<double>(w.nation_get_m0(m)) * w.nation_get_gf(m);
    const double demand = base > 0.0 ? w.nation_get_imports(m) / base : 1.0;
    // FTAs phase in a trade gain (treaties.cpp; Baier & Bergstrand 2007).
    // Deeper agreements (Level-I bargaining in treaties.cpp) liberalise more (Dür, Baccini & Elsig 2014).
    const float depth = w.nation_get_fta_depth(n, k) > 0.0f ? w.nation_get_fta_depth(n, k) : 1.0f;
    const double fta = std::exp(P.fta_trade_gain * w.nation_get_fta(n, k) * depth);
    // Armington (1969): in m's market, n competes with m's other suppliers on relative
    // real exchange rates (its competitiveness against m's own producers is the
    // export elasticity in economy.cpp).
    double rival = 0.0, rival_w = 0.0;
    for(auto o : w.in_nation) {
        if(o.id == m) continue;
        const double share = o.get_x0() * o.get_trade_share(k);
        rival += share * std::log(std::max(0.05f, o.get_exchange_rate()));
        rival_w += share;
    }
    const double rel = std::log(std::max(0.05f, w.nation_get_exchange_rate(n))) - (rival_w > 0.0 ? rival / rival_w : 0.0);
    const double armington = std::exp(-(P.armington - 1.0) * rel);
    return w.nation_get_trade_share(n, k) * demand * (1.0 - P.sanction_trade_cut * barrier) * (1.0 - war) * w.nation_get_pair_cost(n, k) * fta * armington;
}

float trade_access(game_state const& state, dcon::nation_id n) {
    double access = 0.0;
    for(auto m : state.world.in_nation) {
        if(m.id != n) access += pair_access(state, n, m.id);
    }
    return static_cast<float>(access);
}

// Bilateral exports of n (real, per day): total exports split by pair access.
void record_bilateral_exports(game_state& state, dcon::nation_id n, float exports) {
    auto& w = state.world;
    double total = 0.0;
    for(auto m : w.in_nation) {
        if(m.id != n) total += pair_access(state, n, m.id);
    }
    for(auto m : w.in_nation) {
        const auto k = static_cast<uint8_t>(m.id.index());
        w.nation_set_bilateral_exports(n, k, m.id == n || total <= 0.0 ? 0.0f : static_cast<float>(exports * pair_access(state, n, m.id) / total));
    }
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
                                     * (0.3f + excluded) * (1.0f + 3.0f * youth_jobless) * peacekeeping_factor(state, n, {})
                                     * (1.0f + static_cast<float>(P.ruggedness_insurgency) * r.get_ruggedness()); // rough terrain (Fearon & Laitin 2003)
                if(u < hazard) {
                    r.set_insurgency(1.0f);
                    log_event(state, n, n, journal_insurgency, 0.0f, 1.0f);
                }
            }
        } else {
            w.region_set_war_deaths(r.id, r.get_war_deaths() + r.get_population() * static_cast<float>(P.war_casualty_rate) * 0.5f * years);
            for(uint8_t s = 0; s < state.derived.sector_count; ++s) r.set_va0(s, r.get_va0(s) * (1.0f - static_cast<float>(P.war_destruction_rate) * 0.5f * years));
            w.nation_set_state_capacity(n, std::max(0.05f, w.nation_get_state_capacity(n) - 0.02f * years));
            // Outside support for either side (diplomacy.cpp).
            float pro = 0.0f, anti = 0.0f;
            insurgency_support(state, n, pro, anti);
            w.nation_set_intervention(n, pro - anti);
            const float end = static_cast<float>(P.insurgency_end_rate) * years * w.nation_get_state_capacity(n) * (r.get_unrest() < 2.0f ? 3.0f : 1.0f)
                              * (1.0f + static_cast<float>(P.intervention_gain) * pro) / (1.0f + static_cast<float>(P.rebel_support) * anti);
            if(u < end) {
                r.set_insurgency(0.0f);
                log_event(state, n, n, journal_insurgency, 1.0f, 0.0f);
            }
        }
    }
}

// Alliance obligations: each ally of `victim` decides whether to join its war with
// `enemy`. Alliances are honoured about three times in four (Leeds 2003), more by
// democracies (Gaubatz 1996), by allies the enemy threatens too, and by allies
// strong enough to matter; a broken promise costs credibility (Gibler 2008).
void call_allies(game_state& state, dcon::nation_id victim, dcon::nation_id enemy) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto kv = static_cast<uint16_t>(victim.index()), ke = static_cast<uint16_t>(enemy.index());
    for(auto a : w.in_nation) {
        if(a.id == victim || a.id == enemy || a.get_alliance(kv) <= 0.0f || a.get_at_war(ke) > 0.0f) continue;
        const float strength = a.get_cinc() / std::max(1e-6f, a.get_cinc() + w.nation_get_cinc(enemy));
        const float p = std::clamp(static_cast<float>(P.alliance_honor) * (0.7f + 0.3f * a.get_electoral()) * (1.0f + a.get_threat_to(ke))
                                       * (0.5f + strength) * a.get_credibility(), 0.0f, 0.95f);
        const bool join = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::conflict), static_cast<uint32_t>(state.current_tick),
                                          0x50000000u + static_cast<uint32_t>(a.id.index()) * 1024u + ke) < p;
        if(join) {
            a.set_at_war(ke, 1.0f);
            w.nation_set_at_war(enemy, static_cast<uint16_t>(a.id.index()), 1.0f);
            a.set_sanction(ke, 1.0f);
            w.nation_set_sanction(enemy, static_cast<uint16_t>(a.id.index()), 1.0f);
        } else {
            lose_credibility(state, a.id, static_cast<float>(P.credibility_loss));
            w.nation_set_trust(victim, static_cast<uint16_t>(a.id.index()), std::max(0.0f, w.nation_get_trust(victim, static_cast<uint16_t>(a.id.index())) - 0.3f));
        }
        const auto d = w.create_decision();
        w.decision_set_agent(d, a.get_executive_seat().get_agent());
        w.decision_set_target(d, a.id);
        w.decision_set_tick(d, state.current_tick);
        w.decision_set_lever(d, journal_alliance_call);
        w.decision_set_before(d, static_cast<float>(enemy.index()));
        w.decision_set_after(d, join ? 1.0f : 0.0f);
        w.decision_set_pragmatic(d, static_cast<float>(victim.index()));
        w.decision_set_ideological(d, p);
        w.decision_set_prior(d, NAN);
    }
}

// Fronts (monthly, per warring pair): the share of each side's territory the other
// holds moves with the Lanchester square law on committed capability (Lanchester
// 1916; Epstein 1985): forces are CINC x war effort, projected over distance with a
// loss-of-strength gradient (Boulding 1962) and multiplied on home ground (the
// defender's advantage; Mearsheimer 1989, Biddle 2004 on force employment).
// The front first rolls back an invader before crossing into its territory.
void update_front(game_state& state, dcon::nation_id n, dcon::nation_id m, float years) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto kn = static_cast<uint16_t>(n.index()), km = static_cast<uint16_t>(m.index());
    const float reach = 1.0f / (1.0f + distance_km(state, n, m) / static_cast<float>(P.supply_km));
    // Committed force: capability x war effort x willingness to serve (politics_ext.cpp).
    const float wn = 0.5f + static_cast<float>(P.willingness_war) * (w.nation_get_willingness(n) - 0.5f);
    const float wm = 0.5f + static_cast<float>(P.willingness_war) * (w.nation_get_willingness(m) - 0.5f);
    const float fn = w.nation_get_cinc(n) * std::max(0.1f, w.nation_get_war_effort(n)) * 2.0f * std::max(0.1f, wn);
    const float fm = w.nation_get_cinc(m) * std::max(0.1f, w.nation_get_war_effort(m)) * 2.0f * std::max(0.1f, wm);
    // Rugged terrain favours the defender (Fearon & Laitin 2003; Tkach 2023 on mountains).
    const float def = static_cast<float>(P.defender_advantage);
    // Fighting on the soil one side already holds; with no line yet, on the soil of the
    // side with less projected strength.
    const bool on_n = w.nation_get_control(n, km) > 0.0f
                      || (w.nation_get_control(m, kn) <= 0.0f && fm >= fn);
    const float attack = on_n ? fm * reach : fn * reach;
    const float defend = (on_n ? fn : fm) * def * (1.0f + w.nation_get_terrain(on_n ? n : m));
    const float adv = (attack * attack - defend * defend) / std::max(1e-12f, attack * attack + defend * defend);
    const float move = static_cast<float>(P.lanchester_speed) * adv * years;
    if(on_n) {
        // m advances into n (adv > 0) or is pushed back.
        float c = w.nation_get_control(n, km) + move;
        if(c < 0.0f) c = 0.0f;
        w.nation_set_control(n, km, std::min(0.6f, c));
    } else {
        float c = w.nation_get_control(m, kn) + move;
        if(c < 0.0f) c = 0.0f;
        w.nation_set_control(m, kn, std::min(0.6f, c));
    }
    for(auto x : {n, m}) {
        float occ = 0.0f;
        for(auto o : w.in_nation) occ += w.nation_get_control(x, static_cast<uint16_t>(o.id.index()));
        w.nation_set_occupied(x, std::min(0.9f, occ));
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
            // Reputation (strategy.cpp) and diaspora ties (affinity) shift trust.
            const float ties = std::min(1.0f, 100.0f * (n.get_diaspora(k) + m.get_diaspora(kn)) / std::max(1.0f, static_cast<float>(n.get_population())));
            // Cultural affinity: common language and religion (Guiso, Sapienza & Zingales 2009),
            // constant, so it sits in the trust level, not in its changes.
            const float culture = (n.get_language() != 0 && n.get_language() == m.get_language() ? static_cast<float>(P.language_trust) : 0.0f)
                                  + (shared_religion(n.get_religion(), m.get_religion()) ? static_cast<float>(P.religion_trust) : 0.0f);
            const float trust_target = std::clamp(0.5f + 0.25f * n.get_alliance(k) - 0.4f * regime_distance(state, n.id, m.id)
                                                      - 0.3f * sanctioned - 0.5f * war - 0.2f * claims
                                                      - static_cast<float>(P.credibility_trust) * (1.0f - m.get_credibility())
                                                      + static_cast<float>(P.affinity_trust) * ties + culture
                                                      - 0.2f * n.get_rivalry(k) - 0.1f * std::max(0.0f, std::min(4.0f, n.get_hostility(k)) - 1.0f)
                                                      + static_cast<float>(P.ga_alignment_trust) * (un_alignment(state, n.id, m.id) - n.get_un_alignment0(k)), 0.0f, 1.0f);
            float trust = n.get_trust(k);
            trust += (trust_target - trust) * std::min(1.0f, years / static_cast<float>(P.trust_years));
            n.set_trust(k, trust);
            const float threat = threat_from(state, n.id, m.id);
            n.set_threat_to(k, threat);

            // Sanctions (two-level: escalation needs a governing majority where elections compete).
            if(exec) {
                const float dependence = n.get_trade_share(k);
                // Reciprocity: a partner's tariff rise invites retaliation.
                const float retaliation = static_cast<float>(P.tariff_retaliation) * std::max(0.0f, m.get_tariff() - m.get_tariff0()) * 10.0f;
                // Energy dependence beyond its 2019 level raises the cost; a secure-energy goal
                // softens sanctions on suppliers (strategy.cpp).
                const float energy = n.get_energy_dependence(k);
                const float pragmatic = std::clamp(static_cast<float>(P.sanction_security_gain) * (threat - 0.2f)
                                                       - static_cast<float>(P.sanction_dependence_cost) * dependence + retaliation
                                                       - static_cast<float>(P.energy_dependence_cost) * (energy - n.get_energy_dependence0(k))
                                                       // Holdings of the target's debt (reserve assets) beyond 2019: the
                                                       // sender's own assets are at stake (Farrell & Newman 2019).
                                                       - static_cast<float>(P.debt_dependence_cost) * (n.get_debt_holdings(k) - n.get_debt_holdings0(k))
                                                       - 10.0f * goal_bias(state, n.id, goal::secure_energy) * energy, 0.0f, 1.0f);
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
                // UNSC sanctions and authorised WTO retaliation set a floor (un.cpp).
                choice = std::clamp(choice, war > 0.0f ? 1.0f : un_sanction_floor(state, n.id, m.id), 1.0f);
                n.set_sanction(k, choice);
                if(std::abs(choice - before) >= 0.1f) log_event(state, n.id, m.id, journal_sanction, before, choice);
            }

            // War onset (bargaining failure) and settlement; each pair handled by the lower index.
            if(n.id.index() < m.id.index()) {
                if(war <= 0.0f && n.get_alliance(k) <= 0.0f && n.get_hostility(k) < 2.0f) {
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
                                       static_cast<uint32_t>(n.id.index()) * 1024u + static_cast<uint32_t>(m.id.index()))
                       < p * static_cast<float>(P.mid_ratio) * (1.0f + static_cast<float>(P.rivalry_gain) * n.get_rivalry(k)) * peacekeeping_factor(state, n.id, m.id)) {
                        // The bargaining failure opens a militarised dispute; war is its
                        // last rung (diplomacy.cpp).
                        start_dispute(state, n.id, m.id);
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
                    update_front(state, n.id, m.id, years);
                    // Battle outcomes reveal the balance of power, so decisive fronts end
                    // wars (bargaining model: Fearon 1995; Slantchev 2003; Reiter 2009).
                    const float decisive = std::max(n.get_control(k), m.get_control(kn));
                    const float settle = static_cast<float>(P.peace_base_rate) * years * (1.0f + 10.0f * (n.get_war_cost() + m.get_war_cost()))
                                         * (1.0f + static_cast<float>(P.occupation_settle) * decisive)
                                         * (1.0f + static_cast<float>(P.mediation_gain) * mediation_score(state, n.id, m.id))
                                         * (n.get_hostility(k) >= 5.5f ? 1.0f + static_cast<float>(P.ceasefire_settle) : 1.0f) // UNSC ceasefire demand
                                         * (2.0f - std::min(n.get_willingness(), m.get_willingness())); // war weariness (Mueller 1973)
                    if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::conflict), static_cast<uint32_t>(state.current_tick),
                                       0x80000000u + static_cast<uint32_t>(n.id.index()) * 1024u + static_cast<uint32_t>(m.id.index())) < settle) {
                        n.set_at_war(k, 0.0f);
                        m.set_at_war(kn, 0.0f);
                        n.set_sanction(k, 0.5f); // lingering hostility
                        m.set_sanction(kn, 0.5f);
                        n.set_hostility(k, 0.0f);
                        m.set_hostility(kn, 0.0f);
                        // Occupied land is returned unless the occupier claims it (then the
                        // line freezes: Fortna 2004 on fragile ceasefires; Vasquez 1993).
                        if(claim_strength(state, m.id, n.id) <= 0.0f) n.set_control(k, 0.0f);
                        if(claim_strength(state, n.id, m.id) <= 0.0f) m.set_control(kn, 0.0f);
                        for(auto x : {n.id, m.id}) {
                            float occ = 0.0f;
                            for(auto o : w.in_nation) occ += w.nation_get_control(x, static_cast<uint16_t>(o.id.index()));
                            w.nation_set_occupied(x, std::min(0.9f, occ));
                        }
                        log_event(state, n.id, m.id, journal_peace, 1.0f, 0.0f);
                    }
                }
            }
        }
    }
}

} // namespace geosim::sim
