// AI agents (docs/06-geopolitics-ai.md#agent-utility-pragmatism-vs-ideology).
//
// Every lever is chosen by maximising a quadratic utility
//
//     U(x) = -(1 - rho) (x - P)^2  -  rho (x - I)^2  -  kappa (x - A)^2
//
// P = pragmatic target from the simulated situation, I = the agent's
// ideological ideal, A = the historical anchor (prior), present only when a
// dated real-world decision exists and historical_anchors is on. The optimum
// is the precision-weighted mean, so the agent tracks history while the
// simulation tracks history, and drifts toward P as the two diverge.
//
// rho (ideological rigidity) falls under threat: pragmatism under pressure.
// Autonomous jurisdictions blend their choice with their sovereign's
// (weight 1 - autonomy). Central banks of pegged currencies follow the anchor.
// Player overrides (advise / direct / force) replace or bias the choice; the
// AI's own choice is still recorded for the "Why?" panel.

#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {

struct blend_result {
    float choice;
    float pragmatic;
    float ideological;
    float prior;
};

blend_result blend(float pragmatic, float ideological, float rho, bool has_prior, float prior, float kappa, float lo, float hi) {
    const float wp = 1.0f - rho;
    const float wi = rho;
    const float wa = has_prior ? kappa : 0.0f;
    const float x = (wp * pragmatic + wi * ideological + wa * prior) / std::max(1e-6f, wp + wi + wa);
    return {std::clamp(x, lo, hi), pragmatic, ideological, has_prior ? prior : NAN};
}

dcon::agent_id executive_of(game_state const& state, dcon::nation_id n) {
    return state.world.nation_get_agent_from_executive_seat(n);
}

// Applies a player override (if any) and logs the outcome on the agent.
float settle(game_state& state, dcon::agent_id a, uint8_t lev, blend_result const& r, float lo, float hi) {
    auto& w = state.world;
    const auto mode = static_cast<override_mode>(w.agent_get_override_mode(a, lev));
    float choice = r.choice;
    switch(mode) {
    case override_mode::none:
        break;
    case override_mode::advise: {
        // Advice is one more (strong) prior the agent weighs against its own targets.
        const float k = static_cast<float>(state.params.advise_weight);
        choice = std::clamp((r.choice + k * w.agent_get_override_value(a, lev)) / (1.0f + k), lo, hi);
        break;
    }
    case override_mode::direct:
        choice = std::clamp(w.agent_get_override_value(a, lev), lo, hi); // within legal range
        break;
    case override_mode::force:
        choice = w.agent_get_override_value(a, lev); // beyond legal range; politics charges the cost
        break;
    }
    w.agent_set_last_pragmatic(a, lev, r.pragmatic);
    w.agent_set_last_ideological(a, lev, r.ideological);
    w.agent_set_last_prior(a, lev, r.prior);
    w.agent_set_last_ai_choice(a, lev, r.choice);
    w.agent_set_last_choice(a, lev, choice);
    return choice;
}

void journal(game_state& state, dcon::agent_id a, dcon::nation_id n, uint8_t lev, float before, float after,
             blend_result const& r, float threshold) {
    if(std::abs(after - before) < threshold) {
        return;
    }
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, a);
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, lev);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, r.pragmatic);
    w.decision_set_ideological(d, r.ideological);
    w.decision_set_prior(d, r.prior);
    w.decision_set_mode(d, w.agent_get_override_mode(a, lev));
}

// Situation summary used by an executive's pragmatic targets.
struct situation {
    float cases_pm = 0;      // detected cases per million per day, projected over the horizon
    float threat = 0;        // 0..1 pragmatic stringency from the epidemic
    float travel_threat = 0; // 0..1 from world prevalence
};

