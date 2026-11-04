#pragma once
// Simulation systems. Each takes the whole game_state; cadence and sharding
// are decided in sim/simulation.cpp (docs/02-simulation-core.md#cadence-scheduler).

#include "sim/game_state.hpp"

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

// Every 28 days: exchange rates, sovereign spreads and default, remittance index;
// long-run potential, trend growth and climate (docs/03-economy.md).
void update_finance(game_state& state);
void update_longrun(game_state& state);
[[nodiscard]] float compute_spread(game_state const& state, dcon::nation_id n);
[[nodiscard]] float payer_income_index(game_state const& state, dcon::nation_id n);
[[nodiscard]] float world_oil_demand(game_state const& state);

// Daily: annual accumulators; monthly-sharded Gini.
void update_indicators(game_state& state);

// Gini of disposable income across a nation's pops, assuming a lognormal
// within each group (employed / unemployed / inactive) with dispersion `sigma`
// (pass a negative value to use each pop's calibrated inc_sigma).
[[nodiscard]] float compute_gini(game_state const& state, dcon::nation_id n, float sigma = -1.0f);

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
