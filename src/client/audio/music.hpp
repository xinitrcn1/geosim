#pragma once
// Live dynamic music (docs/14-audio.md#real-time-design).
//
// Threads:
//  - the caller (UI thread) sets the mood: set_mood() stores atomics;
//  - a worker thread composes the next song for the current mood ahead of
//    time and frees finished songs;
//  - SDL's audio thread pulls samples through a callback that runs the
//    jukebox (no allocation, no locks; songs are handed over through
//    atomic pointer slots).
// If no audio device can be opened start() returns false and logs a
// warning; the game runs without music.

#include "song.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

struct SDL_AudioStream;

namespace geosim::client::audio {

class jukebox;

class music_system {
public:
    music_system();
    music_system(music_system const&) = delete;
    music_system& operator=(music_system const&) = delete;
    ~music_system();

    // seed 0 = random. Returns false when audio is unavailable.
    bool start(uint64_t seed, float volume);
    void stop();
    [[nodiscard]] bool running() const noexcept { return stream_ != nullptr; }

    // Any thread. Intensity 0..1 thickens/thins the arrangement within the
    // song (layers, brightness); a lasting mood change wraps the song up and
    // the next song is composed for the new mood.
    void set_mood(mood m, float intensity) noexcept;
    void set_volume(float v) noexcept { volume_.store(v, std::memory_order_relaxed); }
    void set_muted(bool m) noexcept { muted_.store(m, std::memory_order_relaxed); }
    [[nodiscard]] bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

private:
    static void audio_callback(void* user, SDL_AudioStream* stream, int additional, int total);
    static song* take(void* user);
    static void retire(void* user, song* s);
    void worker();
    void free_retired();

    SDL_AudioStream* stream_ = nullptr;
    std::unique_ptr<jukebox> box_; // audio thread only once started
    std::jthread thread_; // joins on destruction
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> quit_{false};

    std::atomic<song*> next_{nullptr};
    static constexpr int retire_slots = 8;
    std::atomic<song*> retired_[retire_slots] = {};
    std::atomic<int> mood_{0};
    std::atomic<float> intensity_{0.3f};
    std::atomic<float> volume_{0.7f};
    std::atomic<bool> muted_{false};

    // worker only
    uint64_t seed_ = 0;
    song* queued_ = nullptr;
    int queued_mood_ = -1;

    static constexpr int block_frames = 512;
    float scratch_[block_frames * 2] = {};
};

} // namespace geosim::client::audio
