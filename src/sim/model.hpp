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

// pop.kind
enum class pop_kind : uint8_t { low_skill = 0, mid_skill = 1, high_skill = 2, retired = 3 };
inline constexpr uint8_t worker_kinds = 3;

// Epidemic age groups (region.ep_* arrays)
inline constexpr uint8_t age_young = 0;
inline constexpr uint8_t age_old = 1;
inline constexpr uint8_t age_groups = 2;

// event.kind
enum class event_kind : uint8_t {
    npi_level = 1,            // param = indicator index, value = level, value2 = targeted share
    policy_rate = 2,          // value = rate (pct)
    fiscal_package = 3,       // value = size (pct of 2019 GDP)
    epidemic_seed = 4,        // value = infections
    job_retention = 5,        // value = coverage, value2 = replacement
    oil_production = 6,       // value = absolute target (mb/d)
    oil_production_delta = 7, // value = change of target (mb/d)
    test_trace_isolate = 8,   // value = strength 0..1 of a suppression strategy
};

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
    count
};

} // namespace geosim::sim
