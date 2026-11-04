#pragma once
// Counterfactual forks (docs/01-gameplay-loop.md#52-counterfactual-fork,
// docs/02-simulation-core.md#counterfactual-forks): copy the whole game state,
// apply a change to the copy, run it ahead and return indicator paths. The live
// state is never touched. The copy goes through DCON's serialisation in memory
// (the same path as saves), so a fork is bit-identical to a save/load.

#include "core/diagnostics.hpp"
#include "sim/game_state.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace geosim::sim {

// One nation's indicator path in a fork (one entry per simulated day).
struct fork_path {
    std::string iso3;
    std::vector<float> gdp_index, unemployment, cpi, unrest;
};

struct fork_result {
    std::vector<fork_path> nations;
    std::string error;
};

// Deep copy of a state (no shared data with the original).
[[nodiscard]] std::unique_ptr<game_state> copy_state(game_state const& state, core::diagnostics& diag);

// Copies `state`, calls `change` on the copy (e.g. set an override), advances it
// `days` days and records the paths. `state` is unchanged.
[[nodiscard]] fork_result run_fork(game_state const& state, int days, std::function<void(game_state&)> const& change);

} // namespace geosim::sim