situation assess(game_state const& state, dcon::nation_id n, double world_cases_pm, dcon::agent_id a) {
    auto const& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    // Severity-weighted detected cases over all pathogens, and their growth.
    const float testing = D.npi_testing != 0xFF ? npi_fraction(state, n, D.npi_testing) : 0.0f;
    // WHO surveillance sharing improves what transparent states see (IHR 2005).
    const float who = is_member(state, org_kind::health, n) ? static_cast<float>(P.who_detection) : 0.0f;
    const float detected_share = (0.2f + 0.8f * testing) * (1.0f + who * w.nation_get_media_freedom(n));
    double cases = 0, growth_num = 0, pop = 0;
    for(auto own : w.nation_get_region_ownership(n)) {
        auto r = own.get_region();
        pop += r.get_population();
        for(uint8_t p = 0; p < D.pathogen_count; ++p) {
            const dcon::pathogen_id pid{p};
            double alive = 0, susceptible = 0;
            for(uint8_t g = 0; g < age_groups; ++g) {
                const uint8_t i = epi_index(p, g);
                susceptible += r.get_ep_s(i);
                alive += r.get_ep_s(i) + r.get_ep_e(i) + r.get_ep_i(i) + r.get_ep_r(i);
            }
            const double reff = alive > 0 ? r.get_beta(p) * w.pathogen_get_infectious_days(pid) * susceptible / alive : 0.0;
            const double c = r.get_infections_p(p) * w.pathogen_get_detection_ratio(pid) * detected_share * w.pathogen_get_severity(pid);
            cases += c;
            growth_num += c * (reff - 1.0) / (w.pathogen_get_infectious_days(pid) + w.pathogen_get_incubation_days(pid));
        }
    }
    situation s;
    if(pop <= 0) {
        return s;
    }
    const float cases_pm = static_cast<float>(cases / pop * 1e6);
    const float growth = cases > 0 ? static_cast<float>(growth_num / cases) : 0.0f;
    // Perceived through the regime's information quality (strategy.cpp).
    s.cases_pm = cases_pm * std::exp(std::clamp(growth * static_cast<float>(P.ai_horizon_days), -5.0f, 5.0f)) * perception_noise(state, n, 0);
    // Large winning coalitions make mass welfare (health) matter more (selectorate theory).
    const float health = w.agent_get_health_weight(a) * (0.6f + 0.8f * w.nation_get_winning_coalition(n));
    const float tolerance = static_cast<float>(P.ai_case_tolerance) / std::max(0.1f, health);
    const float econ = static_cast<float>(P.ai_econ_brake) * w.agent_get_econ_weight(a);
    s.threat = std::clamp(static_cast<float>(P.ai_threat_slope) * std::log(std::max(1e-3f, s.cases_pm) / tolerance) - econ, 0.0f, 1.0f);
    s.travel_threat = std::clamp(static_cast<float>(P.ai_threat_slope)
                                 * std::log(std::max(1e-3f, static_cast<float>(world_cases_pm)) / tolerance) - econ, 0.0f, 1.0f);
    return s;
}

