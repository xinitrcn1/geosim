#include "sim/simulation.hpp"

#include "core/scheduler.hpp"
#include "sim/systems.hpp"

namespace geosim::sim {

void advance_day(game_state& state) {
    apply_events(state);
    update_ai(state);
    update_epidemic(state);
    update_logistics(state);
    update_commodity_markets(state);
    update_economy(state);
    update_oil_market(state);
    update_demography(state);
    update_migration(state);
    update_groups(state);
    update_opinion(state);
    update_unrest(state);
    update_protest(state);
    update_politics(state);
    update_geopolitics(state);
    update_finance(state);
    update_credit(state);
    update_bonds(state);
    update_longrun(state);
    update_indicators(state);
    ++state.current_tick;
}

} // namespace geosim::sim
