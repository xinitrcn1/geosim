#pragma once
// Scenario creation: parse moddable text assets (assets/) into game_state::world
// and calibrate the 2019 baseline. The result is usually saved straight away
// as a binary (sim/save.hpp) so later starts skip parsing.

#include "sim/game_state.hpp"

#include <string>
#include <vector>

namespace geosim::sim {

struct scenario_report {
    std::vector<std::string> warnings; // missing data replaced by defaults, clamps, ...
    std::size_t events = 0;
};

// Throws std::runtime_error (with file:line) on malformed assets.
// `state` must be freshly constructed.
scenario_report build_scenario(game_state& state, std::string const& assets_dir, std::string const& scenario);

// Recomputes game_state::derived from world (call after build or load).
void rebuild_derived(game_state& state);

[[nodiscard]] dcon::nation_id find_nation(game_state const& state, std::string_view iso3) noexcept;

} // namespace geosim::sim
