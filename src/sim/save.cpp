#include "sim/save.hpp"

#include "core/csv.hpp"
#include "sim/scenario.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace geosim::sim {

namespace {

constexpr char magic[8] = {'G', 'E', 'O', 'S', 'I', 'M', 'S', 'V'};
constexpr uint32_t version = 1;

template<typename T>
void put(std::vector<std::byte>& out, T const& v) {
    const auto* p = reinterpret_cast<std::byte const*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template<typename T>
T get(std::byte const*& in, std::byte const* end) {
    if(static_cast<std::size_t>(end - in) < sizeof(T)) {
        throw std::runtime_error("save file truncated");
    }
    T v;
    std::memcpy(&v, in, sizeof(T));
    in += sizeof(T);
    return v;
}

} // namespace

void save_game(game_state const& state, std::string const& path) {
    std::vector<std::byte> out;
    out.insert(out.end(), reinterpret_cast<std::byte const*>(magic), reinterpret_cast<std::byte const*>(magic) + 8);
    put(out, version);
    put(out, state.current_tick);
    put(out, state.rng_seed);

    uint32_t count = 0;
    for_each_param(state.params, [&](char const*, double, char const*) { ++count; });
    put(out, count);
    for_each_param(state.params, [&](char const* name, double value, char const*) {
        const auto len = static_cast<uint32_t>(std::strlen(name));
        put(out, len);
        out.insert(out.end(), reinterpret_cast<std::byte const*>(name), reinterpret_cast<std::byte const*>(name) + len);
        put(out, value);
    });

    const auto record = state.world.serialize_entire_container_record();
    const uint64_t size = state.world.serialize_size(record);
    put(out, size);
    const std::size_t at = out.size();
    out.resize(at + size);
    std::byte* cursor = out.data() + at;
    state.world.serialize(cursor, record);

    std::ofstream f(path, std::ios::binary);
    if(!f) {
        throw std::runtime_error("cannot write " + path);
    }
    f.write(reinterpret_cast<char const*>(out.data()), static_cast<std::streamsize>(out.size()));
}

void load_game(game_state& state, std::string const& path) {
    const std::string bytes = core::read_text_file(path);
    auto const* in = reinterpret_cast<std::byte const*>(bytes.data());
    auto const* end = in + bytes.size();
    if(bytes.size() < 8 || std::memcmp(bytes.data(), magic, 8) != 0) {
        throw std::runtime_error(path + ": not a geosim save");
    }
    in += 8;
    if(get<uint32_t>(in, end) != version) {
        throw std::runtime_error(path + ": unsupported save version");
    }
    state.current_tick = get<int32_t>(in, end);
    state.rng_seed = get<uint64_t>(in, end);
    const auto count = get<uint32_t>(in, end);
    for(uint32_t i = 0; i < count; ++i) {
        const auto len = get<uint32_t>(in, end);
        if(static_cast<std::size_t>(end - in) < len) {
            throw std::runtime_error(path + ": save file truncated");
        }
        std::string name(reinterpret_cast<char const*>(in), len);
        in += len;
        const double value = get<double>(in, end);
        set_param(state.params, name, value); // unknown (removed) parameters are ignored
    }
    const auto size = get<uint64_t>(in, end);
    if(static_cast<uint64_t>(end - in) < size) {
        throw std::runtime_error(path + ": save file truncated");
    }
    dcon::load_record loaded;
    std::byte const* cursor = in;
    state.world.deserialize(cursor, in + size, loaded);
    rebuild_derived(state);
}

} // namespace geosim::sim
