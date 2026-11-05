#pragma once

#include "sim/game_state.hpp"

#include <cstdio>

namespace geosim::sim {

// Simulates one day (tick) and advances current_tick.
// Order: events (anchors -> priors) -> AI decisions -> epidemic -> economy (weekly shards) -> oil -> indicators.
void advance_day(game_state& state);

// Worker threads the simulation's parallel loops may use (oneTBB global_control; the
// calling thread counts). Default 2; set before the first tick, process-wide.
void set_thread_limit(int threads);

// Per-system wall-time profile of advance_day (headless --profile 1).
void set_profiling(bool on);
void write_profile(std::FILE* out);

} // namespace geosim::sim