void decide_npis(game_state& state, dcon::agent_id a, dcon::nation_id n, situation const& s) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const bool anchors = P.historical_anchors != 0.0 && w.nation_get_prior_npi_set(n);
    const float kappa = static_cast<float>(P.anchor_prior_weight);
    const float rho = w.agent_get_rho(a);
    const float capacity = w.nation_get_state_capacity(n);
    const float u_rise = std::max(0.0f, w.nation_get_unemployment(n) - w.nation_get_unemployment0(n));
    float autonomy = 1.0f;
    const auto sov = sovereign_of(state, n, autonomy);

    for(uint8_t k = 0; k < D.npi_count; ++k) {
        const dcon::npi_indicator_id ind{k};
        const float max = w.npi_indicator_get_max_level(ind);
        const auto role = static_cast<npi_role>(w.npi_indicator_get_role(ind));
        float pragmatic = s.threat;
        switch(role) {
        case npi_role::travel: pragmatic = s.travel_threat; break;
        case npi_role::testing:
        case npi_role::tracing: pragmatic = s.threat * capacity; break;
        case npi_role::income_support: pragmatic = std::clamp(u_rise / static_cast<float>(P.ai_unemployment_alarm), 0.0f, 1.0f); break;
        default: break;
        }
        // Ideology shapes the *response*: order-minded leaders answer a threat harder,
        // liberty-minded ones softer; without a threat neither restricts.
        const float ideal = role == npi_role::income_support
                                ? 0.0f
                                : std::clamp(pragmatic * (1.0f + static_cast<float>(P.ai_order_gain) * (w.agent_get_order_ideal(a) - 0.2f)), 0.0f, 1.0f);
        auto r = blend(pragmatic * max, ideal * max, rho, anchors, w.nation_get_prior_npi(n, k), kappa, 0.0f, max);
        if(sov) {
            r.choice = autonomy * r.choice + (1.0f - autonomy) * w.nation_get_npi(sov, k);
        }
        const float before = w.nation_get_npi(n, k);
        const float after = settle(state, a, k, r, 0.0f, max);
        w.nation_set_npi(n, k, after);
        // Regional (targeted) application follows the anchor when there is one.
        w.nation_set_npi_targeted(n, k, anchors ? w.nation_get_prior_npi_targeted(n, k) : static_cast<float>(P.ai_default_targeted));
        journal(state, a, n, k, before, after, r, 0.5f);
    }

    // Suppression strategy.
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::tti));
    const bool tti_anchor = P.historical_anchors != 0.0 && w.nation_get_prior_tti_set(n);
    const float early = s.cases_pm < static_cast<float>(P.ai_tti_window) ? 1.0f : 0.0f;
    auto r = blend(capacity * capacity * early * s.threat, w.agent_get_order_ideal(a) * s.threat, rho, tti_anchor,
                   w.nation_get_prior_tti(n), kappa, 0.0f, 1.0f);
    if(sov) {
        r.choice = autonomy * r.choice + (1.0f - autonomy) * w.nation_get_tti_strength(sov);
    }
    const float before = w.nation_get_tti_strength(n);
    const float after = settle(state, a, lev, r, 0.0f, 1.0f);
    w.nation_set_tti_strength(n, after);
    journal(state, a, n, lev, before, after, r, 0.2f);
}

void decide_fiscal(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const float rho = w.agent_get_rho(a);
    const float kappa = static_cast<float>(P.anchor_prior_weight);

    // Counter-cyclical support limited by fiscal space (debt relative to a soft limit).
    const double gdp_nominal = w.nation_get_gdp0(n) * w.nation_get_core_price(n);
    const float debt_ratio = static_cast<float>(std::max(0.0, -w.nation_get_bal_gov(n)) / gdp_nominal);
    const float space = std::clamp(1.0f - debt_ratio / static_cast<float>(P.ai_debt_limit), 0.2f, 1.0f);
    const float slack = std::max(0.0f, -w.nation_get_gap_smoothed(n)) * 100.0f
                        + 0.5f * std::max(0.0f, w.nation_get_unemployment(n) - w.nation_get_unemployment0(n)) * 100.0f;
    // Package size scales with the learned response (a weak multiplier asks for more).
    const float pragmatic = static_cast<float>(P.ai_fiscal_response) * slack * space * fiscal_belief(state, n);
    const float ideal = pragmatic * (1.0f - w.agent_get_fiscal_hawk(a));
    const bool anchors = P.historical_anchors != 0.0 && w.nation_get_prior_fiscal_set(n);
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::fiscal));
    const float announced = w.nation_get_fiscal_announced_cum(n);
    // In historical mode the prior always exists: no announcement so far means "no package yet".
    (void)anchors;
    // Anchored only where the scenario records this nation's packages (an empty record
    // then means "no package yet"); elsewhere the agent decides (docs/13).
    auto r = blend(pragmatic, ideal, rho, P.historical_anchors != 0.0 && w.nation_get_fiscal_data(n), w.nation_get_prior_fiscal_cum(n), kappa, announced, 100.0f);
    const float target = settle(state, a, lev, r, announced, 100.0f);
    const float fresh = target - announced;
    if(fresh >= static_cast<float>(P.ai_fiscal_min_package)) {
        w.nation_set_fiscal_announced_cum(n, target);
        w.nation_set_fiscal_pending(n, w.nation_get_fiscal_pending(n) + fresh / 100.0 * gdp_nominal);
        journal(state, a, n, lev, announced, target, r, 0.0f);
    }
}

