#include "core/scheduler.hpp"

namespace geosim::sim {

std::uint32_t shard_of(std::uint32_t entity_index, cadence c) noexcept {
    return entity_index % c.shards;
}

std::int32_t active_shard(tick_t tick, cadence c) noexcept {
    const std::uint32_t stride = c.period_days / c.shards; // ticks between shard runs
    if(tick % stride != 0) {
        return -1;
    }
    return static_cast<std::int32_t>((tick / stride) % c.shards);
}

bool is_due(std::uint32_t entity_index, tick_t tick, cadence c) noexcept {
    const std::int32_t s = active_shard(tick, c);
    return s >= 0 && static_cast<std::uint32_t>(s) == shard_of(entity_index, c);
}

} // namespace geosim::sim
