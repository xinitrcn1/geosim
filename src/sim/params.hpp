#pragma once
// Global scalar model parameters. Defaults here; every one can be overridden
// in assets/defs/parameters.txt as `name{value}`. They are saved with the game.
//
// X(name, default, description)

#include <string_view>

#define GEOSIM_PARAMS(X)                                                                                   \
    /* ---- mode ---- */                                                                                    \
    X(historical_anchors, 1.0, "1 = dated real-world decisions are priors for the AI; 0 = AI alone")      \
    X(anchor_prior_weight, 10.0, "weight of the historical anchor vs pragmatic+ideological targets (=1)") \
    X(advise_weight, 3.0, "weight of player advice relative to the agent's own choice")                  \
    /* ---- AI agents ---- */                                                                               \
    X(ai_horizon_days, 14.0, "epidemic projection horizon of executives (days)")                          \
    X(ai_case_tolerance, 5.0, "detected cases/million/day tolerated at health weight 1")                 \
    X(ai_threat_slope, 0.3, "pragmatic stringency per log-unit of projected cases over tolerance")        \
    X(ai_econ_brake, 0.15, "pragmatic stringency removed per unit economic weight")                        \
    X(ai_unemployment_alarm, 0.05, "unemployment rise that triggers full income support")                 \
    X(ai_order_gain, 3.0, "how much an order-minded ideology amplifies the response to a threat")         \
    X(ai_default_targeted, 0.3, "targeted share of NPIs chosen without an anchor")                        \
    X(ai_tti_window, 20.0, "projected cases/million/day below which suppression is still feasible")       \
    X(ai_debt_limit, 1.5, "debt/GDP at which fiscal space is exhausted")                                   \
    X(ai_fiscal_response, 0.5, "pragmatic cumulative package (% GDP) per point of slack")                 \
    X(ai_fiscal_min_package, 0.1, "smallest new package announced (% GDP)")                              \
    /* ---- epidemic (SEIR) ---- */                                                                         \
    X(r0, 3.0, "basic reproduction number of the wild-type virus (Flaxman et al. 2020: ~3.3)")                                         \
    X(incubation_days, 4.0, "mean latent period E->I (days)")                                              \
    X(infectious_days, 5.0, "mean infectious period (days)")                                               \
    X(death_delay_days, 14.0, "mean delay from end of infectiousness to death (days)")                    \
    X(ifr_young, 0.0015, "infection fatality rate, under 65")                                              \
    X(ifr_old, 0.045, "infection fatality rate, 65 and over")                                              \
    X(old_contact_factor, 0.6, "relative contact rate of the 65+ group")                                  \
    X(seasonality, 0.25, "amplitude of seasonal transmission forcing (Gavenciak et al. 2022)")                                     \
    X(voluntary_k, 0.08, "behavioural distancing: transmission x 1/(1 + k * daily deaths per million)")   \
    X(tracing_max, 0.3, "max transmission cut from comprehensive contact tracing (H3 at max level)")                   \
    X(tracing_capacity, 20.0, "daily detected cases per million above which tracing saturates")          \
    X(tracing_cluster_pop, 3e6, "population of an outbreak cluster for tracing saturation")              \
    X(npi_compliance, 1.0, "share of the literature NPI transmission effect realised at first")          \
    X(fatigue_max, 0.35, "loss of NPI effect after prolonged restrictions (Sharma et al. 2021)")          \
    X(fear_fatigue_max, 0.8, "loss of consumers' and travellers' response to deaths after prolonged exposure") \
    X(fatigue_days, 180.0, "days at full containment until fatigue is complete")                         \
    X(informality_compliance_loss, 0.4, "NPI effect lost per unit informality (Maloney & Taskin 2020)")   \
    X(npi_targeted_reach, 0.4, "share of the economy a regional (targeted) measure reaches")              \
    X(npi_targeted_transmission, 0.5, "share of transmission a regional (targeted) measure reaches")     \
    X(border_effect, 0.8, "cut in imported infections at the maximum travel-control level")             \
    X(testing_effect, 0.1, "transmission cut from testing (H2 at max level)")                            \
    X(tti_capacity_mult, 9.0, "extra tracing capacity (x) under a full suppression strategy")            \
    X(tti_isolation, 0.5, "transmission cut from centralised isolation under a full suppression strategy") \
    X(quarantine_effect, 0.98, "cut in imported infections under full arrival quarantine")              \
    X(tti_extra_effect, 0.4, "extra transmission cut from tracing under a full suppression strategy")   \
    X(detection_ratio, 0.15, "share of infections detected, at max testing level")                        \
    X(travel_rate, 1e-3, "daily imported infections per resident per unit world prevalence")              \
    X(hosp_share, 0.05, "share of current infectious needing a hospital bed")                             \
    X(covid_bed_share, 0.3, "share of hospital beds available for COVID patients")                        \
    X(overload_ifr_mult, 1.0, "IFR increase per 100% hospital overload (capped at 2x)")                   \
    /* ---- production / demand ---- */                                                                     \
    X(adaptation_max, 0.5, "share of the NPI output cost removed by business adaptation")                 \
    X(adaptation_days, 120.0, "days at full containment until adaptation is complete")                   \
    X(voluntary_demand_k, 0.1, "consumer avoidance: contact demand x 1/(1 + k * daily deaths per million)") \
    X(substitution, 0.3, "share of blocked contact-services spending redirected to goods")                \
    X(tourism_share, 0.5, "share of contact-services exports that is inbound tourism")                    \
    X(tourism_own_border, 0.7, "cut in tourism exports at the nation's own maximum travel controls")       \
    X(tourism_world_border, 0.5, "cut in tourism exports at maximum travel controls everywhere else")      \
    X(supply_chain_share, 0.25, "imported-input share of manufacturing exposed to foreign shutdowns")     \
    X(sickness_absence, 0.5, "share of infectious working-age people absent from work")                    \
    X(income_smoothing_days, 90.0, "permanent-income smoothing time constant (days)")                     \
    X(precautionary, 0.3, "consumption cut per unit rise in unemployment rate")                           \
    X(wealth_spend_rate, 0.25, "share of excess household deposits spent per year")                       \
    X(accelerator, 0.8, "investment response to expected utilisation")                                     \
    X(investment_floor, 0.4, "minimum investment as share of baseline")                                    \
    X(util_smoothing_days, 30.0, "expected-utilisation smoothing (days)")                                 \
    X(rate_c_elasticity, 0.005, "consumption change per 1pp policy-rate deviation (fraction)")              \
    X(rate_i_elasticity, 0.02, "investment change per 1pp policy-rate deviation (fraction)")               \
    /* ---- labour ---- */                                                                                  \
    X(layoff_speed, 0.6, "weekly share of excess labour laid off when EPL = 0")                           \
    X(epl_hoarding, 0.6, "share of excess labour hoarded at EPL = 1 (no scheme)")                        \
    X(match_efficiency, 0.25, "weekly matching efficiency (hires = m * sqrt(U*V))")                       \
    X(lf_exit_base, 0.15, "share of laid-off workers leaving the labour force")                           \
    X(lf_exit_informal, 0.7, "extra LF exit share per unit informality")                                   \
    X(lf_exit_lockdown, 0.5, "extra LF exit share at maximum stay-at-home orders (search impossible)")   \
    X(layoff_lag_days, 7.0, "smoothing of labour demand used for layoffs, at EPL = 0 (days)")             \
    X(layoff_lag_epl, 6.0, "extra layoff lag per unit EPL (x layoff_lag_days)")                          \
    X(lf_return_rate, 0.03, "weekly share of crisis-inactive returning to job search")                    \
    X(wage_low, 0.6, "relative wage of low-skill workers")                                                 \
    X(wage_mid, 1.0, "relative wage of mid-skill workers")                                                 \
    X(wage_high, 1.9, "relative wage of high-skill workers")                                               \
    X(e1_replacement_boost, 0.3, "extra benefit replacement at OxCGRT E1 max level")                      \
    X(e1_coverage_boost, 0.4, "extra benefit coverage at OxCGRT E1 max level")                            \
    X(firm_support_effect, 1.0, "layoff-speed cut per unit (firm support / private wage bill)")          \
    /* ---- prices ---- */                                                                                  \
    X(phillips, 0.1, "annual inflation response to the output gap")                                       \
    X(gap_smoothing_days, 60.0, "output-gap smoothing (days)")                                            \
    X(expectations_days, 730.0, "inflation-expectation adaptation time (days)")                           \
    X(oil_cpi_share, 0.025, "energy pass-through weight of oil price in CPI")                               \
    /* ---- fiscal / income ---- */                                                                         \
    X(household_saving_rate, 0.08, "baseline aggregate household saving rate")                            \
    X(tax_share_hh, 0.55, "share of revenue from household income taxes and contributions")               \
    X(tax_share_indirect, 0.30, "share of revenue from indirect taxes")                                    \
    X(fiscal_disburse_days, 90.0, "mean time to disburse an announced fiscal package (days)")             \
    X(fiscal_firm_share, 0.4, "share of fiscal packages going to firms (rest to households)")            \
    X(gov_effective_rate, 0.025, "average effective interest rate on government debt")                    \
    X(profit_smoothing_days, 90.0, "dividend smoothing (days)")                                           \
    /* ---- monetary ---- */                                                                                \
    X(taylor_neutral, 0.5, "neutral real rate (pct) setting the starting rate of nations without a BIS rate") \
    X(effective_lower_bound, -0.5, "policy-rate floor (pct)")                                              \
    /* ---- oil ---- */                                                                                     \
    X(oil_demand0, 100.0, "world liquids demand 2019 (mb/d)")                                              \
    X(oil_price0, 64.0, "Brent price at scenario start (USD/bbl)")                                         \
    X(oil_flow_sensitivity, 5.0, "log-price response to (supply-demand)/demand")                           \
    X(oil_inventory_sensitivity, 1.0, "log-price response to excess inventory in demand-months")          \
    X(oil_price_speed, 0.15, "daily adjustment of price toward its target")                                \
    X(oil_production_speed, 0.1, "daily adjustment of production toward target")                           \
    X(oil_mobility_weight, 0.35, "share of oil demand from personal mobility")                             \
    X(oil_demand_elasticity, -0.07, "short-run price elasticity of oil demand")                            \
    X(voluntary_mobility_k, 0.04, "travel avoidance: mobility x 1/(1 + k * daily deaths per million)")    \
    X(oil_value_added_share, 0.7, "value added per dollar of crude sold")

namespace geosim::sim {

struct params {
#define GEOSIM_PARAM_FIELD(name, def, desc) double name = def;
    GEOSIM_PARAMS(GEOSIM_PARAM_FIELD)
#undef GEOSIM_PARAM_FIELD
};

// Name-based access for asset loading and saves. Returns false if unknown.
bool set_param(params& p, std::string_view name, double value) noexcept;
double const* find_param(params const& p, std::string_view name) noexcept;

template<typename F>
void for_each_param(params const& p, F&& f) {
#define GEOSIM_PARAM_VISIT(name, def, desc) f(#name, p.name, desc);
    GEOSIM_PARAMS(GEOSIM_PARAM_VISIT)
#undef GEOSIM_PARAM_VISIT
}

} // namespace geosim::sim
