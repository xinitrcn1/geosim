#include "sim/fork.hpp"

#include "core/keys.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/systems.hpp"

#include <cstddef>
#include <vector>

namespace geosim::sim {

std::unique_ptr<game_state> copy_state(game_state const& state, core::diagnostics& diag) {
    auto out = std::make_unique<game_state>();
    const auto record = state.world.serialize_entire_container_record();
    const uint64_t size = state.world.serialize_size(record);
    std::vector<std::byte> buffer(size);
    std::byte* cursor = buffer.data();
    state.world.serialize(cursor, record);
    dcon::load_record loaded;
    std::byte const* in = buffer.data();
    out->world.deserialize(in, buffer.data() + size, loaded);
    if(in != buffer.data() + size) {
        diag.fail("fork: state copy incomplete");
    }
    out->params = state.params;
    out->current_tick = state.current_tick;
    out->rng_seed = state.rng_seed;
    rebuild_derived(*out);
    return out;
}

namespace {

void run_copy(game_state& copy_ref, int days, fork_result& result);

} // namespace

fork_result run_fork(game_state const& state, int days, std::function<void(game_state&)> const& change, fork_options options) {
    fork_result result;
    core::diagnostics diag;
    auto copy = copy_state(state, diag);
    if(diag.failed()) {
        result.error = diag.error();
        return result;
    }
    if(options.reduced) {
        copy->params.ai_frozen = 1.0;
    }
    if(change) change(*copy);
    run_copy(*copy, days, result);
    return result;
}

fork_job::fork_job(game_state const& state, int days, std::function<void(game_state&)> change, fork_options options) {
    core::diagnostics diag;
    copy_ = copy_state(state, diag);
    if(diag.failed()) {
        result_.error = diag.error();
        joined_ = true;
        return;
    }
    if(options.reduced) {
        copy_->params.ai_frozen = 1.0;
    }
    if(change) change(*copy_);
    thread_ = std::thread([this, days] { run_copy(*copy_, days, result_); });
}

fork_job::~fork_job() {
    if(!joined_ && thread_.joinable()) thread_.join();
}

fork_result const& fork_job::wait() {
    if(!joined_ && thread_.joinable()) {
        thread_.join();
        joined_ = true;
    }
    return result_;
}

namespace {

void run_copy(game_state& copy_ref, int days, fork_result& result) {
    auto* copy = &copy_ref;
    auto& w = copy->world;
    for(auto n : w.in_nation) result.nations.push_back(fork_path{core::unpack_key(n.get_iso3()), {}, {}, {}, {}});
    for(int d = 0; d < days; ++d) {
        advance_day(*copy);
        for(auto n : w.in_nation) {
            auto& path = result.nations[n.id.index()];
            path.gdp_index.push_back(n.get_gdp_real() / std::max(1.0f, n.get_gdp0_daily() * growth_factor(*copy, n.id)));
            path.unemployment.push_back(n.get_unemployment());
            path.cpi.push_back(n.get_cpi());
            float u = 0.0f, pop = 0.0f;
            for(auto own : n.get_region_ownership()) {
                u += own.get_region().get_unrest() * own.get_region().get_population();
                pop += own.get_region().get_population();
            }
            path.unrest.push_back(pop > 0.0f ? u / pop : 0.0f);
        }
    }
}

} // namespace

} // namespace geosim::sim
