#pragma once
// Binary scenario/save files: a small header, the global parameters, then the
// whole DCON container in DCON's own serialisation format.

#include "core/diagnostics.hpp"
#include "sim/game_state.hpp"

#include <string>

namespace geosim::sim {

// Both return false and record the reason in `diag` on failure.
bool save_game(game_state const& state, std::string const& path, core::diagnostics& diag);
// `state` must be freshly constructed.
bool load_game(game_state& state, std::string const& path, core::diagnostics& diag);

} // namespace geosim::sim
