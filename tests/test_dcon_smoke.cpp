// Checks that the generated DCON container works inside game_state::world:
// relationship indices, array properties, and DCON's TBB-backed
// execute_parallel / execute_serial.

#include "sim/game_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

int main() {
    // game_state is large (SoA arrays sized by the schema), so keep it off the stack.
    auto state = std::make_unique<geosim::game_state>();
    auto& world = state->world;

    const auto nation = world.create_nation();
    const auto region_a = world.create_region();
    const auto region_b = world.create_region();
    world.force_create_region_ownership(region_a, nation);
    world.force_create_region_ownership(region_b, nation);

    const auto pop = world.create_pop();
    world.pop_set_size(pop, 1000.0f);
    world.force_create_pop_location(pop, region_a);

    world.nation_resize_va0(3);
    world.nation_set_va0(nation, 2, 5.0f);

    int owned = 0;
    world.nation_for_each_region_ownership(nation, [&](dcon::region_ownership_id) { ++owned; });

    world.execute_parallel_over_region([&](auto ids) {
        world.region_set_population(ids, ve::fp_vector(2.0f));
    });
    float sum = 0.0f;
    world.execute_serial_over_region([&](auto ids) {
        sum += world.region_get_population(ids).reduce();
    });

    const bool ok = owned == 2
        && sum == 4.0f
        && world.nation_get_va0(nation, 2) == 5.0f
        && world.nation_get_va0_size() == 3
        && world.region_get_nation_from_region_ownership(region_b) == nation
        && world.pop_get_region_from_pop_location(pop) == region_a
        && world.pop_get_size(pop) == 1000.0f;

    std::puts(ok ? "test_dcon_smoke: OK" : "test_dcon_smoke: FAILED");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
