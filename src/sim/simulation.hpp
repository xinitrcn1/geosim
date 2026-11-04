#pragma once

#include "sim/game_state.hpp"

namespace geosim::sim {

// Simulates one day (tick) and advances current_tick.
// Order: events -> epidemic -> economy (weekly shards) -> oil -> monetary -> indicators.
void advance_day(game_state& state);

} // namespace geosim::sim