// Directed credit through state-owned banks (% of GDP per year; credit.cpp).
// Pragmatic: lend into slack in proportion to the state-bank share, less when the
// currency is already falling (the import and capital-flight leak). Ideological:
// growth-first executives (credit_bias: large state banks x a dependent central
// bank; Sapienza 2004, Dinc 2005) amplify it - until a currency crisis makes them
// turn orthodox (rigidity falls with the threat, and the bias with depreciation).
// No historical anchor: the level is the agent's own.
void decide_directed_credit(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    // Reach: state banks fully, private banks through guarantee schemes (KfW, PGE, PPP, ECLGS).
    const float sb = w.nation_get_state_bank_share(n)
                     + static_cast<float>(P.guarantee_reach) * (1.0f - w.nation_get_state_bank_share(n)) * w.nation_get_state_capacity(n);
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::directed_credit));
    const float slack = std::max(0.0f, -w.nation_get_gap_smoothed(n)) * 100.0f
                        + 0.5f * std::max(0.0f, w.nation_get_unemployment(n) - w.nation_get_unemployment0(n)) * 100.0f;
    const float depreciation = std::max(0.0f, -w.nation_get_fx_change(n));
    // State banks lend from their balance sheets: capacity scales with banking depth.
    const double gdp = std::max(1.0, w.nation_get_gdp0(n) * w.nation_get_core_price(n) * w.nation_get_gf(n));
    const float depth = std::clamp(static_cast<float>((w.nation_get_loans_hh(n) + w.nation_get_loans_firm(n)) / gdp / P.credit_depth_ref), 0.05f, 1.0f);
    const float reach = sb * depth;
    const float pragmatic = std::max(0.0f, static_cast<float>(P.directed_slack) * slack * reach - static_cast<float>(P.directed_fx_brake) * depreciation * reach);
    const float crisis = std::clamp(depreciation / static_cast<float>(P.fx_crisis_depreciation), 0.0f, 1.0f);
    const float bias = w.agent_get_credit_bias(a) * (1.0f - crisis);
    const float ideal = pragmatic * (1.0f + static_cast<float>(P.directed_bias_gain) * bias);
    // Economic ideology is not suspended by an epidemic: base rigidity.
    auto r = blend(pragmatic, ideal, w.agent_get_rho_base(a), false, 0.0f, 0.0f, 0.0f, 20.0f * reach);
    const float before = w.nation_get_directed_credit(n);
    const float after = settle(state, a, lev, r, 0.0f, 20.0f * reach);
    w.nation_set_directed_credit(n, after);
    journal(state, a, n, lev, before, after, r, 0.5f);
}

// Information control (0..1): restrictions on social media and messaging.
// Pragmatic: none until unrest passes a trigger, then rising with it; the legal
// ceiling is set by media freedom (direct overrides stay below it, force goes
// beyond). Ideological: order-minded rulers restrict earlier and harder.
void decide_info_control(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::info_control));
    float unrest = 0.0f, pop = 0.0f;
    for(auto own : w.nation_get_region_ownership(n)) {
        unrest += own.get_region().get_unrest() * own.get_region().get_population();
        pop += own.get_region().get_population();
    }
    unrest = pop > 0.0f ? unrest / pop : 0.0f;
    // The status quo (starting controls) is the reference; the ceiling is set by media freedom.
    const float base = w.nation_get_info_control0(n);
    const float ceiling = std::max(base, std::clamp(1.0f - w.nation_get_media_freedom(n), 0.0f, 1.0f));
    const float pressure = std::clamp((unrest - static_cast<float>(P.censor_trigger)) / 2.0f, 0.0f, 1.0f);
    const float room = ceiling - base;
    const float pragmatic = base + 0.3f * pressure * room;
    const float ideal = base + std::clamp(pressure * (0.5f + 2.0f * w.agent_get_order_ideal(a)), 0.0f, 1.0f) * room;
    auto r = blend(pragmatic, ideal, w.agent_get_rho(a), false, 0.0f, 0.0f, 0.0f, ceiling);
    const float before = w.nation_get_info_control(n);
    const float after = std::clamp(settle(state, a, lev, r, 0.0f, ceiling), 0.0f, 1.0f);
    w.nation_set_info_control(n, after);
    journal(state, a, n, lev, before, after, r, 0.1f);
}

