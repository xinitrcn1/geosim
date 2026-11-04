#pragma once
// The procedural composer (docs/14-audio.md#composer): seed + mood -> a
// complete song (form, harmony, motifs, rhythm, instruments, mix).
// Deterministic: the same seed and mood always give the same song, on any
// platform (own PRNG, no std distributions). Runs on a worker thread; it
// allocates freely, the audio thread never calls it.

#include "song.hpp"

#include <cstdint>

namespace geosim::client::audio {

[[nodiscard]] song compose(uint64_t seed, mood m);

// splitmix64 step: the next song's seed from the previous one.
[[nodiscard]] uint64_t next_seed(uint64_t seed) noexcept;

} // namespace geosim::client::audio
