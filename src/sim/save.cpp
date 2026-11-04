#include "sim/save.hpp"

#include "sim/world_io.hpp"

#include "core/csv.hpp"
#include "sim/scenario.hpp"

#include <cstring>
#include <fstream>
#include <optional>
#include <vector>

namespace geosim::sim {

namespace {

constexpr char magic[8] = {'G', 'E', 'O', 'S', 'I', 'M', 'S', 'V'};
constexpr uint32_t version = 3; // 2: logistics, commodities, credit, bonds, identity groups; 3: scenario epoch

template<typename T>
void put(std::vector<std::byte>& out, T const& v) {
    const auto* p = reinterpret_cast<std::byte const*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template<typename T>
std::optional<T> get(std::byte const*& in, std::byte const* end) {
    if(static_cast<std::size_t>(end - in) < sizeof(T)) {
        return std::nullopt;
    }
    T v;
    std::memcpy(&v, in, sizeof(T));
    in += sizeof(T);
    return v;
}

} // namespace

bool save_game(game_state const& state, std::string const& path, core::diagnostics& diag) {
    std::vector<std::byte> out;
    out.insert(out.end(), reinterpret_cast<std::byte const*>(magic), reinterpret_cast<std::byte const*>(magic) + 8);
    put(out, version);
    put(out, state.current_tick);
    put(out, state.epoch_days);
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

    std::vector<std::byte> world;
    serialize_world(state.world, world);
    put(out, static_cast<uint64_t>(world.size()));
    out.insert(out.end(), world.begin(), world.end());

    std::ofstream f(path, std::ios::binary);
    if(!f) {
        diag.fail("cannot write " + path);
        return false;
    }
    f.write(reinterpret_cast<char const*>(out.data()), static_cast<std::streamsize>(out.size()));
    if(!f) {
        diag.fail("write failed: " + path);
        return false;
    }
    return true;
}

bool load_game(game_state& state, std::string const& path, core::diagnostics& diag) {
    const auto text = core::read_text_file(path, diag);
    if(!text) {
        return false;
    }
    std::string const& bytes = *text;
    auto const* in = reinterpret_cast<std::byte const*>(bytes.data());
    auto const* end = in + bytes.size();
    auto truncated = [&] {
        diag.fail(path + ": save file truncated");
        return false;
    };
    if(bytes.size() < 8 || std::memcmp(bytes.data(), magic, 8) != 0) {
        diag.fail(path + ": not a geosim save");
        return false;
    }
    in += 8;
    const auto ver = get<uint32_t>(in, end);
    if(!ver) return truncated();
    if(*ver != version) {
        diag.fail(path + ": unsupported save version " + std::to_string(*ver) + " (expected " + std::to_string(version) + ")");
        return false;
    }
    const auto tick = get<int32_t>(in, end);
    const auto epoch = get<int64_t>(in, end);
    const auto seed = get<uint64_t>(in, end);
    const auto count = get<uint32_t>(in, end);
    if(!tick || !epoch || !seed || !count) return truncated();
    state.current_tick = *tick;
    state.epoch_days = *epoch;
    state.rng_seed = *seed;
    for(uint32_t i = 0; i < *count; ++i) {
        const auto len = get<uint32_t>(in, end);
        if(!len || static_cast<std::size_t>(end - in) < *len) return truncated();
        std::string name(reinterpret_cast<char const*>(in), *len);
        in += *len;
        const auto value = get<double>(in, end);
        if(!value) return truncated();
        set_param(state.params, name, *value); // unknown (removed) parameters are ignored
    }
    const auto size = get<uint64_t>(in, end);
    if(!size || static_cast<uint64_t>(end - in) < *size) return truncated();
    if(!deserialize_world(state.world, in, static_cast<std::size_t>(*size))) {
        diag.fail(path + ": save file corrupt (container size mismatch)");
        return false;
    }
    rebuild_derived(state);
    return true;
}

} // namespace geosim::sim
