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
#include <thread>
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

struct fork_options {
    // Reduced detail (docs/02): every nation at low detail (28-day economy steps) and
    // agents frozen on their current plans. Faster and approximate; label it so.
    bool reduced = false;
};

// Copies `state`, calls `change` on the copy (e.g. set an override), advances it
// `days` days and records the paths. `state` is unchanged.
[[nodiscard]] fork_result run_fork(game_state const& state, int days, std::function<void(game_state&)> const& change,
                                   fork_options options = {});

// A fork on a worker thread. The state is copied before the thread starts, so the
// caller may keep simulating the live state. The UI runs up to two at once.
class fork_job {
public:
    fork_job(game_state const& state, int days, std::function<void(game_state&)> change, fork_options options = {});
    ~fork_job();
    fork_job(fork_job const&) = delete;
    fork_job& operator=(fork_job const&) = delete;
    // Blocks until the fork has finished.
    fork_result const& wait();

private:
    std::unique_ptr<game_state> copy_;
    fork_result result_;
    std::jthread thread_; // joins on destruction
    bool joined_ = false;
};

} // namespace geosim::sim
