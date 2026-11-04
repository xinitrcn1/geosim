// Invariant: under any cadence, every entity is updated exactly once per
// period, and per-tick work is balanced across ticks.

#include "core/scheduler.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace geosim::sim;

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if(!(cond)) {                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while(0)

static void check_exactly_once_per_period(cadence c, std::uint32_t entities, tick_t start) {
    CHECK(is_valid(c));
    std::vector<int> hits(entities, 0);
    std::uint32_t max_per_tick = 0;
    for(tick_t t = start; t < start + c.period_days; ++t) {
        std::uint32_t this_tick = 0;
        for(std::uint32_t e = 0; e < entities; ++e) {
            if(is_due(e, t, c)) {
                ++hits[e];
                ++this_tick;
            }
        }
        if(this_tick > max_per_tick) {
            max_per_tick = this_tick;
        }
    }
    for(std::uint32_t e = 0; e < entities; ++e) {
        CHECK(hits[e] == 1);
    }
    // Balanced load: no tick does more than one shard's worth (ceil).
    CHECK(max_per_tick <= (entities + c.shards - 1) / c.shards);
}

int main() {
    const cadence all[] = {daily, weekly, monthly, quarterly, annual, cadence{28, 7}, cadence{364, 52}};
    for(const cadence c : all) {
        // Region-scale count, and a start tick not aligned to the period.
        check_exactly_once_per_period(c, 4096, 0);
        check_exactly_once_per_period(c, 4093, 1000);
    }

    // Invalid cadences are detected.
    CHECK(!is_valid(cadence{30, 28}));
    CHECK(!is_valid(cadence{7, 0}));

    // Daily cadence touches everything every tick.
    CHECK(is_due(123, 42, daily));

    if(failures == 0) {
        std::puts("test_scheduler: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_scheduler: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
