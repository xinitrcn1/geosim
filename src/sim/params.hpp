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
    /* ---- epidemics (pathogen-specific values: assets/defs/pathogens.txt) ---- */                         \
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
    X(travel_rate, 1e-3, "daily imported infections per resident per unit world prevalence")              \
    X(epidemic_bed_share, 0.3, "share of hospital beds available for epidemic patients")                    \
    X(tracing_reference_detection, 0.15, "detection ratio at which a pathogen is fully traceable")           \
    X(severity_reference_ifr, 0.008, "population IFR of severity 1 (about SARS-CoV-2 wild type)")           \
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
    X(layoff_speed, 0.4, "weekly share of excess labour laid off when EPL = 0")                           \
    X(epl_hoarding, 0.8, "share of excess labour hoarded at EPL = 1 (no scheme)")                        \
    X(match_efficiency, 0.25, "weekly matching efficiency (hires = m * sqrt(U*V))")                       \
    X(lf_exit_base, 0.15, "share of laid-off workers leaving the labour force")                           \
    X(lf_exit_informal, 0.7, "extra LF exit share per unit informality")                                   \
    X(lf_exit_lockdown, 0.8, "extra LF exit share at maximum stay-at-home orders (ILO 2020: ~71% of job losses became inactivity)")   \
    X(layoff_lag_days, 7.0, "smoothing of labour demand used for layoffs, at EPL = 0 (days)")             \
    X(layoff_lag_epl, 12.0, "extra layoff lag per unit EPL (x layoff_lag_days)")                          \
    X(lf_return_rate, 0.03, "weekly share of crisis-inactive returning to job search")                    \
    X(wage_low, 0.6, "relative wage of low-skill workers")                                                 \
    X(wage_mid, 1.0, "relative wage of mid-skill workers")                                                 \
    X(wage_high, 1.9, "relative wage of high-skill workers")                                               \
    X(e1_replacement_boost, 0.3, "extra benefit replacement at OxCGRT E1 max level")                      \
    X(e1_coverage_boost, 0.4, "extra benefit coverage at OxCGRT E1 max level")                            \
    X(firm_support_effect, 1.0, "layoff-speed cut per unit (firm support / private wage bill)")          \
    /* ---- society ---- */                                                                                 \
    X(childhood_years, 15.0, "years from birth to labour-force entry")                                     \
    X(working_years, 50.0, "years in the working-age group (15-64)")                                      \
    X(young_mortality, 0.0015, "annual non-COVID mortality under 65")                                     \
    X(migration_rate_internal, 0.02, "annual internal migration per log-unit of expected-income gap")     \
    X(migration_rate_international, 0.002, "annual international migration per log-unit of gap")        \
    X(migration_cost_internal, 0.1, "log expected-income gap needed before people move internally")      \
    X(migration_cost_international, 1.0, "log expected-income gap needed before people emigrate")        \
    X(refugee_push, 2.0, "extra log-gap pushing emigration from nations at war")                         \
    X(migration_cap, 0.01, "largest share of a pop that can move in one 28-day step")                    \
    X(values_insecurity_pull, 0.05, "survival-values shift per year at full insecurity")                  \
    X(values_loss_left, 0.1, "leftward economic shift per year at full grievance")                       \
    X(opinion_eps_base, 0.3, "bounded-confidence radius at zero media freedom")                           \
    X(opinion_eps_media, 0.4, "extra confidence radius at full media freedom")                            \
    X(opinion_speed, 0.5, "annual convergence speed toward confidant opinions")                          \
    X(expectations_income_days, 365.0, "adaptation time of income expectations (relative deprivation)") \
    X(unrest_grievance, 1.0, "unrest target per unit grievance x (1 - legitimacy)")                       \
    X(unrest_psi, 0.3, "unrest target per unit Political Stress Index")                                   \
    X(unrest_days, 60.0, "adjustment time of regional unrest (days)")                                     \
    X(riot_capital_loss, 0.002, "share of regional capital destroyed per 28 days per unrest level above 3") \
    /* ---- politics ---- */                                                                                \
    X(vote_distance_weight, 4.0, "utility loss per squared distance to a party")                          \
    X(vote_retrospective, 1.0, "weight of the incumbent's performance in vote choice")                    \
    X(vote_health_weight, 0.1, "performance penalty per daily death per million")                         \
    X(vote_temperature, 0.3, "logit temperature of vote choice")                                          \
    X(legitimacy_years, 1.0, "adjustment time of legitimacy (years)")                                     \
    X(force_legitimacy_cost, 0.1, "legitimacy lost per year per forced lever")                            \
    X(force_liberal_cost, 0.05, "liberal-index loss per year per forced lever")                           \
    X(coup_rate, 0.02, "monthly coup probability per unrest level above 2 (when conditions hold)")       \
    X(party_speed, 0.15, "distance an adaptive party moves in value space per year (Laver & Sergenti 2011)") \
    X(window_base, 0.25, "half-width of the Overton window at zero polarisation (value-axis units)")   \
    X(window_polarisation, 1.0, "extra half-width per unit standard deviation of public values")        \
    X(window_media, 0.1, "extra half-width at full social-media connectivity (fragmented attention)")   \
    X(window_years, 3.0, "adjustment time of the window centre toward public opinion (years)")         \
    X(window_normalisation, 0.15, "annual pull of the window toward the governing party's position (Bursztyn et al. 2020)") \
    X(window_stigma, 3.0, "vote utility lost per squared distance of a party outside the window")       \
    X(overton_legitimacy, 0.3, "legitimacy target lost per unit of policy outside the window")          \
    X(overton_backlash, 0.5, "protest drive per unit of liberty-restricting policy outside the window")  \
    X(overton_rally, 0.6, "shift of the acceptable range toward restriction at full perceived threat (rally; Bol et al. 2021)") \
    X(thermostat, 0.05, "annual opinion shift against the direction of economic policy (Wlezien 1995)") \
    X(party_entry_distance, 0.5, "distance to the nearest party beyond which a voter is unrepresented")  \
    X(party_entry_share, 0.15, "unrepresented share of voters at which a challenger party forms")      \
    X(party_exit_months, 24.0, "months below 1% of the vote after which a party dissolves")            \
    X(selectorate_firm_tilt, 0.3, "extra fiscal share to firms as the winning coalition shrinks")        \
    /* ---- geopolitics ---- */                                                                             \
    X(trust_years, 2.0, "adjustment time of bilateral trust (years)")                                     \
    X(sanction_trade_cut, 0.8, "trade lost at full sanctions")                                            \
    X(sanction_security_gain, 1.0, "pragmatic sanction level per unit threat above 0.2")                  \
    X(sanction_dependence_cost, 3.0, "pragmatic sanction reduction per unit export dependence")            \
    X(sanction_ideology_gain, 1.0, "ideological sanction level per unit regime distance above 0.3")       \
    X(war_base_rate, 0.002, "annual war-onset hazard scale for a fully threatened, distrustful pair")      \
    X(peace_base_rate, 0.5, "annual settlement hazard of a costless war")                                 \
    X(war_casualty_rate, 0.001, "annual deaths per resident in front regions")                            \
    X(war_destruction_rate, 0.05, "annual capital destroyed in front regions")                            \
    /* ---- finance ---- */                                                                                 \
    X(fx_current_account, 2.0, "annual log real appreciation per unit current-account improvement / GDP")  \
    X(fx_interest, 0.5, "annual log appreciation per 100pp interest differential vs the US (UIP)")       \
    X(fx_risk, 1.0, "annual log depreciation per 100pp rise of the sovereign spread (capital flight)")    \
    X(fx_export_elasticity, 0.6, "export volume elasticity to the real exchange rate")                    \
    X(fx_import_elasticity, 0.4, "import volume elasticity to the real exchange rate")                    \
    X(fx_passthrough, 0.2, "share of depreciation passed into core inflation")                            \
    X(spread_debt, 3.0, "spread (pp) per unit debt/GDP above 60%")                                        \
    X(spread_deficit, 20.0, "spread (pp) per unit deficit/GDP")                                           \
    X(spread_capacity, 4.0, "spread (pp) at zero state capacity")                                         \
    X(spread_legitimacy, 3.0, "spread (pp) at zero legitimacy")                                           \
    X(spread_reserve, 3.0, "spread reduction (pp) for a full reserve currency")                           \
    X(debt_rollover_years, 7.0, "average debt maturity: years for spreads to reach the effective rate")   \
    X(default_spread, 12.0, "spread (pp above start) at which a sovereign defaults")                      \
    X(default_haircut, 0.3, "share of government debt written off in a default")                         \
    /* ---- long run ---- */                                                                               \
    X(capital_hysteresis, 0.07, "annual potential change per unit investment shortfall/excess")            \
    X(institutions_growth, 0.02, "trend-growth change per unit liberal-index change")                     \
    X(human_capital_growth, 0.03, "trend-growth change per unit tertiary-share change")                   \
    X(co2_ppm0, 410.0, "atmospheric CO2 at the start (ppm)")                                              \
    X(temperature0, 1.1, "warming above pre-industrial at the start (C)")                                 \
    X(emissions0, 36.4, "world CO2 emissions at the start (Gt/yr)")                                        \
    X(airborne_fraction, 0.45, "share of emissions staying in the atmosphere")                            \
    X(climate_sensitivity, 3.0, "equilibrium warming per CO2 doubling (C)")                               \
    X(climate_lag_years, 27.0, "adjustment time of transient warming (years)")                            \
    X(damage_quadratic, 0.00236, "DICE-2016R output loss per C^2")                                       \
    X(damage_heat_tilt, 0.06, "extra damage per C of regional mean temperature above 13 C (Burke et al.)") \
    /* ---- logistics (systems/logistics.cpp) ---- */                                                       \
    X(ship_speed_knots, 15.0, "mean container-ship service speed (knots; slow steaming)")                 \
    X(port_time_days, 1.0, "ship time per port call at normal utilisation (days)")                       \
    X(port_util0, 0.7, "berth utilisation of container ports at the start")                               \
    X(port_util_max, 0.97, "berth utilisation cap of the congestion queue")                               \
    X(port_absence_effect, 1.0, "port throughput lost per unit of working-age infectious absence")       \
    X(port_npi_effect, 0.1, "port throughput lost at maximum workplace closures (ports were essential)") \
    X(inventory_cover_days, 25.0, "days of imported inputs held by manufacturers")                         \
    X(inventory_reorder, 0.05, "daily restocking toward target cover (share of the gap)")                  \
    X(sea_supply_share, 0.3, "share of manufacturing inputs that arrive by sea from abroad")               \
    X(delay_smoothing_days, 90.0, "smoothing of transit delays treated as persistent trade costs (days)") \
    X(trade_delay_semi, 0.01, "trade volume lost per day of persistent extra transit (Djankov et al. 2010)") \
    X(trade_freight_elasticity, 4.0, "trade elasticity to ad-valorem freight costs (Simonovska & Waugh 2014)") \
    X(freight_cost_share0, 0.03, "ocean freight cost as a share of shipped goods value at the start (Hummels 2007)") \
    X(freight_passthrough, 0.6, "share of freight-driven import-cost changes passed into consumer prices") \
    X(fleet_util0, 0.88, "container-fleet utilisation at the start")                                     \
    X(fleet_idle0, 0.03, "idle share of the container fleet at the start (Alphaliner 2019)")              \
    X(fleet_deploy_days, 30.0, "time for carriers to adjust deployed capacity (blank sailings)")          \
    X(fleet_util_target, 0.88, "utilisation carriers aim for when deploying capacity (= fleet_util0 for a steady start)")                    \
    X(freight_curve, 1.5, "convexity of the freight supply curve near full utilisation (Stopford 2009)") \
    X(freight_speed, 0.04, "daily adjustment of spot freight rates toward their target")                 \
    X(freight_contract_days, 180.0, "lag of contract rates behind spot rates (days)")                    \
    X(freight_spot_share, 0.4, "share of cargo moving at spot (vs contract) rates")                       \
    X(newbuild_order_rate, 0.06, "annual orders (share of fleet) per unit spot rate above 1.3")           \
    X(newbuild_lag_days, 900.0, "mean delivery lag of new ships (days; Kalouptsidi 2014)")               \
    X(vessel_size_growth, 0.03, "annual growth of mean vessel size (raises grounding hazard)")            \
    X(incident_rate_scale, 1.0, "multiplier on chokepoint incident and drought hazards (0 = none; what-if and tests)") \
    X(fleet_ships, 5300.0, "container ships in the world fleet (2019; converts queues into fleet capacity)") \
    X(container_ship_share, 0.3, "share of chokepoint transits that are container ships")                \
    X(war_risk_days, 30.0, "routing penalty (days) of a passage closed by conflict (war-risk insurance)") \
    X(io_network, 1.0, "1 = dynamic input-output production network; 0 = output = min(demand, capacity)") \
    X(io_import_trade_share, 0.6, "share of imports that are intermediate inputs (OECD TiVA: ~60%)")     \
    X(io_order_days, 10.0, "time over which firms restore input stocks to target (Pichler et al. 2022)")   \
    X(io_import_floor, 0.15, "smallest imported share of intermediate inputs")                            \
    X(divert_cost_ratio, 1.0, "carriers' cost of a diversion day relative to a waiting day")              \
    X(chokepoint_war_closure, 0.8, "share of a chokepoint closed while a littoral nation is at war")     \
    X(chokepoint_unrest_closure, 0.15, "share closed per unrest level above 3 in a littoral nation")     \
    X(drought_temp_sensitivity, 0.5, "drought hazard increase per C of warming above the start")         \
    X(domestic_logistics_npi, 0.25, "internal freight capacity lost at maximum movement restrictions")   \
    X(domestic_logistics_absence, 1.0, "internal freight capacity lost per unit working-age absence")    \
    /* ---- commodities (systems/commodities.cpp) ---- */                                                     \
    X(food_passthrough, 0.3, "share of world grain price changes passed to domestic food prices")        \
    X(food_logistics_shock, 0.04, "food relative price rise at zero internal freight capacity, per unit demand inelasticity") \
    X(food_price_days, 45.0, "adjustment time of domestic relative food prices (days)")                  \
    X(gas_cpi_share, 0.015, "default CPI weight of gas-linked household energy")                         \
    /* ---- credit and banking (systems/credit.cpp) ---- */                                                   \
    X(credit_rate_semi, 0.015, "annual credit growth per 1pp fall of the real lending rate")             \
    X(credit_cycle, 0.2, "annual credit growth per unit smoothed output gap")                            \
    X(credit_tightening, 0.4, "annual credit growth lost per unit unemployment rise (lending standards)") \
    X(credit_liquidity, 0.8, "annual firm-credit growth per unit negative output gap: credit-line draws (Li, Strahan & Zhang 2020)") \
    X(guarantee_reach, 0.5, "share of private banks' lending reachable by state guarantee schemes (directed credit)") \
    X(credit_deleveraging, 0.2, "annual credit growth lost per unit credit gap (stock above path)")      \
    X(credit_crunch, 2.0, "annual credit growth lost per unit capital-ratio shortfall")                  \
    X(credit_npl_overhang, 0.5, "annual credit growth lost per unit NPL ratio above npl_reference")      \
    X(npl_reference, 0.03, "NPL ratio above which weak balance sheets restrain lending")                  \
    X(credit_spend_hh, 0.6, "share of extra household credit spent on goods (rest held as deposits)")   \
    X(credit_spend_firm, 0.8, "share of extra firm credit spent on investment")                           \
    X(credit_fx_leak, 0.25, "share of extra household credit converted to foreign assets, x deposit dollarisation") \
    X(directed_firm_share, 0.75, "share of directed credit lent to firms")                               \
    X(directed_slack, 0.6, "pragmatic directed credit (% GDP/yr) per point of slack x state-bank share") \
    X(directed_fx_brake, 30.0, "pragmatic directed credit cut (% GDP/yr) per unit annual depreciation")  \
    X(directed_bias_gain, 3.0, "how much a growth-first executive amplifies directed credit")           \
    X(cb_pressure_pp, 3.0, "rate cut (pp) a growth-first executive presses on a dependent central bank") \
    X(fx_crisis_depreciation, 0.3, "annual depreciation at which a growth-first executive turns orthodox") \
    X(capital_ratio_min, 0.12, "capital ratio below which banks cut lending (requirement + buffers)")   \
    X(rwa_density, 0.6, "risk-weighted assets per unit of loans")                                         \
    X(npl_unemployment, 0.5, "NPL ratio rise per unit unemployment rise (Nkusu 2011)")                                 \
    X(npl_gap, 0.3, "NPL ratio rise per unit negative output gap")                                        \
    X(npl_fx, 0.3, "NPL ratio rise per unit log depreciation x FX loan share")                           \
    X(npl_boom, 0.1, "NPL ratio rise per unit credit gap (bad loans from booms)")                         \
    X(npl_days, 365.0, "recognition lag of non-performing loans (days; moratoria, forbearance)")        \
    X(npl_forbearance, 2.0, "extra NPL recognition lag (x) while crisis support (job retention, directed credit) runs (BCBS 2020)") \
    X(credit_depth_ref, 0.6, "credit/GDP at which state banks can direct lending at full scale")     \
    X(npl_spread, 0.5, "lending-rate rise (pp) per pp NPL ratio rise")                                    \
    X(loss_given_default, 0.45, "share of a non-performing loan lost")                                    \
    X(workout_years, 3.0, "years to work out non-performing loans")                                       \
    X(bank_margin_share, 0.3, "share of loan-interest changes kept by banks after deposit repricing")   \
    X(guarantee_share, 0.6, "share of losses on directed loans covered by government guarantees")       \
    X(fx_balance_sheet, 0.5, "investment lost per unit log depreciation x FX loan share (Cespedes et al. 2004)") \
    X(fx_dollarisation, 2.0, "annual log depreciation per unit (deposit dollarisation x real-rate fall / 100)") \
    X(fx_flight, 1.0, "annual log depreciation per unit capital flight / GDP")                            \
    X(fx_passthrough_base, 0.1, "exchange-rate pass-through at zero trend inflation (Taylor 2000)")       \
    X(fx_passthrough_inflation, 2.5, "extra pass-through per unit trend inflation")                       \
    X(fx_passthrough_max, 0.8, "largest exchange-rate pass-through")                                       \
    /* ---- bonds and yields (systems/bonds.cpp) ---- */                                                     \
    X(rate_persistence, 0.35, "annual mean reversion of expected short rates toward neutral")            \
    X(bond_horizon_years, 10.0, "maturity of the benchmark bond")                                         \
    X(short_issue_share, 0.25, "share of new government borrowing issued short (bills)")                 \
    X(tp_inflation, 0.3, "term premium (pp) per pp of expected inflation away from trend")              \
    X(tp_qe, 0.035, "term premium cut (pp) per % of GDP held by the central bank (Gagnon et al. 2011; Altavilla et al. 2015)") \
    X(tp_global, 0.4, "share of US term-premium changes transmitted abroad (Obstfeld 2015)")            \
    X(tp_fx, 0.5, "yield (pp) per pp expected annual depreciation x foreign holder share")              \
    X(tp_haven, 10.0, "term-premium cut (pp) per unit world output gap below zero, for a full reserve currency (flight to quality)") \
    X(tp_reserve_spread, 0.8, "share of spread changes a full reserve currency does not pay")         \
    X(qe_max, 15.0, "largest pragmatic purchase rate (% GDP/yr)")                                        \
    X(qe_gap, 2.0, "Taylor-rate shortfall below the lower bound (pp) that triggers ~63% of qe_max")      \
    X(qe_spread, 4.0, "pragmatic purchases (% GDP/yr) per pp of sovereign spread rise above 1pp")       \
    X(qe_cap, 0.5, "largest central-bank share of government debt")                                       \
    X(i_long_weight, 0.5, "weight of the long yield (vs policy rate) in the cost of capital")            \
    /* ---- identity groups, youth, collective action (systems/society.cpp) ---- */                          \
    X(network_insurance, 0.3, "share of a networked pop's income shortfall covered by its group")      \
    X(network_return_drag, 0.7, "slower return to job search of networked laid-off workers (return migration)") \
    X(horizontal_weight, 0.5, "grievance per unit log income gap to the national mean (Stewart 2008)") \
    X(exclusion_weight, 0.2, "grievance per unit political exclusion of the group (Cederman et al. 2010)") \
    X(group_mobilisation, 0.6, "drive from a group's grievance x network cohesion")                     \
    X(youth_bulge_ref, 0.33, "youth (15-29) share of 15-64 at which mass mobilisation is unamplified")   \
    X(corruption_amplifier, 1.0, "grievance amplification per unit corruption x connectivity")          \
    X(exit_blocked, 0.3, "grievance from blocked emigration per unit emigrant stock x travel ban (Hirschman 1970)") \
    X(cascade_mu, 0.6, "threshold location of protest participation (Granovetter 1978)")                \
    X(cascade_sigma, 0.15, "threshold dispersion of protest participation")                               \
    X(cascade_social, 2.0, "bandwagon weight of observed participation (x 0.5 + connectivity)")         \
    X(cascade_max, 0.1, "largest share of the population that ever takes part")                           \
    X(protest_fatigue_gain, 0.01, "daily rise of the joining cost at full participation (exhaustion; Tarrow 1998)") \
    X(protest_fatigue_days, 120.0, "recovery time of protest fatigue (days)")                             \
    X(unrest_rise_days, 4.0, "escalation time of regional unrest (days)")                                 \
    X(unrest_decay_days, 60.0, "de-escalation time of regional unrest (days)")                            \
    X(spark_rate, 3.0, "annual rate of triggering incidents per region (Kuran 1991)")                    \
    X(spark_size, 0.08, "mean drive added by a triggering incident")                                      \
    X(spark_days, 10.0, "decay time of a triggering incident (days)")                                     \
    X(censor_effect, 0.6, "coordination lost at full information control")                                \
    X(censor_backfire, 0.8, "drive from a sudden information-control rise x connectivity x youth (Hobbs & Roberts 2018)") \
    X(censor_trigger, 1.5, "unrest level at which an order-minded executive restricts media")           \
    X(strike_capacity, 0.03, "regional capacity lost per unrest level above 2")                           \
    /* ---- internet and cyber (systems/internet.cpp) ---- */                                               \
    X(net_control_scale, 0.8, "starting domestic information control per unit (1 - net freedom)")       \
    X(net_filter_threshold, 0.4, "lack of net freedom above which cross-border filtering starts")        \
    X(net_diffusion, 0.3, "annual value convergence toward coupled foreign publics (Gleditsch & Ward 2006)") \
    X(net_contagion, 0.6, "protest drive per unit coupled foreign protest participation (share of x_max)") \
    X(net_tie_floor, 0.02, "minimum tie between two societies (language, diaspora, media) besides trade") \
    X(net_digital_trade, 0.3, "share of services exports and imports delivered online")                 \
    X(net_growth_cost, 0.005, "trend-growth loss per unit filtering above the start (Czernich et al. 2011)") \
    X(net_filter_gain, 0.8, "pragmatic filtering per unit foreign protest contagion")                   \
    X(cyber_rate, 1.0, "annual cyber-operation hazard scale for a capable, threatened, distrustful pair") \
    X(cyber_damage_mean, 0.01, "mean capacity loss of critical sectors from a cyber operation")         \
    X(cyber_days, 20.0, "recovery time of cyber damage (days)")                                         \
    X(cyber_influence, 0.05, "protest drive from an influence operation (decays with cyber_days)")      \
    /* ---- war economics (systems/geopolitics.cpp) ---- */                                                 \
    X(war_rents, 2.0, "war-onset hazard increase per unit resource rents / GDP of the target (Collier & Hoeffler 2004)") \
    X(war_diversion, 0.5, "hazard increase per unrest level above 1 in a non-electoral regime (Levy 1989)") \
    X(war_transition, 5.0, "hazard increase per unit gap in realised growth (power transition; Organski & Kugler 1980)") \
    X(war_spending, 0.05, "military procurement during war, share of GDP per year")                     \
    X(drone_strike, 0.5, "extra destruction from precision strike (drones, missiles) per unit technology") \
    X(drone_infrastructure, 0.02, "annual energy-infrastructure destruction across the defender per unit attacker strike technology") \
    X(insurgency_days, 60.0, "days at insurgency-level unrest before an insurgency can start")         \
    X(insurgency_rate, 0.5, "annual insurgency onset hazard at full exclusion and no state capacity (Fearon & Laitin 2003)") \
    X(insurgency_end_rate, 0.3, "annual hazard that an insurgency ends at full state capacity")         \
    /* ---- firms (systems/firms.cpp) ---- */                                                                \
    X(sme_cash_days, 27.0, "cash buffer of small firms in days of costs (JPMorgan Chase Institute 2016)") \
    X(large_cash_days, 60.0, "cash buffer of large firms in days of costs")                             \
    X(firm_fixed_cost, 0.35, "share of firm costs that do not fall with output in the short run (rent, debt, salaried staff)") \
    X(firm_exit_base_sme, 0.08, "annual exit rate of small firms in normal times (OECD business demography)") \
    X(firm_exit_base_large, 0.03, "annual exit rate of large firms in normal times")                    \
    X(firm_fragility, 6.0, "exit-rate multiple at an exhausted cash buffer (financial fragility; Delli Gatti et al. 2005)") \
    X(firm_support_cover, 0.8, "share of a firm's shortfall covered per unit of support / private wage bill") \
    X(firm_reallocation, 0.5, "share of an exiting firm's capacity taken up by survivors (Caballero & Hammour 1994)") \
    X(firm_entry_gain, 1.0, "entry above the base rate per unit of buffer above normal (Dosi et al. 2010)") \
    X(firm_lgd, 0.5, "loss on the loans of failed firms")                                                \
    X(board_hoarding_gain, 0.4, "extra labour hoarding of a fully coordinated board (Hall & Soskice 2001)") \
    X(tariff_pain, 0.05, "pragmatic tariff (ad valorem) per unit manufacturing job loss (Bown & Crowley 2013)") \
    X(tariff_appreciation, 0.05, "pragmatic tariff per unit log real appreciation (Bown & Crowley 2013)") \
    X(tariff_inflation_brake, 2.0, "pragmatic tariff cut per unit inflation above trend")              \
    X(tariff_nationalism, 0.08, "ideological tariff per unit nationalist (TAN) position of the governing party") \
    X(tariff_elasticity, 4.0, "import volume elasticity to tariffs (Simonovska & Waugh 2014)")          \
    X(tariff_retaliation, 0.5, "partner sanction step per unit tariff rise (reciprocity)")              \
    /* ---- institutions (systems/institutions.cpp) ---- */                                                 \
    X(bill_threshold, 0.5, "legislative support needed to pass a bill with no veto players")           \
    X(bill_veto_threshold, 0.2, "extra support needed per unit veto-player strength (Tsebelis 2002)")   \
    X(bill_discipline, 0.9, "support of governing parties for their government's bills")                \
    X(bill_rally, 0.4, "opposition support added per unit perceived threat (crisis bills)")             \
    X(no_confidence_rate, 0.5, "annual no-confidence success hazard of a minority government at zero approval") \
    X(snap_election_days, 60.0, "days from a lost confidence vote to the snap election")                \
    X(court_rate, 0.3, "monthly probability that an independent judiciary strikes down a forced lever") \
    X(military_loyalty_years, 1.0, "adjustment time of military loyalty (years)")                      \
    X(coup_loyalty, 0.35, "loyalty below which the military may attempt a coup (Powell 2012; Svolik 2012)") \
    X(defection_participation, 0.035, "protest participation at which a disloyal military defects (Chenoweth & Stephan 2011)") \
    X(defense_threat, 0.05, "pragmatic defence spending (% GDP) per unit external threat")               \
    /* ---- labour (systems/labour.cpp) ---- */                                                              \
    X(wage_curve, 0.1, "real-wage elasticity to unemployment (Blanchflower & Oswald 1994)")            \
    X(wage_rigidity, 0.6, "share of the wage-curve response suppressed at full bargaining coverage")    \
    X(wage_indexation, 0.5, "share of an inflation surprise recovered per unit bargaining coverage")    \
    X(wage_adjust_years, 1.0, "adjustment time of real wages toward the bargaining outcome (years)")   \
    X(labor_demand_elasticity, 0.3, "employment response to real labour costs (Lichter, Peichl & Siegloch 2015)") \
    X(min_wage_low_share, 0.5, "share of low-skill wages tied to the minimum wage at full bite")       \
    X(min_wage_job_loss, 0.05, "low-skill job loss per unit log minimum-wage rise (Cengiz et al. 2019: small)") \
    X(union_militancy, 0.02, "annual real-wage push per unit union density x left position")          \
    X(strike_drive, 0.5, "protest drive per unit real-wage loss x union density")                      \
    /* ---- fiscal (systems/fiscal.cpp) ---- */                                                                \
    X(eti, 0.25, "elasticity of taxable income to the net-of-tax rate (Saez, Slemrod & Giertz 2012)")   \
    X(compliance_legitimacy, 0.3, "tax compliance per unit legitimacy (Levi 1988)")                    \
    X(compliance_trust, 0.3, "tax compliance per unit social trust")                                    \
    X(consolidation_spread, 0.02, "pragmatic household tax rise per pp spread above start")           \
    X(tax_ideology, 0.1, "ideological tax rise per unit economic-left position of the governing party") \
    X(public_investment_slack, 0.1, "pragmatic public investment (% GDP) per point of slack")          \
    X(public_capital_elasticity, 0.1, "output elasticity to public capital (Bom & Ligthart 2014)")      \
    X(public_capital_ratio, 0.6, "public capital stock / GDP at the start")                             \
    /* ---- society extensions ---- */                                                                       \
    X(social_trust_years, 10.0, "adjustment time of social trust (years; Putnam 1993)")                       \
    X(trust_inequality, 1.0, "trust lost per unit Gini rise")                                           \
    X(trust_unrest, 0.1, "trust lost per unrest level")                                                  \
    X(trust_compliance, 0.5, "NPI compliance elasticity to social trust (Bargain & Aminjonov 2020)")     \
    X(crime_unemployment, 3.0, "crime index rise per unit unemployment rise (Becker 1968; Raphael & Winter-Ebmer 2001)") \
    X(crime_investment, 0.05, "investment lost per unit crime-index rise")                             \
    X(fertility_income, 0.3, "elasticity of the birth rate to income per head (demographic transition)") \
    X(fertility_education, 0.5, "elasticity of the birth rate to the tertiary share (Bongaarts 1978)")  \
    X(fertility_years, 10.0, "adjustment time of the birth rate to its long-run level (years)")         \
    X(baby_bust, 1.0, "birth-rate cut per unit unemployment rise (Kearney & Levine 2021)")             \
    X(tertiary_drift, 0.004, "annual rise of the tertiary share with income growth (educational expansion)") \
    X(scarring, 0.5, "permanent wage loss of a cohort per unit unemployment rise at entry (Kahn 2010)") \
    X(engel_elasticity, 0.4, "fall of the food budget share per unit log income (Engel's law)")        \
    /* ---- health extensions ---- */                                                                        \
    X(vaccine_world_capacity, 0.003, "world vaccinations per resident per day at full production")      \
    X(vaccine_income_priority, 1.0, "allocation weight elasticity to income per head (advance purchases)") \
    X(pandemic_rate, 0.02, "annual probability that a novel epidemic emerges (Marani et al. 2021)")    \
    /* ---- assets (systems/assets.cpp) ---- */                                                                  \
    X(housing_wealth_gdp, 2.0, "housing wealth / GDP at the start")                                     \
    X(house_user_cost_base, 0.05, "user cost of housing at the start (real rate + depreciation + risk)") \
    X(house_price_years, 3.0, "adjustment time of house prices to fundamentals (years)")               \
    X(house_credit, 0.5, "house-price response to the household credit gap (Favara & Imbs 2015)")     \
    X(equity_risk_premium, 0.05, "equity risk premium")                                                  \
    X(mpc_housing, 0.05, "consumption per unit housing-wealth change (Mian, Rao & Sufi 2013)")         \
    X(mpc_equity, 0.02, "consumption per unit equity-wealth change")                                    \
    X(equity_wealth_gdp, 1.0, "household equity wealth / GDP at the start")                             \
    /* ---- treaties (systems/treaties.cpp) ---- */                                                             \
    X(alliance_threat, 0.3, "shared threat above which allies form (balance of threat; Walt 1987)")    \
    X(fta_rate, 0.2, "annual FTA-formation hazard for a close, trusting, interdependent pair")          \
    X(fta_trade_gain, 0.7, "log trade gain of a mature FTA (Baier & Bergstrand 2007)")                  \
    X(fta_years, 10.0, "phase-in time of an FTA's trade gain (years)")                                 \
    X(imf_spread, 6.0, "spread (pp above start) that triggers an IMF programme")                       \
    X(imf_loan, 0.05, "IMF programme size (share of GDP)")                                               \
    X(imf_catalytic, 2.0, "spread relief (pp) of a programme (catalytic effect)")                      \
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