// Cross-border filtering of the internet (0..1). Pragmatic: more filtering when
// foreign protest contagion and influence operations threaten the regime;
// ideological: order-minded rulers filter more. The technical ceiling needs state
// capacity and a muzzled press (Deibert et al. 2008; Roberts 2018; Rod & Weidmann
// 2015). Starting levels are the status quo.
void decide_net_filtering(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::net_filtering));
    const float base = w.nation_get_net_filtering0(n);
    const float ceiling = std::max(base, std::clamp(1.5f * w.nation_get_state_capacity(n) * (1.0f - w.nation_get_media_freedom(n)), 0.0f, 1.0f));
    const float threat = w.nation_get_net_contagion(n) + w.nation_get_influence_ops(n);
    const float room = ceiling - base;
    const float pragmatic = base + std::clamp(static_cast<float>(P.net_filter_gain) * threat, 0.0f, 1.0f) * room;
    const float ideal = base + std::clamp(static_cast<float>(P.net_filter_gain) * threat * (1.0f + 3.0f * w.agent_get_order_ideal(a)), 0.0f, 1.0f) * room;
    auto r = blend(pragmatic, ideal, w.agent_get_rho_base(a), false, 0.0f, 0.0f, 0.0f, ceiling);
    const float before = w.nation_get_net_filtering(n);
    // Internet filtering is set by law (a bill where legislatures compete).
    const float after = std::clamp(enact(state, n, lev, before, settle(state, a, lev, r, 0.0f, ceiling), value_axis::self_expression, -0.5f, false),
                                   0.0f, 1.0f);
    w.nation_set_net_filtering(n, after);
    journal(state, a, n, lev, before, after, r, 0.1f);
}

void decide_job_retention(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const float rho = w.agent_get_rho(a);
    const float kappa = static_cast<float>(P.anchor_prior_weight);
    const bool anchors = P.historical_anchors != 0.0;
    const bool has = anchors && w.nation_get_prior_stw_set(n);
    // Pragmatic: capable states protect jobs when closures bite.
    float closure = 0.0f;
    for(uint8_t k = 0; k < D.npi_count; ++k) {
        closure = std::max(closure, w.sector_get_npi_capacity(D.contact_sector, k) > 0.0f ? npi_fraction(state, n, k) : 0.0f);
    }
    const float capacity = w.nation_get_state_capacity(n);
    const uint8_t lc = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::stw_coverage));
    const uint8_t lr = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::stw_replacement));
    // Anchored only to a recorded scheme; nations without one decide for themselves.
    (void)anchors;
    auto rc = blend(capacity * closure * 0.85f, 0.0f, rho, has, w.nation_get_prior_stw_coverage(n), kappa, 0.0f, 1.0f);
    auto rr = blend(0.7f * capacity, 0.5f, rho, has, w.nation_get_prior_stw_replacement(n), kappa, 0.0f, 1.0f);
    const float before = w.nation_get_stw_coverage(n);
    w.nation_set_stw_coverage(n, settle(state, a, lc, rc, 0.0f, 1.0f));
    w.nation_set_stw_replacement(n, settle(state, a, lr, rr, 0.0f, 1.0f));
    journal(state, a, n, lc, before, w.nation_get_stw_coverage(n), rc, 0.1f);
}

void decide_oil(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const float prod0 = w.nation_get_oil_production0(n);
    if(prod0 <= 0.0f || !D.oil) {
        return;
    }
    const float price = w.commodity_get_price(D.oil) / w.commodity_get_price0(D.oil);
    // Capacity grows along the trend of world demand: the 2019 balance holds on the
    // growth path (new fields replace depletion; IEA World Energy Investment), so a
    // tightening market needs a real shock, not the passage of time.
    const float capacity = prod0 * oil_trend_index(state);
    const float market = capacity * std::pow(std::max(0.05f, price), w.nation_get_oil_supply_elasticity(n));
    const bool anchors = P.historical_anchors != 0.0 && w.nation_get_prior_oil_set(n);
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::oil_target));
    auto r = blend(market, capacity, w.agent_get_rho(a), anchors, w.nation_get_prior_oil(n), static_cast<float>(P.anchor_prior_weight),
                   0.0f, prod0 * 2.0f);
    const float before = w.nation_get_oil_target(n);
    const float after = settle(state, a, lev, r, 0.0f, prod0 * 2.0f);
    w.nation_set_oil_target(n, after);
    w.nation_set_oil_target_set(n, anchors ? 1 : 0); // anchored (quota) producers adjust fast
    journal(state, a, n, lev, before, after, r, 0.25f);
}

