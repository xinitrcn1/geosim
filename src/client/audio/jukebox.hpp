#pragma once
// Song-to-song sequencing shared by the live music system (music.hpp) and
// the offline renderer (render.hpp): plays a song, leaves a short gap, takes
// the next one; follows the mood (intensity live, mood changes by wrapping
// the current song up through its outro). Audio-thread code: no allocation,
// no locks; songs come from and go back to the caller through two plain
// function pointers.

#include "synth.hpp"

#include <cstdint>

namespace geosim::client::audio {

// The intensity a mood implies when the caller has no finer estimate.
[[nodiscard]] float default_intensity(mood m) noexcept;

class jukebox {
public:
    using take_fn = song* (*)(void* user);              // next song or nullptr (not ready)
    using retire_fn = void (*)(void* user, song* done); // finished song, freed elsewhere

    jukebox(float sample_rate, take_fn take, retire_fn retire, void* user);

    // Desired mood and intensity; may be called between render() calls.
    void set_mood(mood m, float intensity) noexcept;
    void set_volume(float v) noexcept { player_.set_volume(v); }
    void render(float* out, int frames) noexcept;

    [[nodiscard]] song const* current() const noexcept { return current_; }
    [[nodiscard]] player const& voice_player() const noexcept { return player_; }
    [[nodiscard]] uint64_t songs_started() const noexcept { return started_; }

    float gap_seconds = 3.0f;       // silence between songs (effect tails ring out)
    float wrap_after_seconds = 20.0f; // a new mood must hold this long before the song wraps up

private:
    player player_;
    take_fn take_;
    retire_fn retire_;
    void* user_;
    song* current_ = nullptr;
    double gap_left_ = 0.5;         // seconds before the first song
    double mood_held_ = 0;          // seconds the desired mood differs from the song's
    mood mood_ = mood::calm;
    uint64_t started_ = 0;
    float sr_;
};

} // namespace geosim::client::audio
