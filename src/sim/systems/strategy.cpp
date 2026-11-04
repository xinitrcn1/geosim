// Strategic layer, information, learning and reputation (docs/06-geopolitics-ai.md).
//
// - Strategic goals (monthly per executive; an HTN-lite layer). Each goal in a small
//   catalogue is scored from the state relative to 2019; the best goal above a
//   threshold (with hysteresis) is adopted and decomposes into tactical biases on
//   its levers (goal_bias): restore growth -> public investment; consolidate ->
//   pension reform, less investment; regime security -> repression; deter a
//   threat -> defence; secure energy -> softer sanctions on energy suppliers (buying
//   from rivals in a crisis); contain an epidemic -> health spending.
// - Response tables (monthly): each nation learns the output-gap response to its own
//   fiscal packages by a lagged Bayesian regression shrunk toward a prior multiplier
//   (bounded rationality; Blanchard & Leigh 2013 on misjudged multipliers).
// - Information quality: perceived signals are noisier with low media freedom and
//   state capacity, and autocrats under-see discontent (Wintrobe 1998; Kuran 1991).
// - Credibility (reputation): abandoning an ally costs credibility in every
//   relationship, recovered over years (Tomz 2007; Gibler 2008; Crescenzi 2018).
// - Energy dependence: the share of a nation's oil demand supplied by each exporter
//   (gravity over trade shares), raising the cost of sanctioning it (Keohane & Nye
//   1977; asymmetric interdependence, Farrell & Newman 2019).

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};

float unrest_mean(game_state const& state, dcon::nation_id n) {
    float u = 0.0f, pop = 0.0f;
    for(auto own : state.world.nation_get_region_ownership(n)) {
        u += own.get_region().get_unrest() * own.get_region().get_population();
        pop += own.get_region().get_population();
    }
    return pop > 0.0f ? u / pop : 0.0f;
}

float max_threat(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    float t = 0.0f;
    for(auto m : w.in_nation) {
        if(m.id != n) t = std::max(t, w.nation_get_threat_to(n, static_cast<uint16_t>(m.id.index())));
    }
    return t;
}
} // namespace

float perceived_unrest(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    return unrest_mean(state, n) * (1.0f - static_cast<float>(state.params.info_bias) * (1.0f - std::clamp(w.nation_get_media_freedom(n), 0.0f, 1.0f)));
}

float perception_noise(game_state const& state, dcon::nation_id n, uint32_t channel) {
    auto const& w = state.world;
    const float sd = static_cast<float>(state.params.info_noise) * (1.0f - std::clamp(w.nation_get_media_freedom(n), 0.0f, 1.0f))
                     * (1.0f - 0.5f * std::clamp(w.nation_get_state_capacity(n), 0.0f, 1.0f));
    if(sd <= 0.0f) return 1.0f;
    // Weekly draws: the misperception persists through the week's decisions.
    const auto week = static_cast<uint32_t>(state.current_tick) / 7u;
    const float u1 = std::max(1e-6f, core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::ai), week, static_cast<uint32_t>(n.index()), channel * 2u));
    const float u2 = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::ai), week, static_cast<uint32_t>(n.index()), channel * 2u + 1u);
    const float z = std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * std::numbers::pi_v<float> * u2);
    return std::exp(sd * z - 0.5f * sd * sd);
}

float security_of(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const float approval = std::min(1.0f, 2.0f * w.nation_get_approval(n));
    const float l0 = w.nation_get_legitimacy0(n);
    const float legit = l0 > 0.0f ? std::min(1.0f, w.nation_get_legitimacy(n) / l0) : 1.0f;
    return std::clamp(std::min(approval, legit), 0.05f, 1.0f);
}

float rigidity(game_state const& state, dcon::agent_id a, dcon::nation_id n, float crisis) {
    auto const& P = state.params;
    return state.world.agent_get_rho_base(a) * std::pow(security_of(state, n), static_cast<float>(P.rho_security_k))
           * std::pow(std::clamp(1.0f - crisis, 0.0f, 1.0f), static_cast<float>(P.rho_crisis_m));
}

float goal_bias(game_state const& state, dcon::nation_id n, goal g) {
    auto const& w = state.world;
    const auto a = w.nation_get_agent_from_executive_seat(n);
    return a && w.agent_get_goal(a) == static_cast<uint8_t>(g) ? static_cast<float>(state.params.goal_gain) : 0.0f;
}

float fiscal_belief(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    const float est = w.nation_get_mult_est(n), prior = w.nation_get_mult_prior(n);
    if(est <= 0.0f || prior <= 0.0f) return 1.0f;
    const float k = static_cast<float>(state.params.response_weight);
    return std::pow(std::clamp(prior / std::max(0.05f, est), 0.85f, 1.2f), k);
}

void init_strategy(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    update_dependence(state);
    for(auto n : w.in_nation) {
        n.set_credibility(1.0f);
        n.set_legitimacy0(n.get_legitimacy());
        n.set_threat_max0(max_threat(state, n.id));
        n.set_mult_prior(static_cast<float>(P.response_prior));
        n.set_mult_est(static_cast<float>(P.response_prior));
        for(auto m : w.in_nation) {
            const auto k = static_cast<uint16_t>(m.id.index());
            n.set_energy_dependence0(k, n.get_energy_dependence(k));
        }
    }
}

