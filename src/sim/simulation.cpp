#include "sim/simulation.hpp"

#include "core/scheduler.hpp"
#include "sim/systems.hpp"

namespace geosim::sim {

void advance_day(game_state& state) {
    apply_events(state);
    update_ai(state);
    update_epidemic(state);
    update_economy(state);
    update_oil_market(state);
    update_demography(state);
    update_migration(state);
    update_opinion(state);
    update_unrest(state);
    update_politics(state);
    update_geopolitics(state);
    update_indicators(state);
    ++state.current_tick;
}

} // namespace geosim::sim
