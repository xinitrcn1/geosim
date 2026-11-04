#pragma once
// Enumerations shared by the scenario loader and the systems. These are schema
// constants (what a value means), not game variables.

#include <cstdint>

namespace geosim::sim {

// sector.role
enum class sector_role : uint8_t {
    generic = 0,
    government = 1,    // output = public services, paid for by G; wages paid by government
    energy = 2,        // linked to crude oil production
    contact = 3,       // contact-intensive services: voluntary distancing, substitution source
    manufacturing = 4, // tradable goods: supply-chain exposure, substitution target
};

// npi_indicator.role (indicators with mechanics beyond capacity/transmission weights)
enum class npi_role : uint8_t {
    generic = 0,
    travel = 1,         // C8: cuts imported infections
    testing = 2,        // H2
    tracing = 3,        // H3
    income_support = 4, // E1: raises unemployment benefit replacement/coverage
    stay_home = 5,      // C6: cuts personal mobility (oil demand)
    movement = 6,       // C7: cuts personal mobility (oil demand)
};

// Goods that cross borders by sea and need port and ship capacity are marked
// with sector.shipped = 1 (assets/defs/sectors.txt).

// Flow cause tags (docs/02-simulation-core.md#accounting-core): the ledger
// attributes each flow to the cause set in game_state::cause while it is posted.
// code = (kind << 24) | (nation << 8) | detail; 0 = endogenous (no tag).
enum class cause_kind : uint8_t { none = 0, lever = 1, journal = 2, event = 3 };
[[nodiscard]] constexpr uint32_t cause_code(cause_kind k, uint32_t nation, uint32_t detail) noexcept {
    return (static_cast<uint32_t>(k) << 24) | ((nation & 0xFFFFu) << 8) | (detail & 0xFFu);
}
inline constexpr uint8_t tags_per_flow = 4;

// pop.stratum (docs/02-simulation-core.md#long-campaign-detail-extension)
enum class stratum : uint8_t { base = 0, precarious = 1 };

// pop.kind
enum class pop_kind : uint8_t { low_skill = 0, mid_skill = 1, high_skill = 2, retired = 3 };
inline constexpr uint8_t worker_kinds = 3;

// Epidemic age groups (region.ep_* arrays)
inline constexpr uint8_t age_young = 0;
inline constexpr uint8_t age_old = 1;
inline constexpr uint8_t age_groups = 2;
// Index into region epidemic arrays (ep_s, ep_e, ...): one block of age groups per pathogen.
[[nodiscard]] constexpr uint8_t epi_index(uint8_t pathogen, uint8_t age) noexcept {
    return static_cast<uint8_t>(pathogen * age_groups + age);
}

// event.kind
enum class event_kind : uint8_t {
    npi_level = 1,            // param = indicator index, value = level, value2 = targeted share
    policy_rate = 2,          // value = rate (pct)
    fiscal_package = 3,       // value = size (pct of 2019 GDP)
    epidemic_seed = 4,        // param = pathogen index, value = infections
    job_retention = 5,        // value = coverage, value2 = replacement
    oil_production = 6,       // value = absolute target (mb/d)
    oil_production_delta = 7, // value = change of target (mb/d)
    test_trace_isolate = 8,   // value = strength 0..1 of a suppression strategy
    house_price_shock = 9,    // value = relative change of house prices (e.g. -0.2)
    equity_shock = 10,        // value = relative change of equity prices
    credit_spread_shock = 11, // value = lending-spread change (pp); value2 = days to unwind (0 = persistent)
    bank_capital_shock = 12,  // value = bank capital ratio loss (pp)
};

// pop.values axes (Inglehart-Welzel + economic left-right), each roughly -1..1
enum class value_axis : uint8_t { secular = 0, self_expression = 1, economic_right = 2, count };
// pop.salience issues (weights sum to 1)
enum class issue : uint8_t { economy = 0, health = 1, security = 2, identity = 3, immigration = 4, environment = 5, corruption = 6, count };

// agent.kind
enum class agent_kind : uint8_t { executive = 0, central_bank = 1, board = 2, union_federation = 3, military = 4, governor = 5, rebel = 6 };

// organisation.kind, .rule and membership.role (systems/organisations.cpp)
enum class org_kind : uint8_t { assembly = 0, security = 1, trade = 2, union_ = 3, rights = 4, health = 5, count };
enum class org_rule : uint8_t { majority = 0, veto = 1, qualified = 2, consensus = 3 };
enum class org_role : uint8_t { member = 0, elected = 1, permanent = 2 };

// treaty.kind (systems/agreements.cpp)
enum class treaty_kind : uint8_t { climate = 0, migration = 1, debt_relief = 2, conditional_aid = 3, power_sharing = 4, count };

// region.age_band (systems/demography.cpp): 0-14, 15-29, 30-49, 50-64, 65+
inline constexpr uint8_t age_bands = 5;

// nation.portfolio_party: ministries held by coalition parties (Gamson's law).
// agent.goal: the strategic layer's current goal (systems/strategy.cpp).
enum class goal : uint8_t { none = 0, contain_epidemic, restore_growth, regime_security, deter_threat, consolidate, secure_energy, count };

enum class portfolio : uint8_t { finance = 0, health = 1, interior = 2, foreign = 3, count };

// agent.override_mode (docs/01-gameplay-loop.md#3-act--overrides)
enum class override_mode : uint8_t { none = 0, advise = 1, direct = 2, force = 3 };

// Decision levers. NPI levers come first (one per npi_indicator, index = indicator
// index); the fixed levers follow at derived_indices::lever_base + offset.
enum class lever : uint8_t {
    fiscal = 0,
    stw_coverage,
    stw_replacement,
    tti,
    policy_rate,
    oil_target,
    directed_credit, // executive: extra state-bank lending, % of GDP per year (systems/credit.cpp)
    info_control,    // executive: restrictions on online media and messaging, 0..1 (systems/society.cpp)
    asset_purchases, // central bank: government bond purchases (QE), % of GDP per year (systems/bonds.cpp)
    net_filtering,   // executive: filtering of foreign internet services and content, 0..1 (systems/internet.cpp)
    tariff,          // executive: average import tariff, ad valorem (systems/firms.cpp)
    hoarding,        // board: labour hoarding in a downturn, 0..1 (systems/firms.cpp)
    investment,      // board: investment drive relative to the baseline (systems/firms.cpp)
    tax_scale,       // executive (bill): household tax rate relative to the start (systems/fiscal.cpp)
    public_investment, // executive (bill): public investment above the baseline, % of GDP (systems/fiscal.cpp)
    min_wage,        // executive (bill): minimum wage relative to the start (systems/labour.cpp)
    defense,         // executive (bill): defence spending, % of GDP (systems/institutions.cpp)
    wage_push,       // union: real-wage demand, annual log change (systems/labour.cpp)
    carbon_price,    // executive (bill): carbon price, USD per tonne CO2 (systems/climate.cpp)
    repression,      // executive: use of security forces against protest, 0..1 (systems/society.cpp)
    immigration,     // executive (bill): openness to immigration relative to the start (systems/society.cpp)
    retirement_age,  // executive (bill): statutory retirement age change, years (systems/policy.cpp)
    education,       // executive (bill): education spending above the baseline, % of GDP (systems/policy.cpp)
    health,          // executive (bill): health spending above the baseline, % of GDP (systems/policy.cpp)
    capital_controls,// executive: controls on portfolio flows, 0..1 (systems/policy.cpp)
    regional_npi,    // governor: regional NPI stringency relative to the national level (systems/policy.cpp)
    police,          // executive (bill): police spending, % GDP above baseline (systems/reforms.cpp)
    asylum,          // executive: refugee admission openness relative to 2019
    vat,             // executive (bill): VAT rate shift (pp)
    corporate_tax,   // executive (bill): corporate tax rate shift (pp)
    wealth_tax,      // executive (bill): annual tax on top wealth (pct)
    rd,              // executive (bill): public R&D, % GDP above baseline
    subsidy,         // executive: fuel and food price support (share of the price rise absorbed)
    epl,             // executive (bill): employment protection shift (OECD index points)
    energy_mandate,  // executive (bill): renewables mandate strength 0..1
    vaccine_priority,// executive: share of doses to the old first 0..1
    fx_intervention, // central bank: reserve sales against depreciation 0..1
    macroprudential, // central bank: countercyclical capital buffer (pp)
    count
};

// party.strategy (Laver 2005; Laver & Sergenti 2011)
enum class party_strategy : uint8_t {
    sticker = 0,    // keeps its position (ideological, niche and ruling single parties)
    aggregator = 1, // moves toward the centroid of its own supporters (mainstream parties)
    hunter = 2,     // keeps moving the same way while its vote grows, turns when it falls (challengers)
    predator = 3,   // moves toward the largest party
};
// party.family (ParlGov / CHES families)
enum class party_family : uint8_t { oth = 0, soc, con, lib, chr, grn, rad_right, rad_left, com, reg, agr, challenger };

// nation.electoral_system
enum class electoral_system : uint8_t { none = 0, pr = 1, fptp = 2, mixed = 3, two_round = 4 };

// decision.lever codes for non-lever journal entries
inline constexpr uint8_t journal_election = 0xF0;
inline constexpr uint8_t journal_coup = 0xF1;
inline constexpr uint8_t journal_war = 0xF2;
inline constexpr uint8_t journal_peace = 0xF3;
inline constexpr uint8_t journal_sanction = 0xF4;
inline constexpr uint8_t journal_default = 0xF5;
inline constexpr uint8_t journal_chokepoint = 0xF6; // before = chokepoint index, after = blockage days drawn
inline constexpr uint8_t journal_party_entry = 0xF7; // after = new party's local index
inline constexpr uint8_t journal_party_exit = 0xF8;
inline constexpr uint8_t journal_cyber = 0xF9;      // pragmatic = target nation index, after = damage
inline constexpr uint8_t journal_insurgency = 0xFA; // after = 1 onset, 0 end
inline constexpr uint8_t journal_bill = 0xFB;       // pragmatic = lever, before = status quo, after = enacted, prior = support
inline constexpr uint8_t journal_no_confidence = 0xFC;
inline constexpr uint8_t journal_court = 0xFD;      // pragmatic = lever struck down
inline constexpr uint8_t journal_regime_change = 0xFE;
inline constexpr uint8_t journal_treaty = 0xEF;
inline constexpr uint8_t journal_goal = 0xED;          // before = old goal, after = new goal
inline constexpr uint8_t journal_alliance_call = 0xEC; // pragmatic = ally index, after = 1 joined / 0 declined
inline constexpr uint8_t journal_dispute = 0xEB;       // pragmatic = partner, before/after = hostility level (0 ends; 2..5)
inline constexpr uint8_t journal_concession = 0xEA;    // target concedes to sanctions; pragmatic = sender
inline constexpr uint8_t journal_unsc = 0xE9;          // pragmatic = target, before = kind (0 ceasefire, 1 sanctions, 2 peacekeeping), after = 1 adopted / 0 vetoed / -1 too few votes
inline constexpr uint8_t journal_wto = 0xE8;           // pragmatic = respondent, before = 0 filed / 1 ruled, after = 1 complied / 0 retaliation / -1 appealed into the void
inline constexpr uint8_t journal_variant = 0xE7;       // pragmatic = pathogen index, after = variant number
inline constexpr uint8_t journal_term_limit = 0xE6;    // after = 1 evaded, 0 respected (new leader)
inline constexpr uint8_t journal_court_seat = 0xE5;    // before/after = judicial independence
inline constexpr uint8_t journal_agreement = 0xE4;     // pragmatic = partner, before = treaty kind, after = 1 signed / 0 broken / 2 ended
inline constexpr uint8_t journal_rebels = 0xE3;        // after = 1 rebels formed, 0 settlement, -1 defeated
inline constexpr uint8_t journal_eu_fund = 0xE2;       // after = fund size (share of EU GDP)
inline constexpr uint8_t journal_plan = 0xE1;          // before = goal, after = plan step
inline constexpr uint8_t journal_disaster = 0xEE;   // pragmatic = region index, after = capital lost (share)     // pragmatic = partner, before = kind (0 alliance, 1 FTA, 2 IMF)

// nation.flows index: year-to-date nominal flows (USD), the "Why?" ledger.
enum class flow : uint8_t {
    consumption = 0,
    gov_purchases,
    exports,
    imports,
    wages_private,
    wages_public,
    job_retention_pay,
    unemployment_benefits,
    pensions,
    fiscal_households,
    fiscal_firms,
    tax_households,
    tax_indirect,
    tax_firms,
    tax_resource,
    dividends,
    interest_gov,
    interest_households,
    household_transfers, // between pops: ageing, migration
    remittances_in,
    remittances_out,
    sovereign_default,
    credit,                // bank lending above / below the baseline path, spent on goods
    credit_deposits,       // bank lending above the baseline path, held as deposits
    interest_loans,        // loan interest above / below the baseline path
    credit_guarantee,      // government payments on guaranteed loans that went bad
    commodity_terms,       // terms-of-trade gains / losses on non-oil commodities
    network_transfers,     // kin / caste / hometown network insurance between pops
    war_spending,          // military procurement during wars
    tariffs,               // import tariff revenue (above the start)
    public_investment,     // public investment above the baseline
    defense,               // defence spending above the baseline
    imf_lending,           // IMF programme disbursements (from the rest of the world)
    carbon_tax,            // carbon-price revenue
    fdi,                   // foreign direct investment inflows (net, above baseline)
    portfolio,             // portfolio inflows (net, above baseline)
    public_services,       // education and health spending above baseline
    aid,                   // crisis aid between governments (via the rest of the world)
    arms,                  // arms imports: buyer's firms to the seller's firms
    subsidies,             // fuel and food subsidies (government to households)
    wealth_tax,            // annual wealth tax on top wealth holders
    eu_fund,               // EU recovery-fund grants (via the rest of the world)
    debt_relief,           // sovereign debt restructuring (creditors to the debtor government)
    count
};

} // namespace geosim::sim
