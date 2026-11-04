#pragma once
// Binary scenario/save files: a small header, the global parameters, then the
// whole DCON container in DCON's own serialisation format.

#include "sim/game_state.hpp"

#include <string>

namespace geosim::sim {

void save_game(game_state const& state, std::string const& path);
// `state` must be freshly constructed. Throws std::runtime_error on bad files.
void load_game(game_state& state, std::string const& path);

} // namespace geosim::sim