void update_dependence(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        const float demand = n.get_oil_demand();
        const float imported = demand > 0.0f ? std::max(0.0f, demand - n.get_oil_production()) / demand : 0.0f;
        double total = 0.0;
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const float net = std::max(0.0f, m.get_oil_production() - m.get_oil_demand());
            total += net * (0.02f + m.get_trade_share(static_cast<uint16_t>(n.id.index())));
        }
        for(auto m : w.in_nation) {
            const auto k = static_cast<uint16_t>(m.id.index());
            const float net = m.id == n.id ? 0.0f : std::max(0.0f, m.get_oil_production() - m.get_oil_demand());
            const float share = total > 0.0 ? static_cast<float>(net * (0.02f + m.get_trade_share(static_cast<uint16_t>(n.id.index()))) / total) : 0.0f;
            n.set_energy_dependence(k, imported * share);
        }
    }
}

void update_strategy(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    if(static_cast<uint32_t>(state.current_tick) % monthly_cadence.period_days == 0) update_dependence(state);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        // Credibility recovers.
        n.set_credibility(std::min(1.0f, n.get_credibility() + (1.0f - n.get_credibility()) * std::min(1.0f, years / static_cast<float>(P.credibility_years))));

        // Response table: next month's gap change on last month's fiscal move.
        const float fiscal = n.get_fiscal_announced_cum();
        const float gap = n.get_gap_smoothed() * 100.0f;
        if(state.current_tick > 0) {
            const float dx = n.get_fiscal_prev_move();
            const float dy = gap - n.get_gap_prev();
            n.set_mult_sxy(n.get_mult_sxy() + dx * dy);
            n.set_mult_sxx(n.get_mult_sxx() + dx * dx);
            const float k0 = static_cast<float>(P.response_prior_weight);
            n.set_mult_est(std::clamp((n.get_mult_prior() * k0 + n.get_mult_sxy()) / (k0 + n.get_mult_sxx()), 0.05f, 3.0f));
        }
        n.set_fiscal_prev_move(fiscal - n.get_fiscal_prev());
        n.set_fiscal_prev(fiscal);
        n.set_gap_prev(gap);

        // Strategic goal.
        const auto a = n.get_executive_seat().get_agent();
        if(!a) continue;
        const double gdp = n.get_gdp0() * n.get_core_price();
        const float debt = gdp > 0.0 ? static_cast<float>(std::max(0.0, -n.get_bal_gov()) / gdp) : 0.0f;
        const float debt0 = n.get_gdp0() > 0.0 ? static_cast<float>(n.get_gov_debt0() / n.get_gdp0()) : 0.0f;
        float energy = 0.0f;
        for(auto m : w.in_nation) energy += n.get_energy_dependence(static_cast<uint16_t>(m.id.index()));
        float oil_shock = 0.0f;
        for(auto c : w.in_commodity) {
            if(c.id == state.derived.oil && c.get_price0() > 0.0f) oil_shock = std::max(0.0f, c.get_price() / c.get_price0() - 1.3f);
        }
        std::array<float, static_cast<std::size_t>(goal::count)> score{};
        score[static_cast<std::size_t>(goal::contain_epidemic)] = n.get_threat();
        score[static_cast<std::size_t>(goal::restore_growth)] = 10.0f * std::max(0.0f, -n.get_gap_smoothed())
                                                                + 10.0f * std::max(0.0f, n.get_unemployment() - n.get_unemployment0());
        score[static_cast<std::size_t>(goal::regime_security)] = 0.5f * std::max(0.0f, perceived_unrest(state, n.id) - 1.0f)
                                                                 + std::max(0.0f, 1.0f - security_of(state, n.id));
        score[static_cast<std::size_t>(goal::deter_threat)] = 3.0f * std::max(0.0f, max_threat(state, n.id) - n.get_threat_max0())
                                                              + n.get_war_effort();
        score[static_cast<std::size_t>(goal::consolidate)] = 5.0f * std::max(0.0f, debt - debt0 - 0.1f)
                                                             + 0.3f * std::max(0.0f, n.get_spread() - n.get_spread0());
        score[static_cast<std::size_t>(goal::secure_energy)] = 3.0f * energy * oil_shock;
        const auto current = static_cast<goal>(w.agent_get_goal(a));
        std::size_t best = 0;
        float best_score = 0.0f;
        for(std::size_t g = 1; g < score.size(); ++g) {
            const float sg = score[g] + (static_cast<std::size_t>(current) == g ? static_cast<float>(P.goal_hysteresis) : 0.0f);
            if(sg > best_score) {
                best_score = sg;
                best = g;
            }
        }
        if(best_score < static_cast<float>(P.goal_threshold)) best = 0;
        w.agent_set_goal_score(a, best_score);
        if(best != static_cast<std::size_t>(current)) {
            w.agent_set_goal(a, static_cast<uint8_t>(best));
            const auto d = w.create_decision();
            w.decision_set_agent(d, a);
            w.decision_set_target(d, n.id);
            w.decision_set_tick(d, state.current_tick);
            w.decision_set_lever(d, journal_goal);
            w.decision_set_before(d, static_cast<float>(current));
            w.decision_set_after(d, static_cast<float>(best));
            w.decision_set_pragmatic(d, best_score);
            w.decision_set_ideological(d, NAN);
            w.decision_set_prior(d, NAN);
        }
    }
}

void lose_credibility(game_state& state, dcon::nation_id n, float amount) {
    auto& w = state.world;
    w.nation_set_credibility(n, std::clamp(w.nation_get_credibility(n) - amount, 0.0f, 1.0f));
}

} // namespace geosim::sim
