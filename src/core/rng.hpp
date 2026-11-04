#pragma once
// Counter-based random numbers (Philox4x32-10; Salmon et al. 2011).
//
// There is no RNG state: every draw is a pure function of (seed, stream,
// counter), so parallel systems stay deterministic and a save never needs to
// store generator state. Convention: stream = system id, counter = (tick,
// entity index, draw index). See docs/02-simulation-core.md#determinism.

#include <array>
#include <cstdint>

namespace geosim::core {

namespace detail {
inline constexpr uint32_t philox_m0 = 0xD2511F53u;
inline constexpr uint32_t philox_m1 = 0xCD9E8D57u;
inline constexpr uint32_t philox_w0 = 0x9E3779B9u;
inline constexpr uint32_t philox_w1 = 0xBB67AE85u;

constexpr void mulhilo(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) noexcept {
    const uint64_t p = static_cast<uint64_t>(a) * b;
    hi = static_cast<uint32_t>(p >> 32);
    lo = static_cast<uint32_t>(p);
}
} // namespace detail

[[nodiscard]] constexpr std::array<uint32_t, 4> philox4x32(std::array<uint32_t, 4> ctr, std::array<uint32_t, 2> key) noexcept {
    for(int round = 0; round < 10; ++round) {
        uint32_t hi0 = 0, lo0 = 0, hi1 = 0, lo1 = 0;
        detail::mulhilo(detail::philox_m0, ctr[0], hi0, lo0);
        detail::mulhilo(detail::philox_m1, ctr[2], hi1, lo1);
        ctr = {hi1 ^ ctr[1] ^ key[0], lo1, hi0 ^ ctr[3] ^ key[1], lo0};
        key[0] += detail::philox_w0;
        key[1] += detail::philox_w1;
    }
    return ctr;
}

// Uniform in [0, 1) from one draw.
[[nodiscard]] constexpr float uniform01(uint64_t seed, uint32_t stream, uint32_t tick, uint32_t entity, uint32_t draw = 0) noexcept {
    const auto r = philox4x32({tick, entity, draw, stream},
                              {static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32)});
    return static_cast<float>(r[0] >> 8) * (1.0f / 16777216.0f);
}

// System ids used as RNG streams (stable across versions: append only).
enum class rng_stream : uint32_t { conflict = 1, unrest = 2, elections = 3, migration = 4, logistics = 5, commodities = 6, cyber = 7, ai = 8, climate = 9 };

} // namespace geosim::core
