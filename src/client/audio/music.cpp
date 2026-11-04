#include "music.hpp"

#include "composer.hpp"
#include "jukebox.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>

namespace geosim::client::audio {

music_system::music_system() = default;
music_system::~music_system() { stop(); }

bool music_system::start(uint64_t seed, float volume) {
    if(stream_) return true;
    if(!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("music: audio unavailable (%s); continuing without music", SDL_GetError());
        return false;
    }
    seed_ = seed ? seed : next_seed(static_cast<uint64_t>(SDL_GetTicksNS()) ^ static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
    volume_.store(volume, std::memory_order_relaxed);
    constexpr int rate = 48000;
    box_ = std::make_unique<jukebox>(static_cast<float>(rate), &music_system::take, &music_system::retire, this);
    const mood m = static_cast<mood>(mood_.load());
    box_->set_mood(m, intensity_.load());
    box_->set_volume(volume);

    // Compose the first song before the device starts pulling.
    queued_menu_ = menu_.load();
    queued_ = new song(queued_menu_ ? compose_menu(seed_) : compose(seed_, m));
    queued_mood_ = static_cast<int>(m);
    next_.store(queued_, std::memory_order_release);
    SDL_Log("music: queued %s", queued_->description.c_str());

    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, rate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &music_system::audio_callback, this);
    if(!stream_) {
        SDL_Log("music: cannot open an audio device (%s); continuing without music", SDL_GetError());
        delete next_.exchange(nullptr);
        queued_ = nullptr;
        box_.reset();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_Log("music: audio driver %s", SDL_GetCurrentAudioDriver());
    quit_.store(false);
    thread_ = std::jthread([this] { worker(); });
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void music_system::stop() {
    if(!stream_) return;
    SDL_DestroyAudioStream(stream_); // stops the callback
    stream_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_.store(true);
    }
    wake_.notify_all();
    if(thread_.joinable()) thread_.join();
    free_retired();
    song const* current = box_ ? box_->current() : nullptr;
    delete current;
    delete next_.exchange(nullptr);
    queued_ = nullptr;
    box_.reset();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void music_system::set_mood(mood m, float intensity) noexcept {
    mood_.store(static_cast<int>(m), std::memory_order_relaxed);
    intensity_.store(std::clamp(intensity, 0.0f, 1.0f), std::memory_order_relaxed);
}

// --- audio thread --------------------------------------------------------------

song* music_system::take(void* user) {
    auto* self = static_cast<music_system*>(user);
    return self->next_.exchange(nullptr, std::memory_order_acq_rel);
}

void music_system::retire(void* user, song* s) {
    auto* self = static_cast<music_system*>(user);
    for(auto& slot : self->retired_) {
        song* expected = nullptr;
        if(slot.compare_exchange_strong(expected, s, std::memory_order_acq_rel)) return;
    }
    // All slots busy (the worker frees them every 250 ms, songs last minutes):
    // leak rather than free on the audio thread.
}

void music_system::audio_callback(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* self = static_cast<music_system*>(user);
    jukebox& box = *self->box_;
    box.set_mood(static_cast<mood>(self->mood_.load(std::memory_order_relaxed)), self->intensity_.load(std::memory_order_relaxed));
    box.set_volume(self->muted_.load(std::memory_order_relaxed) ? 0.0f : self->volume_.load(std::memory_order_relaxed));
    if(self->skip_.exchange(false, std::memory_order_relaxed)) box.skip();
    int frames = additional / static_cast<int>(2 * sizeof(float));
    while(frames > 0) {
        const int n = std::min(frames, block_frames);
        box.render(self->scratch_, n);
        SDL_PutAudioStreamData(stream, self->scratch_, n * static_cast<int>(2 * sizeof(float)));
        frames -= n;
    }
}

// --- worker ----------------------------------------------------------------------

void music_system::free_retired() {
    for(auto& slot : retired_) delete slot.exchange(nullptr, std::memory_order_acq_rel);
}

void music_system::worker() {
    std::unique_lock<std::mutex> lock(mutex_);
    while(!quit_.load()) {
        wake_.wait_for(lock, std::chrono::milliseconds(250));
        if(quit_.load()) break;
        free_retired();
        // Keep the queued song matching the current mood. If the audio
        // thread took it (next_ empty), compose the following one.
        const int want = mood_.load(std::memory_order_relaxed);
        song* in_slot = next_.load(std::memory_order_acquire);
        const bool menu = menu_.load(std::memory_order_relaxed);
        if(in_slot == nullptr || in_slot != queued_ || queued_mood_ != want || queued_menu_ != menu) {
            seed_ = next_seed(seed_);
            song* fresh = new song(menu ? compose_menu(seed_) : compose(seed_, static_cast<mood>(want)));
            queued_menu_ = menu;
            song* old = next_.exchange(fresh, std::memory_order_acq_rel);
            delete old; // nullptr if the audio thread took it
            queued_ = fresh;
            queued_mood_ = want;
            SDL_Log("music: queued %s", fresh->description.c_str());
        }
    }
}

} // namespace geosim::client::audio
