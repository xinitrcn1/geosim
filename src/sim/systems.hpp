#pragma once
// Simulation systems. Each takes the whole game_state; cadence and sharding
// are decided in sim/simulation.cpp (docs/02-simulation-core.md#cadence-scheduler).

#include "sim/game_state.hpp"
#include "sim/model.hpp"

#include <string>
#include "sim/model.hpp"
#include <vector>

namespace geosim::sim {

// Daily: applies events dated today (historical anchors, seeds).
void apply_events(game_state& state);

// Daily: metapopulation SEIR (docs/04-society.md#health-and-epidemics).
void update_epidemic(game_state& state);

// Daily driver; each nation updates weekly on its shard (docs/03-economy.md).
// baseline_probe: every nation, one day, incomes only (no money moves), for calibration.
enum class economy_mode { simulate, baseline_probe };
void update_economy(game_state& state, economy_mode mode = economy_mode::simulate);

// Daily: world crude market (docs/03-economy.md#prices).
void update_oil_market(game_state& state);

// Daily: AI agents choose policy levers from pragmatic targets, ideology and
// historical anchors (priors); applies player overrides.
void update_ai(game_state& state);

// Every 28 days (sharded): society (docs/04-society.md).
void update_demography(game_state& state);
void update_migration(game_state& state);
void update_opinion(game_state& state);
void update_unrest(game_state& state);

// Every 28 days (sharded): politics and geopolitics (docs/05, docs/06).
void init_politics(game_state& state);
void update_politics(game_state& state);
void init_geopolitics(game_state& state);
void update_geopolitics(game_state& state);
void update_power(game_state& state);
// Bilateral export access (partners' import demand x trade share x sanctions/war).
[[nodiscard]] float trade_access(game_state const& state, dcon::nation_id n);
// Armington rivals of every market (nation.trade_rival); call before trade_access.
void update_trade_rivals(game_state& state);
void record_bilateral_exports(game_state& state, dcon::nation_id n, float exports);

// Every 28 days: exchange rates, sovereign spreads and default, remittance index;
// long-run potential, trend growth and climate (docs/03-economy.md).
void update_finance(game_state& state);
void update_longrun(game_state& state);
[[nodiscard]] float compute_spread(game_state const& state, dcon::nation_id n);
[[nodiscard]] float payer_income_index(game_state const& state, dcon::nation_id n);
[[nodiscard]] float world_oil_demand(game_state const& state);

// Daily: logistics (docs/03-economy.md#logistics-and-transport). The two init
// functions run once at scenario build: routes before gravity trade shares exist,
// flows after.
void init_logistics_routes(game_state& state);
void init_logistics_flows(game_state& state);
void update_logistics(game_state& state);

// Daily: non-oil commodity markets; CPI factor from food and gas prices.
void update_commodity_markets(game_state& state);
[[nodiscard]] float commodity_cpi_factor(game_state const& state, dcon::nation_id n);

// Every 28 days (sharded): bank credit and bonds (docs/03-economy.md).
void update_credit(game_state& state);
void update_bonds(game_state& state);
[[nodiscard]] float lending_rate(game_state const& state, dcon::nation_id n);
[[nodiscard]] float expected_short_rate(game_state const& state, dcon::nation_id n);

// Daily: internet coupling, protest contagion; monthly: value diffusion and cyber
// operations (docs/06-geopolitics-ai.md#information-and-the-internet).
void update_internet(game_state& state);

// Firms: cohorts, bankruptcies, entry and boards (weekly, economy shard); the
// executive's tariff decision (docs/03-economy.md#firms).
void init_firms(game_state& state);
void update_firms(game_state& state);
void decide_tariff(game_state& state, dcon::agent_id a, dcon::nation_id n);

// Institutions (docs/05): bills through legislatures with veto players; confidence,
// courts and military (monthly). enact() returns the part of a proposed change the
// legislature passes (or the executive can decree).
float enact(game_state& state, dcon::nation_id n, uint8_t lev, float status_quo, float proposed, value_axis axis, float shift_per_unit, bool crisis);
void update_institutions(game_state& state);
void decide_defense(game_state& state, dcon::agent_id a, dcon::nation_id n);
// Labour (monthly): unions, the wage curve, the minimum wage.
void update_labour(game_state& state);
void decide_min_wage(game_state& state, dcon::agent_id a, dcon::nation_id n);
// Fiscal structure (monthly + weekly decisions): progressivity, compliance, public investment.
void update_fiscal(game_state& state);
void decide_fiscal_structure(game_state& state, dcon::agent_id a, dcon::nation_id n);
[[nodiscard]] float household_tax_multiplier(game_state const& state, dcon::nation_id n);
[[nodiscard]] float compliance_ratio(game_state const& state, dcon::nation_id n);
[[nodiscard]] float compliance_target(game_state const& state, dcon::nation_id n);
// Assets and treaties (monthly), vaccines and emergent epidemics (daily).
void update_assets(game_state& state);
void update_treaties(game_state& state);
void update_vaccination(game_state& state);
// Society extensions (monthly): trust, crime, fertility, education, scarring, CPI per pop.
void update_social_capital(game_state& state);

// Long-campaign detail (docs/02): technology diffusion and strata split / merge
// (monthly), spatial LOD (daily).
struct observed_adoption {
    dcon::nation_id nation;
    dcon::technology_id tech;
    float share = 0.0f;
};
void init_technologies(game_state& state, std::vector<observed_adoption> const& observed);
void update_technology(game_state& state);
void update_strata(game_state& state);
[[nodiscard]] float tech_oil_factor(game_state const& state, dcon::nation_id n);
[[nodiscard]] float tech_emission_factor(game_state const& state);
[[nodiscard]] float tech_vaccine_factor(game_state const& state);
[[nodiscard]] float tech_progress(game_state const& state, dcon::nation_id n, uint8_t t);
// Climate policy (docs/03): carbon price (lever), energy vintages, stranded assets,
// disasters (monthly).
void decide_carbon_price(game_state& state, dcon::agent_id a, dcon::nation_id n);
void update_climate_policy(game_state& state);
// Policy catalogue (docs/05): repression, immigration, retirement age, education,
// health, capital controls (weekly levers); cabinets (Gamson's law), governors and
// lobbies (monthly).
void decide_policies(game_state& state, dcon::agent_id a, dcon::nation_id n);
void update_policy(game_state& state);
void allocate_portfolios(game_state& state, dcon::nation_id n);
// Remaining policy catalogue (reforms.cpp): police, asylum, VAT, corporate and wealth
// taxes, R&D, subsidies, EPL, energy mandates, vaccine priority (executive, weekly);
// FX intervention and macroprudential buffers (central bank, daily); monthly channels.
void decide_reforms(game_state& state, dcon::agent_id a, dcon::nation_id n);
void decide_bank_tools(game_state& state, dcon::agent_id bank);
void init_reforms(game_state& state);
void update_reforms(game_state& state);
[[nodiscard]] float reform_growth(game_state const& state, dcon::nation_id n);
[[nodiscard]] float fiscal_capacity_of(game_state const& state, dcon::nation_id n, dcon::region_id r);
[[nodiscard]] float oil_trend_index(game_state const& state);
[[nodiscard]] float price_spike(game_state const& state, dcon::nation_id n);
[[nodiscard]] float pension_ageing_factor(game_state const& state, dcon::nation_id n);
[[nodiscard]] float ifr_age_factor(game_state const& state, dcon::region_id r);
void update_variants(game_state& state);
// Politics extensions (politics_ext.cpp): court appointments, term limits, identity
// legitimacy, willingness to serve (monthly).
void init_politics_ext(game_state& state);
void update_politics_ext(game_state& state);
void term_limit_check(game_state& state, dcon::nation_id n, bool same_party);
// Agreements with terms and compliance (agreements.cpp, monthly): climate pledges,
// migration deals, debt relief, conditional aid; rebels and power sharing.
void init_agreements(game_state& state);
void update_agreements(game_state& state);
[[nodiscard]] bool has_treaty(game_state const& state, treaty_kind k, dcon::nation_id a, dcon::nation_id b);
// HTN-lite plan steps (strategy.cpp): the bias of goal `g` on a lever used from step `step`.
[[nodiscard]] float plan_bias(game_state const& state, dcon::nation_id n, goal g, uint8_t step);
// Strategy layer (docs/06): goals, response tables, information quality,
// credibility and energy dependence (strategy.cpp).
void init_strategy(game_state& state);
void update_strategy(game_state& state);
void update_dependence(game_state& state);
void lose_credibility(game_state& state, dcon::nation_id n, float amount);
[[nodiscard]] float perceived_unrest(game_state const& state, dcon::nation_id n);
[[nodiscard]] float perception_noise(game_state const& state, dcon::nation_id n, uint32_t channel);
[[nodiscard]] float security_of(game_state const& state, dcon::nation_id n);
[[nodiscard]] float rigidity(game_state const& state, dcon::agent_id a, dcon::nation_id n, float crisis);
[[nodiscard]] float goal_bias(game_state const& state, dcon::nation_id n, goal g);
[[nodiscard]] float fiscal_belief(game_state const& state, dcon::nation_id n);
// War (geopolitics.cpp): alliance calls at onset, Lanchester fronts (monthly).
void call_allies(game_state& state, dcon::nation_id victim, dcon::nation_id enemy);
void update_front(game_state& state, dcon::nation_id n, dcon::nation_id m, float years);
[[nodiscard]] float claim_of(game_state const& state, dcon::nation_id claimant, dcon::nation_id holder);
// Disputes and diplomacy (diplomacy.cpp, monthly): escalation ladder, audience costs,
// rivalry, nuclear deterrence, mediation, sanction concessions, crisis aid, arms.
void start_dispute(game_state& state, dcon::nation_id n, dcon::nation_id m);
void start_war(game_state& state, dcon::nation_id n, dcon::nation_id m);
void update_diplomacy(game_state& state);
[[nodiscard]] float mediation_score(game_state const& state, dcon::nation_id n, dcon::nation_id m);
void insurgency_support(game_state const& state, dcon::nation_id n, float& for_government, float& for_rebels);
// International organisations (organisations.cpp): generic membership and voting
// rules; General Assembly sessions, Security Council resolutions, WTO disputes, the
// EU's customs union and recovery fund, Human Rights Council shaming, WHO.
void init_organisations(game_state& state);
void update_organisations(game_state& state);
void compute_un_ideals(game_state& state);
[[nodiscard]] dcon::organisation_id organisation_of(game_state const& state, org_kind k);
[[nodiscard]] dcon::membership_id membership_in(game_state const& state, dcon::organisation_id o, dcon::nation_id n);
[[nodiscard]] bool is_member(game_state const& state, org_kind k, dcon::nation_id n);
[[nodiscard]] int org_role_of(game_state const& state, org_kind k, dcon::nation_id n); // -1 if not a member
[[nodiscard]] bool membership_active(game_state const& state, dcon::membership_id m);
// Do a and b share an (unsuspended) organisation, optionally one with `flag`?
[[nodiscard]] bool shares_org(game_state const& state, dcon::nation_id a, dcon::nation_id b, uint16_t flag);
[[nodiscard]] dcon::organisation_id customs_union_of(game_state const& state, dcon::nation_id n);
// Alliances, clubs, the cartel, development lenders and regional organisations (geo_orgs.cpp);
// mega-events (megaevents.cpp).
void init_geo_organisations(game_state& state);
void update_geo_organisations(game_state& state);
void on_coup(game_state& state, dcon::nation_id n);
// A successful coup (politics.cpp): curtailed elections and liberties, order-minded rulers.
void apply_coup(game_state& state, dcon::nation_id n);
// The player's override on a lever decided outside the AI's blend (advise: a strong prior;
// direct: the value within [lo, hi]; force: the value as given, at a political cost).
[[nodiscard]] float apply_override(game_state& state, dcon::agent_id a, uint8_t lev, float choice, float lo, float hi);
// Weight of the historical anchors in the AI's blend: anchor_prior_weight while the data
// lasts, fading after its last dated decision (else the AI held the last observed lockdown
// for ever once the record ended).
[[nodiscard]] float anchor_weight(game_state const& state);
// Pressure from another agent on lever `lev` of `a` (actions.cpp), blended as weighted
// advice while it lasts; apply_override applies it when the player sets no mode.
[[nodiscard]] float apply_pressure(game_state const& state, dcon::agent_id a, uint8_t lev, float choice, float lo, float hi);
[[nodiscard]] float coup_deterrence(game_state const& state, dcon::nation_id n);
[[nodiscard]] float alliance_burden_push(game_state const& state, dcon::nation_id n);
[[nodiscard]] float club_sanction_shift(game_state const& state, dcon::nation_id n, dcon::nation_id target, float choice);
[[nodiscard]] float regional_mediation_score(game_state const& state, dcon::nation_id n, dcon::nation_id m);
void init_mega_events(game_state& state);
// Natural hazards (disasters.cpp): ENSO, water balance (droughts, floods), tropical
// cyclones, earthquakes and tsunamis.
void init_disasters(game_state& state);
void update_disasters(game_state& state);
struct disaster_text {
    std::string title; // "Hurricane Laura", "Magnitude 7.8 earthquake", "Floods", ...
    std::string date;  // start, YYYY-MM-DD
    disaster_kind kind = disaster_kind::cyclone;
    disaster_status status = disaster_status::ongoing;
    dcon::nation_id nation;
    dcon::region_id region; // the worst-hit region
    float deaths = 0.0f, affected = 0.0f;
    double damage_usd = 0.0;   // capital destroyed and output lost (USD)
    float gdp_loss_pct = 0.0f; // damage as a share of the nation's annual GDP (%)
    bool potential = false;    // a warning: the impact is a forecast
};
[[nodiscard]] disaster_text describe_disaster(game_state const& state, dcon::disaster_id d);

// Player actions (actions.cpp): the diplomatic and political moves the simulation models,
// open to the agent the player controls. Each is answered by the target's own rules (the
// same ones the AI uses) and journalled; the result says whether it was accepted and why.
enum class action_kind : uint8_t {
    set_sanction,       // executive -> nation: sanction level `value` (0..1), held until cleared
    clear_sanction,     // executive -> nation: hand the sanction choice back to the AI
    escalate_dispute,   // executive -> nation: raise hostility one step (opens a dispute at 2)
    back_down,          // executive -> nation: concede an open dispute (audience cost)
    declare_war,        // executive -> nation: war (allies are called; aggression costs trust)
    offer_peace,        // executive -> nation at war: accepted by a side losing or war-weary
    send_aid,           // executive -> nation: one-off grant of `value` % of the donor's GDP
    propose_treaty,     // executive -> nation: treaty `treaty` (conditional aid, migration deal, debt relief)
    climate_pledge,     // executive: pledge a 2030 carbon price of `value` USD/t (Paris-style)
    apply_membership,   // executive -> organisation: apply (decided by its rule)
    leave_membership,   // executive -> organisation: leave
    offer_power_sharing,// executive -> rebels of its nation: settlement (accepted at a stalemate)
    accept_settlement,  // rebel -> its government: seek a settlement (accepted if the state is weak or tired)
    lobby,              // board / union / central bank / military / governor -> its government: press for `value` on lever `lever`
    // Toward another agent (action_request::target_agent), see actions.cpp:
    press,              // any -> a domestic agent with levers: press for `value` on its lever `lever`
    replace_cb_governor,// executive -> its central bank: less independence (refused by courts if it is protected)
    rescue_firm,        // executive -> a domestic board: `value` % of GDP for state equity
    social_pact,        // executive -> its union: a year of wage moderation
    restrict_unions,    // executive -> its union: density and coverage fall
    military_pay,       // executive -> its military: a bonus of `value` % of GDP buys loyalty
    purge_officers,     // executive -> its military: loyalty for fighting power
    joint_exercises,    // executive / military -> a foreign military: mutual trust
    regional_transfer,  // executive -> a governor: `value` % of GDP of public investment in the region
    counterinsurgency,  // executive / military -> its rebels: an offensive
    arm_rebels,         // executive -> foreign rebels: strength rises, their government resents it
    swap_line,          // central bank -> a foreign central bank: `value` months of imports (offer or request)
    emergency_liquidity,// central bank -> a domestic board or governor: a loan of `value` % of GDP
    freeze_rebel_funds, // central bank -> its rebels
    strike,             // union -> a domestic board: lost output for job guarantees
    general_strike,     // union -> its government: strong pressure on lever `lever`, with protest
    wage_deal,          // board -> its union: jobs for wage restraint
    rights_complaint,   // union / rebel -> a rights organisation: shames a repressive government
    seize_power,        // military -> its government: a coup
    demand_autonomy,    // governor -> its government: more decentralisation
    foreign_backing,    // rebel -> a foreign government: arms from the government's rivals
    court_defectors,    // rebel -> its military: loyalty falls
    count
};
struct action_request {
    action_kind kind = action_kind::count;
    dcon::agent_id actor;
    dcon::nation_id target;
    dcon::organisation_id organisation;
    treaty_kind treaty = treaty_kind::count;
    uint8_t lever = 0;
    float value = 0.0f;
    dcon::agent_id target_agent; // actions toward another agent
    bool silent = false;         // not journalled as a player action (the effect of an answered request)
};
// The value an option takes beyond the built-in inputs (sanction level, aid, pledge, treaty).
enum class action_value : uint8_t { none, amount, lever };
struct action_option {
    action_kind kind;
    char const* label;
    bool available;      // allowed now for this actor and target
    std::string reason;  // why not (or what it would do)
    action_value input = action_value::none; // amount: `value` in [lo, hi]; lever: one of the target's levers
    char const* unit = nullptr;
    float lo = 0, hi = 0, step = 0, initial = 0;
};
struct action_result {
    bool accepted = false;
    std::string message;
};
// What `actor` can do toward `target` / `organisation` / `target_agent` (any may be empty;
// with a target agent only the options toward it are listed).
[[nodiscard]] std::vector<action_option> available_actions(game_state const& state, dcon::agent_id actor, dcon::nation_id target,
                                                           dcon::organisation_id organisation, dcon::agent_id target_agent = {});
// Performs it (call on the sim thread, between ticks).
action_result perform_action(game_state& state, action_request const& request);
// Pressure on `target`'s lever `lev` toward `value`, weighed at `weight`, for `days`
// (the AI blends it in apply_pressure).
void put_pressure(game_state& state, dcon::agent_id target, dcon::agent_id from, uint8_t lev, float value, float weight, int days = 182);
// A power-sharing settlement with the nation's rebels (ends the insurgency).
void settle_rebellion(game_state& state, dcon::nation_id n);

// Requests (requests.cpp): AI agents ask other agents for something each month when
// their situation calls for it; the target answers by its own rule at once, or - when
// a player controls it and answers its requests - the request waits for the player
// until it expires, and then the target's AI answers.
enum class request_kind : uint8_t {
    restrain_spending, // central bank -> its government: less public investment (pressure)
    cut_rates,         // government -> its central bank: a lower policy rate (pressure)
    raise_min_wage,    // union -> its government: a higher minimum wage (pressure)
    rescue,            // board of a distressed firm -> its government: a rescue package
    military_pay,      // military -> its government: a pay rise
    regional_aid,      // governor of a restless region -> its government: a regional transfer
    swap_line,         // central bank short of reserves -> a foreign central bank
    settlement,        // stalemated rebels -> their government: power sharing
    wage_restraint,    // board of a distressed firm -> its union: jobs for wage restraint
    count
};
struct request_text {
    std::string title;  // "The central bank asks the government to restrain spending"
    std::string detail; // what accepting does
};
void update_requests(game_state& state);
[[nodiscard]] request_text describe_request(game_state const& state, dcon::request_id r);
// The target's own answer (its AI's rule).
[[nodiscard]] bool request_ai_answer(game_state const& state, dcon::request_id r);
// Answers request `r` (accepted or not) by `who` (0: the target's AI at once, 1: the
// player, 2: the AI after the player's window), journals it and deletes it.
action_result answer_request(game_state& state, dcon::request_id r, bool accept, uint8_t who);
void update_mega_events(game_state& state);
// Votes a draft by `sponsor` against `target` (shift: extra support); journals under
// `code` (0 = silent). Returns 1 adopted, 0 vetoed or blocked, -1 too few votes.
float org_vote(game_state& state, dcon::organisation_id o, dcon::nation_id sponsor, dcon::nation_id target, float shift, uint8_t code, float kind_tag);
[[nodiscard]] float un_alignment(game_state const& state, dcon::nation_id a, dcon::nation_id b);
[[nodiscard]] float un_sanction_floor(game_state const& state, dcon::nation_id n, dcon::nation_id target);
[[nodiscard]] float peacekeeping_factor(game_state const& state, dcon::nation_id n, dcon::nation_id m);
[[nodiscard]] float minister_position(game_state const& state, dcon::nation_id n, portfolio pf, value_axis axis);
void init_frontier_gaps(game_state& state);
[[nodiscard]] float carbon_cost_share(game_state const& state, dcon::nation_id n);
[[nodiscard]] float carbon_emission_factor(game_state const& state);

// Every 28 days (sharded): identity groups - horizontal inequality, network
// insurance (docs/04-society.md#identity-groups). Daily: protest cascades.
void update_groups(game_state& state);
void update_protest(game_state& state);

// Daily: annual accumulators; monthly-sharded Gini.
void update_indicators(game_state& state);

// Gini of disposable income across a nation's pops, assuming a lognormal
// within each group (employed / unemployed / inactive) with dispersion `sigma`
// (pass a negative value to use each pop's calibrated inc_sigma).
[[nodiscard]] float compute_gini(game_state const& state, dcon::nation_id n, float sigma = -1.0f);
void compute_distribution(game_state const& state, dcon::nation_id n, float& palma, float& poverty);

// Polity links. DCON relationships keyed on a unique primary link have a slot for
// every primary object, so a lookup can return a relationship with an empty
// partner: always go through these helpers, which check the partner.
[[nodiscard]] dcon::nation_id peg_anchor(game_state const& state, dcon::nation_id n) noexcept;
[[nodiscard]] dcon::nation_id sovereign_of(game_state const& state, dcon::nation_id n, float& autonomy) noexcept;

// Helpers shared by systems.
[[nodiscard]] float growth_factor(game_state const& state, dcon::nation_id n) noexcept;
[[nodiscard]] float npi_fraction(game_state const& state, dcon::nation_id n, uint8_t indicator) noexcept;
[[nodiscard]] float reported_deaths_per_million_daily(game_state const& state, dcon::nation_id n) noexcept;
[[nodiscard]] dcon::region_id capital_region(game_state const& state, dcon::nation_id n) noexcept;

} // namespace geosim::sim
