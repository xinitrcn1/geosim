#include "jukebox.hpp"

#include <algorithm>

namespace geosim::client::audio {

float default_intensity(mood m) noexcept {
    switch(m) {
    case mood::calm: return 0.3f;
    case mood::tense: return 0.55f;
    case mood::crisis: return 0.85f;
    case mood::triumph: return 0.72f;
    case mood::wartime: return 0.9f;
    }
    return 0.5f;
}

jukebox::jukebox(float sample_rate, take_fn take, retire_fn retire, void* user)
    : player_(sample_rate), take_(take), retire_(retire), user_(user), sr_(sample_rate) {}

void jukebox::set_mood(mood m, float intensity) noexcept {
    mood_ = m;
    player_.set_intensity(std::clamp(intensity, 0.0f, 1.0f));
}

void jukebox::skip() noexcept {
    if(current_) skipping_ = true;
    else gap_left_ = std::min(gap_left_, 0.3); // between songs: start the next one now
}

void jukebox::render(float* out, int frames) noexcept {
    const double dt = static_cast<double>(frames) / static_cast<double>(sr_);
    if(!current_) {
        gap_left_ -= dt;
        if(gap_left_ <= 0) {
            current_ = take_(user_);
            if(current_) {
                ++started_;
                mood_held_ = 0;
                player_.set_song(current_);
            }
        }
    } else {
        // A mood different from the song's, held long enough, wraps the
        // song up: the player jumps to the outro at the next 8-bar line.
        // War starting or ending is an event, not noise: it wraps up at once
        // (still at the next 8-bar line).
        mood_held_ = current_->m != mood_ ? mood_held_ + dt : 0.0;
        const bool war_change = current_->m != mood_ && (current_->m == mood::wartime || mood_ == mood::wartime);
        if(mood_held_ >= static_cast<double>(war_change ? 0.0f : wrap_after_seconds) && current_->m != mood_) player_.request_outro();
    }
    player_.render(out, frames);
    if(skipping_) {
        // linear fade over the block, then stop at silence
        const float step = static_cast<float>(1.0 / (static_cast<double>(skip_fade_seconds) * static_cast<double>(sr_)));
        for(int i = 0; i < frames; ++i) {
            fade_ = std::max(0.0f, fade_ - step);
            out[2 * i] *= fade_;
            out[2 * i + 1] *= fade_;
        }
        if(fade_ <= 0.0f) {
            player_.hard_stop();
            if(current_) retire_(user_, current_);
            current_ = nullptr;
            skipping_ = false;
            fade_ = 1.0f;
            gap_left_ = 0.3;
        }
        return;
    }
    if(current_ && player_.finished()) {
        player_.set_song(nullptr);
        retire_(user_, current_);
        current_ = nullptr;
        gap_left_ = gap_seconds;
    }
}

} // namespace geosim::client::audio
