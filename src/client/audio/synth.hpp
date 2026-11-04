#pragma once
// The song player (docs/14-audio.md#synthesis): sequencer + voices + mixer.
//
// Runs on the audio thread. Real-time rules: render() never allocates,
// locks or does I/O; all buffers are sized in the constructor. The song is
// read-only while it plays and must outlive set_song(nullptr).
//
// Signal path per channel: voices (2 oscillators + sub, or a drum model)
// -> optional 8-bit sample-and-hold -> SVF low-pass -> amp envelope; the
// channel sum is gain/pan/sidechain-ducked into the stereo bus and sent to
// a tempo-synced ping-pong delay and a small Schroeder reverb; the bus goes
// through a DC blocker, a look-ahead peak limiter (-1 dBFS) and a soft clip.

#include "song.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace geosim::client::audio {

class player {
public:
    explicit player(float sample_rate);

    // Starts `s` from its first tick (nullptr: stop the sequencer; voices
    // and effect tails ring out).
    void set_song(song const* s) noexcept;
    [[nodiscard]] bool playing() const noexcept { return song_ != nullptr; }
    // True once the song's last tick has passed (the caller then calls set_song).
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    void set_intensity(float target) noexcept { intensity_target_ = target; } // 0..1, smoothed over ~6 s
    void set_intensity_now(float value) noexcept { intensity_ = intensity_target_ = value; }
    void set_volume(float v) noexcept { volume_target_ = v; }                   // master, smoothed
    // Jump to the outro at the next 8-bar boundary (mood changed mid-song).
    void request_outro() noexcept { outro_requested_ = true; }

    // Interleaved stereo float frames.
    void render(float* out, int frames) noexcept;

    [[nodiscard]] uint32_t tick() const noexcept { return tick_; }
    [[nodiscard]] float intensity() const noexcept { return intensity_; }
    [[nodiscard]] int section_index() const noexcept { return section_; }
    [[nodiscard]] uint64_t kicks_played() const noexcept { return kicks_; }

    static constexpr int voices_per_channel = 6;

private:
    struct voice {
        bool active = false, gate = false;
        uint8_t ch = 0, count = 1, drum = 0, arp_index = 0;
        note_fx fx = note_fx::none;
        uint32_t start_tick = 0;
        float freq[4] = {};
        float phase1[4] = {}, phase2[4] = {}, phase_sub[4] = {};
        float phase_u[4][4] = {};  // unison saws
        float glide_from = 1.0f;   // frequency ratio at the start of a glide, decays to 1
        float vel = 1.0f;
        float env = 0.0f, fenv = 1.0f;
        int stage = 0;             // 0 attack, 1 decay/sustain, 2 release
        float release_coef = 0.0f; // per-sample multiplier while releasing
        double gate_left = 0;      // samples until note-off
        float age = 0.0f;          // seconds
        float len_s = 0.0f;        // event length in seconds (risers)
        float ic1 = 0, ic2 = 0;    // filter state
        float g = 0, k = 1;        // filter coefficients (control rate)
        float ic1b = 0, ic2b = 0;  // second filter (drums)
        float freq_mul = 1.0f, duty = 0.5f;
        float dcoef = 0, fcoef = 0, ratio2 = 1; // per-sample decay multipliers, osc 2 ratio
        float crush_phase = 0, crush_hold = 0;
        float attack = 0.005f, sustain = 0.7f; // envelope (the patch's, or a stab's / swell's)
        float bend = 0.0f;         // slide_up: semitones reached at the event's end
        uint16_t gate_mask = 0;    // trance gate over the 16ths of a bar (0 = off)
        float gate_amp = 1.0f;     // smoothed gate level
        float dphase = 0;          // drum oscillator phase
        float metal[6] = {};       // hat oscillator phases
        uint32_t noise = 22222u;
    };

    void process_tick() noexcept;
    void trigger(note_event const& e) noexcept;
    void release_channel(int ch, uint32_t before_tick, bool fast, float min_release = 0.0f) noexcept;
    voice& allocate(int ch) noexcept;
    void update_control(voice& v) noexcept;
    float render_tonal(voice& v, patch const& p) noexcept;
    float render_drum(voice& v, patch const& p, bool control) noexcept;
    [[nodiscard]] bool layer_on(uint8_t layer) const noexcept;
    void jump_to_outro() noexcept;

    float sr_;
    float inv_sr_;
    song const* song_ = nullptr;
    bool finished_ = false;
    std::size_t next_event_ = 0;
    uint32_t tick_ = 0;
    double samples_per_tick_ = 1000;
    double to_next_tick_ = 0;
    int section_ = 0;
    bool outro_requested_ = false;
    int control_counter_ = 0;
    uint64_t kicks_ = 0;

    float intensity_ = 0.5f, intensity_target_ = 0.5f;
    float volume_ = 0.0f, volume_target_ = 0.8f;
    float bright_mul_ = 1.0f;
    float row_seconds_ = 0.125f; // one 16th note

    std::array<std::array<voice, voices_per_channel>, num_channels> voices_{};
    patch patches_[num_channels];
    mix_settings mix_;
    float pan_l_[num_channels]{}, pan_r_[num_channels]{};
    float duck_t_ = 10.0f; // seconds since the last kick

    // Bus low-pass sweep (dr_lp_close / dr_lp_open): every channel but the
    // kick and the fx goes through its own SVF while a sweep runs.
    int sweep_dir_ = 0;         // 0 off, -1 closing, +1 opening
    float sweep_t_ = 0, sweep_len_ = 1;
    float sweep_g_ = 1, sweep_k_ = 1.3f;
    float sweep_ic1_[num_channels]{}, sweep_ic2_[num_channels]{};

    // effects
    // stereo chorus per channel: a short delay line read by two LFO-swept taps
    static constexpr int chorus_len = 2048;
    std::vector<float> chorus_buf_; // num_channels * chorus_len
    int chorus_pos_ = 0;
    float chorus_phase_ = 0.0f;
    std::vector<float> delay_l_, delay_r_;
    std::size_t delay_pos_ = 0, delay_len_ = 1;
    float delay_lp_l_ = 0, delay_lp_r_ = 0;
    static constexpr int comb_count = 4, allpass_count = 2, comb_max = 3400, allpass_max = 1400;
    struct comb {
        std::array<float, comb_max> buf{};
        int len = 1000, pos = 0;
        float store = 0;
    };
    struct allpass {
        std::array<float, allpass_max> buf{};
        int len = 500, pos = 0;
    };
    std::array<comb, comb_count * 2> combs_{};
    std::array<allpass, allpass_count * 2> allpasses_{};
    float dc_x_l_ = 0, dc_y_l_ = 0, dc_x_r_ = 0, dc_y_r_ = 0;

    // look-ahead limiter
    static constexpr int lookahead = 64;
    std::array<float, lookahead * 2> la_buf_{};
    int la_pos_ = 0;
    float lim_hold_ = 1.0f, lim_gain_ = 1.0f;
    int lim_hold_count_ = 0;

    float tables_[wavetable_count][wavetable_len]{};
};

} // namespace geosim::client::audio