// Central bank of a currency area: one decision for all member nations.
void decide_rate(game_state& state, dcon::agent_id bank) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    double weight = 0, pi = 0, target_pi = 0, gap = 0, rate0 = 0;
    dcon::nation_id first;
    bool anchored = false;
    float prior = 0.0f;
    for(auto m : w.agent_get_monetary_authority(bank)) {
        const auto n = m.get_member();
        const double g = w.nation_get_gdp0(n);
        weight += g;
        pi += g * w.nation_get_inflation_expect(n) * 100.0;
        target_pi += g * w.nation_get_inflation_trend(n) * 100.0;
        gap += g * w.nation_get_gap_smoothed(n) * 100.0;
        rate0 += g * w.nation_get_policy_rate0(n);
        if(!first) {
            first = n;
        }
        if(w.nation_get_prior_rate_set(n) && !anchored) {
            anchored = true;
            prior = w.nation_get_prior_rate(n);
        }
    }
    if(weight <= 0) {
        return;
    }
    pi /= weight;
    target_pi /= weight;
    gap /= weight;
    rate0 /= weight;
    // Pragmatic: Taylor (1993) with the neutral rate implied by the starting setting,
    // or the anchor's rate under a peg.
    float pragmatic = static_cast<float>(rate0 - target_pi + pi + 0.5 * (pi - target_pi) + 0.5 * gap);
    if(const auto anchor = peg_anchor(state, first)) {
        pragmatic = w.nation_get_policy_rate(anchor);
    }
    const float taylor = pragmatic;
    // A dependent central bank leans toward the rate a growth-first executive wants
    // (Alesina & Summers 1993; Binder 2021 on political pressure). The executive's
    // bias fades in a currency crisis.
    {
        double cbi = 0.0, bias = 0.0, dep = 0.0;
        for(auto m : w.agent_get_monetary_authority(bank)) {
            const auto n = m.get_member();
            const double g = w.nation_get_gdp0(n);
            cbi += g * w.nation_get_cb_independence(n);
            dep += g * std::max(0.0f, -w.nation_get_fx_change(n));
            if(auto e = w.nation_get_agent_from_executive_seat(n)) bias += g * w.agent_get_credit_bias(e);
        }
        cbi /= weight;
        bias /= weight;
        dep /= weight;
        const float crisis = std::clamp(static_cast<float>(dep) / static_cast<float>(P.fx_crisis_depreciation), 0.0f, 1.0f);
        const float wanted = pragmatic - static_cast<float>(bias) * (1.0f - crisis) * static_cast<float>(P.cb_pressure_pp + 0.5 * std::max(0.0, -gap));
        pragmatic = static_cast<float>(cbi) * pragmatic + static_cast<float>(1.0 - cbi) * wanted;
    }
    pragmatic = std::max(static_cast<float>(P.effective_lower_bound), pragmatic);
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::policy_rate));
    auto r = blend(pragmatic, static_cast<float>(rate0), w.agent_get_rho(bank), anchored && P.historical_anchors != 0.0, prior,
                   static_cast<float>(P.anchor_prior_weight), static_cast<float>(P.effective_lower_bound), 100.0f);
    const float before = w.nation_get_policy_rate(first);
    const float after = settle(state, bank, lev, r, static_cast<float>(P.effective_lower_bound), 100.0f);
    for(auto m : w.agent_get_monetary_authority(bank)) {
        w.nation_set_policy_rate(m.get_member(), after);
    }
    journal(state, bank, first, lev, before, after, r, 0.1f);

    // Asset purchases (QE, % of GDP per year): when the Taylor rate is below the lower
    // bound, and against a sovereign-spread blowout (fragmentation). Pegged currencies
    // follow their anchor's rate and do not buy.
    const uint8_t lq = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::asset_purchases));
    double spread_rise = 0.0;
    for(auto m : w.agent_get_monetary_authority(bank)) {
        const auto n = m.get_member();
        spread_rise += w.nation_get_gdp0(n) * std::max(0.0f, w.nation_get_spread(n) - w.nation_get_spread0(n) - 1.0f);
    }
    spread_rise /= weight;
    const float shortfall = std::max(0.0f, static_cast<float>(P.effective_lower_bound) + 0.25f - taylor);
    // Purchases saturate (balance-sheet and free-float limits): qe_max (1 - e^(-shortfall / qe_gap)).
    const float qe_target = peg_anchor(state, first) ? 0.0f
                                                     : static_cast<float>(P.qe_max) * (1.0f - std::exp(-shortfall / static_cast<float>(P.qe_gap)))
                                                           + static_cast<float>(P.qe_spread) * static_cast<float>(spread_rise);
    auto rq = blend(qe_target, 0.0f, w.agent_get_rho(bank), false, 0.0f, 0.0f, 0.0f, 30.0f);
    const float q_before = w.nation_get_qe_rate(first);
    const float q_after = settle(state, bank, lq, rq, 0.0f, 30.0f);
    for(auto m : w.agent_get_monetary_authority(bank)) {
        w.nation_set_qe_rate(m.get_member(), q_after);
    }
    journal(state, bank, first, lq, q_before, q_after, rq, 2.0f);
}

} // namespace

