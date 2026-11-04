#pragma once
// Scenario creation: parse moddable text assets (assets/) into game_state::world
// and calibrate the 2019 baseline. The result is usually saved straight away
// as a binary (sim/save.hpp) so later starts skip parsing.

#include "core/diagnostics.hpp"
#include "sim/game_state.hpp"

#include <string>
#include <vector>

namespace geosim::sim {

struct scenario_report {
    std::vector<std::string> warnings; // missing data replaced by defaults, clamps, ...
    std::size_t events = 0;
    std::string error;                 // first error ("file:line: message"); empty = built
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Malformed assets stop the build: `error` holds the first problem (file:line)
// and `state` is left partly built (discard it). `state` must be freshly constructed.
scenario_report build_scenario(game_state& state, std::string const& assets_dir, std::string const& scenario);

// Recomputes game_state::derived from world (call after build or load).
void rebuild_derived(game_state& state);

[[nodiscard]] dcon::nation_id find_nation(game_state const& state, std::string_view iso3) noexcept;

// Extension loaders (src/sim/scenario_ext.cpp): the shipping network and commodity
// definitions (assets/defs, after nations exist), and per-nation ports, overland
// shares, logistics and commodity tables (after calibration).
void load_sea_network(game_state& state, std::string const& assets_dir, core::diagnostics& diag);
void load_commodity_defs(game_state& state, std::string const& assets_dir, core::diagnostics& diag);
void load_technology_defs(game_state& state, std::string const& assets_dir, core::diagnostics& diag);
void load_trade_tables(game_state& state, std::string const& scenario_dir, core::diagnostics& diag);
// Nuclear arsenals and strategic rivalries (optional; docs/06).
void load_conflict_tables(game_state& state, std::string const& scenario_dir, core::diagnostics& diag);
// International organisations (defs/organisations.txt; memberships per scenario).
void load_organisation_defs(game_state& state, std::string const& assets_dir, core::diagnostics& diag);
// Faults, cyclone basins and ENSO teleconnections (assets/defs; systems/disasters.cpp).
void load_hazard_defs(game_state& state, std::string const& assets_dir, core::diagnostics& diag);

} // namespace geosim::sim
