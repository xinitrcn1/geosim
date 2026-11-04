#pragma once
// The single owner of all simulation state.
//
// Rule: every game variable lives on `game_state`. Per-entity data (nations,
// regions, pops, sectors, events, ...) lives in the DCON container
// `game_state::world`; the remaining scalars are plain members here. No globals
// or function-local statics hold game data. Systems take a `game_state&`.
// See docs/09-engine-architecture.md#game-state.

#include "core/calendar.hpp"
#include "core/scheduler.hpp"
#include "sim/params.hpp"

#if !defined(GEOSIM_HAS_DCON)
#error "game_state requires the generated DCON container (configure with -DGEOSIM_USE_DCON=ON)"
#endif

#include "geosim_dcon.hpp"

#include <cstdint>

namespace geosim {

// Lookups recomputed from `world` after a scenario build or a load
// (sim::rebuild_derived). They are not saved: they hold no information of their own.
struct derived_indices {
    dcon::sector_id gov_sector;
    dcon::sector_id energy_sector;
    dcon::sector_id contact_sector;
    dcon::sector_id manufacturing_sector;
    uint8_t npi_travel = 0xFF;
    uint8_t npi_testing = 0xFF;
    uint8_t npi_tracing = 0xFF;
    uint8_t npi_income_support = 0xFF;
    uint8_t npi_stay_home = 0xFF;
    uint8_t npi_movement = 0xFF;
    dcon::commodity_id oil;
    uint8_t sector_count = 0;
    uint8_t npi_count = 0;
    uint8_t pathogen_count = 0;
    // Levers: [0, npi_count) are NPIs, then lever_base + sim::lever::*.
    uint8_t lever_base = 0;
    uint8_t lever_count = 0;
    uint8_t tech_count = 0;
    // The organisation of each kind (first defined), if any (organisations.cpp).
    dcon::organisation_id org[6];
};

struct game_state {
    dcon::data_container world; // all entity data (SoA)

    sim::params params;           // global model parameters (assets/defs/parameters.txt)
    int32_t current_tick = 0;     // days since the scenario start (next tick to simulate)
    int64_t epoch_days = core::epoch_days_1970; // scenario start, days since 1970-01-01 (scenario.txt start{})
    uint64_t rng_seed = 0;        // Philox key; see docs/02-simulation-core.md#determinism

    derived_indices derived;      // rebuilt, never saved
    uint32_t cause = 0;           // cause tag of the flows being posted (sim::cause_*); transient, never saved
};

} // namespace geosim
