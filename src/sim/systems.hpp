#pragma once
// Simulation systems. Each takes the whole game_state; cadence and sharding
// are decided in sim/simulation.cpp (docs/02-simulation-core.md#cadence-scheduler).

#include "sim/game_state.hpp"

namespace geosim::sim {

// Daily: applies events dated today (historical anchors, seeds).
void apply_events(game_state& state);

// Daily: metapopulation SEIR (docs/04-society.md#health-and-epidemics).
void update_epidemic(game_state& state);

// Weekly per nation: production, demand, labour, prices, ledger flows
// (docs/03-economy.md). `dt` is the number of days covered.
enum class economy_mode { simulate, baseline_probe };
void update_economy(game_state& state, dcon::nation_id n, float dt, economy_mode mode = economy_mode::simulate);

// Daily: world crude market (docs/03-economy.md#prices).
void update_oil_market(game_state& state);

// Daily: AI agents choose policy levers from pragmatic targets, ideology and
// historical anchors (priors); applies player overrides.
void update_ai(game_state& state);

// Daily: annual accumulators; monthly-sharded Gini.
void update_indicators(game_state& state);

// Gini of disposable income across a nation's pops, assuming a lognormal
// within each group (employed / unemployed / inactive) with dispersion `sigma`
// (pass a negative value to use each pop's calibrated inc_sigma).
[[nodiscard]] float compute_gini(game_state const& state, dcon::nation_id n, float sigma = -1.0f);

// Helpers shared by systems.
[[nodiscard]] float growth_factor(game_state const& state, dcon::nation_id n) noexcept;
[[nodiscard]] float npi_fraction(game_state const& state, dcon::nation_id n, uint8_t indicator) noexcept;
[[nodiscard]] float reported_deaths_per_million_daily(game_state const& state, dcon::nation_id n) noexcept;
[[nodiscard]] dcon::region_id capital_region(game_state const& state, dcon::nation_id n) noexcept;

} // namespace geosim::sim
