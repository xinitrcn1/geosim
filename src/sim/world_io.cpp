#include "sim/world_io.hpp"

namespace geosim::sim {

void serialize_world(dcon::data_container const& world, std::vector<std::byte>& out) {
    const auto record = world.serialize_entire_container_record();
    const auto size = static_cast<std::size_t>(world.serialize_size(record));
    const std::size_t at = out.size();
    out.resize(at + size);
    std::byte* cursor = out.data() + at;
    world.serialize(cursor, record);
}

bool deserialize_world(dcon::data_container& world, std::byte const* data, std::size_t size) {
    dcon::load_record loaded;
    std::byte const* cursor = data;
    world.deserialize(cursor, data + size, loaded);
    return cursor == data + size;
}

} // namespace geosim::sim
