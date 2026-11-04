#pragma once
// Cadence scheduler: spreads periodic system updates evenly across daily ticks.
// See docs/02-simulation-core.md#cadence-scheduler.

#include <cstdint>

namespace geosim::sim {

using tick_t = std::uint32_t; // days since 2020-01-01

// A system with cadence {period, shards} updates every entity once per
// `period` ticks. The work is split into `shards` slices, and one slice runs
// every period/shards ticks. Requires shards >= 1, period % shards == 0.
struct cadence {
    std::uint16_t period_days;
    std::uint16_t shards;
};

inline constexpr cadence daily{1, 1};
inline constexpr cadence weekly{7, 7};
inline constexpr cadence monthly{28, 28};    // sim "month" = 4 weeks; calendar months are a reporting concern
inline constexpr cadence quarterly{91, 91};
inline constexpr cadence annual{364, 364};   // 52 weeks, keeps all cadences on a common weekly grid

[[nodiscard]] constexpr bool is_valid(cadence c) noexcept {
    return c.shards >= 1 && c.period_days >= c.shards && c.period_days % c.shards == 0;
}

// Shard an entity belongs to for a given cadence.
[[nodiscard]] std::uint32_t shard_of(std::uint32_t entity_index, cadence c) noexcept;

// Shard that runs on `tick`, or -1 if this cadence does nothing on that tick.
[[nodiscard]] std::int32_t active_shard(tick_t tick, cadence c) noexcept;

// True if `entity_index` is updated on `tick` under cadence `c`.
[[nodiscard]] bool is_due(std::uint32_t entity_index, tick_t tick, cadence c) noexcept;

} // namespace geosim::sim
