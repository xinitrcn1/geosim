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
};

// pop.values axes (Inglehart-Welzel + economic left-right), each roughly -1..1
enum class value_axis : uint8_t { secular = 0, self_expression = 1, economic_right = 2, count };
// pop.salience issues (weights sum to 1)
enum class issue : uint8_t { economy = 0, health = 1, security = 2, identity = 3, count };

// agent.kind
enum class agent_kind : uint8_t { executive = 0, central_bank = 1 };

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
    count
};

// nation.electoral_system
enum class electoral_system : uint8_t { none = 0, pr = 1, fptp = 2 };

// decision.lever codes for non-lever journal entries
inline constexpr uint8_t journal_election = 0xF0;
inline constexpr uint8_t journal_coup = 0xF1;
inline constexpr uint8_t journal_war = 0xF2;
inline constexpr uint8_t journal_peace = 0xF3;
inline constexpr uint8_t journal_sanction = 0xF4;
inline constexpr uint8_t journal_default = 0xF5;
inline constexpr uint8_t journal_chokepoint = 0xF6; // before = chokepoint index, after = blockage days drawn

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
    count
};

} // namespace geosim::sim