void update_ai(game_state& state) {
    if(state.params.ai_frozen != 0.0) {
        return; // reduced-detail forks: agents keep their current plans
    }
    auto& w = state.world;
    const auto tick = static_cast<tick_t>(state.current_tick);

    // World severity-weighted detected cases (per million per day), serial in index order.
    double world_cases = 0, world_pop = 0;
    for(auto r : w.in_region) {
        for(uint8_t p = 0; p < state.derived.pathogen_count; ++p) {
            const dcon::pathogen_id pid{p};
            world_cases += r.get_infections_p(p) * w.pathogen_get_detection_ratio(pid) * w.pathogen_get_severity(pid) * 0.5;
        }
        world_pop += r.get_population();
    }
    const double world_cases_pm = world_pop > 0 ? world_cases / world_pop * 1e6 : 0.0;

    // Sovereigns decide before their subjects so subjects can follow them.
    std::vector<dcon::nation_id> order;
    float autonomy = 1.0f;
    for(auto n : w.in_nation) {
        if(!sovereign_of(state, n.id, autonomy)) order.push_back(n.id);
    }
    for(auto n : w.in_nation) {
        if(sovereign_of(state, n.id, autonomy)) order.push_back(n.id);
    }

    for(auto n : order) {
        const auto a = executive_of(state, n);
        if(!a) {
            continue;
        }
        const auto s = assess(state, n, world_cases_pm, a);
        // Secure agents can afford principles: rigidity falls with insecurity and crisis.
        w.agent_set_rho(a, rigidity(state, a, n, s.threat));
        w.nation_set_threat(n, s.threat);
        decide_npis(state, a, n, s);
        decide_oil(state, a, n);
        decide_info_control(state, a, n);
        if(is_due(n.index(), tick, weekly)) {
            decide_fiscal(state, a, n);
            decide_job_retention(state, a, n);
            decide_directed_credit(state, a, n);
            decide_net_filtering(state, a, n);
            decide_tariff(state, a, n);
            decide_defense(state, a, n);
            decide_min_wage(state, a, n);
            decide_fiscal_structure(state, a, n);
            decide_carbon_price(state, a, n);
            decide_policies(state, a, n);
            decide_reforms(state, a, n);
        }
    }
    for(auto bank : w.in_agent) {
        if(bank.get_kind() != static_cast<uint8_t>(agent_kind::central_bank)) {
            continue;
        }
        auto members = bank.get_monetary_authority();
        if(members.begin() == members.end()) {
            continue;
        }
        decide_rate(state, bank.id); // daily: emergency meetings happen (Fed, 2020-03-03 and 03-15)
        decide_bank_tools(state, bank.id);
    }
}

} // namespace geosim::sim
