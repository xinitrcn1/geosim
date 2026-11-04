#pragma once
// The one place that instantiates DCON's whole-container serialisation (a large
// amount of generated code, slow to compile): save files and fork copies use it.

#include "sim/game_state.hpp"

#include <cstddef>
#include <vector>

namespace geosim::sim {

// Appends the serialised container to `out`.
void serialize_world(dcon::data_container const& world, std::vector<std::byte>& out);
// Reads a container serialised by serialize_world from [data, data + size); returns
// false if it did not consume exactly `size` bytes.
[[nodiscard]] bool deserialize_world(dcon::data_container& world, std::byte const* data, std::size_t size);

} // namespace geosim::sim
