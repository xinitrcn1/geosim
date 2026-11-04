#include "sim/simulation.hpp"

#include "core/scheduler.hpp"
#include "sim/systems.hpp"

namespace geosim::sim {

void advance_day(game_state& state) {
    const auto tick = static_cast<tick_t>(state.current_tick);
    apply_events(state);
    update_ai(state);
    update_epidemic(state);
    for(auto n : state.world.in_nation) {
        if(is_due(n.id.index(), tick, weekly)) {
            update_economy(state, n.id, static_cast<float>(weekly.period_days));
        }
    }
    update_oil_market(state);
    update_indicators(state);
    ++state.current_tick;
}

} // namespace geosim::sim
