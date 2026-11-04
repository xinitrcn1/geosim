#pragma once
// Procedural music: the data a composed song is made of (docs/14-audio.md).
//
// A song is a tracker-style score: 8 channels, a fixed BPM, 16th-note rows
// (4 rows per beat, 64 rows per 4-bar pattern) subdivided into 6 tracker
// ticks per row. The composer (composer.hpp) writes it once, on a worker
// thread; the player (synth.hpp) only reads it, on the audio thread.

#include <cstdint>
#include <string>
#include <vector>

namespace geosim::client::audio {

enum class mood : uint8_t { calm, tense, crisis, triumph };
inline constexpr int mood_count = 4;
[[nodiscard]] char const* mood_name(mood m) noexcept;
[[nodiscard]] bool parse_mood(char const* text, mood& out) noexcept;

inline constexpr int num_channels = 8;
enum channel : uint8_t { ch_kick, ch_snare, ch_hats, ch_bass, ch_lead, ch_arp, ch_pad, ch_fx };
[[nodiscard]] char const* channel_name(int ch) noexcept;

inline constexpr int rows_per_beat = 4;   // 16th notes
inline constexpr int rows_per_bar = 16;   // 4/4
inline constexpr int ticks_per_row = 6;   // tracker "speed 6"
inline constexpr int ticks_per_bar = rows_per_bar * ticks_per_row;

// Drum-kit note numbers (General MIDI-like). Channels whose patch is a kit
// pick the sound from the note.
enum drum : uint8_t {
    dr_kick = 36,
    dr_rim = 37,
    dr_snare = 38,
    dr_clap = 39,
    dr_hat_closed = 42,
    dr_tom_low = 45,
    dr_hat_open = 46,
    dr_tom_mid = 47,
    dr_crash = 49,
    dr_tom_high = 50,
    dr_shaker = 70,
    dr_riser = 90,  // noise sweep up over the event length
    dr_impact = 91, // low boom + noise burst (start of a drop)
    dr_swell = 92,  // reversed-cymbal-like swell over the event length
    dr_lp_close = 93,  // not a sound: the music bus low-pass closes over the event length, then reopens
    dr_lp_open = 94,   // not a sound: the music bus low-pass starts closed and opens over the event length
    dr_downlifter = 95, // noise sweep down (falling energy after a peak)
};

enum class wave : uint8_t { pulse, saw, triangle, sine, noise, table };
inline constexpr int wavetable_count = 4; // "sample-like" 32-step 8-bit cycles
inline constexpr int wavetable_len = 32;

// One instrument. Tonal patches are two oscillators -> state-variable
// low-pass with its own envelope -> amp ADSR, optional Amiga-style
// sample-rate/bit reduction. `kit` patches synthesise drums instead.
struct patch {
    bool kit = false;
    bool mono = true;        // a new note cuts the previous one (tracker channel)
    wave w1 = wave::saw, w2 = wave::saw;
    int table1 = 0, table2 = 0;
    float mix2 = 0.0f;       // osc 2 level (osc 1 is 1)
    float detune = 0.0f;     // osc 2 detune, cents
    float semi2 = 0.0f;      // osc 2 transpose, semitones
    float duty = 0.5f, pwm_depth = 0.0f, pwm_rate = 0.0f; // pulse width and its LFO (Hz)
    float sub = 0.0f;        // sine one octave below
    float attack = 0.005f, decay = 0.2f, sustain = 0.7f, release = 0.15f; // seconds / level
    float cutoff = 4000.0f;  // Hz
    float resonance = 0.2f;  // 0..1
    float fenv = 0.0f;       // filter envelope depth, octaves
    float fdecay = 0.2f;     // filter envelope decay, seconds
    float key_track = 0.5f;  // cutoff follows the note (0..1)
    float glide = 0.0f;      // portamento time when notes overlap, seconds
    float vib_depth = 0.0f, vib_rate = 5.0f, vib_delay = 0.2f; // semitones, Hz, s
    float crush_rate = 0.0f; // sample-and-hold rate in Hz (0 = off), with 8-bit quantisation
    float drive = 0.0f;      // pre-filter saturation
    float gain = 1.0f;
    // kit parameters
    float kick_pitch = 160.0f, kick_end = 48.0f, kick_decay = 0.35f, kick_click = 0.5f;
    float snare_tone = 190.0f, snare_decay = 0.16f, hat_decay = 0.045f, hat_tone = 1.0f;
};

// Layer flags: an event plays only while the live intensity is above
// (layer_min_*) or below (layer_max_*) a threshold, so the arrangement can
// thicken or thin out within a song without recomposing it.
enum layer : uint8_t {
    layer_always = 0,
    layer_min_mid = 1,  // intensity >= 0.4
    layer_min_high = 2, // intensity >= 0.7
    layer_max_mid = 3,  // intensity < 0.4
    layer_max_high = 4, // intensity < 0.7
};
inline constexpr float layer_mid = 0.4f, layer_high = 0.7f;

// arp: cycle through notes[] every tick (chip chord); stab: short pluck
// envelope (2 ms attack, fast decay to silence) whatever the patch says.
enum class note_fx : uint8_t { none, arp, slide_up, stab };

struct note_event {
    uint32_t tick = 0;       // absolute tracker tick (row * 6 + offset)
    uint16_t len = 6;        // ticks until note-off
    uint8_t ch = 0;
    uint8_t vel = 100;       // 0..127
    uint8_t layer = layer_always;
    note_fx fx = note_fx::none; // arp: cycle through notes[] every tick (chip chord)
    uint8_t count = 1;       // notes used
    uint8_t notes[4] = {60, 0, 0, 0}; // MIDI notes (or drum ids on kit channels)
    uint16_t gate = 0;       // trance gate: bit r open on the r-th 16th of each bar from the note start (0 = no gate)
};

enum class section_kind : uint8_t { intro, verse, chorus, bridge, breakdown, drop, outro };
[[nodiscard]] char const* section_name(section_kind k) noexcept;

struct section {
    section_kind kind = section_kind::verse;
    uint32_t start_bar = 0, bars = 8;
};

struct mix_settings {
    float gain[num_channels]{};
    float pan[num_channels]{};    // -1..1
    float delay[num_channels]{};  // send levels
    float reverb[num_channels]{};
    float duck[num_channels]{};   // sidechain depth from the kick
    bool bright[num_channels]{};  // cutoff follows the live intensity
    float delay_rows = 3.0f;      // dotted eighth
    float delay_feedback = 0.4f;
    float reverb_size = 0.8f;
};

struct song {
    uint64_t seed = 0;
    mood m = mood::calm;
    float bpm = 120.0f;
    int key = 0;             // tonic MIDI note (e.g. 57 = A3)
    int mode = 0;            // index into the composer's mode table
    std::string description; // "seed 7, calm, 116 BPM, D dorian, ..." for logs
    patch patches[num_channels];
    mix_settings mix;
    std::vector<section> sections;
    std::vector<note_event> events; // sorted by tick
    uint32_t total_bars = 0;

    [[nodiscard]] uint32_t total_ticks() const noexcept { return total_bars * ticks_per_bar; }
    [[nodiscard]] double seconds_per_tick() const noexcept { return 60.0 / (static_cast<double>(bpm) * rows_per_beat * ticks_per_row); }
    [[nodiscard]] double duration_seconds() const noexcept { return total_ticks() * seconds_per_tick(); }
};

} // namespace geosim::client::audio
